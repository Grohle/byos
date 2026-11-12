# On-screen keyboard

ByOS has its own iOS-style keyboard. Every app that asks for text uses it: search in Music,
Radio, Vinyl and Books, renaming in Files, the text editor, the tag editor and Wi-Fi passwords.

There is one keyboard for each of the 13 interface languages.

## Choosing keyboards

1. Open **Settings › Keyboard › Keyboards**.
2. Turn on the keyboards you want. At least one always stays on.

When you change the interface language, its keyboard is turned on and selected, as on iOS.
A new install starts with the keyboard of its language and English.

While typing, use the **globe** key at the bottom left:

- **Tap** it to go to the next keyboard you turned on.
- **Touch and hold** it, or slide up from it, to see the list. Lift your finger on a keyboard
  to pick it. The first row, **Keyboard Settings**, opens the keyboard sheet.
- If only one keyboard is on, a tap opens the list.

The keyboard sheet also has the keyboards that are on: as a segmented control when there are up
to three, or as a row of buttons that scrolls sideways when there are more.

## Layouts

| Keyboard | Layout | Notes |
|---|---|---|
| English | QWERTY | |
| Español | QWERTY with ñ | |
| Français | AZERTY | |
| Deutsch | QWERTZ with ü ö ä | ß on the long press of s |
| Italiano | QWERTY | accents on the long press of the vowels |
| Polski | QWERTY | ą ć ę ł ń ó ś ź ż on the long press of the letter |
| Русский | ЙЦУКЕН | ё on the long press of е, ъ on ь and х |
| Українська | ЙЦУКЕН with ї і є | ґ on the long press of г, the apostrophe on ь |
| 日本語 | romaji (QWERTY) | see Japanese below |
| 한국어 | 2-set (Dubeolsik) | shift gives ㅃ ㅉ ㄸ ㄲ ㅆ ㅒ ㅖ |
| 简体拼音 | QWERTY pinyin | see Chinese below |
| 繁體拼音 | QWERTY pinyin | traditional characters |
| ไทย | Kedmanee, 4 rows | shift gives the other half of the layout and the Thai digits |

On the Latin keyboards, a long press on a letter shows its accented forms, in the order that
iOS uses for that language. The 123 page has the currency of the language (€, $, ₩, ¥, ฿, ₴).
The Russian keyboard shows € because the firmware fonts have no ₽ sign.

Automatic capitals and the double-space period shortcut work on the Latin and Cyrillic keyboards.

## Korean

Letters join into syllables as you type, inside the text itself. Backspace removes one key
press at a time: 닭 then ㅏ gives 달가, and backspace brings back 닭.

The syllable is committed when you type a space, a sign, switch pages or keyboards, move the
cursor, or tap somewhere else in the field. Shift does not commit it, so 있다 and 밖 can be
typed with the shifted ㅆ and ㄲ.

## Japanese

Type in romaji. The kana appear in the candidate bar above the keys; the text field only
changes when you choose.

- Both Hepburn and kunrei spellings work: shi or si, chi or ti, tsu or tu, fu or hu, ja or zya.
- A doubled consonant gives っ, as in kitte.
- For ん, type nn, or n followed by another consonant.
- Small kana use an x or l prefix, as in xtu or lya.
- On the 123 page, ー continues the word.

The candidate bar offers hiragana and katakana:

- **Space** or **Enter** commits the hiragana.
- Tap the katakana to commit it instead.
- A sign such as 。 commits the hiragana and then adds the sign.

There is no kanji conversion yet.

## Chinese (simplified and traditional)

Type pinyin without tones; use v for ü (lv, nve); lue and nue also work for lüe and nüe. Candidates for the first syllable appear in
the candidate bar:

- **Tap** a candidate to type it. The rest of the pinyin stays, so nihao gives 你, then 好.
- **Space** types the first candidate.
- **Enter** types the letters as they are.
- A sign such as ， converts what is left and then adds the sign.
- An incomplete syllable shows the characters of every syllable that starts that way. For
  example, zh shows 这, 中 and so on.
- Type ' to separate syllables, as in xi'an.

The traditional keyboard uses the same pinyin and shows traditional characters. When one
simplified character has several traditional forms, they are all offered: 发 gives 發 and 髮,
and 后 gives 後 and 后.

Chinese is typed **one character at a time**: there is no phrase dictionary yet. Zhuyin
(bopomofo) is planned for Taiwan users.

## Fields that only accept Latin letters

Some fields only accept Latin letters, such as the Wi-Fi password. If the current keyboard is
not Latin, these fields use your first Latin keyboard, or English. This applies to that field
only and does not change your saved choice. The list shows the other keyboards greyed out,
with a note.

## For developers

| File | Content |
|---|---|
| `src/kb_ime.inc.c` | The keyboard's typing logic, with no screen or touch code. It holds the layout table, the edit engine, the Hangul engine, romaji to kana and pinyin lookup. |
| `src/ime_zh.inc.c` | The pinyin tables, about 34 KB. It is generated, so do not edit it by hand. |
| `tools/gen_ime_tables.py` | The script that generates the pinyin tables. |
| `src/keyboard.inc.c` | The drawing and touch code: keys, popups, candidate bar, globe menu and sheet. |

The generator takes its characters from GB2312 (levels 1 and 2), through Python's codec. It
gets readings and traditional forms from the ICU library installed on the PC. Candidates are
ordered by `tools/ime_zh_freq.txt` plus a hand-made list of polyphones. ICU is used only to
generate the tables, so the firmware build does not depend on it. To regenerate the tables, run
this in WSL:

```
python3 -I tools/gen_ime_tables.py
```

The layout id is the language's `LANG_*` value. `lconf` stores the current keyboard as
`kb <height> <number row> <layout>` and the keyboards that are on as `kbs <bit mask>`.

To make a field accept Latin letters only, set `kb_latin_only = 1` before `kb_layout()`.
`kb_end()` clears it.

The tests run on the PC and do not use the device:

- `bash tests/kb_test.sh` checks that every key and variant has a glyph and that keys are at
  least 20 px wide. It types and deletes all 11172 Hangul syllables key by key, and checks the
  romaji, pinyin, Thai cursor and edit-engine cases.
- `bash tests/kb_render.sh` writes PNG frames of every layout, popup, candidate bar and menu to
  `out/kb_render/`. It also drives the real `kb_touch()` with scripted touches.
