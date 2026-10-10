const assert=require('node:assert/strict'),vm=require('node:vm'),fs=require('node:fs');
const timers=new Map();let next=0;
const ctx=vm.createContext({crypto:{randomUUID:()=> 'test'},setTimeout:fn=>{timers.set(++next,fn);return next},clearTimeout:id=>timers.delete(id)});
vm.runInContext(fs.readFileSync('server/web/voice_setup.js','utf8')+';globalThis.Setup=VoiceSetup;',ctx);
function setup(voice='juniper'){
 const sent=[],ready=[],errors=[];
 const gate=new ctx.Setup({send:raw=>sent.push(JSON.parse(raw))},voice,v=>ready.push(v),e=>errors.push(e));
 return {gate,sent,ready,errors};
}
let x=setup();assert.equal(x.sent[0].session.audio.output.voice,'juniper');
x.gate.handle({type:'session.started'});assert.equal(x.ready.length,0);
x.gate.handle({type:'session.updated',client_event_id:'unrelated'});assert.equal(x.ready.length,0);
x.gate.handle({type:'session.updated',session:{id:'upstream'}});assert.deepEqual(x.ready,['juniper']);assert.equal(timers.size,0);
x.gate.handle({type:'session.updated'});assert.equal(x.ready.length,1);
x=setup('ember');x.gate.handle({type:'session.updated',session:{audio:{output:{voice:'cove'}}}});assert.equal(x.ready.length,0);assert.equal(x.errors.length,1);
x=setup();x.gate.handle({type:'error'});assert.equal(x.ready.length,0);assert.equal(x.errors.length,1);
x=setup();[...timers.values()][0]();assert.equal(x.ready.length,0);assert.equal(x.errors.length,1);
x=setup();x.gate.close();assert.equal(timers.size,0);x.gate.handle({type:'session.updated'});assert.equal(x.ready.length,0);
console.log('PASS: startup voice message, wait for ack, unrelated ack, mismatch, rejection, timeout, duplicate and closed-session acknowledgements');
