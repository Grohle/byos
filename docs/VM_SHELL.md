# Running hibyshell in the QEMU harness

The real `hibyshell` binary (same build as on the device) runs unmodified inside the MIPS harness of
`apps/harness_vm` (see `docs/research_apps_vm.md` for the VM itself). It is used to exercise and
measure UI code (first user: the Apps store) without touching the HiBy.

## How it works

The shell normally lives next to `hiby_player` with our `hibymod_disp.so` preloaded. In the VM a
small stand-in replaces both:

| Piece | File | Role |
|---|---|---|
| `hm_vmplayer` | `apps/harness_vm/tools/hm_vmplayer.c` | Creates the shared state of `src/hm_shared.h` (`/dev/shm/hibymod_fbstate`, `/dev/shm/hibymod_frames`), the FIFO `/tmp/hibymod_disp` and the input socket. Emulates the panel: a `P` waits for the next 44.3 Hz frame edge and copies `frames[0]` (dirty rows) into `/dev/fb0`, so `present()` blocks like on the device. The "player" only shows a stock home (painted to match `home.sig`) and bumps `home_hits`, so the shell enters its launcher. Logs every present and can record frames. |
| `hibyinput` | `apps/harness_vm/tools/hibyinput.c` | The harness input daemon (5 evdev devices named like the HiBy's, TCP control). It now also logs each touch to `/tmp/hibyinput.log` (`D/M/U ms x y`, CLOCK_MONOTONIC). |
| `shell_vm.sh` | `apps/harness_vm/guest/shell_vm.sh` | Guest side: tmpfs `/usr/data` (24 MB, like the free space of the device) and a separate tmpfs at `/usr/data/mnt/sd_0` (so the store sees an SD card), copies the shell assets, writes `user.ini` (language), the extra repo URL, starts `hm_vmplayer` and `hibyshell`. |
| `shell_vm_prep.sh` | `apps/harness_vm/shell_vm_prep.sh` | Host side (WSL): makes a **private** copy of the harness in `~/hiby/vm_store` (own rootfs, own `share/`, so it never collides with experiments in `~/hiby/vm`), builds the two tools, stages the shell, the ByOS SD payload assets, `hibynet` + `cacert.pem`, the firmware font, `out/apps/hola.hpk` as an SD package and a local test repository. |
| `mk_testrepo.py` | `apps/harness_vm/scripts/mk_testrepo.py` | Local store repository: the real `hola` package plus 24 filler apps with generated icons (some featured, some "in testing"), served over HTTP from the host. |
| `vmdrive.py` | `apps/harness_vm/vmdrive.py` | Scriptable driver: taps, swipes, held drags, screenshots, frame recording, shell restarts; at the end it pulls the logs and writes a timing report. |

The guest CPU must have an FPU (`--cpu 24Kf`): the shell is hard-float.

## Usage

```bash
# WSL, once (and after rebuilding the shell)
bash tools/wsl/zbuild_shell.sh                       # or any build of src/hibyshell.c
bash apps/harness_vm/shell_vm_prep.sh /path/to/hibyshell
# start the VM (keeps running; the shell starts at boot via share/autorun.sh)
cd ~/hiby/vm_store && ./run_vm.sh --cpu 24Kf --port 5566
# in another terminal: the local test repository (guest sees it as http://10.0.2.2:8765/index.json)
cd ~/hiby/vm_store/share/hs/repo && python3 -m http.server 8765 --bind 127.0.0.1
# drive it
cd <repo>/apps/harness_vm
HIBY_PORT=5566 python3 vmdrive.py scripts/store/store_first.cmds /tmp/out_first   # empty cache
HIBY_PORT=5566 python3 vmdrive.py scripts/store/store_flow.cmds  /tmp/out_flow    # full walkthrough
HIBY_PORT=5566 python3 hibyctl.py shot /tmp/s.png                                 # ad hoc
```

To swap the shell without rebooting: copy the binary to `~/hiby/vm_store/share/hs/bin/hibyshell`
and use `restart` in a script (or `sh /mnt/share/hs/shell_vm.sh restart [nocache]` in the guest).
The guest's `/usr/data` is a tmpfs: it survives shell restarts but not a VM reboot. Options:
`share/hs/lang` (`es`, `en`, `fr`), `share/hs/repo_url` (`none` for no extra repository),
`share/hs/noauto` (do not start the shell at boot). The default ByOS repository is fetched for
real from GitHub through QEMU's user-mode network (`hibynet`, TLS) and works.

### Script commands (`vmdrive.py`)

`mark LABEL` names the next touch in the report; `tap X Y [MS]`, `swipe X1 Y1 X2 Y2 [MS]`,
`down/move/up`, `drag X1 Y1 X2 Y2 MS` (no release: combine with `sleep` + `up`, e.g. pull to
refresh), `sleep MS`, `shot NAME`, `rec N NAME` (the next N presented frames to
`share/hs/rec/NAME.bin`, viewable with `tools/recsheet.py`), `sh CMD`, `restart [nocache]`,
`wait_log TEXT [MS]`.

Store scripts: `apps/harness_vm/scripts/store/` (`store_first.cmds`, `store_flow.cmds`,
`store_pull_rec.cmds`). Coordinates assume the default launcher (Apps icon at 265,165).

### Report

For every marked touch, `report.txt` gives:

- **feedback**: ms from touch-down (hibyinput log) to the first presented frame whose content
  changed (hm_vmplayer log). This is touch-to-feedback including any queueing in the shell.
- **frames / mean / max**: presents between touch-down and 600 ms after release. Gaps while
  nothing changes (finger held still, idle) are expected and not stalls.
- the shell's own profile lines (`perfil tienda: … dibujo X ms`, `tienda: respuesta (…)`).

## Fidelity

QEMU TCG is not the XBurst: absolute times are not the device's. Compare builds against each
other (before/after) and look at the in-shell render times relative to the 22.6 ms panel frame.
Not emulated: real player frames (only a static home), the overlay layer, the watchdog, audio,
WiFi latency. `/dev/mem` is never touched (LCD clock changes are disabled in `power.inc.c`).
