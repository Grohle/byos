# Third-party notices

ByOS is published under its own licence (see `LICENSE`). The files that ByOS
distributes also contain software, fonts and data written by other people. This document
lists every such component, with its version, where it comes from, its licence, its
copyright notice as written by its authors, and where it ends up.

The full licence texts are in the `LICENSES/` folder of the repository. Every released
file set carries a copy of this document together with the licence texts that apply to it
(`tools/notices.py` selects them). In a released file set the licence files are in the same
folder as this document, so read `LICENSES/name.txt` below as `name.txt`.

Nothing in this document changes the licence of any component. Each component stays under
its own licence, which takes precedence over the ByOS licence for that component.

ByOS does not distribute HiBy's firmware. The patcher works on the user's own copy of
the official firmware; HiBy's software is not covered here.

Where something could not be established with certainty from the upstream files, it is
marked **Uncertain**.

## 1. What is distributed

| Name used by `tools/notices.py` | What it is |
| --- | --- |
| `firmware` | The files ByOS adds to the firmware image (the `overlay/` tree). MIPS programs: `usr/bin/hibyboot`, `usr/bin/hibyctl`, `usr/lib/hibymod/bin/wpa_cli`. |
| `sd-payload` | The `ByOS/payload/` folder for the SD card. MIPS programs in `bin/`: `hibyshell`, `hibywall`, `hibymix`, `hibytag`, `hibynet`, `hibymod_disp.so`; plus `bin/cacert.pem`. |
| `patcher-python` | `ByOS-Patcher-<version>-python.zip`: the patcher as Python scripts, a copy of pycdlib in `vendor/`, and a copy of the `firmware` files. |
| `patcher-windows` | `ByOS-Patcher.exe` (and the zip that contains it): the same patcher frozen with PyInstaller, with a Python runtime inside, and a copy of the `firmware` files. |
| `hpk:org.hibymod.navegador` | The "Browser" app package: `bin/nsfb` (NetSurf), `bin/navegador` (launcher), `res/` (NetSurf resources), `ca.pem`. |
| `hpk:org.hibymod.hola` | The "Hello" sample app package: `bin/hola`. |
| `byos-cli` | `byos-cli.jar`: the command-line client for computers (section 7). Not yet handled by `tools/notices.py`; the jar carries its own copy of the notices. |

## 2. Which component is in which file set

| Component | firmware | sd-payload | patcher-python | patcher-windows | navegador | hola |
| --- | :-: | :-: | :-: | :-: | :-: | :-: |
| musl libc, Zig runtime (3.1, 3.2) | yes | yes | yes | yes | yes | yes |
| GNU C Library static pieces (3.3) | | yes | | | | |
| stb (3.4) | yes | yes | yes | yes | | |
| SQLite (3.5) | | yes | | | | |
| IJG libjpeg (3.6) | | yes | | | yes | |
| Mbed TLS (3.7) | | yes | | | yes | |
| Mozilla CA certificates (3.8) | | yes | | | yes | |
| TweetNaCl-derived Ed25519 (3.9) | | yes | | | | |
| Inter, rendered glyphs only (3.10) | yes | yes | yes | yes | | |
| Terminus Font (4.1) | | | | | | yes |
| NetSurf and its libraries (5.1 to 5.8) | | | | | yes | |
| curl, zlib, libpng, Expat (5.9 to 5.12) | | | | | yes | |
| pycdlib (6.1) | | | yes | yes | | |
| Python runtime and what it bundles (6.2) | | | | yes | | |
| PyInstaller bootloader, with zlib (6.3) | | | | yes | | |
| Microsoft runtime libraries (6.4) | | | | yes | | |

The two patcher packages contain a copy of the `firmware` files, so everything listed for
`firmware` also applies to them.

## 3. Components in the firmware files and in the SD payload

### 3.1 musl libc

- Version: 1.2.5 (the copy shipped with Zig 0.17.1, `lib/libc/musl`).
- Upstream: https://musl.libc.org
- Licence: MIT (SPDX `MIT`). Some files have their own permissive notices; they are all
  listed in the musl `COPYRIGHT` file.
- Copyright: `Copyright © 2005-2020 Rich Felker, et al.`
- Where: linked statically into every MIPS executable ByOS builds (all programs listed in
  section 1 except `hibymod_disp.so`, which uses the C library already on the player).
