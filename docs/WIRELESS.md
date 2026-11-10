# Wireless app (Wi-Fi and Bluetooth)

Our replacement for HiBy's stock "Wireless" app, in the iOS Settings style. Code:

| File | What |
|---|---|
| `src/wireless.inc.c` | UI (pages, drawing, gestures), the two worker threads, wpa_supplicant and bluez clients. Entry points: `wireless_app()`, `wireless_app_at(ix, iy)`, `wireless_preload()` |
| `src/wireless_parse.inc.c` | Pure parsers (no I/O), shared with the PC tests |
| `src/lang/wireless_tr.inc.c` | Its texts in the 13 languages (one row per key, hooked into `tr_lang()` by `wtr_lookup`) |
| `tests/wireless_test.c`, `tests/wireless_i18n_test.c`, `tests/wireless_test.sh` | PC tests: parsers (87 checks) and the 13 languages (keys, `%s`, glyphs) |

Status: **builds with zero warnings, PC tests pass, never run on the device yet.** See "Device test plan".

Evidence tags: **[V]** verified statically in the extracted rootfs (`~/hiby/rootfs`) or by disassembly of `sys_server`; **[I]** inferred; **[D]** must be verified on the device.

## 1. How the firmware does Wi-Fi

- **Who owns it [V]:** `hiby_player` does not touch wpa_supplicant itself. It sends text commands (`WIFI:ON:wlan0`, `WIFI:SCAN:...`, `WIFI:SSID_CONNECT:...`) over Unix stream sockets to **`/usr/bin/sys_server`** (started by `/etc/init.d/S50sys_server`), which listens on `/var/run/sys_server` and answers by connecting to the player's `/var/run/sys_client`. sys_server accepts **one client at a time** ("client disconnected, need wait new connect"), so we must not talk to it: its replies would land in the player.
- **On/off [V]:** `WIFI:ON` runs `sh /usr/bin/wifi_on.sh`: kills `udhcpc` and `wpa_supplicant`, `ifconfig wlan0 up`, `wpa_supplicant -Dnl80211 -iwlan0 -c/data/wpa_supplicant.conf -B`, then `udhcpc -b -i wlan0 -q -x hostname:...`. `WIFI:OFF` runs `wifi_off.sh`: kills both and `ifconfig wlan0 down`. No rfkill, no module load (the `bcmdhd` driver stays loaded; `wifi_down.sh`/`wifi_up.sh` with rfkill exist but are not used by sys_server).
- **The on/off state** is the player's setting `0x33` (`user.ini` offset `0x3c8`), applied by `setting_set(0x33, v)` → sys_server (see `docs/research_settings_api.md`). `src/hibyset.inc.c` (`hs_set("wifi", v)`) calls exactly that path. **We use only that** to switch the radio, so the player's UI, its `user.ini` and the radio always agree.
- **Config and control socket [V]:** `/data/wpa_supplicant.conf` (`/data` → `/usr/data`), created by `/etc/init.d/S43wifi_bcm_init_config` with `ctrl_interface=/var/run/wpa_supplicant`, `update_config=1`, `country=GB`. Control socket `/var/run/wpa_supplicant/wlan0`. Saved networks are the `network={}` blocks of that file (sys_server does `save_config` after connecting/removing).
- **sys_server's own wpa usage [V]:** it shells out `wpa_cli -i wlan0 add_network / set_network N ssid "..." / psk "..." / key_mgmt NONE / scan_ssid 1 / enable_network / select_network / save_config / remove_network / scan / scan_result > /data/wifi_result.txt / list_network > /data/wifi_network.txt / status > /data/wifi_status.txt`, and it keeps a monitor connection (`wpa_ctrl_attach`) on the same socket.
- **DHCP [V, disassembly of sys_server @0x4373ec]:** on every `CTRL-EVENT-CONNECTED` from wpa_supplicant, sys_server runs `udhcpc -b -i wlan0 -q`, refreshes `/data/wifi_status.txt` and `/data/wifi_network.txt`, and sends `WIFI:STATUS:COMPLETED:<bssid>` to the player. Therefore **joining a network through the control socket is fully seen by the player and gets an IP without us running udhcpc** (a second udhcpc would even deconfigure `wlan0` for a moment).
- **The player polls** `wpa_cli -i wlan0 signal_poll` and `status` every 3 s while Wi-Fi is on [V, strings]; that is why our overlay puts `overlay/usr/lib/hibymod/bin/wpa_cli` (`src/wpa_cli_fast.c`) first in the player's `PATH`: a static binary that answers those two queries on the socket directly (the stock wpa_cli costs ~23 ms per run loading readline). Our app does not use wpa_cli at all.

