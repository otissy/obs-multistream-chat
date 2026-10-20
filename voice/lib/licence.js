'use strict';
// Decides from a voice pack's own MODEL_CARD / README / LICENSE text whether the plugin may ship it.
// Shipping means streamers use it on monetised streams, so anything non-commercial, no-derivatives,
// research-only or simply unstated is refused. A licence counts when it is named, or linked by its
// canonical URL (creativecommons.org, apache.org, opensource.org); any other link does not (it is what
// research-only datasets such as the Blizzard/Lessac corpus give).
const REFUSE = [
  [/non[\s-]?commercial|\bBY[\s-]?NC\b|\bNC[\s-]?(SA|ND)\b/i, 'non-commercial'],
  [/no[\s-]?derivatives|\bBY[\s-]?ND\b/i, 'no-derivatives'],
  [/research (purposes )?only|for research use|academic use only|not for commercial/i, 'research only'],
];

const ALLOW = [
  [/\bCC0\b|creative commons zero|public[\s-]domain|creativecommons\.org\/publicdomain\/(zero|mark)\//i, 'CC0-1.0 / public domain'],
  [/unlicense\.org|\bthe unlicense\b/i, 'Unlicense'],
  [/\bCC[\s-]?BY[\s-]?SA\b|attribution[\s-]sharealike|share[\s-]?alike|creativecommons\.org\/licenses\/by-sa\//i, 'CC-BY-SA'],
  [/\bCC[\s-]?BY\b|creative commons attribution|attribution 4\.0|\bODC[\s-]?By\b|creativecommons\.org\/licenses\/by\//i, 'CC-BY'],
  [/apache(\s+license)?[\s,-]*(version\s*)?2(\.0)?|apache\.org\/licenses\/LICENSE-2\.0/i, 'Apache-2.0'],
  [/\bMIT\s+licen[cs]e\b|licen[cs]e\s*:\s*MIT\b|opensource\.org\/licenses?\/MIT/i, 'MIT'],
  [/\bBSD[\s-]?\d[\s-]?clause\b|\bBSD\s+licen[cs]e\b/i, 'BSD'],
];

// Piper voices fine-tuned from the Lessac checkpoint: the dataset licence (Blizzard) restricts use.
const DERIVED_WARN = /fine[\s-]?tun(ed|ing) (from|on)[^\n]*lessac/i;

function classify(text) {
  const t = String(text || '');
  if (!t.trim()) return { ok: false, licence: '', reason: 'no licence text in the pack' };
  for (const [re, why] of REFUSE) if (re.test(t)) return { ok: false, licence: why, reason: why };
  for (const [re, name] of ALLOW) {
    if (re.test(t)) {
      const warn = DERIVED_WARN.test(t) ? 'fine-tuned from Lessac (Blizzard dataset licence): check before shipping' : '';
      return { ok: true, licence: name, reason: '', warn };
    }
  }
  return { ok: false, licence: '', reason: 'no recognised licence named (a bare link does not count)' };
}

module.exports = { classify };