- Licence text: `LICENSES/musl-COPYRIGHT.txt`

### 3.2 Zig runtime (compiler-rt and C library functions written in Zig)

- Version: Zig 0.17.1 (PyPI package `ziglang` 0.17.1; clang 22.1.8 and LLD 22.1.8). All
  MIPS programs are compiled with `zig cc`.
- Upstream: https://ziglang.org
- Licence: MIT (SPDX `MIT`).
- Copyright: `Copyright (c) Zig contributors`
- Where: Zig links its own runtime support (`lib/compiler_rt`, `lib/c`) statically into
  every MIPS program and shared object it produces.
- Licence text: `LICENSES/Zig-LICENSE.txt`
- Notes: many of these Zig files say they were ported from musl (see 3.1). A few say they
  were ported from LLVM's compiler-rt (https://llvm.org), which is under the Apache License
  2.0 with LLVM Exceptions (SPDX `Apache-2.0 WITH LLVM-exception`); the LLVM licence file
  shipped with Zig is reproduced in `LICENSES/LLVM-LICENSE.txt`. **Uncertain**: which of
  those functions actually end up in each stripped binary was not determined, so the LLVM
  text is included with every MIPS file set.

### 3.3 GNU C Library (glibc), small statically linked pieces

- Version: the glibc support files shipped with Zig 0.17.1 (`lib/libc/glibc`); the target
  is glibc 2.22 (`-target mipsel-linux-gnueabihf.2.22`).
- Upstream: https://www.gnu.org/software/libc/
- Licence: LGPL-2.1-or-later with a special exception that allows linking these files into
  other programs without restriction (SPDX `LGPL-2.1-or-later` plus that exception; the
  exact wording is reproduced in the notice file).
- Copyright: `Copyright (C) 1996-2020 Free Software Foundation, Inc.`
- Where: `hibymod_disp.so` in the SD payload. This shared object is linked dynamically
  against the C library already installed on the player (glibc is not shipped by ByOS),
  but the tool chain adds a few small glibc helper objects to it (for example the `stat` /
  `fstat` wrappers of `libc_nonshared`).
- Licence text: `LICENSES/glibc-nonshared-NOTICE.txt`, `LICENSES/LGPL-2.1.txt`,
  `LICENSES/glibc-LICENSES.txt`
- **Uncertain**: the binary is stripped, so the exact list of glibc objects inside it could
  not be read back; the evidence is its undefined references to `__xstat` and `__fxstat`.

### 3.4 stb single-file libraries

- Version: stb_image 2.30, stb_truetype 1.26, stb_image_write 1.16.
- Upstream: https://github.com/nothings/stb
- Licence: the author offers a choice of two: MIT or public domain (SPDX
  `MIT OR Unlicense`).
- Copyright: `Copyright (c) 2017 Sean Barrett`
- Where: `hibyboot` (stb_image) in the firmware files; in the SD payload `hibyshell`
  (stb_image, stb_truetype, stb_image_write), `hibywall` (stb_image) and `hibynet`
  (stb_image, stb_image_write).
- Licence text: `LICENSES/stb-LICENSE.txt`

### 3.5 SQLite

- Version: 3.50.4 (amalgamation, source id `2025-07-30 19:33:53`).
- Upstream: https://www.sqlite.org
- Licence: public domain (SPDX `blessing`).
- Copyright: none. The source says: `The author disclaims copyright to this source code.`
- Where: `hibymix` and `hibytag` in the SD payload.
- Licence text: `LICENSES/SQLite-blessing.txt`

### 3.6 IJG libjpeg

- Version: 9f (14-Jan-2024).
- Upstream: https://www.ijg.org
- Licence: Independent JPEG Group licence (SPDX `IJG`).
- Copyright: `This software is copyright (C) 1991-2024, Thomas G. Lane, Guido Vollbeding.`
- Where: `hibywall` and `hibynet` in the SD payload; `bin/nsfb` in the Browser app.
- Licence text: `LICENSES/IJG-libjpeg-README.txt` (section "LEGAL ISSUES")
- Required statement: **this software is based in part on the work of the Independent JPEG
  Group.**

### 3.7 Mbed TLS

