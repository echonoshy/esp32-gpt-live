#include "firmware_update.h"
#include "portal.h"
#include "device_config.h"
#include "update_public_key.h"
#include "wifi.h"
#include "ui.h"
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static atomic_bool active;
bool firmware_update_active(void) { return atomic_load(&active); }
static bool version_newer(const char *candidate, const char *installed)
{
    unsigned a[3],b[3];char extra;
    if(strspn(candidate,"0123456789.")!=strlen(candidate)||strspn(installed,"0123456789.")!=strlen(installed))return false;
    if(sscanf(candidate,"%u.%u.%u%c",&a[0],&a[1],&a[2],&extra)!=3 ||
       sscanf(installed,"%u.%u.%u%c",&b[0],&b[1],&b[2],&extra)!=3) return false;
    for(unsigned i=0;i<3;i++){if(a[i]!=b[i])return a[i]>b[i];}
    return false;
}
static bool unhex(const char *s, unsigned char *out, size_t length)
{
    if(strlen(s)!=length*2)return false;
    for(size_t i=0;i<length;i++){
        unsigned char hi=(unsigned char)s[i*2],lo=(unsigned char)s[i*2+1];
        hi=hi>='0'&&hi<='9'?hi-'0':hi>='a'&&hi<='f'?hi-'a'+10:255;
        lo=lo>='0'&&lo<='9'?lo-'0':lo>='a'&&lo<='f'?lo-'a'+10:255;
        if(hi>15||lo>15)return false;
        out[i]=(hi<<4)|lo;
    }
    return true;
}
static esp_http_client_handle_t request(const char *url)
{
    esp_http_client_config_t config={.url=url,.timeout_ms=10000,.buffer_size=1024,.disable_auto_redirect=true};
    esp_http_client_handle_t client=esp_http_client_init(&config);
    if(!client)return NULL;
    char authorization[256];
    snprintf(authorization,sizeof(authorization),"Bearer %s",DEVICE_TOKEN);
    if(esp_http_client_set_header(client,"Authorization",authorization)!=ESP_OK ||
       esp_http_client_open(client,0)!=ESP_OK){esp_http_client_cleanup(client);return NULL;}
    esp_http_client_fetch_headers(client);
    return client;
}
static bool manifest_valid(const cJSON *j, char version[32], char sha[65], unsigned *size)
{
    const cJSON *p=cJSON_GetObjectItemCaseSensitive(j,"project"),*v=cJSON_GetObjectItemCaseSensitive(j,"version"),
        *n=cJSON_GetObjectItemCaseSensitive(j,"size"),*s=cJSON_GetObjectItemCaseSensitive(j,"sha256"),*sig=cJSON_GetObjectItemCaseSensitive(j,"signature");
    if(!cJSON_IsString(p)||strcmp(p->valuestring,"passport-gpt-live")||!cJSON_IsString(v)||
       strlen(v->valuestring)>=32||!cJSON_IsNumber(n)||n->valuedouble<1024||n->valuedouble>0x380000||
       n->valuedouble!=n->valueint||!cJSON_IsString(s)||!cJSON_IsString(sig))return false;
    unsigned char expected[32],signature[80],digest[32];
    size_t sig_size=strlen(sig->valuestring)/2;
    if(sig_size<8||sig_size>sizeof(signature)||!unhex(s->valuestring,expected,32)||!unhex(sig->valuestring,signature,sig_size))return false;
    char canonical[160];int length=snprintf(canonical,sizeof(canonical),"passport-gpt-live\n%s\n%d\n%s\n",v->valuestring,n->valueint,s->valuestring);
    if(length<=0||length>=sizeof(canonical))return false;
    mbedtls_pk_context key;mbedtls_pk_init(&key);
    int e=mbedtls_sha256((unsigned char*)canonical,length,digest,0);
    if(!e)e=mbedtls_pk_parse_public_key(&key,(unsigned char*)UPDATE_PUBLIC_KEY,strlen(UPDATE_PUBLIC_KEY)+1);
    if(!e)e=mbedtls_pk_verify(&key,MBEDTLS_MD_SHA256,digest,32,signature,sig_size);
    mbedtls_pk_free(&key);
    if(e)return false;
    strcpy(version,v->valuestring);strcpy(sha,s->valuestring);*size=(unsigned)n->valueint;
    return true;
}
static void update_task(void *unused)
{
    const char *failure="Could not reach server\nOK: back / retry";
    char host[128],base[160],url[256],version[32],sha[65],buffer[1024];unsigned size=0;
    esp_http_client_handle_t client=NULL;esp_ota_handle_t ota=0;bool writing=false;
    mbedtls_sha256_context hash;mbedtls_sha256_init(&hash);
    if(sscanf(VOICE_URL,"ws://%127[^/]",host)!=1)goto done;
    snprintf(base,sizeof(base),"http://%s",host);
    snprintf(url,sizeof(url),"%s/api/firmware/latest",base);
    client=request(url);if(!client)goto done;
    int status=esp_http_client_get_status_code(client);
    if(status==404){failure="No update published\nOK: back";goto done;}
    if(status!=200)goto done;
    unsigned received=0;
    while(received<sizeof(buffer)-1){int n=esp_http_client_read(client,buffer+received,sizeof(buffer)-1-received);if(n<0)goto done;if(!n)break;received+=n;}
    buffer[received]=0;
    if(!esp_http_client_is_complete_data_received(client)){failure="Invalid update metadata\nOK: back";goto done;}
    esp_http_client_cleanup(client);client=NULL;
    cJSON *manifest=cJSON_Parse(buffer);bool valid=manifest_valid(manifest,version,sha,&size);cJSON_Delete(manifest);
    if(!valid){failure="Update signature invalid\nOK: back";goto done;}
    if(!version_newer(version,esp_app_get_description()->version)){
        ui_set("UP TO DATE", "Latest version installed\nOK: back");failure=NULL;goto done;
    }
    const esp_partition_t *slot=esp_ota_get_next_update_partition(NULL);
    if(!slot||size>slot->size){failure="Firmware does not fit\nOK: back";goto done;}
    snprintf(url,sizeof(url),"%s/api/firmware/%s",base,sha);
    client=request(url);if(!client||esp_http_client_get_status_code(client)!=200)goto done;
    int64_t length=esp_http_client_get_content_length(client);
    if(length>=0&&length!=size){failure="Firmware size mismatch\nOK: back";goto done;}
    if(esp_ota_begin(slot,OTA_WITH_SEQUENTIAL_WRITES,&ota)!=ESP_OK){failure="Could not prepare update\nOK: back";goto done;}
    writing=true;
    if(mbedtls_sha256_starts(&hash,0))goto done;
    received=0;int previous=-1;
    int64_t download_started=esp_timer_get_time();
    while(received<size){
        if(esp_timer_get_time()-download_started>180000000){failure="Download timed out\nOK: retry";goto done;}
        int n=esp_http_client_read(client,buffer,size-received>sizeof(buffer)?sizeof(buffer):size-received);
        if(n<=0){failure="Download interrupted\nOK: retry";goto done;}
        if(mbedtls_sha256_update(&hash,(unsigned char*)buffer,n)||esp_ota_write(ota,buffer,n)!=ESP_OK){failure="Could not write firmware\nOK: retry";goto done;}
        received+=n;int percent=received*100/size;
        if(percent/5!=previous/5){char detail[80];snprintf(detail,sizeof(detail),"v%s / %d%%\nKeep power connected",version,percent);ui_set("DOWNLOADING UPDATE",detail);previous=percent;}
    }
    int extra=esp_http_client_read(client,buffer,1);
    unsigned char digest[32],expected[32];
    if(extra!=0||!esp_http_client_is_complete_data_received(client)||mbedtls_sha256_finish(&hash,digest)||!unhex(sha,expected,32)||memcmp(digest,expected,32)){
        failure="Firmware checksum failed\nOK: retry";goto done;
    }
    esp_http_client_cleanup(client);client=NULL;
    esp_err_t e=esp_ota_end(ota);writing=false;
    if(e!=ESP_OK||esp_ota_set_boot_partition(slot)!=ESP_OK){failure="Firmware validation failed\nOK: retry";goto done;}
    ui_set("FIRMWARE SAVED","Restarting to verify");
    mbedtls_sha256_free(&hash);
    vTaskDelay(pdMS_TO_TICKS(1200));esp_restart();
    return;
 done:
    if(client)esp_http_client_cleanup(client);
    if(writing)esp_ota_abort(ota);
    mbedtls_sha256_free(&hash);
    if(failure)ui_set("UPDATE NOT INSTALLED",failure);
    atomic_store(&active,false);portal_update_unlock();vTaskDelete(NULL);
}
esp_err_t firmware_update_start(void)
{
    if(!voice_wifi_connected())return ESP_ERR_INVALID_STATE;
    if(!portal_update_lock())return ESP_ERR_INVALID_STATE;
    atomic_store(&active,true);
    ui_set("CHECKING UPDATE","Please wait\nKeep power connected");
    if(xTaskCreate(update_task,"firmware_update",8192,NULL,4,NULL)!=pdPASS){atomic_store(&active,false);portal_update_unlock();return ESP_ERR_NO_MEM;}
    return ESP_OK;
}
