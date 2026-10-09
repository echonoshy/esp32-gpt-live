// Offline audio regression checks: no microphone, browser or subscription session.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const processors = {};
const context = vm.createContext({
  AudioWorkletProcessor: class { constructor() { this.port = { postMessage: value => this.messages.push(value) }; this.messages = []; } },
  registerProcessor: (name, cls) => processors[name] = cls,
  Int16Array, Float32Array, Math,
});
vm.runInContext(fs.readFileSync('server/web/worklet.js', 'utf8'), context);
const input = new processors['pcm-input']();
const block = new Float32Array(128);
input.port.onmessage({data: new Int16Array(480).fill(8192).buffer});
input.process([], [[block]]); assert(block.every(v => v === .25));
input.port.onmessage({data: 'clear'}); input.process([], [[block]]);
assert(block.every(v => v === 0), 'interrupted input must not reach the next turn');
for (let i=0; i<40; i++) input.port.onmessage({data:new Int16Array(480).fill(i).buffer});
assert(input.count <= 12000, 'backpressure must remain bounded');
input.port.onmessage({data:'clear'});
input.process([], [[block]]); assert(block.every(v=>v===0));
const output = new processors['pcm-output']();
for (let i=0; i<15; i++) output.process([[new Float32Array(128).fill(.5)]]);
assert.equal(output.messages.length,4);
for (const frame of output.messages) {assert.equal(frame.byteLength,960);assert(new Int16Array(frame).every(v=>v===16384));}
for (const path of ['server/web/worker.html','firmware/main/settings.html']) {
  const html=fs.readFileSync(path,'utf8');const script=html.match(/<script>([\s\S]*?)<\/script>/)[1];
  new vm.Script(script,{filename:path});
}
console.log('PASS: interrupted audio clear, bounded input, PCM framing, worker/settings JavaScript syntax');