- Version: 2.28.9.
- Upstream: https://github.com/Mbed-TLS/mbedtls
- Licence: dual, at the recipient's choice: `Apache-2.0 OR GPL-2.0-or-later`.
- Copyright: `Copyright The Mbed TLS Contributors`
- Where: `hibynet` in the SD payload; `bin/nsfb` in the Browser app (as the TLS back end of
  curl).
- Licence text: `LICENSES/MbedTLS-LICENSE.txt` (the upstream `LICENSE` file, which contains
  both licence texts). `LICENSES/Apache-2.0.txt` and `LICENSES/GPL-2.0.txt` are the same
  two texts as separate files.
- Notes: in `hibynet` Mbed TLS is used under the Apache License 2.0. In the Browser app it
  is linked into NetSurf, which is under GPL version 2 only (see 5.1); there it is used
  under its GPL-2.0-or-later option. The optional `3rdparty/everest` code of the Mbed TLS
  archive is not enabled in either build.

### 3.8 Mozilla CA certificate list

- Version: `bin/cacert.pem` in the SD payload is the bundle published by the curl project,
  "Certificate data from Mozilla as of: Thu Nov  5 04:12:01 2026 GMT" (118 certificates,
  converted with mk-ca-bundle.pl 1.35), unchanged. `ca.pem` in the Browser app is a
  hand-picked subset (41 root certificates) of that list, with a different comment header.
- Upstream: https://curl.se/docs/caextract.html (conversion) and Mozilla's `certdata.txt`,
  https://raw.githubusercontent.com/mozilla-firefox/firefox/refs/heads/release/security/nss/lib/ckfw/builtins/certdata.txt
  (the source form).
- Licence: Mozilla Public License 2.0 (SPDX `MPL-2.0`). The PEM file itself carries no
  licence line; `certdata.txt` says "This Source Code Form is subject to the terms of the
  Mozilla Public License, v. 2.0", and the curl page says the PEM file "is licensed under
  the same license as the Mozilla source file: MPL 2.0".
- Copyright: no copyright line is given upstream.
- Where: `bin/cacert.pem` in the SD payload (trust anchors for `hibynet`, and the fallback
  for the Browser when its own `ca.pem` is missing); `ca.pem` in the Browser app.
- Licence text: `LICENSES/MPL-2.0.txt`

### 3.9 TweetNaCl (Ed25519 arithmetic)

- Version: derived from `tweetnacl.c`; only the field arithmetic and group law needed to
  verify Ed25519 signatures, adapted (see `src/ota_net.inc.c`).
