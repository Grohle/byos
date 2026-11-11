# Licensing research for HiByOS

> **Update (2026-11-11):** the project was renamed **ByOS** (the name no longer contains HiBy's
> product name), and the boot animation and logo were redrawn from scratch
> (`theme/bootanim/intro.html`, Inter font under the SIL OFL): item 22 no longer applies.
> The text below is kept as written.


Date: 2026-11-10. Scope: which licence(s) HiByOS should use, what has to be fixed in the
repository regardless of that choice, and how to roll it out.

**This is not legal advice.** It is an engineering-level analysis written to help the owner
decide. Anything that could end in a dispute (trademarks, the vendor's firmware, a commercial
deal) deserves an hour with an IP lawyer.

Evidence labels used throughout:

- **[V]** verified in this repository (file read, `git`, GitHub API) on the date above.
- **[S]** verified in a primary source fetched for this report (URL given).
- **[K]** stated from general knowledge of a well-known text; not re-fetched today.
- **[I]** inference or judgement.

Nothing in the repository was changed to write this report. No licence file was added.

---

## 0. Recommendation in one page

**Relicense HiByOS from the current custom non-commercial licence to a standard, layered
open-source scheme:**

| Part | Licence (SPDX) |
|---|---|
| All of our code: shell, daemons, tools, patcher, build scripts, first-party apps | `GPL-3.0-or-later` |
| The `LD_PRELOAD` shim and the headers it shares with the shell | `GPL-3.0-or-later WITH GPL-3.0-linking-exception` (FSF template text, filled in for the vendor's player) |
| App SDK header, sample app, app/package format examples | `MIT` |
| Documentation, screenshots, original artwork (icons, wallpapers) | `CC-BY-SA-4.0` |
| Small data files, test fixtures we wrote, config snippets | `CC0-1.0` |
| Project name, logo, wordmark, boot animation | not licensed as trademarks; short `TRADEMARKS.md` policy |
| Third-party parts | their own licences, listed in `THIRD_PARTY_NOTICES.md` |

Why this one and not the others, in four sentences. It is the only option that is both real
open source and blocks closed forks: a vendor or reseller that ships HiByOS must pass on the
source of their changes and, under GPLv3 section 6, the means to install modified builds.
GPLv3 (rather than v2) has an explicit mechanism for the shim exception (section 7), is
compatible with the GPL-3.0 headphone projects whose protocol notes we used and with
Apache-2.0 dependencies, and carries a patent licence from anyone who distributes it. A
permissive SDK header keeps third-party `.hpk` apps free to choose any licence, which the
architecture already supports because apps are separate processes. It is also what the closest
comparable projects do (muOS, Onion, Magisk and RetroArch are GPL-3.0).

**The one thing this scheme does not do is forbid selling.** No open-source licence can. If
"nobody may sell this at all" is a hard requirement, HiByOS has to stay source-available and
should stop describing itself as open source. That is decision 1 in section 8.

Three things need fixing whatever licence is chosen (details in section 2):

1. The repository's blanket non-commercial `LICENSE` covers, and conflicts with, a GPL-2.0
   NetSurf port whose binary is already published in release `apps-1`.
2. Modified copies of HiBy's own scripts and layout files are tracked in `overlay/` and shipped
   in the patcher and SD payload, which contradicts the README's promise that the official
   firmware "is never redistributed".
3. Release artefacts carry almost no third-party notices (Mbed TLS, IJG, musl, Mozilla CA
   bundle, Terminus font, pycdlib in the Windows build).

And one naming fact the owner should know: HiBy's own specification page calls the stock
system "HiBy OS" [S]. The project name is the vendor's product name.

---

## 1. What is in the repository today

### 1.1 Existing licence and headers

- `LICENSE` exists [V]: "HiByOS Community License 1.0", Copyright (c) 2026 Grohle. It allows
  non-commercial use, modification and sharing under the same licence; selling, bundling with
  sold devices and paid installation need a separate commercial licence; donations are allowed.
  Clause 4 names stb, SQLite and IJG libjpeg and contains a HiBy non-affiliation statement.
- `README.md` [V] shows a badge "license: free non-commercial" and a "Licence" section with the
  same terms. GitHub reports the licence as `NOASSERTION` ("Other") [V, GitHub API].
- No file in `src/`, `tools/`, `theme/`, `apps/` carries an SPDX identifier or a copyright
  header [V]. Provenance is only in prose comments.
- History [V]: 40 commits, one author (Grohle, two e-mail addresses), 0 forks, 0 stars, one
  release (`apps-1`). **Relicensing therefore needs nobody's consent but the owner's.** Copies
  already obtained under the Community License keep that licence; new releases can use the new
  one.

Consequences of the current licence [I]:

- It is not open source under the Open Source Definition (clauses 1 and 6: no restriction on
  selling, no discrimination against fields of endeavour) [K, https://opensource.org/osd], and
  not free software ([S] FSF FAQ `#NoMilitary`: "a license that limits who can use a program,
  or for what, is not a free software license").
- It is incompatible with every GPL version. Nobody can combine HiByOS code with GPL code, and
  HiByOS cannot take code from the GPL-3.0 projects it sits next to.
- Distributions and many community projects will not package or build on it.
- It is home-made text. Terms such as "non-commercial" and "share" are undefined, and a
  custom licence is the hardest kind to enforce or to get reviewed.

### 1.2 Third-party components

How to read the table: "Where" says how the component reaches users. Most third-party sources
are downloaded at build time by `tools/wsl/get*.sh` and `tools/wsl/zbuild_*.sh` and statically
linked, so they are not in git but they are in every binary we publish [V].

| # | Component | Where it ends up | Licence | What the licence asks of us | Satisfied today? |
|---|---|---|---|---|---|
| 1 | stb_truetype, stb_image, stb_image_write | static in `hibyshell`, `hibyboot`, `hibywall`, `hibynet` | MIT **or** public domain (Unlicense), our choice [S] | Nothing if taken as public domain; MIT notice otherwise | Yes (credited in README and LICENSE) |
| 2 | SQLite amalgamation | static in `hibymix`, `hibytag` | Public domain [K] | Nothing | Yes |
| 3 | IJG libjpeg 9f | static in `hibywall`, `hibynet`, NetSurf build | IJG licence [S] | For binaries, the accompanying documentation must state: "this software is based in part on the work of the Independent JPEG Group" | Partly. README credits IJG but not with the required sentence, and the release zips do not carry it |
| 4 | Mbed TLS 2.28.9 | static in `hibynet`, NetSurf build | `Apache-2.0 OR GPL-2.0-or-later` [S] | Under Apache-2.0: ship a copy of the licence and keep notices [K] | No. No Apache-2.0 text in any artefact |
| 5 | Mozilla CA bundle (`bin/cacert.pem` from curl.se; `apps/navegador/ca.pem`, a 41-certificate subset) | SD payload; tracked in git (`ca.pem`) | MPL-2.0 [S] | Say that the file is under MPL-2.0 and where the source form is [K] | No. No notice anywhere; the subset is a modified MPL file with no header |
| 6 | musl libc | static in every `zig cc` binary | MIT [K] | Copyright and permission notice with copies | No |
| 7 | Zig compiler-rt | static in every `zig cc` binary | MIT [K] | Same | No |
| 8 | TweetNaCl-derived Ed25519 / SHA-512 in `src/ota_net.inc.c` | static in shell | Public domain [V, comment] | Nothing | Yes |
| 9 | Inter (`theme/fonts/*.ttf`) | in git; used only to render PNG glyphs and `src/digits.h` | OFL-1.1 [V, `theme/fonts/Inter-LICENSE.txt`] | Licence and copyright travel with the font files. Images made with the font are not covered ([S] OFL FAQ 1.13) | Yes |
| 10 | Terminus bitmap font converted to `apps/hola/font8x16.h` | in git; inside the published `org.hibymod.hola-1.0.0.hpk` | OFL-1.1 with Reserved Font Name [K] | Copyright statement, licence notice and licence text must accompany the font ([S] OFL FAQ 1.20); a format conversion is a Modified Version that may not use the reserved name ([S] OFL FAQ 2.2) | No. No OFL text, no copyright line |
| 11 | pycdlib 1.22 | patcher: `vendor/pycdlib` in the Python zip; frozen inside `HiByOS-Patcher.exe` | LGPL-2.1 [S] | Licence text; user must be able to replace the library (source of the application or equivalent) | Python zip: yes, licence file is included [V]. Windows zip and exe: no licence text [V] |
| 12 | PyInstaller bootloader | inside the exe | GPL-2.0-or-later with Bootloader Exception [S] | Nothing for the generated exe | Yes |
| 13 | CPython runtime and its bundled libraries | inside the exe | PSF-2.0 and others [K] | Notices | No [I: exe contents not unpacked] |
| 14 | NetSurf 3.11 framebuffer build (`nsfb`) with curl, zlib, libpng, expat, Mbed TLS, libjpeg | binary in release `apps-1` (`org.hibymod.navegador-1.0.1.hpk`) | NetSurf: GPL version 2, with an OpenSSL linking exception; artwork MIT [S]. Libraries: curl, zlib, libpng, MIT (expat), as above | GPLv2 section 3: complete corresponding source from the same place or a written offer; section 6: no further restrictions [K]. Notices for each library | No. The release links to upstream and to `apps/netsurf-hiby`, but the exact sources of the build are not offered, and the repository `LICENSE` puts a non-commercial restriction over the whole tree |
| 15 | NetSurf resources in `apps/netsurf-hiby/res/` | in git | GPL-2.0 (text, CSS, messages), MIT (artwork) [S] | Keep licence; do not relicense | Partly. `licence.html` is there, but the repository `LICENSE` claims everything |
| 16 | `apps/netsurf-hiby/build/patch_gui.py` | in git | Produces a modified NetSurf `gui.c`, so the result is GPL-2.0 [I] | Mark the script and its output accordingly | No marking |
| 17 | **Vendor files, modified**: `overlay/usr/bin/hiby_player.sh`, `overlay/etc/init.d/S11jpeg_display_shell`, `overlay/usr/resource/layout/theme{2,3}/hiby_about_dev.view`, `.../topbar/topbar.view`; views written by `theme/build_theme.py` from stock views; `volume.dlg` generated in `tools/make_payload.py` from the stock dialog | in git and/or in the patcher and the SD payload | HiBy proprietary, no licence to us [V: vendor fragments and comments are still in the files] | We have no right to redistribute adaptations | **No** |
| 18 | Vendor machine code used as signatures in `src/hibymod_disp.c` (`pl_sig`, `st_sig`, `qc_sig`: 16 to 32 bytes each) | in git and in the shim | HiBy proprietary | Probably too small and too functional to matter [I], but it is literally vendor code in our source | Tolerable; trivially avoidable (store a hash instead) |
| 19 | Headphone protocol knowledge from OpenSCQ30, OpenFreebuds, LibrePods, GalaxyBudsClient (GPL-3.0) and Gadgetbridge (AGPL-3.0) | `src/buds_*.inc.c` | Facts are not copyrightable. `docs/HEADPHONES.md` states "facts only, no code" [V]. The code is in the project's own C style and the sources are in other languages; not audited line by line [I] | Credit (given in the docs) | Yes, with a caveat: the same author read the sources and wrote the code, so describe it as "independent implementation from documented protocol facts", not "clean room" |
| 20 | `tools/lzo1x.py`, `tools/squashfs.py` | patcher | Own format-level implementations per their docstrings [V]; not audited against GPL liblzo2 or squashfs-tools [I] | None if independent | Yes [I] |
| 21 | Segoe UI / Arial used by `theme/hibyos_brand.py` to render the "GitHub" pill | rendered into PNGs in `overlay/` | Microsoft font licence [I: allows producing images, not embedding the font] | None for output images | Yes; switching to Inter would remove the question |
| 22 | `theme/bootanim/source.mp4`, `docs/media/hibyos_bootanimation.mp4`, logo | in git, in every install | **Unknown provenance** (which tool made the video, under what terms) | Depends | **Owner must confirm** |
| 23 | API fixtures in `tests/*_samples/` (archive.org, LibriVox, Gutendex, Jamendo JSON) | in git | Mostly factual metadata; some descriptive text by third parties [I] | Low risk | Acceptable; label as third-party data |

Items 19 and 20 are where GPL-3.0-or-later quietly helps: if anyone ever argued that a
headphone module or the LZO code follows a GPL original too closely, the answer under GPLv3
is "then it is licensed compatibly"; under the current non-commercial licence it would be an
infringement.

### 1.3 Other observations

- No Jamendo API key is embedded; the user supplies their own (`src/freemusic.inc.c`) [V].
- The firmware container has no cryptographic signature. `tools/build.py` only recomputes MD5
  chunk manifests, and the stock recovery accepts the rebuilt image [V]. This matters for
  section 5.
- `docs/research_*.md` contain function addresses and short disassembly fragments of the
  vendor's `hiby_player` (for example `docs/research_play_list.md` line 65,
  `docs/research_settings_api.md` line 22) [V]. See section 5.2.
- The README credits LRCLIB, the iTunes Search API and the Cover Art Archive [V]. Their terms
  of use are separate from software licensing and were not audited here. Project Gutenberg's
  robot policy says its site is "intended for human users only" and can block automated
  access [K]; the Books app should keep to Gutendex, mirrors and modest request rates.

---

## 2. Problems to fix regardless of the licence chosen

Ordered by how much they could hurt.

**R1. A non-commercial blanket licence over GPL-2.0 code, with a binary already published.**
`apps/netsurf-hiby/` and the `nsfb` binary inside `org.hibymod.navegador-1.0.1.hpk` (release
`apps-1`) are GPL-2.0 [S]. GPLv2 forbids adding restrictions, and distributing the binary
requires offering the exact corresponding source. Mbed TLS must be taken under its
GPL-2.0-or-later option in that build, because Apache-2.0 is not compatible with GPLv2 [K].
Simplest fix today: remove the browser `.hpk` from the release (the store already marks it
`available: false`), and when it returns, attach a source archive of the exact NetSurf and
library versions plus our patches and build scripts next to the binary, and mark the
directory GPL-2.0-only.

**R2. Modified vendor files are distributed.** Table row 17. The README says the official
firmware "is never redistributed" [V], but adapted copies of two vendor shell scripts and
several vendor layout files are in git, in the patcher (which bundles all of `overlay/`), and
in the SD theme payload. `tools/make_payload.py` only drops files that are byte-identical to
stock, so modified stock files pass its check [V]. Fix: ship these as edits that the patcher
applies to the user's own firmware (the `TEXT_PATCHES` mechanism in `tools/build.py` already
does this for other files), and generate modified views and `volume.dlg` on the user's side
or on the device at install time. Then remove the adapted copies from the tree. Until that is
done they must be marked as not covered by the project licence.

**R3. Missing third-party notices in everything we publish.** Table rows 3 to 7, 10, 11, 13.
Fix once with a generated `THIRD_PARTY_NOTICES.md` plus a `LICENSES/` folder, and put both in
the Windows zip, inside the exe (`--add-data`, plus a `--licenses` command-line flag), the
Python zip, the SD payload (`HiByOS/licenses/`), every `.hpk` we publish, and an "Open-source
licences" screen in Settings > About.

**R4. Project name and look.** Section 4.

**R5. No headers, no per-directory licence information.** Section 6.

**R6. Unknown provenance of the boot animation video.** Table row 22.

**R7. Vendor code bytes in the shim and disassembly in docs.** Table row 18 and section 5.2.
Low risk, cheap to tidy.

**R8. Authorship statement.** The README and the GitHub description say the project is "built
mostly by AI agents" [V]. See section 3.6: this does not change the licence choice, but it
weakens any licence's enforceability and should be handled deliberately.

---

## 3. Which licence for our code

### 3.1 The owner's goals, and what each family delivers

| Goal | MIT / BSD / Apache-2.0 | MPL-2.0 | LGPL-3.0 | GPL-2.0 | **GPL-3.0-or-later** | AGPL-3.0 | Current NC licence |
|---|---|---|---|---|---|---|---|
| Really free and open (OSI/FSF) | yes | yes | yes | yes | **yes** | yes | no |
| Forkable, portable to other HiBy/Ingenic players | yes | yes | yes | yes | **yes** | yes | only non-commercially |
| Vendor or reseller cannot take it closed | no | only per file; new files can be closed | weak (made for libraries) | yes | **yes** | yes | yes (in theory) |
| Reseller who preloads must let buyers install modified builds | no | no | partly | not explicit | **yes (section 6)** | yes | n/a |
| Patent licence from whoever distributes | Apache only | yes | yes | no explicit grant | **yes (section 11)** | yes | no |
| Explicit, standard way to permit the shim-in-proprietary-binary case | not needed | not needed | awkward fit | home-made exception | **section 7 + FSF template** | same as GPLv3 | n/a |
| Can reuse code from GPL-3.0 headphone projects | no (would become GPL) | no | no | no (v2-only) / yes (v2-or-later) | **yes** | yes | no |
| Compatible with Apache-2.0 dependencies | yes | yes | yes | no | **yes** | yes | n/a |
| Forbids selling | no | no | no | no | **no** | no | yes |

### 3.2 Why not each alternative

- **MIT / BSD / Apache-2.0.** A vendor could ship HiByOS in a closed firmware and never
  publish a line. That is the exact outcome the owner wants to prevent. Good for the SDK
  header, wrong for the system.
- **MPL-2.0.** File-level copyleft. A vendor keeps our files open but adds proprietary
  features in new files and ships a closed product. It would make the shim case easy, but one
  licence plus one exception is simpler than two licences.
- **LGPL.** Designed for a library that applications call. Our shim is the opposite: it is
  injected into an application that does not know it exists. LGPL-3.0 is technically GPLv3
  plus permissions, so it would work, but its "Combined Work" conditions read oddly here and
  it gives no benefit for the shell, daemons or tools.
- **GPL-2.0.** Used by Rockbox and OpenWrt [V] mainly for historical reasons and kernel
  compatibility. No explicit installation-information rule, no explicit patent grant,
  incompatible with Apache-2.0, and "v2 only" could not take GPL-3.0 code.
- **AGPL-3.0.** Its extra clause only bites when users interact with a modified version over
  a network. Device firmware has no remote users, so it adds corporate friction and nothing
  else. KOReader uses it [V]; that is not a reason for us.
- **Staying non-commercial.** Covered in section 1.1 and decision 1.

### 3.3 GPLv3 specifics that matter for a firmware mod

- **Anti-tivoization (section 6).** Installation Information is owed only when object code is
  conveyed "in, or with, or specifically for use in, a User Product, and the conveying occurs
  as part of a transaction in which the right of possession and use of the User Product is
  transferred" [S]. We do not sell or hand over players, so the rule does not bind the
  project [I from the text]. It does bind a reseller who sells players with HiByOS
  preloaded: they must give buyers the source and the means to install modified versions.
  That is the protection the owner is asking for.
- **OTA signing key.** We sign updates with Ed25519. The FSF FAQ is explicit that keys must be
  released only when software is conveyed inside a User Product whose hardware checks the
  signature [S, `#GiveUpKeys`]. Our check lives in our own GPL code, which anyone can rebuild
  with their own key, and the patcher path needs no key at all. The private key stays private.
- **Anti-circumvention (section 3).** Nobody can wrap our code into a "technological
  measure" and then use anti-circumvention law against people who modify it [S].
- **Patents (sections 10 and 11).** Each contributor and distributor licenses its essential
  patent claims, and suing over the program ends the licence [S]. The owner has no patents to
  give up; this only protects the project if a company ever picks it up.
- **Termination and cure (section 8)** is friendlier to honest mistakes by community members
  than GPLv2 [K].
- **"Or later".** Keeps the project compatible with future GPL versions and with
  `GPL-3.0-only` and `-or-later` code alike. Recommended.
- **Selling.** The GPL allows anyone to sell copies [S, `#DoesTheGPLAllowMoney`]. What it
  forbids is selling them closed.

### 3.4 The shim: why an exception, and the exact wording

Facts [V]: `src/hibymod_disp.c` builds to `hibymod_disp.so`, is loaded with `LD_PRELOAD` into
HiBy's proprietary `hiby_player`, interposes libc and ALSA calls, and calls internal vendor
functions by address. It shares an address space, function calls and data structures with the
vendor program.

FSF position [S]: when a main program and a plug-in are dynamically linked, "make function
calls to each other and share data structures, we believe they form a single combined
program" (`#GPLPlugins`). For a GPL plug-in to a non-free program: "you can resolve that legal
problem by adding an exception to your plug-in's license, giving permission to link it with
the nonfree main program" (`#GPLPluginsInNF`). The device's glibc and kernel interfaces are
System Libraries and need nothing (`#SystemLibraryException`); the vendor's player is not a
System Library.

Practical reading [I]: the project itself never conveys the combination, because the shim
only meets `hiby_player` on the user's own device, and the GPL places no conditions on
running or privately modifying. The exception is still worth having: it removes the question
for users, for forks, for anyone who ports the shim to another player, and for contributors
whose code lands in the shim. It costs nothing and it is the FSF's own recommended route.

SPDX already lists the FSF template as `GPL-3.0-linking-exception` [S], which keeps the
project REUSE-compliant. The template is used unchanged; only the bracketed parts are filled
in. The optional sentence about providing the library's source is left out, as the FAQ
instructs when "not everybody can distribute source for the libraries" [S].

Proposed header for `src/hibymod_disp.c`, `src/hm_shared.h` and `src/topbar_layout.h` (the two
headers are included by the shim [V], so they need the same permission):

```c
/*
 * SPDX-FileCopyrightText: 2026 Grohle and the HiByOS contributors
 * SPDX-License-Identifier: GPL-3.0-or-later WITH GPL-3.0-linking-exception
 *
 * This file is part of HiByOS.
 *
 * HiByOS is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. It is distributed WITHOUT ANY WARRANTY; see the GNU General Public
 * License for details.
 *
 * Additional permission under GNU GPL version 3 section 7
 *
 * If you modify this Program, or any covered work, by linking or combining it
 * with the player application and the libraries shipped by the manufacturer
 * in the stock firmware of the audio player on which this Program runs, such
 * as HiBy's "hiby_player" (or a modified version of those programs and
 * libraries), containing parts covered by the terms of the manufacturer's
 * proprietary licence, the licensors of this Program grant you additional
 * permission to convey the resulting work.
 */
```

Notes on that wording [I]:

- It names a class ("the stock firmware of the audio player on which this Program runs")
  rather than one binary, so a port to another HiBy or Ingenic player needs no new exception.
- It applies only to files that carry it. Section 7 says a part with additional permissions
  "may be used separately under those permissions, but the entire Program remains governed by
  this License" [S]. The shell, daemons and tools stay plain GPL.
- It grants nothing in HiBy's software. Whoever conveys a combined image still needs HiBy's
  permission for HiBy's part, which is why the project distributes a patcher.
- Anyone may remove the permission from their own modified copy (section 7) [S].

### 3.5 Third-party `.hpk` apps: no exception needed, but say so

Facts [V]: an app is a separate static executable started by the shell. It talks to
`/dev/fb0` and the input devices, receives its context through the documented package layout
and environment (`docs/APP_FORMAT.md`), and includes one header, `src/sdk/hm_app.h`, which
contains `static inline` code that is compiled into the app.

GPLv3 section 5 [S]: separate and independent works "which are not by their nature extensions
of the covered work, and which are not combined with it such as to form a larger program"
form an aggregate, and the GPL does not reach the other parts. The FSF FAQ adds that "pipes,
sockets and command-line arguments are communication mechanisms normally used between two
separate programs" (`#MereAggregation`) [S]. Apps are on the safe side of that line. The only
real coupling is the header, because inline code is copied into the app. So:

1. License `src/sdk/hm_app.h`, `apps/hola/` (except its font) and the examples in
   `docs/APP_FORMAT.md` under **MIT**.
2. Put this note in `LICENSES/README.md` and `docs/APP_FORMAT.md`. It follows the spirit of
   the Linux kernel's system-call note [K, https://spdx.org/licenses/Linux-syscall-note.html]:

```
HiByOS application interface note

Applications packaged as .hpk files and started by HiByOS as separate
processes, which interact with HiByOS only through the interfaces documented
in docs/APP_FORMAT.md (package layout, manifest, environment, framebuffer and
input devices, exit codes and files), are not regarded by the HiByOS copyright
holders as derivative works of HiByOS or as works "based on" it. Their authors
may distribute them under any licence they choose.

The SDK header src/sdk/hm_app.h and the sample application in apps/hola are
licensed under the MIT licence so that they can be copied into such
applications.

To the extent that such an application would nevertheless be considered a
covered work solely because it uses those interfaces, the licensors of HiByOS
grant, as an additional permission under section 7 of the GNU General Public
License version 3, permission to convey it under terms of your choice.

This note does not apply to an application that copies or links any other
HiByOS source code. Such an application must comply with the GPL.
```

3. Remember the other direction: when the project hosts someone's `.hpk` in the store or in a
   GitHub release, the project is distributing that app and must comply with the app's
   licence. NetSurf is the live example (R1). The store index should carry a `license` field
   (SPDX id) and a `source` URL per app, and the store should refuse GPL apps without one.

### 3.6 AI-assisted authorship

The project says it was "built mostly by AI agents" [V]. Copyright licences, copyleft and
non-commercial alike, only bind where copyright exists.

- United States: the Copyright Office's report on copyrightability was published on
  29 January 2025 [S, https://www.copyright.gov/ai/]. Its widely reported conclusions are
  that purely AI-generated material is not protected, that prompts alone are not enough, and
  that human-authored elements, creative selection and arrangement, and human modifications
  can be protected [K; the PDF could not be parsed in this session, so check the wording
  before quoting it].
- Spain and the EU: the author is "la persona natural que crea alguna obra" (TRLPI art. 5.1)
  [S, https://www.boe.es/buscar/act.php?id=BOE-A-1996-8930], and EU case law requires the
  author's own intellectual creation [K].

What follows from this [I]:

- The choice of licence does not change. Every alternative has the same weakness.
- Keep the claim honest: "Copyright (c) 2026 Grohle and the HiByOS contributors" covers
  whatever is protectable (architecture, design decisions, selection, arrangement, edits).
  Do not claim more in public statements.
- Keep the evidence of human direction that already exists (design documents, specifications,
  review history).
- Do not rely on copyright alone to stop a bad actor. The project name, the signing key for
  OTA and the store, and the community are the stronger practical levers.

### 3.7 What comparable projects do

| Project | Licence | Checked |
|---|---|---|
| Magisk (root and module framework; injects code into other processes) | GPL-3.0 | [V] GitHub API. Not checked for an exception text |
| muOS (`MustardOS/internal`), Onion (`OnionUI/Onion`), RetroArch | GPL-3.0 | [V] GitHub API |
| postmarketOS (`pmbootstrap`, `pmaports`) | GPL-3.0 | [K] not confirmed in this session (the GitLab lookup resolved to a fork namespace) |
| KOReader (runs beside proprietary e-reader firmware) | AGPL-3.0 | [V] GitHub API |
| Rockbox | GPL v2 text in `docs/COPYING`; third-party texts in `docs/LICENSES` | [V] |
| OpenWrt | `GPL-2.0-only`, with a REUSE-style `LICENSES/` directory | [V] `COPYING` |
| LineageOS | Apache-2.0 (inherits Android's) | [V] `android_vendor_lineage/LICENSE` |
| ArkOS (`christianhaitian/arkos`) | MIT | [V] GitHub API |
| spruceOS | CC BY-NC 4.0 based, non-commercial | [V] `LICENSE`. A precedent for staying non-commercial, at the price of not being open source |
| MinUI | no licence detected | [V] GitHub API |
| LSPosed (hooks into proprietary apps) | GPL-3.0 | [V] GitHub API |
| Frida core (instrumentation injected into proprietary processes) | wxWindows Library Licence 3.1, a weak copyleft with a binary exception | [V] `COPYING` |
| `hiby-modding/hiby-mods` (the closest neighbour, R3 Pro II) | MIT for tooling, with a `NOTICE.md` stating firmware binaries remain their owner's property | [V] `LICENSE` |

Reading [I]: community firmware for handhelds clusters on GPL-3.0; projects that descend from
Android are Apache; older players' firmware is GPL-2.0. Code that is injected into
proprietary processes is either GPL-3.0 without ceremony (Magisk, LSPosed) or weak copyleft
(Frida). GPL-3.0 with a narrow, standard linking permission sits between the two and is the
most defensible.

---

## 4. Assets, documentation, names and look

### 4.1 Separate licences for non-code

Yes, license them separately. Creative Commons itself recommends against CC licences for
software and against putting a CC licence on a logo or trademark [S, CC FAQ].

- **Documentation and screenshots (`docs/`, `README.md`, `docs/media/`)**: `CC-BY-SA-4.0`.
  It mirrors the copyleft intent, and BY-SA 4.0 is one-way compatible with GPLv3 [S, CC FAQ],
  so text or artwork can be folded into the GPL firmware when needed.
- **Original artwork (icons, wallpapers, UI layers under `shell/`, `theme/`)**: the generators
  are code and are GPL. Their output that ships inside the firmware is licensed
  `GPL-3.0-or-later OR CC-BY-SA-4.0`, so it can travel with the firmware or be reused as
  artwork [I: a pragmatic dual grant that avoids arguments about what "source" means for an
  image].
- **Small data, fixtures and configuration we wrote**: `CC0-1.0`.
- **Not CC0 for the artwork.** CC0 would let a vendor take the whole look closed.
- **Logo, wordmark, boot animation**: copyright under the same artwork terms is fine, but
  state that no trademark rights are granted (GPLv3 section 7(e) allows exactly that
  reservation [S]).

### 4.2 The name "HiByOS"

HiBy's specification for this very player lists the operating system as "HiBy OS (Linux
4.4.94)" [S, https://guide.hiby.com/en/docs/products/audio_player/hiby_r3ii_2025/spec], and
reviewers write it "HiByOS". The project therefore carries the vendor's mark and the
vendor's own product name for the same kind of product on the same device. A disclaimer
helps with honesty but does not cure a confusingly similar name [I]. Realistic consequences:
a takedown request to GitHub, or a forced rename after the project has an audience. Renaming
is cheapest now (0 forks, 0 stars [V]). The repository was called `hibymod` before; a name
that does not start with the vendor's mark and is described as "for HiBy players" is the
standard safe pattern (compare "Rockbox", "Onion", "muOS"). This is decision 2.

If the name stays, at least: never present it as HiBy's system, keep "unofficial" in the
first line of the README and on the About screen, and do not use HiBy's logo or typography.

### 4.3 The iOS look

What is safe [I]: describing the style ("iOS-inspired"), implementing common interaction
ideas (home grid, control centre, gestures), using Inter rather than Apple's fonts [V], and
drawing icons from scratch [V: `theme/launcher_icons.js` and `theme/icons.js` are procedural
and say the Tidal, Qobuz and App Store icons are not the brands' logos].

Where the risk is [I]:

- Apple's guidelines [S]: "You may not imitate the distinctive Apple packaging, web site
  design, logos, or typefaces"; "You may not use the Apple Logo or any other Apple-owned
  graphic symbol, logo, or icon"; no "variation ... takeoff, or abbreviation" of Apple marks.
  Guidelines are not law, but they show what Apple enforces, and Apple also holds registered
  designs for icons and screen layouts [K].
- The Music (white notes on red), Files (blue folder on white) and Settings (grey gear)
  launcher icons are close to Apple's [V, visual check]. These are the items most likely to
  draw a complaint. Changing glyph, colour or composition enough to be clearly our own is
  cheap.
- "iOS 26 style" and "Apple Music-style" in the README [V] use Apple marks to sell the look.
  Prefer "iOS-inspired" once, in descriptive text, never in the project name, tagline or
  release titles. Note that Apple uses "iOS" under licence from Cisco [K].

### 4.4 Product names and drawings in the Headphones app

Naming compatible products ("AirPods Pro", "Galaxy Buds", "WF-1000XM5") to say what the app
works with is referential use, which Apple's own guidelines accept in phrases such as
"compatible with" as long as there is no "sense of endorsement, sponsorship, or false
association" [S]. Keep names in plain text, never logos. Our own vector drawings of the
products [V] are not photographs or Apple artwork; keep them generic enough that they read as
illustrations, and add the attribution lines below.

### 4.5 Wording to use

README, near the top:

```
HiByOS is an unofficial, community-made modification for HiBy players. It is
not affiliated with, endorsed by, or supported by HiBy. It does not contain or
redistribute HiBy's firmware: the patcher modifies the copy that you download
from HiBy yourself. Installing it is at your own risk and may affect your
warranty.
```

`NOTICE` (also shown on the About screen and placed in every release):

```
HiByOS
Copyright (c) 2026 Grohle and the HiByOS contributors.

Licensed under the GNU General Public License, version 3 or (at your option)
any later version; see LICENSE. Some files carry additional permissions or
other licences; see LICENSES/ and THIRD_PARTY_NOTICES.md.

This software is based in part on the work of the Independent JPEG Group.

Trademarks. HiBy and HiBy OS are trademarks of their owner. Apple, AirPods,
AirPods Pro, AirPods Max, Apple Music and iTunes are trademarks of Apple Inc.,
registered in the U.S. and other countries and regions. IOS is a trademark of
Cisco, used by Apple under license. Galaxy Buds is a trademark of Samsung
Electronics. The Bluetooth word mark is owned by Bluetooth SIG, Inc. All other
product and company names are trademarks of their respective owners. They are
used only to identify compatible products. No affiliation with or endorsement
by any of these companies is implied. The interface is an original work
inspired by common mobile design conventions and contains no Apple artwork,
fonts or code.

The licences of this project do not grant permission to use the HiByOS name or
logo, except to state truthfully that a work is, or is based on, HiByOS.
```

`TRADEMARKS.md`, three rules [I]: unmodified builds may use the name and logo; modified
builds must be clearly marked as modified or use another name; nobody may suggest the project
or its owner endorses a product or a paid service.

---

## 5. Firmware modding, only as it affects licensing and distribution

Not legal advice. The owner is in Spain; users are everywhere.

### 5.1 Patcher-only distribution is the right design, and it must be kept honest

Distributing only our own code plus a tool that modifies the user's lawfully obtained copy
keeps the project out of the distribution right for HiBy's software (Directive 2009/24/EC
art. 4(1)(c)) [S]. It is also the only way to stay clear of the vendor's GPL obligations: the
stock image contains a Linux 4.4.94 kernel [S] and, as usual, GPL user-space tools. Anyone
distributing that image must offer the corresponding source. HiBy must; we could not, so we
must never host a full or patched image. No public HiBy source release was found in a quick
search [I]; anyone who received the firmware may ask HiBy for it, which would also help
ports.

R2 is the gap in this design: modified vendor files are currently distributed.

### 5.2 EU and Spain

- Art. 5(1): a lawful acquirer may reproduce and adapt a program where "necessary for the use
  of the computer program ... in accordance with its intended purpose, including for error
  correction", but only "in the absence of specific contractual provisions" [S]. A vendor
  licence could therefore restrict modification by the user. HiBy's terms were not reviewed.
- Art. 5(3): observing, studying and testing a running program to determine its underlying
  ideas is always allowed and cannot be excluded by contract (art. 8) [S].
- Art. 6: decompilation is allowed without permission where indispensable to make an
  independently created program interoperable, limited to the necessary parts, and the
  information obtained may not be "given to others, except when necessary for the
  interoperability of the independently created program" nor used for a substantially similar
  program [S]. Contract terms to the contrary are void (art. 8) [S]. Spain implements these in
  TRLPI art. 100 [K].
- Court of Justice, C-159/23 Sony v Datel (17 October 2024): changing the content of
  variables that a program holds in RAM, without altering its code, is not covered by the
  Directive's protection [S, press release
  https://curia.europa.eu/jcms/upload/docs/application/pdf/2024-10/cp240181en.pdf]. This
  supports run-time hooking of the kind the shim does [I]. It does not cover the patcher's
  on-disk edits to vendor scripts and to `codec_es9219.ko`, which are adaptations made by the
  user on their own copy under art. 5(1). The licence tag of that kernel module could not be
  checked here; kernel modules are normally GPL, which would make that edit expressly
  permitted [I].

Practical consequences [I]:

- The shim and the patcher are defensible as interoperability work on a lawfully acquired
  device. Art. 6 is the reason to describe HiByOS as an independent program that
  interoperates with the stock player, and to keep the stock player untouched.
- `docs/research_*.md` should publish interfaces (function roles, addresses, struct offsets,
  message formats) and not verbatim disassembly. Art. 6(2) is the weak spot of public
  reverse-engineering notes. Trim listings; keep facts.
- Replace the machine-code signatures in the shim with hashes of the same bytes.

### 5.3 United States

- 17 U.S.C. 1201 prohibits circumventing access controls and trafficking in tools for that
  purpose. The current exemptions (37 CFR 201.40, 2024 rule) cover jailbreaking of
  "smartphones and portable all-purpose mobile computing devices", smart TVs, voice assistant
  devices and "routers and dedicated network devices", and repair of consumer devices [S,
  https://www.law.cornell.edu/cfr/text/37/201.40]. A dedicated music player is not a
  "portable all-purpose mobile computing device", which must be "primarily designed to run a
  wide variety of programs" [S]. Exemptions also never cover distributing tools [K].
- What protects the project is simpler: there appears to be nothing to circumvent. The
  update container is not signed or encrypted, only checksummed, and the stock recovery
  installs the rebuilt image [V]. Keep it that way: if a future HiBy firmware adds signature
  checks, do not add code that defeats them without legal advice.

### 5.4 Warranty and liability

GPLv3 sections 15 and 16 disclaim warranty and liability [K], and the FSF confirms a
conditional warranty stance is fine [S, `#v3ConditionalWarranty`]. Keep the "at your own
risk" box in the README. In the EU a disclaimer does not remove liability for intent or gross
negligence [K], so keep the recovery path documented and tested.

---

## 6. Rollout plan

### 6.1 Files to add

```
LICENSE                      GPL-3.0 text, unmodified (GitHub then detects "GPL-3.0")
NOTICE                       section 4.5
THIRD_PARTY_NOTICES.md       one entry per row of the table in 1.2: name, version, URL,
                             licence, copyright line, which binaries contain it
TRADEMARKS.md                section 4.5
CONTRIBUTING.md              section 6.5
REUSE.toml                   per-directory licence annotations
LICENSES/
  GPL-3.0-or-later.txt
  GPL-3.0-linking-exception.txt
  GPL-2.0-only.txt           NetSurf port
  MIT.txt
  CC-BY-SA-4.0.txt
  CC0-1.0.txt
  OFL-1.1.txt                Inter, Terminus
  MPL-2.0.txt                CA bundle subset
  README.md                  plain-language map, the shim permission, the app interface note
```

Licence texts for components that are only downloaded at build time (Apache-2.0 for Mbed
TLS, the IJG README, the musl and Zig notices, LGPL-2.1 for pycdlib, PSF-2.0) are not needed
in `LICENSES/` for REUSE, because no tracked file is under them, but they must be in the
release artefacts. Keep them in a `third_party/licenses/` folder that the packaging scripts
copy.

### 6.2 REUSE compliance: yes

Recommended. The spec is short: every licence used has its text in `LICENSES/`, named by
SPDX identifier; every file has a copyright notice and an `SPDX-License-Identifier`, in a
comment header, an adjacent `.license` file, or `REUSE.toml`; exceptions must be on the SPDX
list; custom terms use `LicenseRef-` [S, https://reuse.software/spec-3.3/]. This is why the
shim uses the SPDX-listed `GPL-3.0-linking-exception` instead of home-made text.

With about 900 tracked files, most of them images, do it in two layers:

1. `REUSE.toml` for whole directories:

```toml
version = 1

[[annotations]]
path = ["src/**", "tools/**", "theme/**", "shell/**", "overlay/**", "tests/**", "repo/**"]
SPDX-FileCopyrightText = "2026 Grohle and the HiByOS contributors"
SPDX-License-Identifier = "GPL-3.0-or-later"

[[annotations]]
path = ["docs/**", "README.md", "prototipo/**"]
SPDX-FileCopyrightText = "2026 Grohle and the HiByOS contributors"
SPDX-License-Identifier = "CC-BY-SA-4.0"

[[annotations]]
path = ["src/sdk/**", "apps/hola/**"]
precedence = "override"
SPDX-FileCopyrightText = "2026 Grohle and the HiByOS contributors"
SPDX-License-Identifier = "MIT"

[[annotations]]
path = ["apps/netsurf-hiby/res/**"]
precedence = "override"
SPDX-FileCopyrightText = "The NetSurf Developers"
SPDX-License-Identifier = "GPL-2.0-only AND MIT"

[[annotations]]
path = ["theme/fonts/*.ttf"]
precedence = "override"
SPDX-FileCopyrightText = "2016 The Inter Project Authors"
SPDX-License-Identifier = "OFL-1.1"
```

   Add entries for `apps/hola/font8x16.h` (OFL-1.1, Terminus copyright), `apps/navegador/ca.pem`
   (MPL-2.0), `apps/netsurf-hiby/build/nsfb_hiby.c` (MIT, as its header says [V]),
   `apps/netsurf-hiby/build/patch_gui.py` (GPL-2.0-only), test fixtures (CC0-1.0 or a
   third-party note), and, until R2 is fixed, the vendor-derived files under a
   `LicenseRef-HiBy-Proprietary` identifier whose text says "not licensed; to be removed".

2. Real comment headers in hand-written source (`src/*.c`, `*.h`, `tools/*.py`, shell
   scripts), two lines each. The three shim files get the long header from section 3.4.

Then run `reuse lint` in CI.

### 6.3 Corresponding source, and what goes in releases

GPLv3 lets us satisfy the source requirement by offering it from the same place as the
binaries (section 6(d)) [K]. GitHub releases do that, with one catch: Corresponding Source is
"all the source code needed to generate, install, and ... run the object code" [S], and our
third-party libraries are fetched by scripts, not stored in git. So:

- Pin every download in `tools/wsl/get*.sh` and `zbuild_*.sh` to a version and a SHA-256.
- Attach to every release a `hibyos-<version>-source.tar.gz` that contains the tagged tree
  plus the exact third-party source archives. musl and compiler-rt come with the compiler and
  count as System Libraries [I, from the section 1 definition]; the notice is still required.
- State in the release notes: licence, link to the source archive, and the line about the
  Independent JPEG Group.

Per artefact:

| Artefact | Must contain |
|---|---|
| `HiByOS-Patcher-<v>-windows.zip` and the exe | `LICENSE`, `NOTICE`, `THIRD_PARTY_NOTICES.md`, pycdlib's LGPL text, Python's licence. The LGPL replacement requirement is met by publishing the pure-Python zip and the full source next to it |
| `HiByOS-Patcher-<v>-python.zip` | same; pycdlib licence already there [V] |
| SD payload `HiByOS/` | `licenses/` folder with the same files; `cacert.pem` listed as MPL-2.0 |
| On the device | Settings > About > "Open-source licences" showing `NOTICE` and the third-party list; texts in `/usr/share/hibyos/licenses/` |
| OTA bundle | carries the updated `licenses/`; release notes link to the source archive for that exact version |
| Each first-party `.hpk` | a `LICENSE` or `licenses/` entry in the package; for GPL apps, a source link in the store index |
| Store index `repo/index.json` | per-app `license` (SPDX) and `source` fields |

### 6.4 Order of work

1. Owner decides the three points in section 8.
2. Remove the NetSurf `.hpk` from release `apps-1` until it can ship with its sources (R1).
3. Move vendor-derived files out of the tree into patch-time edits (R2); hash the shim
   signatures; trim disassembly from the research docs (R7).
4. Add `LICENSE`, `LICENSES/`, `NOTICE`, `THIRD_PARTY_NOTICES.md`, `TRADEMARKS.md`,
   `CONTRIBUTING.md`, `REUSE.toml`, headers. Add the Terminus OFL notice and the CA bundle
   notice. Confirm the boot video's provenance or replace it (R6).
5. Update the README: badge, licence section, disclaimer, acknowledgements with the IJG
   sentence, "iOS-inspired" wording.
6. Make the packaging scripts (`tools/build_patcher.py`, `tools/make_payload.py`, the OTA
   builder) copy the notices; add the on-device licences screen.
7. Tag the first release under the new scheme and say so in its notes: "From version X,
   HiByOS is licensed under GPL-3.0-or-later. Earlier versions were released under the
   HiByOS Community License 1.0."

### 6.5 Contributors: DCO, plus a small relicensing grant

Today the owner is the only copyright holder [V], so every option is open. The first outside
pull request under plain GPL ends that: from then on, changing the licence or granting a
commercial exception needs every contributor's agreement.

- **DCO only** (`Signed-off-by`, https://developercertificate.org/ [S]): lowest friction;
  certifies the right to contribute under the licence in the file. Keeps nothing open beyond
  what "or later" gives.
- **Full CLA or copyright assignment**: keeps everything open, including selling proprietary
  licences, but deters hobby contributors and needs tooling.

Recommended middle path [I]: require the DCO sign-off, and put a short inbound grant in
`CONTRIBUTING.md`, accepted by a checkbox in the pull-request template:

```
By contributing you certify the Developer Certificate of Origin 1.1 (add a
Signed-off-by line to each commit) and you license your contribution under
the licence of the file you change, including any additional permission that
file carries. You also grant the project maintainer (Grohle) a non-exclusive,
irrevocable right to distribute your contribution under any other licence
approved by the Open Source Initiative or classified as free by the Free
Software Foundation, and to add further permissions under section 7 of the
GNU GPL. Contributions written with AI assistance are welcome; say so in the
pull request, and do not paste code whose origin you cannot vouch for.
```

That keeps the project able to fix its licensing later (new exception for another player,
move artwork terms, a future GPL) without being able to take contributions closed. If the
owner wants to keep selling commercial licences, as the current `LICENSE` offers, this grant
is not enough and a real CLA is needed from the first external contribution. That is
decision 3.

---

## 7. Sources

Licence texts and guidance

- GNU GPL version 3: https://www.gnu.org/licenses/gpl-3.0.html (text read from the SPDX copy,
  https://github.com/spdx/license-list-data/blob/main/text/GPL-3.0-or-later.txt)
- FSF GPL FAQ: https://www.gnu.org/licenses/gpl-faq.html, entries `#GPLIncompatibleLibs`,
  `#GPLPlugins`, `#GPLPluginsInNF`, `#NFUseGPLPlugins`, `#MereAggregation`,
  `#SystemLibraryException`, `#GiveUpKeys`, `#DoesTheGPLAllowMoney`, `#NoMilitary`,
  `#v3ConditionalWarranty` (gnu.org was rate-limiting; read from the Internet Archive copy)
- SPDX exceptions: https://spdx.org/licenses/GPL-3.0-linking-exception.html,
  https://spdx.org/licenses/GPL-3.0-linking-source-exception.html,
  https://spdx.org/licenses/Linux-syscall-note.html
- REUSE specification 3.3: https://reuse.software/spec-3.3/
- Developer Certificate of Origin: https://developercertificate.org/
- Open Source Definition: https://opensource.org/osd
- Creative Commons FAQ (software, GPLv3 compatibility, trademarks): https://creativecommons.org/faq/
- SIL OFL FAQ: https://openfontlicense.org/ofl-faq/

Third-party components

- stb: https://github.com/nothings/stb/blob/master/LICENSE
- SQLite: https://sqlite.org/copyright.html
- IJG legal text (as carried in libjpeg-turbo): https://github.com/libjpeg-turbo/libjpeg-turbo/blob/main/README.ijg
- Mbed TLS: https://github.com/Mbed-TLS/mbedtls/blob/development/LICENSE
- CA bundle: https://curl.se/docs/caextract.html
- pycdlib: https://github.com/clalancette/pycdlib
- PyInstaller: https://github.com/pyinstaller/pyinstaller/blob/develop/COPYING.txt
- NetSurf: https://www.netsurf-browser.org/about/licence
- Inter: https://github.com/rsms/inter

Comparable projects

- https://github.com/topjohnwu/Magisk, https://github.com/MustardOS/internal,
  https://github.com/OnionUI/Onion, https://github.com/libretro/RetroArch,
  https://github.com/koreader/koreader, https://github.com/Rockbox/rockbox,
  https://github.com/openwrt/openwrt, https://github.com/LineageOS/android_vendor_lineage,
  https://github.com/christianhaitian/arkos, https://github.com/spruceUI/spruceOS,
  https://github.com/shauninman/MinUI, https://github.com/LSPosed/LSPosed,
  https://github.com/frida/frida-core, https://github.com/hiby-modding/hiby-mods,
  https://gitlab.postmarketos.org/postmarketOS/pmbootstrap

Law and policy

- Directive 2009/24/EC: https://eur-lex.europa.eu/eli/dir/2009/24/oj (articles read at
  https://www.legislation.gov.uk/eudr/2009/24/body)
- CJEU C-159/23, press release 181/24: https://curia.europa.eu/jcms/upload/docs/application/pdf/2024-10/cp240181en.pdf
- Spanish TRLPI (Real Decreto Legislativo 1/1996): https://www.boe.es/buscar/act.php?id=BOE-A-1996-8930
- 37 CFR 201.40: https://www.law.cornell.edu/cfr/text/37/201.40; 2024 rulemaking: https://www.copyright.gov/1201/2024/
- US Copyright Office, Copyright and Artificial Intelligence: https://www.copyright.gov/ai/
- Apple trademark guidelines: https://www.apple.com/legal/intellectual-property/guidelinesfor3rdparties.html
- HiBy R3 II 2025 specification: https://guide.hiby.com/en/docs/products/audio_player/hiby_r3ii_2025/spec
- Project Gutenberg robot policy: https://www.gutenberg.org/policy/robot_access.html

Not verified in this session, flagged where used: the conclusions of the Copyright Office
report beyond its date; TRLPI art. 100; HiBy's end-user terms; the licence tag of
`codec_es9219.ko`; the terms of the iTunes Search API, LRCLIB, Jamendo and the Internet
Archive; whether Magisk carries any exception text; the contents of the PyInstaller exe; the
postmarketOS licence; the current wording of Project Gutenberg's robot policy.

---

## 8. Decisions that need the owner

1. **Is "nobody may sell it" a hard requirement?** If preventing closed forks and forcing
   source to be given back is enough, adopt the scheme in section 0. A reseller could then
   sell players with HiByOS preloaded, but would have to publish their changes, pass on the
   GPL, give buyers the means to install modified builds, respect the trademark policy, and
   separately clear HiBy's firmware. If any sale at all is unacceptable, keep a
   non-commercial licence, replace the home-made text with a known one for the parts where
   that is possible, stop calling the project open source, and accept that GPL code
   (including the NetSurf port) cannot live under it.
2. **Name and look.** Keep "HiByOS", which is the vendor's own name for its system, or rename
   now while it is free to do so. And how close to Apple's icons to stay.
3. **Future commercial licensing.** Plain DCO plus the narrow grant in 6.5, or a full CLA so
   that the owner can keep selling exceptions. This must be settled before the first outside
   contribution is merged.

One factual question for the owner, not a decision: how `theme/bootanim/source.mp4` was made
and under what terms (R6).
