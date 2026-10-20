'use strict';
// L4 with the real thing: every catalogue voice whose pack is unpacked in OMS_VOICE_PACKS is run
// through voice/index.js with the real sherpa-onnx-node (OMS_SHERPA_NODE_MODULES: a node_modules
// with sherpa-onnx-node and its platform package). Skipped when either is not set.
// Each voice must give real audio: at least 0.6 s, not silence, no ERROR frame.
// OMS_VOICE_SAMPLES=<dir> also writes one WAV per voice there.
const test = require('node:test');
const assert = require('node:assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { CATALOGUE, runHelper, killAll } = require('./helpers');
const { TYPE } = require('../../voice/lib/frames');
const { loadCatalogue, installedVoices } = require('../../voice/lib/catalogue');

const PACKS = process.env.OMS_VOICE_PACKS;
const MODULES = process.env.OMS_SHERPA_NODE_MODULES;
const SAMPLES = process.env.OMS_VOICE_SAMPLES;
const skip = !(PACKS && MODULES) && 'set OMS_VOICE_PACKS and OMS_SHERPA_NODE_MODULES to run the real voices';

const LINES = {
  English: 'Welcome to the stream! That save was absolutely insane.',
  Spanish: '¡Bienvenidos al directo! Esa parada fue increíble.',
  French: 'Bienvenue sur le live ! Cet arrêt était incroyable.',
  German: 'Willkommen im Stream! Diese Parade war unglaublich.',
  Italian: 'Benvenuti in diretta! Quella parata è stata incredibile.',
  Portuguese: 'Bem-vindos à live! Aquela defesa foi incrível.',
  Dutch: 'Welkom bij de stream! Die redding was echt ongelooflijk.',
  Hindi: 'स्ट्रीम में आपका स्वागत है! वह बचाव कमाल का था।',
  Japanese: '配信へようこそ！今のセーブはすごかった。',
  Chinese: '欢迎来到直播间！刚才那个扑救太精彩了。',
  Arabic: 'مرحبا بكم في البث! كان ذلك التصدي رائعا.',
  Catalan: 'Benvinguts al directe! Aquella aturada ha estat increïble.',
  Czech: 'Vítejte na streamu! Ten zákrok byl neuvěřitelný.',
  Welsh: "Croeso i'r ffrwd! Roedd yr arbediad yna'n anhygoel.",
  Danish: 'Velkommen til streamen! Den redning var helt utrolig.',
  Greek: 'Καλώς ήρθατε στη ζωντανή μετάδοση! Αυτή η απόκρουση ήταν απίστευτη.',
  Basque: 'Ongi etorri zuzenekora! Geldiketa hura ikaragarria izan da.',
  Persian: 'به استریم خوش آمدید! آن مهار واقعا فوق العاده بود.',
  Finnish: 'Tervetuloa streamiin! Tuo torjunta oli uskomaton.',
  Hungarian: 'Üdv a streamen! Ez a védés hihetetlen volt.',
  Indonesian: 'Selamat datang di siaran! Penyelamatan tadi luar biasa.',
  Icelandic: 'Velkomin í streymið! Þessi varsla var ótrúleg.',
  Georgian: 'კეთილი იყოს თქვენი მობრძანება! ეს წარმოუდგენელი იყო.',
  Kazakh: 'Стримге қош келдіңіздер! Бұл керемет болды.',
  Kurdish: 'Hûn bi xêr hatin! Ev pir xweş bû.',
  Luxembourgish: 'Wëllkomm am Stream! Dat war onglaublech.',
  Latvian: 'Laipni lūdzam straumē! Tas bija neticami.',
  Malayalam: 'സ്ട്രീമിലേക്ക് സ്വാഗതം! അത് അതിശയകരമായിരുന്നു.',
  Nepali: 'स्ट्रिममा स्वागत छ! त्यो अचम्मको थियो।',
  Norwegian: 'Velkommen til strømmen! Den redningen var helt utrolig.',
  Polish: 'Witajcie na streamie! Ta obrona była niesamowita.',
  Romanian: 'Bun venit pe stream! Parada aceea a fost incredibilă.',
  Russian: 'Добро пожаловать на стрим! Этот сейв был просто невероятным.',
  Slovak: 'Vitajte na streame! Ten zákrok bol neuveriteľný.',
  Slovenian: 'Dobrodošli na prenosu! Ta obramba je bila neverjetna.',
  Albanian: 'Mirë se vini në transmetim! Ajo pritje ishte e jashtëzakonshme.',
  Serbian: 'Добродошли на стрим! Та одбрана је била невероватна.',
  Swedish: 'Välkommen till streamen! Den räddningen var otrolig.',
  Swahili: 'Karibuni kwenye mtiririko! Uokoaji ule ulikuwa wa ajabu.',
  Turkish: 'Yayına hoş geldiniz! O kurtarış inanılmazdı.',
  Ukrainian: 'Ласкаво просимо на стрім! Цей сейв був неймовірний.',
  Urdu: 'سٹریم میں خوش آمدید! وہ بچاؤ کمال کا تھا۔',
  Vietnamese: 'Chào mừng đến với buổi phát trực tiếp! Pha cứu thua đó thật tuyệt vời.',
};

function wav(pcm, rate) {
  const h = Buffer.alloc(44);
  h.write('RIFF', 0); h.writeUInt32LE(36 + pcm.length, 4); h.write('WAVE', 8); h.write('fmt ', 12);
  h.writeUInt32LE(16, 16); h.writeUInt16LE(1, 20); h.writeUInt16LE(1, 22); h.writeUInt32LE(rate, 24);
  h.writeUInt32LE(rate * 2, 28); h.writeUInt16LE(2, 32); h.writeUInt16LE(16, 34); h.write('data', 36); h.writeUInt32LE(pcm.length, 40);
  return Buffer.concat([h, pcm]);
}

test.afterEach(killAll);

test('real voices: every installed catalogue voice speaks its language', { skip, timeout: 3 * 60 * 60 * 1000 }, async () => {
  const cat = loadCatalogue(CATALOGUE);
  const voices = installedVoices(cat, PACKS);
  assert.ok(voices.length > 0, 'no packs unpacked in ' + PACKS);
  for (const v of voices) assert.ok(LINES[v.language], 'no sample line for ' + v.language);
  const env = { NODE_PATH: MODULES };
  if (os.platform() === 'linux') env.LD_LIBRARY_PATH = path.join(MODULES, 'sherpa-onnx-linux-x64');
  const h = runHelper(['--packs', PACKS, '--threads', '4'], env);
  await h.until(fs => fs.some(f => f.type === TYPE.READY || f.type === TYPE.ERROR), 30000);
  const results = [];
  let id = 0;
  for (const v of voices) {
    id++;
    const t0 = Date.now();
    h.send({ op: 'say', id, voice: v.id, text: LINES[v.language], speed: 1 });
    await h.until(fs => fs.some(f => f.type === TYPE.DONE && f.id === id), 10 * 60 * 1000);
    const mine = h.frames.filter(f => f.id === id);
    const err = mine.find(f => f.type === TYPE.ERROR);
    const audio = mine.filter(f => f.type === TYPE.AUDIO);
    const rate = audio.length ? audio[0].sampleRate : 0;
    const pcm = Buffer.concat(audio.map(f => f.payload));
    let sq = 0;
    for (let i = 0; i + 1 < pcm.length; i += 2) sq += (pcm.readInt16LE(i) / 32768) ** 2;
    const secs = rate ? pcm.length / 2 / rate : 0;
    const rms = pcm.length ? Math.sqrt(sq / (pcm.length / 2)) : 0;
    const r = { id: v.id, language: v.language, secs: +secs.toFixed(2), rms: +rms.toFixed(3), ms: Date.now() - t0, error: err ? err.payload.toString() : '' };
    results.push(r);
    process.stdout.write(`# ${v.id} ${r.secs}s rms ${r.rms} in ${r.ms} ms${r.error ? ' ERROR ' + r.error : ''}\n`);
    if (SAMPLES && pcm.length) {
      fs.mkdirSync(SAMPLES, { recursive: true });
      fs.writeFileSync(path.join(SAMPLES, v.id + '.wav'), wav(pcm, rate));
    }
    h.frames.length = 0; // keep memory flat over ~200 voices
  }
  if (SAMPLES) fs.writeFileSync(path.join(SAMPLES, 'results.json'), JSON.stringify(results, null, 1));
  h.close();
  await h.exited;
  const bad = results.filter(r => r.error || r.secs < 0.6 || r.rms < 0.01);
  assert.deepStrictEqual(bad, [], 'voices without real audio');
});