- Upstream: https://tweetnacl.cr.yp.to
- Licence: public domain (no SPDX identifier; the authors state "TweetNaCl is a
  self-contained public-domain C library").
- Authors: Daniel J. Bernstein, Bernard van Gastel, Wesley Janssen, Tanja Lange, Peter
  Schwabe, Sjaak Smetsers.
- Where: `hibyshell` in the SD payload (verification of online updates).
- Licence text: `LICENSES/TweetNaCl-public-domain.txt`
- **Uncertain**: the TweetNaCl version the code was adapted from is not recorded in the
  source; 20140427 is the latest version published upstream.

### 3.10 Inter typeface (rendered glyphs only)

- Version: 4.000 (`Inter-Bold.ttf`, `Inter-SemiBold.ttf`, git-a52131595).
- Upstream: https://github.com/rsms/inter
- Licence: SIL Open Font License 1.1 (SPDX `OFL-1.1`).
- Copyright: `Copyright (c) 2016 The Inter Project Authors (https://github.com/rsms/inter)`
- Where: the font files themselves are **not** distributed in any file set. Inter is used
  at build time to draw some images and digit bitmaps (for example the status bar digits)
  that are part of the firmware files and of `hibyshell`. The notice is given as a
  courtesy.
- Licence text: `LICENSES/OFL-1.1-Inter.txt`

## 4. Component in the "Hello" app (`org.hibymod.hola`)

### 4.1 Terminus Font

- Version: the 8x16 bold console font `Lat15-TerminusBold16.psf.gz` from the Ubuntu package
  `console-setup-linux` 1.226ubuntu1, converted to a C table (`apps/hola/font8x16.h`,
  made by `apps/hola/mkfont.py`). **Uncertain**: the package does not state which Terminus
  Font release its copy corresponds to.
- Upstream: https://terminus-font.sourceforge.net
- Licence: SIL Open Font License 1.1 (SPDX `OFL-1.1`).
- Copyright: `Copyright (c) 2010 Dimitar Toshkov Zhekov, with Reserved Font Name "Terminus Font".`
- Where: compiled into `bin/hola`.
- Licence text: `LICENSES/OFL-1.1-Terminus.txt`

`bin/hola` also contains musl libc and the Zig runtime (3.1, 3.2).

## 5. Components in the "Browser" app (`org.hibymod.navegador`)

`bin/nsfb` is the NetSurf web browser, framebuffer front end, built from the
`netsurf-all-3.11` source archive with the changes in `apps/netsurf-hiby/build/` of the
ByOS repository. It is one statically linked program that contains everything listed in
this section, plus IJG libjpeg (3.6), Mbed TLS (3.7), musl libc and the Zig runtime (3.1,
3.2). `bin/navegador` is a small ByOS launcher that contains only musl libc and the Zig
runtime.

The following optional parts of NetSurf are **not** built in: JavaScript (Duktape),
OpenSSL, libsvgtiny, librosprite, WebP, PDF export.

### 5.1 NetSurf

- Version: 3.11 (28th December 2023).
- Upstream: https://www.netsurf-browser.org
  (source: https://download.netsurf-browser.org/netsurf/releases/source-full/netsurf-all-3.11.tar.gz)
- Licence: source code, documentation, translatable messages and UI definitions: GNU
  General Public License, version 2 only, with a special exception that permits linking
  with OpenSSL (SPDX `GPL-2.0-only`; the exception is written in the licence file). Visual
  artwork: MIT (SPDX `MIT`).
- Copyright: `© 2002–2020 The NetSurf Developers` (as written in NetSurf's `licence.html`;
  the individual source files name their authors).
- Where: `bin/nsfb`; and the resource files in `res/` (`Messages`, `en/Messages`,
  `adblock.css`, `default.css`, `internal.css`, `quirks.css`, `credits.html`,
  `licence.html`, `welcome.html`, `favicon.png`, `icons/`, `pointers/`, `throbber/`), which
  are copied unchanged from `frontends/framebuffer/res` of the NetSurf source.
  `res/licence.html` is NetSurf's own licence page.
- Licence text: `LICENSES/NetSurf-COPYING.txt` (contains the MIT and GPL texts),
  `LICENSES/GPL-2.0.txt`
- Notes: `bin/nsfb` as a whole is a program based on NetSurf; the GPL text sets the
  conditions for distributing it, including access to the corresponding source code. The
  ByOS changes to NetSurf are in `apps/netsurf-hiby/build/` (`build_hf.sh` is the build
  script; `patch_gui.py` modifies `frontends/framebuffer/gui.c`; `nsfb_hiby.c` is an added
  libnsfb surface).

### 5.2 NetSurf project libraries

- Versions: LibCSS 0.9.2, LibDOM 0.4.2, Hubbub 0.3.8, libnsbmp 0.1.7, libnsfb 0.2.2,
  libnsgif 1.0.0, libnslog 0.1.3, libnspsl 0.1.7, libnsutils 0.1.1, LibParserUtils 0.2.5,
  LibWapcaplet 0.4.3. Build tools whose output is compiled in: nsgenbind 0.9,
  buildsystem 1.10.
- Upstream: https://www.netsurf-browser.org/projects/
- Licence: MIT (SPDX `MIT`).
- Copyright (as written in each `COPYING` file):
  - LibCSS, LibDOM, Hubbub: `Copyright (C) 2007 J-M Bell`
  - LibParserUtils: `Copyright (C) 2007-8 J-M Bell`
  - libnsbmp: `Copyright (C) 2006 Richard Wilson`, `Copyright (C) 2008 Sean Fox`
  - libnsgif: `Copyright (C) 2004 Richard Wilson`, `Copyright (C) 2008 Sean Fox`,
    `Copyright (C) 2013-2021 Michael Drake`
  - libnsfb: `Copyright (C) 2009-2016 Vincent Sanders <vince@netsurf-browser.org>`,
    `Copyright (C) 2009-2016 Michael Drake <tlsa@netsurf-browser.org>`
  - libnslog, libnsutils: `Copyright 2014 Vincent Sanders`
  - libnspsl: `Copyright 2016 Vincent Sanders`
  - LibWapcaplet: `Copyright 2009 The NetSurf Browser Project`
- Where: `bin/nsfb`.
- Licence text: `LICENSES/NetSurf-libraries-COPYING.txt` (each `COPYING` file, verbatim)
- Notes: LibCSS contains a 32-bit MurmurHash2 function (`src/select/arena_hash.h`):
  "Created by Austin Appleby, and placed in the public domain."

### 5.3 NetSurf built-in framebuffer font

- Version: as in NetSurf 3.11 (`frontends/framebuffer/res/fonts/glyph_data`).
- Upstream: http://git.netsurf-browser.org/art.git/plain/fonts/netsurf/glyph_data
- Licence: MIT (SPDX `MIT`).
- Copyright: `Copyright Tim Tyler`, `Copyright Michael Drake <tlsa@netsurf-browser.org>`
  (based on the "Default" font of Zap 1.47 by Tim Tyler).
- Where: compiled into `bin/nsfb` (the font the browser draws text with).
- Licence text: last section of `LICENSES/NetSurf-libraries-COPYING.txt`

### 5.4 utf8proc and Unicode data

- Version: 2.4.0-1 (the copy in `netsurf-all-3.11`).
- Upstream: https://github.com/JuliaStrings/utf8proc
- Licence: MIT (SPDX `MIT`); its data tables are derived from the Unicode Character
  Database, under the Unicode data licence (SPDX `Unicode-DFS-2015`, **uncertain** which
  SPDX identifier matches the wording best; the text is in the licence file).
- Copyright: `Copyright © 2014-2019 by Steven G. Johnson, Jiahao Chen, Tony Kelman, Jonas
  Fonseca, and other contributors`; `Copyright (c) 2009, 2013 Public Software Group e. V.,
  Berlin, Germany`; Unicode data: `Copyright (c) 1991-2007 Unicode, Inc.`
- Where: `bin/nsfb` (international domain names).
- Licence text: `LICENSES/utf8proc-LICENSE.md`
- Notes: NetSurf's `utils/idna_props.h` is a table generated from the Unicode IDNA
  property data and is also compiled in.

### 5.5 talloc

- Version: the copy in NetSurf 3.11 (`utils/talloc.c`, from Samba; no version number in
  the file).
- Upstream: https://talloc.samba.org
- Licence: GNU Lesser General Public License, "either version 2 of the License, or (at your
  option) any later version" (SPDX `LGPL-2.0-or-later`).
- Copyright: `Copyright (C) Andrew Tridgell 2004`, `Copyright (C) Stefan Metzmacher 2006`
- Where: `bin/nsfb`.
- Licence text: `LICENSES/talloc-NOTICE.txt` (the file header, verbatim),
  `LICENSES/LGPL-2.0.txt`, `LICENSES/LGPL-2.1.txt`
- **Uncertain**: the header names the "Lesser" licence with version 2; version 2 was
  published as the GNU *Library* General Public License and the first "Lesser" version is
  2.1. Both texts are included.

### 5.6 Punycode sample implementation

- Version: `punycode-sample.c 2.0.0 (2004-Mar-21-Sun)` (NetSurf `utils/punycode.c`).
- Upstream: http://www.nicemice.net/idn/ ; published as RFC 3492, Appendix C.
- Licence: the permission statement of RFC 3492, Appendix B (no SPDX identifier).
- Author: Adam M. Costello.
- Where: `bin/nsfb`.
- Licence text: `LICENSES/punycode-RFC3492.txt`
- **Uncertain**: the NetSurf copy has no licence text of its own; the RFC statement is
  taken to apply because the file identifies itself as that sample code.

### 5.7 Public Suffix List

- Version: the `public_suffix_list.dat` shipped in libnspsl 0.1.7 (no version or date line
  in the file).
- Upstream: https://publicsuffix.org
- Licence: Mozilla Public License 2.0 (SPDX `MPL-2.0`).
- Copyright: no copyright line is given upstream.
- Where: compiled into `bin/nsfb` as a table (through libnspsl). The source form is the
  file `libnspsl/public_suffix_list.dat` of the NetSurf source archive named in 5.1, also
  available from https://publicsuffix.org/list/public_suffix_list.dat.
- Licence text: `LICENSES/MPL-2.0.txt`

### 5.8 Mozilla CA certificates (`ca.pem`)

See 3.8.

### 5.9 curl (libcurl)

- Version: 8.11.1.
- Upstream: https://curl.se
- Licence: curl licence (SPDX `curl`).
- Copyright: `Copyright (c) 1996 - 2024, Daniel Stenberg, <daniel@haxx.se>, and many
  contributors, see the THANKS file.`
- Where: `bin/nsfb` (built with Mbed TLS and zlib).
- Licence text: `LICENSES/curl-COPYING.txt`

### 5.10 zlib

- Version: 1.3.1.
- Upstream: https://zlib.net
- Licence: zlib licence (SPDX `Zlib`).
- Copyright: `(C) 1995-2022 Jean-loup Gailly and Mark Adler` (as written in the zlib 1.3.1
  `LICENSE` file).
- Where: `bin/nsfb`.
- Licence text: `LICENSES/zlib-LICENSE.txt`

### 5.11 libpng

- Version: 1.6.44.
- Upstream: http://www.libpng.org/pub/png/libpng.html
- Licence: PNG Reference Library License version 2 (SPDX `libpng-2.0`).
- Copyright: `Copyright (c) 1995-2024 The PNG Reference Library Authors.`
  `Copyright (c) 2018-2024 Cosmin Truta.`
  `Copyright (c) 2000-2002, 2004, 2006-2018 Glenn Randers-Pehrson.`
  `Copyright (c) 1996-1997 Andreas Dilger.`
  `Copyright (c) 1995-1996 Guy Eric Schalnat, Group 42, Inc.`
- Where: `bin/nsfb`.
- Licence text: `LICENSES/libpng-LICENSE.txt`

### 5.12 Expat

- Version: 2.6.4.
- Upstream: https://libexpat.github.io
- Licence: MIT (SPDX `MIT`).
- Copyright: `Copyright (c) 1998-2000 Thai Open Source Software Center Ltd and Clark Cooper`,
  `Copyright (c) 2001-2022 Expat maintainers`
- Where: built as a static library for the NetSurf build (`build_hf.sh`).
  **Uncertain**: no Expat text strings are present in the final stripped `bin/nsfb`, so it
  is probably not linked in at all; the notice is kept in case some of it is.
- Licence text: `LICENSES/expat-COPYING.txt`

## 6. Components in the patcher packages

Both patcher packages also contain a copy of the `firmware` files (sections 3.1, 3.2, 3.4
and 3.10 apply).

### 6.1 pycdlib

- Version: 1.22.0.
- Upstream: https://github.com/clalancette/pycdlib
- Licence: GNU Lesser General Public License, version 2.1 (SPDX `LGPL-2.1-only`, as
  declared in the package metadata).
- Copyright: `Copyright (C) 2015-2022  Chris Lalancette <clalancette@gmail.com>`
- Where: Python package: unmodified source files in `vendor/pycdlib/` (with
  `vendor/pycdlib-LICENSE.txt`). Windows program: the same modules, unmodified, as Python
  bytecode inside `ByOS-Patcher.exe`. The corresponding source code is the `pycdlib`
  1.22.0 release at the upstream address above (the package is also published on PyPI as
  `pycdlib` 1.22.0, which is where the build takes it from).
- Licence text: `LICENSES/pycdlib-COPYING.txt`

### 6.2 Python runtime (Windows program only)

- Version: CPython 3.14.7 for Windows, 64-bit (`python314.dll`, the standard library
  modules the patcher needs, and the extension modules `_bz2`, `_ctypes`, `_decimal`,
  `_hashlib`, `_lzma`, `_socket`, `_ssl`, `_uuid`, `_wmi`, `_zstd`, `select`,
  `unicodedata`).
- Upstream: https://www.python.org
- Licence: Python Software Foundation License Version 2 (SPDX `PSF-2.0`), together with
  the older BeOpen, CNRI and CWI licences reproduced in the same file.
- Copyright: `Copyright © 2001 Python Software Foundation. Copyright © 2000 BeOpen.com.
  Copyright © 1995-2001 CNRI. Copyright © 1991-1995 SMC.` (as written in `python314.dll`)
- Where: inside `ByOS-Patcher.exe`.
- Licence text: `LICENSES/Python-LICENSE.txt` (the `LICENSE.txt` of the Windows build),
  `LICENSES/Python-incorporated-software.rst` (the "History and License" page of the
  Python 3.14.7 documentation, with the notices of the software incorporated in Python).
- Software that the Python Windows build brings with it and that is inside
  `ByOS-Patcher.exe`:
  - OpenSSL 3.5.7 (`libssl-3.dll`, `libcrypto-3.dll`): Apache License 2.0 (SPDX
    `Apache-2.0`), `Copyright 1998-2026 The OpenSSL Authors. All rights reserved.`
    https://www.openssl.org . Text: `LICENSES/Apache-2.0.txt`, also in
    `LICENSES/Python-LICENSE.txt`.
  - libffi (`libffi-8.dll`): MIT, `Copyright (c) 1996-2022 Anthony Green, Red Hat, Inc and
    others.` https://github.com/libffi/libffi . Text in `LICENSES/Python-LICENSE.txt`.
  - bzip2 / libbzip2 1.0.8 (`_bz2`): bzip2 licence (SPDX `bzip2-1.0.6`),
    `copyright (C) 1996-2019 Julian R Seward`. Text in `LICENSES/Python-LICENSE.txt`.
  - Zstandard (`_zstd`): BSD 3-Clause, `Copyright (c) Meta Platforms, Inc. and affiliates.`
    Text in `LICENSES/Python-LICENSE.txt`; the notice of the Python bindings (pyzstd) is in
    `LICENSES/Python-incorporated-software.rst`.
  - libmpdec (`_decimal`): BSD 2-Clause, `Copyright (c) 2008-2020 Stefan Krah. All rights
    reserved.` Text in `LICENSES/Python-incorporated-software.rst`.
  - Unicode Character Database (`unicodedata`), mimalloc, and the other notices listed in
    `LICENSES/Python-incorporated-software.rst`.
  - **Uncertain / not verified**: the Windows build of Python also contains a zlib
    implementation, the XZ Utils library (liblzma, in `_lzma`) and the HACL\* hash
    functions. Their exact versions, and licence texts for liblzma and HACL\*, are not in
    the two Python licence files above and were not verified; they are therefore not
    reproduced here.

### 6.3 PyInstaller bootloader and run-time files (Windows program only)

- Version: PyInstaller 6.22.3.
- Upstream: https://pyinstaller.org
- Licence: GNU General Public License, version 2 or later, with the PyInstaller Bootloader
  Exception, which gives "unlimited permission to link or embed compiled bootloader and
  related files into combinations with other programs, and to distribute those
  combinations without any restriction coming from the use of those files" (SPDX
  `GPL-2.0-or-later WITH Bootloader-exception`). Run-time hooks: Apache License 2.0 (SPDX
  `Apache-2.0`).
