#!/usr/bin/env python3
"""Compile real control-boundary/retry functions with deterministic queue and socket stubs."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'firmware/main/main.c').read_text()
def function(name):
 start=source.index('static ',source.index(name)-20)
 brace=source.index('{',start);depth=1;end=brace+1
 while depth:
  depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[start:end]
preamble=r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include "audio_transport.h"
typedef struct { uint32_t generation; uint8_t packet[ADPCM_BYTES]; } capture_frame_t;
static capture_frame_t queue[64];
static unsigned queued, sent_frames, text_sends, last_sample, attempts_seen;
static bool fail_audio;
static int capture_queue,send_lock,ws;
static uplink_batch_t uplink;
static atomic_uint capture_generation, epoch, retry_at;
static atomic_int active_voice, attempts;
static atomic_bool connected, recovering,output_timing_pending,interaction_active,ptt_active,ready,starting,wanted,sticky_error;
#define pdTRUE 1
#define portMAX_DELAY 0
#define pdMS_TO_TICKS(x) (x)
static int xQueueReceive(int q, void *dst, int wait) {
 (void)q;(void)wait;if(!queued)return 0;memcpy(dst,queue,sizeof(queue[0]));
 memmove(queue,queue+1,--queued*sizeof(queue[0]));return 1;
}
static void xQueueReset(int q) {(void)q;queued=0;}
static void xSemaphoreTake(int lock,int wait) {(void)lock;(void)wait;}
static void xSemaphoreGive(int lock) {(void)lock;}
static bool send_wire(const uint8_t *p,unsigned frames) {
 if(fail_audio)return false;
 for(unsigned i=0;i<frames;i++){assert(p[i*ADPCM_BYTES]==++last_sample);sent_frames++;}
 return true;
}
static int esp_websocket_client_send_text(int w,const char *s,size_t n,int wait) {
 (void)w;(void)wait;
 if(strstr(s,"ptt_end"))assert(sent_frames==7);
 text_sends++;return n;
}
static uint32_t now_ms(void) {return 1000;}
static void mute(void) {atomic_fetch_add(&epoch,1);}
static void ui_set(const char *a,const char *b) {(void)a;(void)b;attempts_seen++;}
static void append(unsigned generation,unsigned sample) {
 queue[queued]=(capture_frame_t){.generation=generation};queue[queued++].packet[0]=sample;
}
'''
tests=r'''
int main(void) {
 connected=true;capture_generation=3;
 uint8_t packet[ADPCM_BYTES]={1};assert(uplink_append(&uplink,packet,send_wire));
 append(2,99);for(unsigned i=2;i<=7;i++)append(3,i);
 assert(send_json("{\"type\":\"ptt_end\"}"));
 assert(sent_frames==7 && !queued && !uplink.count && text_sends==1);
 append(3,8);uplink.count=1;
 assert(send_json("{\"type\":\"cancel\"}"));
 assert(sent_frames==7 && !queued && !uplink.count && capture_generation==4);
 append(4,8);assert(send_json("{\"type\":\"ptt_start\"}"));
 assert(!queued && capture_generation==5);
 for(unsigned i=8;i<=11;i++)append(5,i);
 fail_audio=true;unsigned before=text_sends;
 assert(!send_json("{\"type\":\"ptt_end\"}"));assert(text_sends==before);
 wanted=true;ready=true;retry();assert(attempts==1 && recovering && !ready);
 retry();retry();assert(attempts==1 && attempts_seen==1);
 recovering=false;starting=true;retry();assert(attempts==2 && attempts_seen==2);
 puts("PASS: capture generation fence; release flush ordering; cancel/start purge; no end after failed audio; one retry per failure");
}
'''
with tempfile.TemporaryDirectory() as td:
 p=Path(td)/'check.c';p.write_text(preamble+'\n'+function('flush_capture(void)')+'\n'+function('send_json(const char')+'\n'+function('retry(void)')+'\n'+tests)
 subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror','-I'+str(root/'firmware/main'),str(p),'-o',str(Path(td)/'check')],check=True)
 subprocess.run([str(Path(td)/'check')],check=True)
