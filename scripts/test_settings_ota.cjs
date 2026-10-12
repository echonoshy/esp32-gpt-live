const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const MicOnOTA=require('../firmware/main/ota.js');
const image=new Uint8Array(288),v=new DataView(image.buffer);image[0]=0xe9;v.setUint32(32,0xabcd5432,true);image.set(Buffer.from('0.10.6'),48);image.set(Buffer.from('passport-gpt-live'),80);image.fill(0x42,176,208);
const nodes=new Map(),get=id=>{if(!nodes.has(id))nodes.set(id,{value:0,textContent:'',disabled:false,replaceChildren(){},append(){}});return nodes.get(id)};
get('firmware').files=[{slice:()=>({arrayBuffer:async()=>image.buffer})}];
let device={nonce:'boot-a',boot_id:'boot-a',device_id:'unit-a',version:'0.10.5',image_id:'old',ota_state:'valid',update_seconds:200,state:'NAVIGATION',battery:90,wifi_networks:[]};
let xhr,store=null,requests=0;
class XHR{constructor(){xhr=this;this.upload={}}open(){}setRequestHeader(){}send(){}}
const ctx={MicOnOTA,Date,JSON,console,document:{getElementById:get,querySelectorAll:()=>[],createElement:()=>({append(){}})},AbortSignal:{timeout:()=>null},XMLHttpRequest:XHR,confirm:()=>true,setInterval:()=>{},setTimeout:()=>{},sessionStorage:{getItem:()=>store,setItem:(k,v)=>store=v,removeItem:()=>store=null},fetch:async()=>{requests++;return{ok:true,json:async()=>structuredClone(device)}}};
const script=fs.readFileSync('firmware/main/settings.html','utf8').match(/<script>([\s\S]*?)<\/script>/)[1];vm.createContext(ctx);vm.runInContext(script,ctx);
const flush=()=>new Promise(resolve=>setImmediate(resolve));
(async()=>{
 await flush();await get('upgrade').onclick();assert(get('upgrade').disabled);assert(store);
 xhr.upload.onprogress({lengthComputable:true,loaded:100,total:100});assert(!get('notice').textContent.includes('CONFIRMED'));
 xhr.status=200;xhr.onload();await flush();assert(store,'same boot is still pending');assert(!get('notice').textContent.includes('CONFIRMED'));
 device={...device,...MicOnOTA.descriptor(image.buffer),boot_id:'boot-b',nonce:'boot-b',ota_state:'pending_verify'};
 await vm.runInContext('verifyUpdate()',ctx);assert(store,'boot is not yet validated');
 device.ota_state='valid';await vm.runInContext('verifyUpdate()',ctx);await flush();assert.equal(store,null);assert(get('notice').textContent.includes('UPDATE CONFIRMED'));assert(!get('upgrade').disabled);
 await get('upgrade').onclick();xhr.status=403;xhr.responseText='Upload window expired';xhr.onload();await flush();assert.equal(store,null);assert.equal(get('notice').textContent,'Upload window expired');assert(!get('upgrade').disabled);
 await get('upgrade').onclick();xhr.onerror();await flush();assert(store,'lost upload response is not treated as success or erased');
 device.boot_id='boot-c';device.nonce='boot-c';device.image_id='rollback-image';await vm.runInContext('verifyUpdate()',ctx);assert(get('notice').textContent.includes('different firmware'));assert.equal(store,null);
 console.log('PASS: settings OTA upload progress, waiting/reboot/valid sequence, expired window, lost response and rollback UI');
})().catch(e=>{console.error(e);process.exitCode=1});
