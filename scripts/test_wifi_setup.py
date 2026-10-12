#!/usr/bin/env python3
"""Exercise the actual setup connection worker without a radio or NVS."""
from pathlib import Path
import tempfile, subprocess, os
root=Path(__file__).resolve().parents[1]
s=(root/'firmware/main/wifi_setup.c').read_text()
worker=s[s.index('static void connect_task('):s.index('esp_err_t wifi_setup_connect(')]
harness=r'''
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <setjmp.h>
#include "wifi_profiles.h"
#define WIFI_REASON_AUTH_FAIL 202
#define WIFI_REASON_HANDSHAKE_TIMEOUT 204
#define WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT 15
#define WIFI_REASON_NO_AP_FOUND 201
static int state_lock, test_result, save_result, save_count, disconnects, released;
static unsigned test_reason;
static char connected_ssid[33], connected_ip[20];
static const char *phase, *failure;
static jmp_buf done;
static void _lock_acquire(int *p){(void)p;}
static void _lock_release(int *p){(void)p;}
static esp_err_t voice_wifi_test(const char*s,const char*p,unsigned*r){assert(!strcmp(s,"new-network"));assert(!strcmp(p,"12345678"));*r=test_reason;return test_result;}
static esp_err_t wifi_profiles_save_fake(const char*s,const char*p){assert(test_result==ESP_OK);assert(!strcmp(s,"new-network"));assert(!strcmp(p,"12345678"));save_count++;return save_result;}
#define wifi_profiles_save wifi_profiles_save_fake
static void esp_wifi_disconnect(void){disconnects++;}
static void voice_wifi_ip(char*p,size_t n){snprintf(p,n,"192.168.1.15");}
static void ui_show_guide(void){}
static void ui_set(const char*a,const char*b){(void)a;(void)b;}
static void voice_wifi_radio_give(void){released++;}
static void vTaskDelete(void*p){(void)p;longjmp(done,1);}
static void checked_free(void *p){for(size_t i=0;i<sizeof(wifi_profile_t);i++)assert(!((unsigned char*)p)[i]);free(p);}
#define free checked_free
'''
harness+=worker+r'''
static void run(int result,unsigned reason,int save){
 test_result=result;test_reason=reason;save_result=save;
 save_count=disconnects=released=0;phase="connecting";failure="";
 connected_ssid[0]=connected_ip[0]=0;
 wifi_profile_t*p=calloc(1,sizeof(*p));strcpy(p->ssid,"new-network");strcpy(p->password,"12345678");
 if(!setjmp(done))connect_task(p);
 assert(released==1);
}
int main(void){
 run(ESP_OK,0,ESP_OK);assert(save_count==1&&!disconnects&&!strcmp(phase,"connected")&&!strcmp(connected_ip,"192.168.1.15"));
 run(-1,WIFI_REASON_AUTH_FAIL,ESP_OK);assert(!save_count&&disconnects==1&&!strcmp(phase,"failed")&&strstr(failure,"Authentication"));
 run(-1,WIFI_REASON_NO_AP_FOUND,ESP_OK);assert(!save_count&&strstr(failure,"not found"));
 run(-1,0,ESP_OK);assert(!save_count&&strstr(failure,"IP address"));
 run(ESP_OK,0,ESP_ERR_NO_MEM);assert(save_count==1&&disconnects==1&&!strcmp(phase,"failed")&&strstr(failure,"Eight"));
 run(ESP_OK,0,-1);assert(save_count==1&&!strcmp(phase,"failed")&&strstr(failure,"saving failed"));
 puts("PASS: setup saves only after IP success; auth/missing AP/DHCP/NVS failures stay retryable; credentials erased and radio released");
}
'''
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'test.c').write_text(harness)
 subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I'+str(root/'scripts/fixtures/wifi'),'-I'+str(root/'firmware/main'),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
