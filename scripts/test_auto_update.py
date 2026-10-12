#!/usr/bin/env python3
"""Offline signed release and private server API regression. No device traffic."""
import hashlib,json,os,socket,subprocess,tempfile,time,urllib.request,urllib.error
from pathlib import Path
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as td:
 p=Path(td);key=p/'key.pem';pub=p/'pub.pem';out=p/'release'
 subprocess.run(['openssl','genpkey','-algorithm','EC','-pkeyopt','ec_paramgen_curve:P-256','-out',str(key)],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 subprocess.run(['openssl','pkey','-in',str(key),'-pubout','-out',str(pub)],check=True,stdout=subprocess.DEVNULL)
 image=bytearray(1024);image[0]=0xe9;image[12:14]=(5).to_bytes(2,'little');image[32:36]=(0xabcd5432).to_bytes(4,'little');image[48:54]=b'0.10.7';image[80:97]=b'passport-gpt-live';(p/'image.bin').write_bytes(image)
 subprocess.run(['python3',str(root/'scripts/publish_firmware.py'),'--image',str(p/'image.bin'),'--key',str(key),'--out',str(out)],check=True,stdout=subprocess.DEVNULL)
 manifest=json.loads((out/'latest.json').read_text());assert manifest['sha256']==hashlib.sha256(image).hexdigest()
 data=p/'data';sig=p/'sig';data.write_text(f"passport-gpt-live\n{manifest['version']}\n{manifest['size']}\n{manifest['sha256']}\n");sig.write_bytes(bytes.fromhex(manifest['signature']))
 args=['openssl','dgst','-sha256','-verify',str(pub),'-signature',str(sig),str(data)]
 subprocess.run(args,check=True,stdout=subprocess.DEVNULL)
 data.write_text(data.read_text().replace('0.10.7','0.99.9'))
 assert subprocess.run(args,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode!=0,'metadata tampering must invalidate signature'
 sock=socket.socket();sock.bind(('127.0.0.1',0));port=sock.getsockname()[1];sock.close()
 env=dict(os.environ,LISTEN=f'127.0.0.1:{port}',DEVICE_TOKEN='offline-private-token',FIRMWARE_DIR=str(out),RUST_LOG='error')
 process=subprocess.Popen([str(root/'target/debug/passport-voice')],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 base=f'http://127.0.0.1:{port}'
 def get(path,token=None):
  req=urllib.request.Request(base+path,headers={'Authorization':'Bearer '+token} if token else {})
  try:
   with urllib.request.urlopen(req,timeout=2) as r:return r.status,r.read()
  except urllib.error.HTTPError as e:return e.code,e.read()
 try:
  for _ in range(100):
   try:get('/health');break
   except OSError:time.sleep(.03)
  assert get('/api/firmware/latest')[0]==401
  assert get('/api/firmware/'+manifest['sha256'])[0]==401
  assert get('/api/firmware/latest','wrong')[0]==401
  code,body=get('/api/firmware/latest','offline-private-token');assert code==200 and json.loads(body)==manifest
  assert get('/api/firmware/'+manifest['sha256'],'offline-private-token')==(200,bytes(image))
  assert get('/api/firmware/not-a-hash','offline-private-token')[0]==404
  (out/'latest.json').unlink();assert get('/api/firmware/latest','offline-private-token')[0]==404
 finally:process.terminate();process.wait(timeout=8)
print('PASS: signed release identity, tamper rejection, private manifest/image, exact image delivery and unpublished state')
# Run the actual device parsing helpers with a host compiler.
source=(root/'firmware/main/firmware_update.c').read_text()
helpers=source[source.index('static bool version_newer('):source.index('static esp_http_client_handle_t request(')]
code='#include <stdbool.h>\n#include <stdio.h>\n#include <string.h>\n#include <assert.h>\n'+helpers+r'''
int main(void){
 assert(version_newer("0.10.7","0.10.6"));assert(version_newer("1.0.0","0.99.99"));
 assert(!version_newer("0.10.7","0.10.7"));assert(!version_newer("0.9.9","0.10.7"));
 assert(!version_newer("-1.0.0","0.10.7"));assert(!version_newer("0.10.7-extra","0.10.6"));
 unsigned char out[2];assert(unhex("01ff",out,2)&&out[0]==1&&out[1]==255);
 assert(!unhex("fffff",out,2));assert(!unhex("zzff",out,2));
 puts("PASS: device version ordering rejects downgrade/malformed versions; strict digest/signature hex parsing");
}
'''
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'test.c').write_text(code)
 subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