- Copyright: `Copyright (c) 2010-2023, PyInstaller Development Team`,
  `Copyright (c) 2005-2009, Giovanni Bajo`,
  `Based on previous work under copyright (c) 2002 McMillan Enterprises, Inc.`
- Where: the start-up code of `ByOS-Patcher.exe` (bootloader `run.exe`) and the small
  loader modules inside it (`pyimod01_archive`, `pyimod02_importers`,
  `pyimod03_ctypes`, `pyimod04_pywin32`, `pyiboot01_bootstrap`, `pyi_rth_inspect`).
- Licence text: `LICENSES/PyInstaller-COPYING.txt` (contains the GPL and Apache texts),
  `LICENSES/GPL-2.0.txt`, `LICENSES/Apache-2.0.txt`
- Notes: the bootloader contains the decompression code of zlib 1.3.2
  (`inflate 1.3.2 Copyright 1995-2026 Mark Adler`), under the zlib licence (SPDX `Zlib`);
  text: `LICENSES/zlib-LICENSE.txt` (the file from zlib 1.3.1; the licence wording is the
  same, **uncertain** only in the copyright years).

### 6.4 Microsoft runtime libraries (Windows program only)

- What: `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll` (Microsoft Visual C++ runtime
  14.51.36247.0), `ucrtbase.dll` and the `api-ms-win-*.dll` forwarders (Universal C
  Runtime 10.0.26100.1742).
