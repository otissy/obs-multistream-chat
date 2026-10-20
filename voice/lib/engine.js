'use strict';
// Turns one chat line into audio with sherpa-onnx. Loaded models are kept (at most maxModels, least
// recently used goes first) because loading Kokoro takes seconds and a line should start in well
// under one. `sherpa` is the sherpa-onnx-node module, or a stand-in with the same shape in tests.
const { engineSpec, packInstalled } = require('./catalogue');

const MIN_SPEED = 0.5;
const MAX_SPEED = 2.0;

class Engine {
  constructor({ sherpa, catalogue, packsDir, maxModels = 2, numThreads = 2 }) {
    this.sherpa = sherpa;
    this.cat = catalogue;
    this.packsDir = packsDir;
    this.maxModels = maxModels;
    this.numThreads = numThreads;
    this.models = new Map(); // key -> Promise<OfflineTts>, in use order (oldest first)
  }

  load(spec) {
    let p = this.models.get(spec.key);
    if (p) {
      this.models.delete(spec.key);
      this.models.set(spec.key, p);
      return p;
    }
    p = this.sherpa.OfflineTts.createAsync(spec.config);
    p.catch(() => this.models.delete(spec.key)); // a failed load is retried on the next line
    this.models.set(spec.key, p);
    while (this.models.size > this.maxModels) this.models.delete(this.models.keys().next().value);
    return p;
  }

  // onAudio(Float32Array, sampleRate) gets each chunk as soon as it exists (one per sentence);
  // cancelled() is polled between chunks. Resolves to the number of samples produced.
  async speak({ voice, text, speed }, { onAudio, cancelled }) {
    const spec = engineSpec(this.cat, voice, this.packsDir, this.numThreads);
    const pack = this.cat.packs.get(this.cat.voices.get(voice).pack);
    if (!packInstalled(pack, this.packsDir)) throw new Error('voice pack not installed: ' + pack.id);
    const tts = await this.load(spec);
    if (cancelled()) return 0;
    const s = Number.isFinite(speed) ? Math.min(MAX_SPEED, Math.max(MIN_SPEED, speed)) : 1.0;
    let sent = 0;
    const audio = await tts.generateAsync({
      text,
      enableExternalBuffer: false,
      generationConfig: new this.sherpa.GenerationConfig({ ...spec.gen, speed: s, silenceScale: 0.2 }),
      onProgress: ({ samples }) => {
        if (cancelled()) return 0;
        if (samples && samples.length) {
          sent += samples.length;
          onAudio(samples, tts.sampleRate);
        }
        return 1;
      },
    });
    // engines that do not stream give everything at the end
    if (!cancelled() && sent === 0 && audio && audio.samples && audio.samples.length) {
      sent = audio.samples.length;
      onAudio(audio.samples, audio.sampleRate || tts.sampleRate);
    }
    return sent;
  }
}

module.exports = { Engine, MIN_SPEED, MAX_SPEED };
