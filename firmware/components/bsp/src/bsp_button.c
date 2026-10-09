#include "bsp_button.h"
#include "bsp_button_ladder.h"
#include "bsp_pins.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_timer.h"
#include "esp_check.h"
#include <stdatomic.h>
static adc_oneshot_unit_handle_t adc;
static adc_cali_handle_t cali;
static bsp_btn_cb_t callback;
static void *callback_user;
static atomic_int voltage = -1;
static atomic_uchar stable;
static void poll(void *arg)
{
    (void)arg;
    static uint8_t candidate = 8, repeats;
    int raw = 0, mv = -1;
    if (adc_oneshot_read(adc, BSP_BTN_ADC_CHANNEL, &raw) == ESP_OK)
        adc_cali_raw_to_voltage(cali, raw, &mv);
    atomic_store(&voltage, mv);
    uint8_t next = bsp_button_decode_mv(mv);
    if (next != candidate) {
        candidate = next;
        repeats = 1;
    } else if (repeats < 3)
        repeats++;
    if (repeats == 3)
        atomic_store(&stable, candidate);
    callback(atomic_load(&stable), callback_user);
}
esp_err_t bsp_button_init(bsp_btn_cb_t cb, void *user)
{
    callback = cb;
    callback_user = user;
    const adc_oneshot_unit_init_cfg_t unit = {.unit_id = BSP_BTN_ADC_UNIT};
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit, &adc), "buttons", "ADC init");
    const adc_oneshot_chan_cfg_t channel = {.atten = ADC_ATTEN_DB_12,
                                            .bitwidth = ADC_BITWIDTH_DEFAULT};
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(adc, BSP_BTN_ADC_CHANNEL, &channel), "buttons",
                        "ADC channel");
    const adc_cali_curve_fitting_config_t calibration = {.unit_id = BSP_BTN_ADC_UNIT,
                                                         .chan = BSP_BTN_ADC_CHANNEL,
                                                         .atten = ADC_ATTEN_DB_12,
                                                         .bitwidth = ADC_BITWIDTH_DEFAULT};
    ESP_RETURN_ON_ERROR(adc_cali_create_scheme_curve_fitting(&calibration, &cali), "buttons",
                        "ADC calibration");
    esp_timer_handle_t timer;
    const esp_timer_create_args_t args = {.callback = poll, .name = "keys"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&args, &timer), "buttons", "timer");
    return esp_timer_start_periodic(timer, 10000);
}
int bsp_button_read_mv(void)
{
    return atomic_load(&voltage);
}
uint8_t bsp_button_read_keys(void)
{
    return atomic_load(&stable);
}
