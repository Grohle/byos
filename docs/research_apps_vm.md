# Research: HiBy R3 II emulation harness + browser + Telegram

Date: 2026-11-09. The real device was not touched (no adb). Everything was measured under emulation.

## 0. Executive summary

| Question | Verdict |
|---|---|
| Is there a reproducible harness? | **Yes.** `qemu-system-mipsel` (malta, MIPS32r2 LE, no FPU) with a custom kernel. It provides a real 320x480 16 bpp `/dev/fb0` (virtual 320x960, panning) and all 5 `/dev/input/eventN` devices with the same names and the same event format as the HiBy. `MemTotal` = 56928 kB (the real HiBy: 56936). Boots in about 4 s. |
| Browser | **Feasible with the player stopped and for lightweight pages.** I ported NetSurf 3.11 as a static mipsel/musl binary with a custom backend for fb0, touch and keys. It works in the harness: tap, drag-to-scroll, keys and on-screen keyboard. RSS: 4–7 MB on light pages and about 20 MB on an average Wikipedia article. Large pages (over 1 MB of HTML) end in OOM. With the player running (about 10 MB free) only very light pages work, and there is thrashing. |
| Lighter alternative | Text or reader mode: `links2 -dump`/`w3m` use 3–7 MB even with the huge article. The most robust option is a **reader proxy on the PC plus a minimal native viewer** (estimated at under 2 MB). |
| Telegram on the device (TDLib) | **Not feasible.** On x86_64, TDLib 1.8.66 takes about 29 MB of RSS just to reach "waiting for phone number", with no account data. In addition there is no mipsel package, and building it needs on the order of 1 GB of RAM per source file. |
| Recommended Telegram | **Thin client on the HiBy plus a relay on the PC/RPi running TDLib** (option b). For notifications a bot is enough (option c). |
| Main blocker (not an emulation issue) | The stock player owns the framebuffer and there is no clean way to launch external apps from its UI. A key-combo daemon or a binary patch is needed, plus audio coordination (see §6). |

---

## 1. The harness

### 1.1 Location