## 2. How the firmware does Bluetooth

- **Stack [V]:** BlueZ 5.54 (`/usr/libexec/bluetooth/bluetoothd -E -C`), `bluealsa -p a2dp-source --a2dp-volume` (the player outputs to `bluealsa:DEV=...`), `bt-agent -c NoInputNoOutput -p /usr/data/pin.conf` (bluez-tools; PIN `0000`), D-Bus system bus. Started once at boot by `/usr/bin/bt_init` (`/etc/init.d/S80_bt_init`), which ends with `bt-adapter --set Powered Off`. Firmware is loaded with `brcm_patchram_plus` on `/dev/ttyS0`; `bt_suspend`/`bt_resume` power the chip via `/sys/class/rfkill/rfkill0/state`.
- **Who owns it [V]:** again sys_server: `BT:ON/OFF/SCAN/CANCEL_SCAN/PAIR:<mac>/CONNECT:<mac>/DISCONNECT/UNPAIR/LIST/CODEC/ABSVOL/...`, implemented with GDBus proxies on `org.bluez.Adapter1` / `Device1` (`StartDiscovery`, `Pair`, `Connect`, `Disconnect`, `RemoveDevice`), plus `org.bluealsa` for codecs. Its listener thread turns `object-added` into `BT:FOUND:...` messages for the player. On a successful `BT:CONNECT` it writes the MAC to **`/data/bt_lastused.txt`** (read back with `%17s` to auto-connect at the next BT start: "Auto connecting to last device").
- **On/off state** is the player's setting `0x23` (`hs_set("bt", v)`), which sends `BT:ON`/`BT:OFF` to sys_server [V, research_settings_api.md].
- **What we can do from our own process [V/I]:** everything except on/off goes straight to bluez with `/usr/bin/dbus-send` (present, 18 KB): `GetManagedObjects` (adapter + devices with `Name`, `Icon`, `Class`, `Paired`, `Trusted`, `Connected`, `RSSI`, `UUIDs`), `Device1.Pair`, `Properties.Set Trusted=true`, `Device1.Connect`, `Device1.Disconnect`, `Adapter1.RemoveDevice`. Pairing prompts are answered by HiBy's own `bt-agent` (default agent). **Discovery** cannot be done with dbus-send because bluez stops a discovery when the client that started it leaves the bus; a `bluetoothctl` child with `agent off` + `scan on` on its stdin holds it while the Bluetooth page is visible (max 45 s, renewed by pull-to-refresh), then `scan off` + `quit`.
- **What needs the player [I, D]:** switching the audio output to the headphones. The player has its own BT state; it is not proven statically that it notices a connection it did not request (sys_server's listener only reports `object-added` and adapter `Powered`). Device test step B4 settles it; fallbacks are in §6.

## 3. What is implemented

Pages (all with the shared iOS list style, back gesture from both edges, bottom bar → home/multitasking via `own_home_gesture`, inertia scrolling, rubber-band pull-to-refresh with the iOS spinner, iOS stack transitions):

- **Inalámbrico (root):** Wi-Fi › (current network / Not connected / Off), Bluetooth › (connected device / On / Off), AirPlay and DLNA toggles (greyed out without Wi-Fi), HiBy Link toggle — all through `hs_set`. Hidden if the v7 library does not answer (`hs_list` fails).
- **Wi-Fi:** toggle (animated, spinner while the radio follows); current network with blue check, lock, signal arcs and (i); "MY NETWORKS" (saved and in range); "OTHER NETWORKS" sorted by signal with spinner in the header while scanning; "Other…" (hidden network: name, security, password); "Known Networks" › list (tap → details/forget). Tap a saved network → joins with the saved key; open → joins; secured → password sheet (our keyboard, masked with an eye toggle, no auto-capitals, "Join"); wrong password (`CTRL-EVENT-SSID-TEMP-DISABLED reason=WRONG_KEY`) → the sheet comes back with "Incorrect password". WPA/WPA2 PSK, PSK+SAE transition, WEP; SAE-only tried as SAE; 802.1X refused with a message.
- **Network details:** Forget (confirmation; extra warning when it is the ADB-over-Wi-Fi network), Join, IPv4 (IP, mask via `SIOCGIFNETMASK`, router from `/proc/net/route`, DNS from `/etc/resolv.conf`), signal, link speed, frequency, security, Wi-Fi MAC.
- **Bluetooth:** toggle; "MY DEVICES" (connected first, Connected/Not Connected, (i)); "OTHER DEVICES" (named, unpaired, by RSSI) with spinner while discovering; tap → pair + trust + connect (spinner on the row); device page: Connect/Disconnect, Forget, address, type, signal; non-audio devices get a footer.
- **Turning Wi-Fi off while ADB is in Wi-Fi mode** asks for confirmation first.

Joining, in order (worker thread, mirrors sys_server): `ADD_NETWORK` (or reuse the saved one), `SET_NETWORK n ssid <hex>` (hex: any byte is safe), `psk "..."`/`key_mgmt NONE`/`wep_key0`/`sae_password`, `scan_ssid 1` for hidden, `ATTACH` a monitor socket, `ENABLE_NETWORK n`, `SELECT_NETWORK n`, wait ≤ 25 s for `CTRL-EVENT-CONNECTED` / `WRONG_KEY` (plus a `STATUS` poll every second), then `ENABLE_NETWORK all` (SELECT disables the others, sys_server does the same) and `SAVE_CONFIG`, and wait ≤ 12 s for sys_server's DHCP to give an IP. On failure the new network is removed, `ENABLE_NETWORK all` + `REASSOCIATE` return to the previous one. Passwords are wiped from the job queue and the stack after use and are never logged.

Performance design: two detached threads (Wi-Fi, Bluetooth) with small job queues; the UI copies a snapshot only when a generation counter changes and never blocks. Threads sleep on a condition variable while the app is hidden (zero CPU); while visible, Wi-Fi state every 3 s (a few socket round trips, logged with its time), a scan every 15 s on the Wi-Fi page, bluez every 2 s while discovering / 4 s on the BT page / 8 s elsewhere (one `dbus-send` fork). The UI repaints only on data changes, every frame while a toggle slides, and at 12 steps/s while a spinner is visible (as iOS). PC render of a full page: 0.1–0.9 ms on x86 (render harness in the session scratchpad); on the device it is logged at exit as `wireless: pintado medio X ms, maximo Y ms`, and the open time as `wireless: abierta en N ms`. Card backgrounds use a row-span fill (no per-pixel sqrt, unlike `fill_rrect`). Memory: ~230 KB of static BSS (page models, two state snapshots, a 96 KB buffer for the dbus-send dump; pages are only touched when used), no heap, no new frame buffers (reuses `img_under`, `img_base`, `img_app`).

Hooks for the owner:
- `wireless_app()` (zoom from the screen centre) / `wireless_app_at(ix, iy)` (zoom from the icon), for the launcher icon `wireless` and the CC tile `CT_WIRELESS` (`cc_open_app("wireless")`).
- `wireless_preload()` once at shell start-up so the first open already has data (otherwise the first open shows the cached-empty state for ~0.1–0.5 s).
- Radio on/off is only `wl_hs_set("wifi"|"bt", v)` (in `wireless.inc.c`): change it there if the settings API changes.

## 4. PC tests

```
bash tests/wireless_test.sh
```
Parsers: SCAN_RESULTS (dedupe by SSID, hidden and `\x00` names skipped, escapes `\xNN`/`\"`/`\\`, security from flags, sorting, bounds), STATUS/SIGNAL_POLL, LIST_NETWORKS, `GET_NETWORK n ssid` (quoted and hex), passphrase rules, events, `/proc/net/route`, and a full `dbus-send --print-reply` GetManagedObjects dump (adapter, devices, services under a device ignored, nested ManufacturerData, quotes inside names, unnamed devices). The samples follow the exact output formats of wpa_supplicant 2.x and dbus-send; **replace them with captures from the device** (step W0/B0 below) when possible. Then every `tr()` key used by the app is checked in the 13 languages (text present, same `%s`, glyphs present in the firmware fonts).

## 5. Device test plan (safe order)

ADB runs over Wi-Fi, so **never switch Wi-Fi off or change networks without a fallback**:
1. Prefer ADB over USB for the Wi-Fi steps: Personalize › Developer › ADB › USB (see ADB_HELP_USB), or keep a USB cable plugged in.
2. Before any Wi-Fi off/forget/join test, arm an automatic restore that does not depend on our UI (it runs the same script sys_server uses for `WIFI:ON`, and does nothing if Wi-Fi is up):
   ```
   adb shell 'nohup sh -c "sleep 180; wpa_cli -i wlan0 status >/dev/null 2>&1 || sh /usr/bin/wifi_on.sh" >/dev/null 2>&1 &'
   ```
   If the player's setting ended up as "off", fix it from its own Control Centre or `hibysetctl set wifi 1`.
3. Back up the Wi-Fi config first: `adb shell cp /data/wpa_supplicant.conf /data/wpa_supplicant.conf.bak` (restore with `cp` back + `wpa_cli -i wlan0 reconfigure`).
4. Install the binary from `out/agent_wireless/hibyshell` the usual way (never overwrite `bin/hibyshell` before it passes).

Steps (read-only first):
- **W0** Capture formats: `adb shell 'wpa_cli -i wlan0 status; wpa_cli -i wlan0 scan_results; wpa_cli -i wlan0 list_networks'` and `adb shell dbus-send --system --print-reply --dest=org.bluez / org.freedesktop.DBus.ObjectManager.GetManagedObjects > bluez.txt`; paste into `tests/wireless_test.c` and rerun.
- **W1** Open the app (CC tile or icon): root shows the current SSID and BT state; log shows `wifi: estado en N ms` (expect < 20 ms) and `bt: estado en N ms` (expect < 150 ms).
- **W2** Wi-Fi page: list fills, spinner in "OTHER NETWORKS" while scanning, pull-to-refresh rescans, details page shows IP/router/DNS. Nothing changes on the device.
- **W3** Join an open network or a phone hotspot (USB ADB connected): wrong password first (must come back with "Incorrect password" and the old network must come back), then the right one. Check `wpa_cli list_networks`, `/data/wpa_supplicant.conf` (new block, others not `disabled=1`), IP in the details page, and the player's own Wi-Fi page/status icon.
- **W4** Join back the home network from "MY NETWORKS" (no password asked). ADB over Wi-Fi should come back.
- **W5** Forget the hotspot network; check the file.
- **W6** Toggle Wi-Fi off/on from our page (with the restore armed): radio and the player's CC tile must follow; with ADB-over-Wi-Fi mode the confirmation must appear.
- **B0** BT page with BT off: toggle on (spinner up to ~8 s while `bt_resume`), "Visible as".
- **B1** Discovery: headphones in pairing mode appear in "OTHER DEVICES"; `ps` shows one `bluetoothctl` while the page is visible and none after leaving it (also after the 45 s limit).
- **B2** Pair + connect headphones; `/data/bt_lastused.txt` = their MAC.
- **B3** Play music: **does the player send audio to the headphones?** Check its BT icon/status and `bluealsa` (`ls /var/run/bluealsa` / audio heard). This is the key open question.
- **B4** Disconnect / Connect from the device page, Forget; reboot and check auto-reconnect.
- **P1** AirPlay/DLNA/HiBy Link toggles mirror the player's CC.
- **P2** Record frames while scrolling and toggling (`hibyctl rec`, `tools/recsheet.py`): no gaps > 30 ms; spinner steps at ~83 ms.

## 6. Open questions and risks

- **Audio routing after a BT connect we initiated [D]** (step B3). If the player does not switch: (a) reconnect through sys_server would require speaking its single-client socket (not done: replies go to the player); (b) let the player do the connect by injecting a tap in its BT list (fragile); (c) RE the player's BT state handler (`BT:CONNECT:OK` handling) and expose it in `hibymod_disp.so` like hibyset does for settings. Pairing/listing/forget do not depend on this.
- **bluetoothctl reading commands from a pipe [I, D]:** works on BlueZ 5.5x in practice (`agent off` / `scan on`); if not, discovery falls back to nothing (lists still show known/cached devices) — alternative: a minimal D-Bus client in C.
- **`SELECT_NETWORK` + `SAVE_CONFIG` [I]:** we re-enable all networks before saving (sys_server does the same); W3 checks the file.
- **Concurrent `hs_*` requests:** hibyset allows one request in flight. Our calls are serialized by `wl_hs_mu`, but `src/sysset.inc.c` has its own thread; a shared mutex inside `hibyset.inc.c` would remove the (small) risk.
- **"Import music via Wi-Fi" (stock tile) is not reproduced:** it is the player's HTTP server on port 4399 that only runs in its own activity. Needs a separate tool (our own upload server) or RE.
- 5 GHz [I]: the AP6212 / BCM43430 module named in `bt_init` is 2.4 GHz only, so the frequency row should always read 2.4 GHz.
