// The subscription V3 channel acknowledges startup voice configuration separately
// from WebRTC connectivity. Do not admit microphone/playback before that ack.
class VoiceSetup {
  constructor(channel, voice, ready, fail) {
    this.voice = voice;
    this.ready = ready;
    this.fail = fail;
    this.pending = true;
    this.eventId = `mic-on-voice-${crypto.randomUUID()}`;
    this.timer = setTimeout(() => this.reject('voice configuration timeout'), 10000);
    try {
      channel.send(JSON.stringify({type:'session.update', event_id:this.eventId,
        session:{audio:{output:{voice}}}}));
    } catch (_) { this.reject('voice configuration could not be sent'); }
  }
  reject(reason) {
    if (!this.pending) return;
    this.pending = false;
    clearTimeout(this.timer);
    this.fail(reason);
  }
  handle(event) {
    if (!this.pending) return false;
    if (event.type === 'error') {
      this.reject('voice configuration rejected');
      return true;
    }
    if (event.type !== 'session.updated') return false;
    if (event.client_event_id && event.client_event_id !== this.eventId) return false;
    const actual = event.session?.audio?.output?.voice;
    if (actual && actual !== this.voice) {
      this.reject('voice configuration mismatch');
      return true;
    }
    this.pending = false;
    clearTimeout(this.timer);
    this.ready(this.voice);
    return true;
  }
  close() { this.pending = false; clearTimeout(this.timer); }
}
