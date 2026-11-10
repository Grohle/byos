# HiByOS patcher

HiByOS cannot redistribute HiBy's firmware. The patcher is a small program that users run
on their own computer to turn **their own copy** of the official HiBy R3 II (2025) firmware
v1.3 (`r3ii_2025.upt`, md5 `0d16d09c1458dd2f2cdaf38d0047917a`) into the HiByOS firmware.
It ships only HiByOS files (the `overlay/` folder) and the list of changes. No file of the
official firmware is inside it, and no text of one: the few official files that HiByOS edits
(two start-up scripts and five screen layouts) are described as a list of edits that the
patcher applies to the copy in the user's own firmware (see "Edits to HiBy's own files").

It needs no external tools, no WSL and no administrator/root rights: it is pure Python
(plus the pure-Python `pycdlib` library), packaged as a single `HiByOS-Patcher.exe` for
Windows. The same code runs with `python3` on macOS and Linux.

## Using it

- Windows: drag `r3ii_2025.upt` onto `HiByOS-Patcher.exe`, or double-click the .exe and
  drop the file into the window, or run `HiByOS-Patcher.exe C:\path\to\r3ii_2025.upt`.
- macOS/Linux: unzip `HiByOS-Patcher-<version>-python.zip` and run
  `python3 hibyos_patcher.py /path/to/r3ii_2025.upt` (pycdlib is bundled in `vendor/`).

The result is `HiByOS-<version>/r3ii_2025.upt`, next to the input file. The input is never
modified. Any other input file is refused (md5 check) and nothing is written.
Options: `--out-dir DIR` (another output folder), `--no-pause` (Windows: do not wait for
Enter at the end).

## How it works

`tools/hibyos_patcher.py`, using `tools/build.py` (the `.upt` code and the patch lists are
shared, not copied), `tools/squashfs.py` and `tools/lzo1x.py`:

1. Checks the md5 of the whole input file.
2. Opens the `.upt` (an ISO 9660 image) and rebuilds the kernel (`xImage`) and root file
   system (`rootfs.squashfs`) images from their 512 KiB chunks, checking every chunk's md5
   chain and the `img_md5`/`img_size` values of `ota_update.in` (same code as `build.py`).
3. Reads the SquashFS 4.0 (LZO) root file system in memory: superblock, id table,
   fragment table, inode and directory tables. Hard links are kept as shared inodes.
