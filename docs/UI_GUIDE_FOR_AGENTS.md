# Mandatory UI and animation guide (for any agent working on hibymod)

## Goal
Rebuild **the entire** HiBy R3 II interface as a blend of **classic iPod + modern iOS**.
It must behave almost exactly like an iOS clone: gestures, physics, hierarchy and typography.
The goal is not "it works": it is that it **impresses** on three axes at once.

1. **Artistic**: the prototype's proportions (`prototipo/r3-glass.html`), glass, soft shadows, clean typography and nothing that looks "generic" or improvised.
2. **Intuitive**: iOS gestures.
   - Swiping up from the bottom goes home or opens the player.
   - The side edge is "back".
   - A long press leads to edit mode or a context menu.
   - Dragging an icon against the edge changes page.
   - No invented buttons where iOS uses a gesture.
3. **Performance**:
   - A sustained 44 fps, which is the panel's maximum, with no stutter, no flicker and no intermediate screens (grey, black or stock).
   - No frame with half-drawn content.
   - Bounded memory.

## The "gauntlet" loop (no task is closed without it)
Every animation or screen goes through this loop **until it is impressive**, not until it "looks fine":

1. **Record every real frame**: `hibyctl rec MS /usr/data/hibymod/rX.bin N`. Never in `/tmp`, which lives in RAM.
2. **Review it**: `tools/recsheet.py rX.bin rX.png` builds the contact sheet with the ms between frames. Anything over 30 ms is shown in red.
3. **Look for**:
   - half-drawn or flickering frames;
   - wrong content (stale cached images, stock screens);
   - position jumps;
   - gaps longer than 30 ms;
   - intermediate states that iOS would not show.
4. **Measure the shell's CPU and memory** (`t/shellstat.sh`). Always build with hardware floating point (`musleabihf`; soft-float is 14x slower).
5. **Fix and repeat**. It is only done when the frame sheet is flawless **and** it has been tested with a finger on the device.
6. **Compare with real iOS**: curves (cubic ease-out or spring), durations (250–350 ms), 1:1 finger tracking and release velocity.

## Technical rules learned (do not repeat mistakes)
- **ST77903 panel with no memory of its own, continuously scanned out from DDR at 44.3 Hz** (`docs/research_60fps.md`): `FBIOPAN_DISPLAY` only selects the buffer and waits for the end of the frame. Write first, present afterwards; writing into the buffer that is being scanned out flickers. 44.3 fps is the real ceiling (the panel accepts ~50 Hz at most): animations are designed for 44 Hz without jumps, not for 60.
- **Shell shared copy**: protected by a sequence lock (`fr0_seq`, `ov_seq`). A half-drawn image must never be copied.
- **Layers**: they are updated without ever turning them off in between.
- **Partial presents** (`present_rows`) when only one area changes, such as the volume pill or the status bar.
- **Stock home screen detection**: by sampling points of the **wallpaper**, never by icons, which change when pressed.
- **The stock home screen is a mirror of the launcher** (`launcher/mirror.png` with the stock icons made transparent). The library replaces its first frame with the launcher (`home_sub`).
- **Volume** is read from the status bar using real HiBy templates (`src/digits.h`), never from `user.ini`, which lags behind.
- **Memory**: the shell uses ~4.5 MB and ~7–11 MB remain free. Large buffers are reused (`img_under`, `img_sheet`, `img_base`, `img_app`) and no new ones are added without measuring.
- **Languages**: the 13 of the HiBy firmware (es, en, fr, de, it, ja, ko, pl, ru, zh-Hans, th, zh-Hant, uk). All new text goes through `tr("literal en español")` (the key is the Spanish literal): add the es/en/fr row to `TR` in `src/i18n.inc.c` and an entry in each `src/lang/<language>.inc.c` (anything missing falls back to English). `bash tests/i18n_test.sh` checks keys, `%s/%d`, glyphs and widths. Long texts (settings footers) are wrapped with `draw_wrapped`, which already knows how to wrap Chinese/Japanese and Thai.

## Remaining scope
- Our own Control Center (opens on the first try, smooth animation).
- Every HiBy menu redesigned in the house style.
- Tag editor with autocomplete and multi-edit with progress.
- Configurable keyboard (height, number row, ES/EN/FR).
- Radio from a song, visible mixes, wallpapers in `/Pictures`, YouTube Music (relay).
