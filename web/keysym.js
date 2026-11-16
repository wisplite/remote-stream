// Maps browser KeyboardEvent (key, code, location) to X11 keysyms expected by X11InputInjector.

const KEY_TO_KEYSYM = {
  // Editing & navigation
  Backspace: 0xff08,
  Tab: 0xff09,
  Clear: 0xff0b,
  Enter: 0xff0d,
  Pause: 0xff13,
  ScrollLock: 0xff14,
  Escape: 0xff1b,
  Home: 0xff50,
  ArrowLeft: 0xff51,
  ArrowUp: 0xff52,
  ArrowRight: 0xff53,
  ArrowDown: 0xff54,
  PageUp: 0xff55,
  PageDown: 0xff56,
  End: 0xff57,
  Select: 0xff60,
  PrintScreen: 0xff61,
  Execute: 0xff62,
  Insert: 0xff63,
  Undo: 0xff65,
  Redo: 0xff66,
  ContextMenu: 0xff67,
  Find: 0xff68,
  Cancel: 0xff69,
  Help: 0xff6a,
  Delete: 0xffff,

  // Lock & modifier keys (location-independent defaults)
  NumLock: 0xff7f,
  AltGraph: 0xfe03,
  CapsLock: 0xffe5,
  Shift: 0xffe1,
  Control: 0xffe3,
  Alt: 0xffe9,
  Meta: 0xffeb,

  // Function keys F1-F24
  F1: 0xffbe,
  F2: 0xffbf,
  F3: 0xffc0,
  F4: 0xffc1,
  F5: 0xffc2,
  F6: 0xffc3,
  F7: 0xffc4,
  F8: 0xffc5,
  F9: 0xffc6,
  F10: 0xffc7,
  F11: 0xffc8,
  F12: 0xffc9,
  F13: 0xffca,
  F14: 0xffcb,
  F15: 0xffcc,
  F16: 0xffcd,
  F17: 0xffce,
  F18: 0xffcf,
  F19: 0xffd0,
  F20: 0xffd1,
  F21: 0xffd2,
  F22: 0xffd3,
  F23: 0xffd4,
  F24: 0xffd5,

  // Media keys (XF86keysym)
  AudioVolumeDown: 0x1008ff11,
  AudioVolumeMute: 0x1008ff12,
  AudioVolumeUp: 0x1008ff13,
  MediaPlayPause: 0x1008ff14,
  MediaStop: 0x1008ff15,
  MediaTrackPrevious: 0x1008ff16,
  MediaTrackNext: 0x1008ff17,
};

const RIGHT_MODIFIER_TO_KEYSYM = {
  Shift: 0xffe2,
  Control: 0xffe4,
  Alt: 0xffea,
  Meta: 0xffec,
};

const NUMPAD_KEY_TO_KEYSYM = {
  Enter: 0xff8d,
  Home: 0xff95,
  ArrowLeft: 0xff96,
  ArrowUp: 0xff97,
  ArrowRight: 0xff98,
  ArrowDown: 0xff99,
  PageUp: 0xff9a,
  PageDown: 0xff9b,
  End: 0xff9c,
  Clear: 0xff9d,
  Insert: 0xff9e,
  Delete: 0xff9f,
  "*": 0xffaa,
  "+": 0xffab,
  ",": 0xffac,
  "-": 0xffad,
  ".": 0xffae,
  "/": 0xffaf,
  "0": 0xffb0,
  "1": 0xffb1,
  "2": 0xffb2,
  "3": 0xffb3,
  "4": 0xffb4,
  "5": 0xffb5,
  "6": 0xffb6,
  "7": 0xffb7,
  "8": 0xffb8,
  "9": 0xffb9,
  "=": 0xffbd,
};

// Physical-key fallback when event.key is Dead, Unidentified, or OS-intercepted
const CODE_TO_KEYSYM = {
  Backquote: 0x0060,
  Minus: 0x002d,
  Equal: 0x003d,
  BracketLeft: 0x005b,
  BracketRight: 0x005d,
  Backslash: 0x005c,
  Semicolon: 0x003b,
  Quote: 0x0027,
  Comma: 0x002c,
  Period: 0x002e,
  Slash: 0x002f,
  Space: 0x0020,
  ShiftLeft: 0xffe1,
  ShiftRight: 0xffe2,
  ControlLeft: 0xffe3,
  ControlRight: 0xffe4,
  AltLeft: 0xffe9,
  AltRight: 0xffea,
  MetaLeft: 0xffeb,
  MetaRight: 0xffec,
};

function eventToKeysym(event) {
  const { key, code, location } = event;

  if (location === KeyboardEvent.DOM_KEY_LOCATION_RIGHT && RIGHT_MODIFIER_TO_KEYSYM[key]) {
    return RIGHT_MODIFIER_TO_KEYSYM[key];
  }

  if (location === KeyboardEvent.DOM_KEY_LOCATION_NUMPAD && NUMPAD_KEY_TO_KEYSYM[key]) {
    return NUMPAD_KEY_TO_KEYSYM[key];
  }

  if (KEY_TO_KEYSYM[key]) {
    return KEY_TO_KEYSYM[key];
  }

  // Single printable character (including surrogate pairs)
  const chars = Array.from(key);
  if (chars.length === 1) {
    const cp = chars[0].codePointAt(0);
    if ((cp >= 0x0020 && cp <= 0x007e) || (cp >= 0x00a0 && cp <= 0x00ff)) {
      return cp;
    }
    if (cp === 0x20ac) {
      return 0x20ac; // XK_EuroSign
    }
    if (cp >= 0x0100) {
      return 0x01000000 | cp;
    }
  }

  if (CODE_TO_KEYSYM[code]) {
    return CODE_TO_KEYSYM[code];
  }
  if (/^Key[A-Z]$/.test(code)) {
    return code.charCodeAt(3) + 32; // 'a'..'z'
  }
  if (/^Digit[0-9]$/.test(code)) {
    return code.charCodeAt(5); // '0'..'9'
  }

  return 0;
}
