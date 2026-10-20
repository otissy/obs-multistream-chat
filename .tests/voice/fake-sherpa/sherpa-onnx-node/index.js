'use strict';
// Stand-in for sherpa-onnx-node in the voice tests: same shape, no models. Each sentence becomes a
// 50 ms chunk of a sine whose pitch encodes the speaker id, so a test can tell voices apart.
// OMS_FAKE_SHERPA_LOG (a file) records every model load and line, to check caching and order.
const fs = require('fs');

function note(o) {
  if (process.env.OMS_FAKE_SHERPA_LOG) fs.appendFileSync(process.env.OMS_FAKE_SHERPA_LOG, JSON.stringify(o) + '\n');
}

class GenerationConfig {
  constructor(o = {}) { Object.assign(this, o); }
}

class OfflineTts {
  constructor(config) {
    this.config = config;
    this.sampleRate = config.model.kokoro ? 24000 : 22050;
    this.numSpeakers = 64;
  }
  static async createAsync(config) {
    const m = config.model.kokoro || config.model.vits;
    if (!fs.existsSync(m.model)) throw new Error('model not found: ' + m.model);
    note({ load: m.model, lexicon: m.lexicon || '', lang: m.lang || '' });
    await new Promise(r => setTimeout(r, 5));
    return new OfflineTts(config);
  }
  async generateAsync({ text, generationConfig: g, onProgress }) {
    note({ text, sid: g.sid, speed: g.speed, lang: g.extra && g.extra.lang });
    const sentences = text.split(/(?<=[.!?])\s+/).filter(Boolean);
    const n = Math.round(this.sampleRate * 0.05);
    let total = 0;
    for (const _ of sentences) {
      await new Promise(r => setTimeout(r, Number(process.env.OMS_FAKE_SHERPA_DELAY_MS || 2)));
      const samples = new Float32Array(n);
      for (let i = 0; i < n; i++) samples[i] = 0.5 * Math.sin((2 * Math.PI * (200 + 10 * g.sid) * i) / this.sampleRate);
      total += n;
      if (onProgress && onProgress({ samples, progress: total }) === 0) break;
    }
    return { samples: new Float32Array(total), sampleRate: this.sampleRate };
  }
}

module.exports = { OfflineTts, GenerationConfig };
