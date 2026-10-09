/* The AudioContext runs at 24 kHz. Chromium resamples its WebRTC tracks. */
class PcmInput extends AudioWorkletProcessor {
  constructor(){
    super();this.ring=new Float32Array(12000);this.read=0;this.write=0;this.count=0;this.dropped=0;this.clock=0;
    this.port.onmessage=({data})=>{
      if(data==='clear'){this.count=0;this.read=this.write=0;return;}
      const samples=new Int16Array(data);
      if(this.count+samples.length>this.ring.length){this.dropped+=this.count;this.count=0;this.read=this.write=0;}
      for(const sample of samples){this.ring[this.write]=sample/32768;this.write=(this.write+1)%this.ring.length;this.count++;}
    };
  }
  process(inputs,outputs){
    const out=outputs[0][0];
    for(let i=0;i<out.length;i++){
      if(this.count){out[i]=this.ring[this.read];this.read=(this.read+1)%this.ring.length;this.count--;}
      else out[i]=0;
    }
    this.clock+=out.length;
    if(this.clock>=48000){this.clock=0;this.port.postMessage({input_queue_ms:Math.round(this.count/24),input_dropped_samples:this.dropped});}
    return true;
  }
}
class PcmOutput extends AudioWorkletProcessor {
  constructor(){super();this.frame=new Int16Array(480);this.used=0;}
  process(inputs){
    const samples=inputs[0]?.[0];if(!samples)return true;
    for(const sample of samples){
      this.frame[this.used++]=Math.round(Math.max(-1,Math.min(1,sample))*32767);
      if(this.used===480){this.port.postMessage(this.frame.buffer,[this.frame.buffer]);this.frame=new Int16Array(480);this.used=0;}
    }
    return true;
  }
}
registerProcessor('pcm-input',PcmInput);
registerProcessor('pcm-output',PcmOutput);
