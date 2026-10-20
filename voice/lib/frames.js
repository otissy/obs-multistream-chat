'use strict';
// Frames the voice helper writes on stdout (the plugin reads them in src/obs/voice_process.cpp).
// Little endian, 20-byte header, then the payload:
//   'OMSV' | type u8 | 3 zero bytes | request id u32 | sample rate u32 | payload length u32
//   AUDIO  16-bit signed mono PCM, one chunk of a line (a line comes in one or more chunks)
//   DONE   empty: the line is finished (also sent after an ERROR, and for a line cut by flush)
//   ERROR  UTF-8 message for the plugin log; never contains the chat text
//   READY  UTF-8 JSON {"voices": <installed voice count>}, once, at start
const MAGIC = Buffer.from('OMSV', 'ascii');
const TYPE = Object.freeze({ AUDIO: 1, DONE: 2, ERROR: 3, READY: 4 });
const HEADER = 20;
const MAX_PAYLOAD = 16 * 1024 * 1024; // ~6 minutes of 22 kHz audio; a chat line is far shorter

function encode(type, id, sampleRate, payload) {
  const body = payload || Buffer.alloc(0);
  if (body.length > MAX_PAYLOAD) throw new RangeError('frame payload too large');
  const h = Buffer.alloc(HEADER);
  MAGIC.copy(h, 0);
  h.writeUInt8(type, 4);
  h.writeUInt32LE(id >>> 0, 8);
  h.writeUInt32LE(sampleRate >>> 0, 12);
  h.writeUInt32LE(body.length, 16);
  return Buffer.concat([h, body]);
}

// Float samples in [-1, 1] to 16-bit PCM; anything outside is clipped, NaN becomes silence.
function pcm16(samples) {
  const out = Buffer.alloc(samples.length * 2);
  for (let i = 0; i < samples.length; i++) {
    let v = samples[i];
    if (!(v === v)) v = 0;
    v = Math.max(-1, Math.min(1, v));
    out.writeInt16LE(Math.round(v * 32767), i * 2);
  }
  return out;
}

// Incremental reader, the same rules as the C++ side: a bad magic or an oversized payload is fatal.
class Decoder {
  constructor() { this.buf = Buffer.alloc(0); }
  push(chunk) {
    this.buf = this.buf.length ? Buffer.concat([this.buf, chunk]) : chunk;
    const frames = [];
    while (this.buf.length >= HEADER) {
      if (!this.buf.subarray(0, 4).equals(MAGIC)) throw new Error('bad frame magic');
      const len = this.buf.readUInt32LE(16);
      if (len > MAX_PAYLOAD) throw new Error('frame payload too large');
      if (this.buf.length < HEADER + len) break;
      frames.push({
        type: this.buf.readUInt8(4),
        id: this.buf.readUInt32LE(8),
        sampleRate: this.buf.readUInt32LE(12),
        payload: Buffer.from(this.buf.subarray(HEADER, HEADER + len)),
      });
      this.buf = this.buf.subarray(HEADER + len);
    }
    return frames;
  }
}

module.exports = { MAGIC, TYPE, HEADER, MAX_PAYLOAD, encode, pcm16, Decoder };