- Licence: proprietary Microsoft redistributable code; `© Microsoft Corporation. All
  rights reserved.` These files are not open source. The conditions Python passes on for
  the Microsoft code in its Windows build are in the section "Additional Conditions for
  this Windows binary build" of `LICENSES/Python-LICENSE.txt`.
- Where: inside `ByOS-Patcher.exe`, collected automatically by PyInstaller so that the
  program starts on systems without these libraries.
- Licence text: none reproduced (no licence file accompanies these DLLs).
- **Uncertain**: the Universal C Runtime files were taken by PyInstaller from another
  program installed on the build computer, not from a Microsoft redistributable package.

## 7. Components in the companion client (`byos-cli.jar` and the Kotlin client core)

`byos-cli.jar` is built from `android/hibyos-install/cli` and contains the client core
(`android/hibyos-install/core`), which the Android installer app also uses. The fat jar
carries a copy of this document and of the licence texts named below in
`META-INF/byos-licenses/`.

| Component | In `byos-cli.jar` | In the Android app (core) |
| --- | :-: | :-: |
| hiby-mods database rules, re-implemented (7.1) | yes | yes |
| TweetNaCl X25519 algorithm, re-implemented (7.2) | yes | yes |
| Kotlin standard library (7.3) | yes | (part of every Kotlin app; not covered by this table) |
| JetBrains annotations (7.4) | yes | |

