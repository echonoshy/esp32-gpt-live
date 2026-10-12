/* Shared with the offline regression: identity, reboot and boot confirmation are distinct. */
(function(root){
'use strict';
function descriptor(buffer){
 const b=new Uint8Array(buffer),v=new DataView(buffer);
 if(b.length<288||b[0]!==0xe9||v.getUint32(32,true)!==0xabcd5432)throw Error('Select a mic-on application .bin file.');
 const text=(start,size)=>new TextDecoder().decode(b.slice(start,start+size)).split('\0')[0];
 if(text(80,32)!=='passport-gpt-live')throw Error('This firmware belongs to a different project.');
 return {version:text(48,32),image_id:Array.from(b.slice(176,208),n=>n.toString(16).padStart(2,'0')).join('')};
}
function verify(pending,info){
 if(!info.device_id||info.device_id!==pending.device_id)return 'wrong_device';
 if(!info.boot_id||info.boot_id===pending.boot_id)return 'waiting';
 if(info.version!==pending.version||info.image_id!==pending.image_id)return 'different';
 return info.ota_state==='valid'?'confirmed':'waiting';
}
const api={descriptor,verify};
if(typeof module!=='undefined')module.exports=api;else root.MicOnOTA=api;
})(globalThis);
