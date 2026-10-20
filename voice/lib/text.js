'use strict';
// What a chat line becomes before it is spoken. Chat is hostile input: the goal is that no message
// can make the voice read a URL letter by letter, hold one note for ten seconds, or talk for a minute.
const MAX_CHARS = 300;

const URL_RE = /\b(?:https?:\/\/|www\.)\S+/giu;
// emoji, skin tones, pictographs, flags, keycaps, tags, variation selectors and joiners: noise to a voice
const SYMBOL_RE = /[\p{Extended_Pictographic}\p{Emoji_Modifier}\p{Regional_Indicator}\u{FE0E}\u{FE0F}\u{200D}\u{20E3}\u{E0020}-\u{E007F}]/gu;
const CONTROL_RE = /[\p{Cc}\p{Cf}]/gu;

function cleanText(input, maxChars = MAX_CHARS) {
  if (typeof input !== 'string') return '';
  let s = input.normalize('NFC');
  s = s.replace(URL_RE, ' a link ');
  s = s.replace(SYMBOL_RE, ' ');
  s = s.replace(CONTROL_RE, ' ');
  // "noooooooo" -> "nooo", "!!!!!!!" -> "!!!", "lolololololol" -> "lololol"
  s = s.replace(/(.)\1{3,}/gu, '$1$1$1');
  s = s.replace(/(..)\1{3,}/gu, '$1$1$1');
  s = s.replace(/\s+/gu, ' ').trim();
  if ([...s].length > maxChars) {
    const cut = [...s].slice(0, maxChars).join('');
    const sp = cut.lastIndexOf(' ');
    s = (sp > maxChars * 0.6 ? cut.slice(0, sp) : cut).trim();
  }
  // nothing left that a voice can say (only symbols / punctuation)
  if (!/[\p{L}\p{N}]/u.test(s)) return '';
  return s;
}

module.exports = { MAX_CHARS, cleanText };