### 7.1 hiby-mods (database updater rules)

- Upstream: https://github.com/hiby-modding/hiby-mods (`tools/Update_Database.py`),
  read at commit a5103f7c.
- Licence: MIT, scoped by its authors to the project's own tooling (SPDX `MIT`); the
  firmware binaries in that repository are not covered and nothing of them is used.
- Copyright: `Copyright (c) 2026 noisetta and contributors`
- Where: no code was copied. The library builder of the client core
  (`core/.../library/LibraryBuilder.kt`) re-implements, in Kotlin, rules learned from that
  script: rebuilding the stock `usrlocal_media.db` on a computer from a copy of the device's
  database, the `a:\` path form with NFC text and a trailing NUL, the stock format codes
  (`FORMAT_CODES`), the cover file names and the list of rebuilt tables. The notice is kept
  because the work is derived from those ideas.
- Licence text: `LICENSES/hiby-mods-LICENSE.txt`

### 7.2 TweetNaCl (X25519 algorithm)

- Upstream: https://tweetnacl.cr.yp.to
- Licence: public domain (see 3.9).
- Where: `core/.../btp/Crypto.kt` implements X25519 with the TweetNaCl field arithmetic,
  rewritten in Kotlin. Salsa20, HSalsa20 and Poly1305 there were written from their
  published specifications.
- Licence text: `LICENSES/TweetNaCl-public-domain.txt`

### 7.3 Kotlin standard library

- Version: 2.2.20 (`org.jetbrains.kotlin:kotlin-stdlib`).
- Upstream: https://github.com/JetBrains/kotlin
- Licence: Apache License 2.0 (SPDX `Apache-2.0`).
- Copyright: `Copyright 2010-2025 JetBrains s.r.o. and Kotlin Programming Language contributors.`
- Where: unmodified classes inside `byos-cli.jar` (`kotlin/`).
- **Uncertain**: the copyright line is the one in the upstream source headers; the jar itself
  carries no notice file to compare with.
- Licence text: `LICENSES/Apache-2.0.txt`

### 7.4 JetBrains annotations

- Version: 13.0 (`org.jetbrains:annotations`, a dependency of the Kotlin standard library).
- Upstream: https://github.com/JetBrains/java-annotations
- Licence: Apache License 2.0 (SPDX `Apache-2.0`).
- Copyright: `Copyright 2000-2013 JetBrains s.r.o.`
- Where: unmodified classes inside `byos-cli.jar` (`org/intellij/`, `org/jetbrains/annotations/`).
- **Uncertain**: the copyright line is the one in the upstream source headers, as for 7.3.
- Licence text: `LICENSES/Apache-2.0.txt`

## 8. Not included in any file set

- HiBy's firmware and applications: never distributed by ByOS.
- Tools used only to build (Zig/clang, Python packages such as Pillow, the NetSurf build
  tools apart from the generated code mentioned in 5.2): not distributed.
- The fonts installed on the player, which `hibyshell` loads at run time: not distributed.
