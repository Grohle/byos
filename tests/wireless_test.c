/* wireless_test.c - PC tests (x86, WSL) of src/wireless_parse.inc.c: wpa_supplicant control
 * interface replies, events, /proc/net/route and dbus-send GetManagedObjects of bluez.
 * Run: bash tests/wireless_test.sh   (gcc with ASan/UBSan; does not touch the device)
 * The samples follow the exact formats of wpa_supplicant 2.x (ctrl_iface.c) and dbus-send
 * (dbus-print-message.c); replace them with captures from the device when available. */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "wireless_parse.inc.c"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FALLO %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECKS(a, b) do { checks++; if (strcmp((a), (b))) { fails++; printf("FALLO %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); } } while (0)

static const char SCAN[] =
    "bssid / frequency / signal level / flags / ssid\n"
    "aa:bb:cc:dd:ee:01\t2437\t-71\t[WPA2-PSK-CCMP][WPS][ESS]\tCasa\n"
    "aa:bb:cc:dd:ee:02\t2412\t-48\t[WPA2-PSK-CCMP][ESS]\tCasa\n"                 /* same SSID, stronger */
    "aa:bb:cc:dd:ee:03\t2462\t-85\t[ESS]\tCafe libre\n"
    "aa:bb:cc:dd:ee:04\t2412\t-60\t[WPA2-EAP-CCMP][ESS]\tEmpresa\n"
    "aa:bb:cc:dd:ee:05\t2412\t-66\t[WPA2-PSK-CCMP][ESS]\t\n"                     /* hidden */
    "aa:bb:cc:dd:ee:06\t2412\t-77\t[WEP][ESS]\tViejo\n"
    "aa:bb:cc:dd:ee:07\t2412\t-52\t[WPA2-SAE-CCMP][ESS]\tSolo WPA3\n"
    "aa:bb:cc:dd:ee:08\t2412\t-58\t[WPA2-PSK+SAE-CCMP][ESS]\tTransicion\n"
    "aa:bb:cc:dd:ee:09\t2412\t-63\t[WPA2-PSK-CCMP][ESS]\tCaf\\xc3\\xa9 \\\"Pepe\\\"\n"   /* UTF-8 + quotes, escaped */
    "aa:bb:cc:dd:ee:0a\t2412\t-90\t[ESS]\t\\x00\\x00\\x00\n";                       /* zeros: hidden */

static const char STATUS[] =
    "bssid=aa:bb:cc:dd:ee:02\nfreq=2412\nssid=Casa\nid=0\nmode=station\npairwise_cipher=CCMP\n"
    "group_cipher=CCMP\nkey_mgmt=WPA2-PSK\nwpa_state=COMPLETED\nip_address=192.168.1.37\n"
    "p2p_device_address=00:11:22:33:44:55\naddress=d0:31:10:aa:bb:cc\nuuid=x\n";
static const char SIGNAL[] = "RSSI=-49\nLINKSPEED=65\nNOISE=9999\nFREQUENCY=2412\n";
static const char LIST[] =
    "network id / ssid / bssid / flags\n"
    "0\tCasa\tany\t[CURRENT]\n"
    "1\tOficina\tany\t[DISABLED]\n"
    "2\tCaf\\xc3\\xa9\tany\t[TEMP-DISABLED]\n"
    "3\tMovil\tany\t\n";
static const char ROUTE[] =
    "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n"
    "wlan0\t0001A8C0\t00000000\t0001\t0\t0\t0\t00FFFFFF\t0\t0\t0\n"
    "wlan0\t00000000\t0101A8C0\t0003\t0\t0\t0\t00000000\t0\t0\t0\n";

static const char BLUEZ[] =
    "method return time=1731230000.123456 sender=:1.2 -> destination=:1.57 serial=812 reply_serial=2\n"
    "   array [\n"
    "      dict entry(\n"
    "         object path \"/org/bluez\"\n"
    "         array [\n"
    "            dict entry(\n"
    "               string \"org.freedesktop.DBus.Introspectable\"\n"
    "               array [\n"
    "               ]\n"
    "            )\n"
    "            dict entry(\n"
    "               string \"org.bluez.AgentManager1\"\n"
    "               array [\n"
    "               ]\n"
    "            )\n"
    "         ]\n"
    "      )\n"
    "      dict entry(\n"
    "         object path \"/org/bluez/hci0\"\n"
    "         array [\n"
    "            dict entry(\n"
    "               string \"org.bluez.Adapter1\"\n"
    "               array [\n"
    "                  dict entry(\n"
    "                     string \"Address\"\n"
    "                     variant                         string \"4C:BC:98:B0:28:1A\"\n"
    "                  )\n"
    "                  dict entry(\n"
    "                     string \"Alias\"\n"
    "                     variant                         string \"HiBy R3 II\"\n"
    "                  )\n"
    "                  dict entry(\n"
    "                     string \"Powered\"\n"
    "                     variant                         boolean true\n"
    "                  )\n"
    "                  dict entry(\n"
    "                     string \"Discovering\"\n"
    "                     variant                         boolean false\n"
    "                  )\n"
    "                  dict entry(\n"
    "                     string \"UUIDs\"\n"
    "                     variant                         array [\n"
    "                           string \"0000110a-0000-1000-8000-00805f9b34fb\"\n"
    "                        ]\n"
    "                  )\n"
    "               ]\n"
    "            )\n"
    "         ]\n"
    "      )\n"
    "      dict entry(\n"
    "         object path \"/org/bluez/hci0/dev_00_1B_66_AA_BB_CC\"\n"
    "         array [\n"
    "            dict entry(\n"
    "               string \"org.bluez.Device1\"\n"
    "               array [\n"
    "                  dict entry(\n"
    "                     string \"Address\"\n"
    "                     variant                         string \"00:1B:66:AA:BB:CC\"\n"
    "                  )\n"
    "                  dict entry(\n"
    "                     string \"Name\"\n"
    "                     variant                         string \"Momentum \"4\" Wireless\"\n"
    "                  )\n"
    "                  dict entry(\n"
    "                     string \"Alias\"\n"
    "                     variant                         string \"Momentum \"4\" Wireless\"\n"
    "                  )\n"
    "                  dict entry(\n"
    "                     string \"Class\"\n"
    "                     variant                         uint32 2360344\n"
    "                  )\n"
    "                  dict entry(\n"
    "                     string \"Icon\"\n"
    "                     variant                         string \"audio-card\"\n"
    "                  )\n"
    "                  dict entry(\n"
    "                     string \"Paired\"\n"
    "                     variant                         boolean true\n"
    "                  )\n"
    "                  dict entry(\n"
    "                     string \"Trusted\"\n"
    "                     variant                         boolean true\n"
    "                  )\n"
    "                  dict entry(\n"
    "                     string \"Connected\"\n"
    "                     variant                         boolean true\n"
    "                  )\n"
    "                  dict entry(\n"
    "                     string \"UUIDs\"\n"
    "                     variant                         array [\n"
    "                           string \"0000110b-0000-1000-8000-00805f9b34fb\"\n"
    "                           string \"0000110e-0000-1000-8000-00805f9b34fb\"\n"
    "                        ]\n"
    "                  )\n"
    "                  dict entry(\n"
    "                     string \"Adapter\"\n"
    "                     variant                         object path \"/org/bluez/hci0\"\n"
    "                  )\n"
    "                  dict entry(\n"
    "                     string \"ManufacturerData\"\n"
    "                     variant                         array [\n"
    "                           dict entry(\n"
    "                              uint16 76\n"
    "                              variant                                 array of bytes [\n"
    "                                    10 05 1c 1e 6f 2d\n"
    "                                 ]\n"
    "                           )\n"
    "                        ]\n"
    "                  )\n"
    "               ]\n"
    "            )\n"
    "            dict entry(\n"
    "               string \"org.bluez.MediaControl1\"\n"
    "               array [\n"
    "                  dict entry(\n"
    "                     string \"Connected\"\n"
    "                     variant                         boolean false\n"
    "                  )\n"
    "               ]\n"
    "            )\n"
    "         ]\n"
    "      )\n"
    "      dict entry(\n"
    "         object path \"/org/bluez/hci0/dev_00_1B_66_AA_BB_CC/sep1\"\n"
    "         array [\n"
    "            dict entry(\n"
    "               string \"org.bluez.MediaEndpoint1\"\n"
    "               array [\n"
    "               ]\n"
    "            )\n"
    "         ]\n"
    "      )\n"
    "      dict entry(\n"
    "         object path \"/org/bluez/hci0/dev_11_22_33_44_55_66\"\n"
    "         array [\n"
    "            dict entry(\n"
    "               string \"org.bluez.Device1\"\n"
    "               array [\n"
    "                  dict entry(\n"
    "                     string \"Address\"\n"
    "                     variant                         string \"11:22:33:44:55:66\"\n"
    "                  )\n"
    "                  dict entry(\n"
    "                     string \"Alias\"\n"
    "                     variant                         string \"11-22-33-44-55-66\"\n"
    "                  )\n"
    "                  dict entry(\n"
    "                     string \"Paired\"\n"
    "                     variant                         boolean false\n"
    "                  )\n"
    "                  dict entry(\n"
    "                     string \"RSSI\"\n"
    "                     variant                         int16 -82\n"
    "                  )\n"
    "               ]\n"
    "            )\n"
    "         ]\n"
    "      )\n"
    "      dict entry(\n"
    "         object path \"/org/bluez/hci0/dev_A0_B1_C2_D3_E4_F5\"\n"
    "         array [\n"
    "            dict entry(\n"
    "               string \"org.bluez.Device1\"\n"
    "               array [\n"
    "                  dict entry(\n"
    "                     string \"Name\"\n"
    "                     variant                         string \"WH-1000XM4\"\n"
    "                  )\n"
    "                  dict entry(\n"
    "                     string \"Icon\"\n"
    "                     variant                         string \"audio-headset\"\n"
    "                  )\n"
    "                  dict entry(\n"
    "                     string \"Paired\"\n"
    "                     variant                         boolean false\n"
    "                  )\n"
    "                  dict entry(\n"
    "                     string \"Connected\"\n"
    "                     variant                         boolean false\n"
    "                  )\n"
    "                  dict entry(\n"
    "                     string \"RSSI\"\n"
    "                     variant                         int16 -61\n"
    "                  )\n"
    "               ]\n"
    "            )\n"
    "         ]\n"
    "      )\n"
    "   ]\n";

int main(void)
{
    /* SCAN_RESULTS */
    struct wl_net nets[16];
    int n = wlp_scan_results(SCAN, nets, 16);
    CHECK(n == 7);                                    /* 10 lines: one duplicate, two hidden */
    CHECKS(nets[0].ssid, "Casa");                     /* strongest first */
    CHECK(nets[0].level == -48 && nets[0].freq == 2412);
    CHECKS(nets[0].bssid, "aa:bb:cc:dd:ee:02");
    CHECK(nets[0].sec == WSEC_PSK && nets[0].saved_id == -1);
    for (int i = 1; i < n; i++) CHECK(nets[i - 1].level >= nets[i].level);
    int found = 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(nets[i].ssid, "Cafe libre")) { found |= 1; CHECK(nets[i].sec == WSEC_OPEN); }
        if (!strcmp(nets[i].ssid, "Empresa")) { found |= 2; CHECK(nets[i].sec == WSEC_EAP); }
        if (!strcmp(nets[i].ssid, "Viejo")) { found |= 4; CHECK(nets[i].sec == WSEC_WEP); }
        if (!strcmp(nets[i].ssid, "Solo WPA3")) { found |= 8; CHECK(nets[i].sec == WSEC_SAE); }
        if (!strcmp(nets[i].ssid, "Transicion")) { found |= 16; CHECK(nets[i].sec == WSEC_PSK); }
        if (!strcmp(nets[i].ssid, "Caf\xc3\xa9 \"Pepe\"")) found |= 32;
    }
    CHECK(found == 63);
    CHECK(wlp_scan_results("", nets, 16) == 0);
    CHECK(wlp_scan_results("FAIL\n", nets, 16) == 0);
    CHECK(wlp_scan_results(SCAN, nets, 3) == 3);      /* bounded */

    /* unescape / display */
    char b[64];
    CHECK(wlp_unescape("a\\\\b\\\"c\\x41", b, sizeof b) == 6); CHECKS(b, "a\\b\"cA");
    CHECK(wlp_unescape("abcdef", b, 4) == 3); CHECKS(b, "abc");
    wlp_display("ok\xff\x01x\xc3\xa9", b, sizeof b); CHECKS(b, "ok??x\xc3\xa9");

    /* STATUS / SIGNAL_POLL */
    char v[64];
    CHECK(wlp_kv(STATUS, "wpa_state", v, sizeof v)); CHECKS(v, "COMPLETED");
    CHECK(wlp_kv(STATUS, "ip_address", v, sizeof v)); CHECKS(v, "192.168.1.37");
    CHECK(wlp_kv(STATUS, "address", v, sizeof v)); CHECKS(v, "d0:31:10:aa:bb:cc");   /* not p2p_device_address */
    CHECK(wlp_kv(STATUS, "id", v, sizeof v)); CHECKS(v, "0");
    CHECK(!wlp_kv(STATUS, "nope", v, sizeof v) && !v[0]);
    CHECK(wlp_kv(SIGNAL, "RSSI", v, sizeof v) && atoi(v) == -49);
    CHECK(wlp_kv(SIGNAL, "LINKSPEED", v, sizeof v) && atoi(v) == 65);
    CHECK(wlp_bars(-49) == 3 && wlp_bars(-65) == 2 && wlp_bars(-75) == 1 && wlp_bars(-90) == 0);

    /* LIST_NETWORKS */
    struct wl_saved sv[8];
    n = wlp_list_networks(LIST, sv, 8);
    CHECK(n == 4);
    CHECK(sv[0].id == 0 && sv[0].current && !sv[0].disabled); CHECKS(sv[0].ssid, "Casa");
    CHECK(sv[1].id == 1 && sv[1].disabled && !sv[1].current);
    CHECK(sv[2].temp_disabled); CHECKS(sv[2].ssid, "Caf\xc3\xa9");
    CHECK(sv[3].id == 3 && !sv[3].current && !sv[3].disabled); CHECKS(sv[3].ssid, "Movil");
    CHECK(wlp_list_networks("network id / ssid / bssid / flags\n", sv, 8) == 0);

    /* GET_NETWORK ssid */
    CHECK(wlp_ssid_value("\"Mi \"red\"\"", b, sizeof b) == 8); CHECKS(b, "Mi \"red\"");
    CHECK(wlp_ssid_value("436166c3a9", b, sizeof b) == 5); CHECKS(b, "Caf\xc3\xa9");
    CHECK(wlp_ssid_value("FAIL\n", b, sizeof b) < 0);
    CHECK(wlp_ssid_value("0000", b, sizeof b) < 0);
    wlp_hex("Caf\xc3\xa9", b, sizeof b); CHECKS(b, "436166c3a9");

    /* passphrases */
    CHECK(wlp_psk_ok("12345678") == 1);
    CHECK(wlp_psk_ok("1234567") == 0);
    CHECK(wlp_psk_ok("contraseña con espacios") == 1);
    CHECK(wlp_psk_ok("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef") == 2);
    CHECK(wlp_psk_ok("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeg") == 0);

    /* events */
    int id;
    CHECK(wlp_event("<3>CTRL-EVENT-CONNECTED - Connection to aa:bb:cc:dd:ee:02 completed [id=4 id_str=]", &id) == WE_CONNECTED && id == 4);
    CHECK(wlp_event("<3>CTRL-EVENT-SSID-TEMP-DISABLED id=2 ssid=\"Casa\" auth_failures=1 duration=10 reason=WRONG_KEY", &id) == WE_WRONG_KEY && id == 2);
    CHECK(wlp_event("<3>CTRL-EVENT-SSID-TEMP-DISABLED id=2 ssid=\"Casa\" auth_failures=2 duration=20 reason=CONN_FAILED", &id) == WE_TEMP_DISABLED);
    CHECK(wlp_event("<3>CTRL-EVENT-DISCONNECTED bssid=aa:bb:cc:dd:ee:02 reason=3 locally_generated=1", &id) == WE_DISCONNECTED);
    CHECK(wlp_event("<3>CTRL-EVENT-NETWORK-NOT-FOUND", &id) == WE_NOT_FOUND);
    CHECK(wlp_event("<3>CTRL-EVENT-SCAN-RESULTS ", &id) == WE_SCAN_RESULTS);
    CHECK(wlp_event("<4>WPA: 4-Way Handshake failed - pre-shared key may be incorrect", &id) == WE_WRONG_KEY);
    CHECK(wlp_event("<3>CTRL-EVENT-BSS-ADDED 3 aa:bb:cc:dd:ee:02", &id) == WE_NONE);

    /* /proc/net/route */
    CHECK(wlp_route_gw(ROUTE, "wlan0", v, sizeof v)); CHECKS(v, "192.168.1.1");
    CHECK(!wlp_route_gw(ROUTE, "eth0", v, sizeof v));

    /* bluez GetManagedObjects */
    struct wl_adapter ad; struct wl_dev dv[8];
    n = wlp_bluez_objects(BLUEZ, &ad, dv, 8);
    CHECK(ad.found && ad.powered && !ad.discovering);
    CHECKS(ad.addr, "4C:BC:98:B0:28:1A"); CHECKS(ad.alias, "HiBy R3 II");
    CHECK(n == 3);                                     /* sep1 is not a device */
    CHECKS(dv[0].addr, "00:1B:66:AA:BB:CC");
    CHECKS(dv[0].name, "Momentum \"4\" Wireless");
    CHECK(dv[0].paired && dv[0].trusted && dv[0].connected && dv[0].audio && dv[0].cls == 2360344);
    CHECKS(dv[0].icon, "audio-card");
    CHECK(!dv[0].has_rssi);
    CHECK(dv[1].name[0] == 0 && dv[1].has_rssi && dv[1].rssi == -82 && !dv[1].paired && !dv[1].audio);
    CHECKS(dv[2].name, "WH-1000XM4");
    CHECKS(dv[2].addr, "A0:B1:C2:D3:E4:F5");             /* from the path (no Address property) */
    CHECK(dv[2].audio && dv[2].rssi == -61 && !dv[2].connected);
    CHECK(wlp_bluez_objects("", &ad, dv, 8) == 0 && !ad.found);
    CHECK(wlp_bluez_objects(BLUEZ, &ad, dv, 1) == 1);   /* bounded */

    /* dbus-send errors */
    char en[64];
    CHECK(wlp_dbus_error("Error org.bluez.Error.AuthenticationFailed: Authentication Failed\n", en, sizeof en));
    CHECKS(en, "org.bluez.Error.AuthenticationFailed");
    CHECK(!wlp_dbus_error("method return time=1 sender=:1.2 -> destination=:1.9 serial=3 reply_serial=2\n", en, sizeof en));

    printf("%d comprobaciones, %d fallos\n", checks, fails);
    return fails != 0;
}