- Harness: `~/hiby/vm` (on Windows: `\\wsl.localhost\Ubuntu\home\alber\hiby\vm`).
- Copy of sources and scripts: `C:\Users\alber\Downloads\Hiby\hibymod\apps\harness_vm\`.

Contents of `~/hiby/vm`:

| Path | What it is |
|---|---|
| `run_vm.sh` | Launches the VM. |
| `hibyctl.py` | Host client: tap, swipe, key, shot, sh. |
| `out/vmlinux-hiby` | Custom 6.1.192 MIPS32r2 LE kernel, configuration in `out/kernel.config`. |
| `rootfs.ext4` | Debian 12 mipsel rootfs with netsurf-fb, links2, w3m and busybox, plus the `/hiby/*` tools. |
| `share/` | 9p folder mounted in the guest as `/mnt/share`. New binaries are dropped there without regenerating the image. |
| `tools/*.c` | `hibyinput` (input and capture daemon), `evdump`, `memhog`, `fbtouch` (test app and template), `nsfb_hiby.c` (NetSurf backend). |
| `scripts/` | Scripts to rebuild everything, plus the test `.cmds` files. `vmexp.sh` runs a complete experiment. |
| `qemu/`, `xtc/` | QEMU 8.2 and cross-gcc 13 extracted with `apt download` + `dpkg -x`, without root. |
| `xsysroot/` | Static mipsel dependencies: zlib, mbedtls 2.28, curl 8.11, libpng, libjpeg 9f, expat. |

### 1.2 Usage

```bash
# in WSL
cd ~/hiby/vm
./run_vm.sh --profile coexist        # or: solo | none ; --icount ; --interactive (serial console on stdio)
python3 hibyctl.py fbinfo
python3 hibyctl.py tap 160 240
python3 hibyctl.py swipe 160 400 160 100 400
python3 hibyctl.py key VOLUMEUP       # POWER, VOLUMEDOWN, PLAYPAUSE, NEXTSONG, PREVIOUSSONG or a numeric code
python3 hibyctl.py shot captura.png   # VISIBLE area of fb0 (honors yoffset)
python3 hibyctl.py sh 'free; ps'
# full unattended experiment:
bash scripts/vmexp.sh logs/x.log solo scripts/nsfb.cmds
```

To test a new app:

1. Build it with `~/hiby/tools/zbuild.sh`.
2. Copy it to `~/hiby/vm/share/`.
3. Run it in the guest as `/mnt/share/app`. If `share/autorun.sh` exists, it runs automatically at boot.

To measure: `sh /hiby/measure.sh SEGUNDOS cmd...` (`SEGUNDOS` = seconds). It samples VmRSS every 0.5 s and at the end reports VmHWM, the minimum MemAvailable and whether the OOM killer stepped in.

Rebuild from scratch, all without root, in this order:

1. `scripts/getqemu.sh`
2. `aptsetup.sh` and `aptdl.sh` (Debian mipsel)
3. `getx.sh` (cross-gcc)
4. `build_kernel.sh`
5. `build_tools.sh`
6. `mkrootfs.sh`
7. `build_deps.sh`, `host_png.sh`, `getpkgconf.sh`, `getgperf.sh`
8. `build_netsurf.sh`
9. `pkg_netsurf.sh`

### 1.3 What is faithful and what is not

| Aspect | Fidelity | Detail |
|---|---|---|
| ISA | High | MIPS32r2 little endian, `24KEc` CPU without FPU, so soft-float must be used as on the HiBy. XBurst-specific instructions (MXU/SIMD) do not exist in QEMU. Debian binaries (hard-float) work through the kernel's FPU emulation, which makes them slower than real. |
| Kernel | Medium | 6.1.192, not 4.4.94. The syscall, fbdev and evdev ABIs are compatible for static musl binaries. The Ingenic drivers are missing: audio, real LCD, power management. |
| Framebuffer | High | Patched `vfb`. Resolution 320x480, 16 bpp RGB565 (R=11, G=5, B=0), virtual 320x960, `FBIOPAN_DISPLAY` and `mmap` work. It does **not** emulate the ~44 Hz limit or `FBIO_WAITFORVSYNC`. There is no fbcon either. The `id` is "hiby-vfb", not the real driver's. |
| Input | High | `uinput` devices with the exact names and order (`event0` md-gpio-keys, `event1` goodix-ts, `event2` jz adc keyboard, `event3` sa-ring-keys, `event4` earpods_adc). Touch uses the identical protocol A, verified with `evdump`: BTN_TOUCH, ABS_MT_POSITION_X/Y, TOUCH_MAJOR=22, WIDTH_MAJOR=22, TRACKING_ID=0, SYN_MT_REPORT, SYN_REPORT. On release: BTN_TOUCH 0 + SYN_MT_REPORT + SYN_REPORT (configurable). Key codes live in `/etc/hibyinput.conf`, because the real ones for event2/3/4 are **unverified**. |
| Total RAM | High | `mem=66248K` leaves MemTotal at 56928 kB (real: 56936). No swap. A real OOM killer acts just like on the device. |
| Available RAM | Medium | There are two profiles based on the budgets you gave. `coexist` uses `memhog`, which pins memory with mlock until about 10 MB of MemAvailable is left, simulating the running player. `solo` leaves about 38 MB. It is an approximation: the real player has evictable code pages, shares libs and keeps audio buffers. |
| CPU / timing | Low–medium | In normal mode (TCG) the speed does not represent the XBurst. With `--icount` the guest clock advances at 1 instruction = 1 ns (1 GIPS, IPC=1). It serves as an optimistic bound: the real XBurst, with slower caches and memory, will have IPC below 1, perhaps 1.5–3 times slower. Networking goes through slirp, so its latency is the PC's. |
| Network | Medium | virtio-net + slirp (NAT to the PC), not WiFi. It does not reproduce the latency or loss of a real WiFi link. |
| Audio | None | There is no audio. Coexistence with playback cannot be measured: dropouts, CPU stolen from the decoder. |

---

## 2. Browser

### 2.1 Candidates evaluated

1. **NetSurf 3.10 from Debian** (`netsurf-fb`, glibc, dynamic, 82 libs, JS via duktape). It only ships the ram, sdl, x, vnc and wld surfaces. **3.11 no longer has a `linux` surface** (direct fbdev), so it has to be written. I measured it with the `ram` surface as a reference.
2. **Ported NetSurf 3.11 (mine).** Static, musl, `-Os`, no JS, built-in bitmap font (no freetype), PNG/JPEG/GIF/BMP, curl + mbedtls. Built with zig cc.
   - Binary: **4.05 MB** in `apps/netsurf-hiby/nsfb`; with resources, 4.7 MB.
   - Custom backend `nsfb_hiby.c`, registered as the `linux` surface, which does the following:
     - draws directly into the fb0 `mmap` (yoffset 0);
     - reads `event1`: a short tap is a click, and a drag of more than 10 px scrolls with the wheel (every 16 px);
     - reads the keys: VOL+ and VOL- are Page Up/Page Down, PREV and NEXT move left and right, PLAY is Escape and POWER quits.
   - `Choices`: `memory_cache_size` 1 MB, no disk cache, on-screen keyboard enabled, JS disabled.
3. **links2 / w3m from Debian** in `-dump` mode, as a measure of the cost of a text-only engine.

### 2.2 Measured results (MIPS, in the harness)

Peak RSS = the process's VmHWM. "anon" is the real private memory; "file" is code pages, which are evictable.

| Page (HTML) | Static NetSurf 3.11, solo profile (38 MB) | Static NetSurf 3.11, coexist profile (10 MB) | NetSurf 3.10 Debian (solo) | links2 -dump (solo) |
|---|---|---|---|---|
| example.com (1 KB) | **4.1 MB** (anon 0.9) | — | 20.1 MB (anon 5.1 / file 15) | — |
| lite.cnn.com | **6.7 MB** (anon 3.1) | 9.1 MB; code evicted down to 0.9 MB, minimum MemAvailable 1.2 MB, **thrashing**: opening a link took 30 s | 23.5 MB (anon 7.3) | about 4.6 MB* |
| text.npr.org | **5.3 MB** (anon 1.7) | 4.4 MB** | 23.1 MB (anon 6.2) | — |
| Wikipedia "Ingenic Semiconductor" (220 KB) | **20.0 MB** (anon 12.8) | 20.4 MB; **the OOM killer killed the "player" (memhog)** | — | about 4.6 MB*; w3m about 3.1 MB* |
| Wikipedia "Telegram (software)" (1.39 MB) | **OOM** (anon at about 39 MB in under 15 s) | OOM | **OOM** (anon 39 MB) | about 7.0 MB*, complete |

\* links2 and w3m exit before VmHWM can be read. The figure given is the drop in MemAvailable during the run, which also includes page cache. It is approximate.

\*\* In that test the previous instance (cnn) was still alive, so the figure is contaminated.

Other observations:

- **Startup**, with `--icount` at 1 GIPS (optimistic estimate of the device):
  - `example.com` over HTTP was drawn about 10 s after launch, judging by when the RSS stabilizes;
  - `lite.cnn.com` over HTTPS took about 45 s, due to the TLS handshake with soft-float mbedtls, the load and the sub-requests;
  - on the real device expect between 10 and 60 s per page depending on size and TLS.
- **Usability at 320x480 with touch only** (screenshots in `docs/capturas/vm/`):
  - The NetSurf toolbar (back, forward, history, stop, reload, URL) fits in 28 px and the text is readable.
  - Drag scrolling works (`c_cnn2_scrolled.png`), as does VOL- to page down.
  - Tapping a link navigates (`c_cnn4_link.png`).
- **On-screen keyboard:**
  - It is needed to type URLs or search, and NetSurf already ships one ("a" button at the bottom right, `r3_osk_key.png`).
  - Its keys are about 23x17 px on a 320 px width, smaller than a fingertip contact (TOUCH_MAJOR 22). Usable but error-prone.
  - I recommend a custom, larger keyboard, or using bookmarks and a start page with links to avoid typing.
- Wikipedia redirects `en.m.` to the desktop version and once returned "429 Too many requests" to the NetSurf User-Agent.

### 2.3 Browser verdict

- **With the player stopped (about 35–40 MB):** static NetSurf is feasible for light websites (lite and text versions of news sites, blogs, docs) and medium Wikipedia articles, at 4–20 MB. It is not feasible for large or modern pages with a heavy DOM.
  - An **OOM safeguard** must be added: `setrlimit(RLIMIT_AS/DATA)` of about 24 MB in the launcher and `oom_score_adj=1000` on the browser, so the kernel kills the browser and not the player or other processes.
- **With the player running (about 8–12 MB):** only minimal pages, and with code thrashing because there is no swap. With a Wikipedia article the OOM killer chose to kill the process acting as the player. **Not recommended without limiting memory.**
- **Recommended lighter alternative:** a **"reader mode" with a proxy on the PC or RPi**. The PC downloads the page, simplifies it (readability, strip JS/CSS, images to 320 px as JPEG or RGB565) and serves it paginated. On the HiBy, a minimal native viewer (static C with a bitmap font, like `fbtouch`) draws the text, estimated at under 2 MB. It is the only comfortable option that coexists with music playback.
- An intermediate option, without writing a viewer: NetSurf pointed at that proxy, at about 5 MB per page.

---

## 3. Telegram

### 3.1 Data

Measured (TDLib 1.8.66, prebuilt `libtdjson.so` from the PyPI package `python-telegram`, x86_64, script `apps/harness_vm/scripts/tdmem.py`):

- Loading the library adds **6.4 MB** of RSS; the `.so` is 41.5 MB on disk.
- From `setTdlibParameters` (test DC, no account, databases enabled) up to `authorizationStateWaitPhoneNumber` adds **+22.4 MB**. In total that is **about 29 MB of RSS with no chat loaded**, with 5 threads.
- `MALLOC_ARENA_MAX=1` changes nothing, so it is not an effect of glibc arenas.
- Inference: on MIPS32 pointers are 4 bytes, which might give 60–70 % of that figure, about 18–20 MB as a baseline. On top of that comes the in-RAM cache of users and chats, which according to the maintainers is not freed without closing the instance.

Sources (checked on the web):

- **Memory:** the maintainer says that "about 10 MB per new instance is acceptable" and that memory grows by about 180 KB/h while idle ([td#2807](https://github.com/tdlib/td/issues/2807)). Disabling the databases **increases** RAM usage ([td#2516](https://github.com/tdlib/td/issues/2516), [td#1704](https://github.com/tdlib/td/issues/1704)). The cache is only emptied by closing the instance ([td#2573](https://github.com/tdlib/td/issues/2573)).
- **Options:** `message_unload_delay`, `ignore_background_updates`, `disable_*` ([core.telegram.org/tdlib/options](https://core.telegram.org/tdlib/options)).
- **Build:** C++17, OpenSSL, zlib, gperf and CMake; up to about 1 GB of RAM per source file with GCC ([README](https://github.com/tdlib/td)). It must be cross-compiled from the PC.
- **Packages:** Debian does not publish libtdjson for mipsel; the installed armhf package takes 19.7 MB ([packages.debian.org](https://packages.debian.org/sid/libtdjson1.8.38)).

Lightweight MTProto alternatives:

- **vysheng/tg, tgl, telegram-purple:** dead. They use an old layer; since 2021 Telegram rejects old layers with `UPDATE_APP_TO_LOGIN` (64-bit IDs) and users report "outdated app" on login ([kenorb-contrib/tg#64](https://github.com/kenorb-contrib/tg/issues/64)). Maintaining a custom C client that keeps up with the layers is not feasible (the current layer is 225).
- **gotd/td (Go):** claims about 150 KB per idle client, and Go supports mipsle soft-float. A static binary would be around 15–30 MB, with an estimated RSS of about 10–15 MB (not measured). It is the only full-client-on-device route with any chance, and only with the player stopped.
- **grammers (Rust):** mipsel is a tier 3 target; no memory data.

### 3.2 Terms and security

- **Your own api_id/api_hash** are mandatory (my.telegram.org; one api_id per phone number) ([obtaining_api_id](https://core.telegram.org/api/obtaining_api_id), [API ToS](https://core.telegram.org/api/terms)).
  - The app name may not contain "Telegram" unless preceded by "Unofficial", and the logo may not be used.
  - "Ghost mode" style features and breaking self-destructing messages are not allowed.
- **Monitoring of unofficial clients:** accounts using unofficial clients "are placed under observation", and there are reports of bans with new api_ids. I recommend using an old account, normal personal use and no aggressive automation.
- **Security:**
  - A full client on the HiBy stores the session *auth key* in unencrypted flash, on a device that is easy to lose and has open root. Losing it means losing the session until it is revoked from another device.
  - With a relay, the auth key stays on the PC and the HiBy only holds a revocable token.
- **Bot API:** a bot cannot read your private chats, only the ones it participates in, and it cannot start conversations. Limit of about 30 messages/s and 1 per chat per second. A self-hostable `telegram-bot-api` server exists ([core.telegram.org/bots](https://core.telegram.org/bots), [faq](https://core.telegram.org/bots/faq)).

### 3.3 Architectures

**(a) Full client on the device: not feasible.**
- With TDLib: more than 20 MB baseline, plus the cache, plus TLS and the UI. It does not fit alongside the player and, with the player stopped, it would be at the limit with no headroom.
- With gotd/td: possible only with the player stopped, and still to be verified.

**(b) Thin client plus relay: recommended.** Outline:

```
[HiBy] hgram (static C, ~1-2 MB RSS, fb0+touch UI, bitmap font)
   │  TLS (mbedtls, pinned relay cert) or TCP on LAN + HMAC-PSK
   ▼
[PC/RPi] hgram-relay (Python + TDLib or Telethon/Pyrogram) ── MTProto ──► Telegram
```

- **Transport:** a single persistent TCP connection. Frames are single-line JSON, 64 KB maximum.
- **Pairing:** the relay shows a 6-digit code. The HiBy sends it and receives a 32-byte `device_token`, revocable from the relay. After that, each session does `hello{device_token, nonce}` and the relay replies with a `challenge` signed with HMAC-SHA256.
- **Requests from the HiBy to the relay:**
  - `chats{offset,limit≤20}`
  - `history{chat_id, from_msg_id, limit≤20}`
  - `send{chat_id, text, reply_to?}`
  - `read{chat_id, max_msg_id}`
  - `media{msg_id, kind:"thumb320"}`: the relay transcodes to JPEG at most 320 px wide, or to raw RGB565.
  - `voice{msg_id}`: the relay converts to MP3 or PCM so the HiBy can play it.
- **Events from the relay to the HiBy:** `new_msg{chat_id,msg_id,from,text,date}`, `edit`, `delete`, `typing`, `unread{chat_id,count}`.
- **Robustness:** the relay keeps the queue and the state. The HiBy only caches in RAM the last N messages of the open chat, and if WiFi drops it requests `history` again on reconnect.
- **Cost on the HiBy:** socket, TLS and text UI estimated at under 2–3 MB, so it **fits with the player running**. Text input needs a large on-screen keyboard or predefined quick replies.
- **Cost on the PC/RPi:** TDLib uses about 30–100 MB depending on the account, which is irrelevant there.
- **Shortcut without writing the relay:** bitlbee + tdlib-purple exposes your account as IRC, and on the HiBy a minimal IRC client would be enough. But tdlib-purple was archived in August 2026, so I do not recommend it long-term.

**(c) Bot API for notifications.**
- The simplest and safest option, but it only covers notifications.
- A script on the PC acts as a user-bot (with Telethon or TDLib) or as a forwarder. Optionally, the HiBy itself polls `getUpdates` of its own bot over HTTPS (curl + mbedtls already built) and shows a banner or plays an alert sound.
- It does not allow reading the real private chats without the relay from (b).

---

## 4. Recommendation

1. **Browser:** use the static NetSurf port already built (`apps/netsurf-hiby/`) for "player stopped" mode, with a launcher that does the following:
   1. stops or pauses the player and frees fb0;
   2. applies `ulimit -v` of about 24–28 MB and `oom_score_adj=1000`;
   3. starts `run.sh` with a start page of lightweight bookmarks (lite and text versions);
   4. restores the player when exiting with POWER.

   In the medium term, a **reader proxy on the PC plus a native viewer** to be able to browse with music playing.
2. **Telegram:** option **(b) relay**. First the Python relay with TDLib on the PC or RPi; then the `hgram` client in static C, reusing the `fbtouch` pattern and mbedtls. Option (c) as a trivial first step for notifications.
3. **Next measurement step on real hardware** (when the HiBy is free):
   - confirm the event2, event3 and event4 codes with `evdump`;
   - measure `free` with the player playing;
   - measure the real load time of `lite.cnn.com`;
   - check that `FBIOPAN_DISPLAY` and the RGB565 format match;
   - above all, whether the player tolerates another app writing to fb0.

## 5. Deliverables

- `C:\Users\alber\Downloads\Hiby\hibymod\apps\netsurf-hiby\`: `nsfb` (static mipsel, 4.05 MB), `res/`, `Choices`, `run.sh`.
- `C:\Users\alber\Downloads\Hiby\hibymod\apps\fbtouch\`: test app and template (double-buffered fb0 plus touch and keys).
- `C:\Users\alber\Downloads\Hiby\hibymod\apps\harness_vm\`: harness sources and scripts, plus `config-6.1.192-hiby` and the logs of all measurements (`logs_mediciones/`).
- `C:\Users\alber\Downloads\Hiby\hibymod\docs\capturas\vm\`: screenshots of the emulated framebuffer.

## 6. Open blockers

1. **The framebuffer belongs to the stock player.** It writes to fb0 and pans. An external app needs one of these routes:
   - stop it (SIGSTOP or kill) and restore it on exit;
   - have the player stop drawing in the meantime, which requires a patch;
   - draw on top and accept flicker.

   This could not be tested without the device.
2. **There is no clean way to launch apps from the stock UI**, which only invokes a few system scripts. One of these two things is needed:
   - a **key-combo daemon**: a tiny static C program that reads event0 and event3 without grabbing them, e.g. on POWER plus a long VOL-, and runs the launcher;
   - a **binary patch** so that a menu entry or one of those scripts invokes the launcher.
3. **Coexistence with playback.** With the player running only about 8–12 MB remain and there is no swap. In the test the OOM killer chose the process simulating the player. `oom_score_adj` must be set (player at -1000, apps at 1000) along with `RLIMIT_AS` limits. In addition, the decoder may suffer CPU-induced dropouts, something the harness cannot measure because it has no audio.
4. **Text input:** NetSurf's on-screen keyboard is too small for a finger. A custom one is needed.
5. **Real key codes** for event2, event3 and event4: unverified. They are configurable in `/etc/hibyinput.conf` and the NetSurf backend uses the standard codes.
6. **Real speed:** the harness only gives an optimistic bound with `--icount` (1 GIPS). Measuring on the device is still pending, especially TLS with soft-float mbedtls.
7. **Telegram:** own api_id and risk of account monitoring or bans with unofficial clients. The auth key must be kept off the HiBy (relay).
