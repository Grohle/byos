# Licence texts of third-party components

This folder holds the full licence texts of the third-party software, fonts and data
contained in the files ByOS distributes. The list of components, with versions,
copyright notices and where each one ends up, is in
[`../THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md).

ByOS's own licence is the `LICENSE` file at the root of the repository; it is not in this
folder.

`tools/notices.py` copies `THIRD_PARTY_NOTICES.md` and the files each release needs:

    python tools/notices.py --list
    python tools/notices.py --check
    python tools/notices.py <artefact> <destination folder>

## Files

Unless stated otherwise each file is an exact copy of the upstream file named in the third
column (line endings normalised to LF).

| File | Applies to | Copied from |
| --- | --- | --- |
| `Apache-2.0.txt` | Mbed TLS, OpenSSL, PyInstaller run-time hooks, Kotlin standard library and JetBrains annotations (inside `byos-cli.jar`) | `/usr/share/common-licenses/Apache-2.0` (Ubuntu `base-files`) |
| `GPL-2.0.txt` | NetSurf, Mbed TLS (when linked into NetSurf), PyInstaller bootloader | `/usr/share/common-licenses/GPL-2` |
| `LGPL-2.0.txt` | talloc | `/usr/share/common-licenses/LGPL-2` |
| `LGPL-2.1.txt` | talloc, glibc pieces | `/usr/share/common-licenses/LGPL-2.1` |
| `MPL-2.0.txt` | Mozilla CA certificates, Public Suffix List | `/usr/share/common-licenses/MPL-2.0` |
| `MbedTLS-LICENSE.txt` | Mbed TLS 2.28.9 | `LICENSE` of the Mbed TLS 2.28.9 source archive |
| `stb-LICENSE.txt` | stb_image, stb_truetype, stb_image_write | licence block at the end of `stb_image.h` 2.30 (identical in the other two headers) |
| `SQLite-blessing.txt` | SQLite 3.50.4 | assembled: header comment of `sqlite3.c` |
| `IJG-libjpeg-README.txt` | IJG libjpeg 9f | `README` of `jpegsrc.v9f.tar.gz` |
| `musl-COPYRIGHT.txt` | musl libc 1.2.5 | `lib/libc/musl/COPYRIGHT` of Zig 0.17.1 |
| `Zig-LICENSE.txt` | Zig runtime (compiler-rt, C library functions) | `LICENSE` of the `ziglang` 0.17.1 package |
| `LLVM-LICENSE.txt` | parts of the Zig runtime ported from LLVM compiler-rt | `lib/libcxx/LICENSE.TXT` of Zig 0.17.1 (the LLVM project licence) |
| `glibc-nonshared-NOTICE.txt` | glibc pieces linked into `hibymod_disp.so` | assembled: header of `lib/libc/glibc/io/stat-2.32.c` of Zig 0.17.1 |
| `glibc-LICENSES.txt` | glibc pieces linked into `hibymod_disp.so` | `lib/libc/glibc/LICENSES` of Zig 0.17.1 |
| `TweetNaCl-public-domain.txt` | Ed25519 code derived from TweetNaCl; the X25519 code of the Kotlin client core | assembled: statement on https://tweetnacl.cr.yp.to |
| `OFL-1.1-Inter.txt` | Inter typeface (rendered glyphs) | `theme/fonts/Inter-LICENSE.txt` (Inter 4.000) |
| `OFL-1.1-Terminus.txt` | Terminus Font (Hello app) | assembled: Terminus section of `copyright.fonts` in Ubuntu `console-setup-linux` 1.226ubuntu1 |
| `NetSurf-COPYING.txt` | NetSurf 3.11 | `netsurf/COPYING` of `netsurf-all-3.11` |
| `NetSurf-libraries-COPYING.txt` | NetSurf project libraries, built-in font | assembled: the `COPYING` file of each library in `netsurf-all-3.11`, and the header of `glyph_data` |
| `utf8proc-LICENSE.md` | utf8proc 2.4.0-1 and its Unicode data | `libutf8proc/LICENSE.md` of `netsurf-all-3.11` |
| `talloc-NOTICE.txt` | talloc inside NetSurf | assembled: header of `netsurf/utils/talloc.c` |
| `punycode-RFC3492.txt` | Punycode sample code inside NetSurf | assembled: header of `netsurf/utils/punycode.c` and RFC 3492, Appendix B |
| `curl-COPYING.txt` | curl 8.11.1 | `COPYING` of the curl 8.11.1 source archive |
| `zlib-LICENSE.txt` | zlib 1.3.1 (Browser app); zlib in the PyInstaller bootloader | `LICENSE` of the zlib 1.3.1 source archive |
| `libpng-LICENSE.txt` | libpng 1.6.44 | `LICENSE` of the libpng 1.6.44 source archive |
| `expat-COPYING.txt` | Expat 2.6.4 | `COPYING` of the Expat 2.6.4 source archive |
| `pycdlib-COPYING.txt` | pycdlib 1.22.0 | `COPYING` in the `pycdlib` 1.22.0 package (the LGPL 2.1 text) |
| `Python-LICENSE.txt` | Python 3.14.7 runtime and the libraries bundled with its Windows build | `LICENSE.txt` of the Python 3.14.7 Windows installation |
| `Python-incorporated-software.rst` | software incorporated in Python 3.14.7 | `Doc/license.rst` of CPython, tag `v3.14.7` |
| `PyInstaller-COPYING.txt` | PyInstaller 6.22.3 bootloader and run-time files | `COPYING.txt` in the `pyinstaller` 6.22.3 package |
| `hiby-mods-LICENSE.txt` | hiby-mods database-updater rules re-implemented in the client core's library builder | `LICENSE` of hiby-modding/hiby-mods, commit a5103f7c |

"Assembled" files start with a few lines that say where their content was taken from; the
quoted text itself is unchanged.