4. Applies exactly what `build.py` does:
   - the `overlay/` files (CRLF converted to LF for scripts, `.sh`, `.json`, `.ini`,
     `.view`); new files are 0755 if they start with `#!` or are ELF, else 0644; a
     replaced file keeps its original mode and owner; new folders are 0755 root:root;
     the overlay may replace an official file only when it is an image (our own artwork
     under HiBy's file name); any other official file in `overlay/` aborts the build;
   - the edits of `tools/vendor_edits.json`: each listed official file is checked (size and
     SHA-256), rebuilt with our changes, checked again (size and SHA-256 of the result) and
     put in place with the mode and owner of the official file. If a file is not the
     expected one the patcher stops and writes nothing;
   - `/etc/hibymod_build` with a random build id;
   - `TEXT_PATCHES` and `BIN_PATCHES` from `build.py`, with the same checks (text found
     exactly once, md5 and original bytes of binary files);
   - ownership as `build.py` ends up producing it (it unpacks in a user namespace and
     restores owners with a mksquashfs pseudo file): symlinks and `/` are root:root,
     symlinks are 0777, every other original entry keeps its owner and mode.
5. Writes a new SquashFS image (`tools/squashfs.py`), mksquashfs layout (data, inode table,
   directory table, fragment table, NFS export table, id table; no xattrs; 128 KiB blocks):
   - the compressed data blocks and fragment blocks of untouched files are **copied byte
     for byte** from HiBy's image (only relocated); HiBy's data is never recompressed;
   - new and patched files are compressed with a pure-Python LZO1X-1 compressor
     (`tools/lzo1x.py`); a block is stored uncompressed when compression does not help,
     as mksquashfs does. Identical new files share their data. Files smaller than a block
     go into new fragment blocks;
   - metadata blocks (8 KiB) are LZO compressed too.
6. Checks the size against the rootfs partition limit used by `build.py`
   (95 % of 0x2D00000 bytes).
7. Verifies the new image by reading it back: every entry's type, mode, owner, link target
   and hard links, and the full content of every file (all of HiByOS and HiBy's files are
   decompressed and compared by SHA-256 with the intended content).
8. Writes the `.upt` exactly like `build.py` (same chunking, `ota_update.in` changes only
   in the rootfs `img_md5`/`img_size`), to a temporary file, re-reads it like the recovery
   does (md5 chains, sizes, kernel unchanged, rootfs identical to the one built) and only
   then renames it to `r3ii_2025.upt`.

Taking about 10 seconds, it is faster than the Linux build because nothing of HiBy's
data is decompressed and recompressed except for the files being patched (and the
verification pass).

## Edits to HiBy's own files (`tools/vendor_edits.json`)

Seven files of the official firmware are changed in a way that the simple text patches of
`build.py` cannot express: `etc/init.d/S11jpeg_display_shell`, `usr/bin/hiby_player.sh`,
the status bar (`usr/resource/layout/theme2|theme3/topbar/topbar.view`) and the About
screen (`usr/resource/layout/theme2|theme3/hiby_about_dev.view`,
`usr/resource/litegui/theme3/hiby_about_dev.view`). The repository and the patcher do not
contain those files. `tools/vendor_edits.json` holds, for each one, how to rebuild the
HiByOS version from the official copy, storing only the text written for HiByOS.

Format (version 1; `tools/vendor_edits.py` is the reference implementation and its
docstring repeats this):

```
{"format": 1,
 "files": [
   {"path": "usr/bin/hiby_player.sh",          path in the root file system, no leading "/"
    "stock":  {"size": N, "sha256": "..."},    the official file, as found in the firmware
    "strip_cr": false,                         true: delete every 0x0D byte of the official
                                               file BEFORE applying the ops
    "result": {"size": N, "sha256": "..."},    the file HiByOS installs
    "ops": [ {"copy": [offset, length]}        bytes of the official file (after strip_cr)
           | {"add": ["line", "line", ...]}    our text: the lines joined with one
                                               line feed (0x0A) between them, UTF-8
           , ... ],
    "previous": [ {"result": {...}, "ops": [...]}, ... ]     optional
   }, ...]}
```

To apply an entry:

1. read the official file; its size and SHA-256 must be `stock`, otherwise stop (unknown
   firmware, nothing is written);
2. base = those bytes, minus every 0x0D byte when `strip_cr` is true;
3. result = the concatenation, in order, of `base[offset : offset + length]` for each
   `copy` (0 <= offset, length > 0, offset + length <= len(base)) and, for each `add`, of the UTF-8 bytes of
   its lines joined with one line feed (0x0A) between them (no line feed after the last
   one; an empty last string therefore means "the text ends with a line feed");
4. the size and SHA-256 of the result must be `result`, otherwise stop;
5. the result replaces the file; the mode and owner of the official file are kept.

`previous` lists older HiByOS versions of the same file (same base, same kind of ops). The
firmware builders ignore it; `tools/make_payload.py` uses it so that a new SD payload can
also update a player whose firmware carries one of those older versions (docs/INSTALLER.md).

Changing one of these files (you need your own unpacked copy of the official root file
system, outside the repository):

```
python tools/vendor_edits.py export --stock ROOTFS     # full files -> out/vendor_files/ (not tracked by git)
#   edit the files in out/vendor_files/
python tools/vendor_edits.py update --stock ROOTFS     # rebuild tools/vendor_edits.json from them
python tools/vendor_edits.py check  --stock ROOTFS     # verify every entry, show the numbers
```

`update --keep-previous` keeps the version being replaced in `previous` (use it when a
released firmware carried it); `update --add PATH` starts editing another official file.
Never commit the exported files: they contain HiBy's text. `check` also prints, per file,
how many bytes are copied from the official file, how many are ours, and the longest run
of stored text that also exists in the official file (15 bytes at most today: short
coincidences such as a path or a JSON key).

A port of the patcher (the Android app) needs `vendor_edits.json`, the five steps above and
the patch lists of `build.py`; it does not need any of the official files.

## Differences from the `build.py` image

The file tree is identical (same files, types, modes, owners, symlinks, hard links and
content; only the random build id differs). The image bytes differ: `build.py` recompresses
everything with mksquashfs (LZO1X-999), the patcher keeps HiBy's original compressed
blocks and uses LZO1X-1 for the new data. The patcher image is about 140 KB bigger
(for 0.6.1: 44,130,304 bytes, 98.4 % of the limit, vs 43,986,944 bytes, 98.1 %).
Data of files replaced by the overlay that lived in a shared fragment block stays in that
block (the block is still used by other files).

## Verification done for 0.6.1

- Reference image built with `tools/build_hibyos.sh` (WSL) and patcher image both
  extracted with `unsquashfs` (liblzo2): zero errors, same 4,830 entries, same type,
  mode, uid/gid, size and symlink target for each, same SHA-256 for all 4,095 files
  (except `/etc/hibymod_build`), same 208 hard link groups. `unsquashfs -s` reports the
  usual LZO, 128 KiB, exportable, no xattrs image.
- Both images mounted read-only with the Linux kernel squashfs driver (WSL2, Linux 6.6):
  identical `find` listing (type, mode, owner, link count, size, link target), identical
  `diff -r` and SHA-256 of every file read through the kernel; no kernel errors.
- `.upt` container: chunk md5 chains, `img_md5`/`img_size`, kernel image identical to
  HiBy's, `ota_config.in`/`ota_v0.ok` identical to the official ones.
- `tools/lzo1x.py` cross-checked against the real liblzo2 on ~3,000 inputs: liblzo2's
  `lzo1x_decompress_safe` decodes all of our streams, and we decode all of liblzo2's
  `lzo1x_1_compress` streams. The decompressor also unpacks all 84 MB of HiBy's rootfs.
- The Windows .exe, Windows Python 3.14 and Linux Python 3.12 (from the python zip)
  produce byte-identical `.upt` files for the same build id (`--build-id`, hidden test
  option) and `SOURCE_DATE_EPOCH` (fixes the ISO 9660 dates).

- After the seven edited official files were removed from `overlay/` and replaced by
  `tools/vendor_edits.json`: each file rebuilt from the official firmware is byte-identical
  to the copy that used to be in `overlay/`, and the patcher produces a byte-identical
  `.upt` (same md5) for the same build id and `SOURCE_DATE_EPOCH`, from the sources, from
  the python zip and from the Windows .exe.

To repeat the comparison after changing the overlay or the patches, build both images and
compare their `unsquashfs -lln` listings and extracted files as above.

## Building the patcher

```
python tools/build_patcher.py [--out DIR]
```

Creates (in `out/patcher/`, not tracked by git) a private venv with PyInstaller and
pycdlib if they are missing, then writes to `out/patcher/` (or `DIR`):

- `HiByOS-Patcher.exe` (Windows only) and `HiByOS-Patcher-<version>-windows.zip`
  (the .exe, `README.txt` and `licenses/`);
- `HiByOS-Patcher-<version>-python.zip`: the patcher modules (`hibyos_patcher.py`,
  `build.py`, `vendor_edits.py`, `squashfs.py`, `lzo1x.py`), `vendor_edits.json`,
  `overlay/`, pycdlib in `vendor/` (LGPL-2.1, license included), a README and `licenses/`.

`licenses/` holds `THIRD_PARTY_NOTICES.md` and the licence texts of the third-party
components inside that package (`tools/notices.py` decides which; the build stops if one
is missing). Publish the two zips, not the bare `HiByOS-Patcher.exe`: a single .exe cannot
carry the notices next to it.

`<version>` comes from `overlay/usr/bin/hibymod` (`HiByOS 0.6.1` -> `0.6.1`).
