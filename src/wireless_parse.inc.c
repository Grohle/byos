/* wireless_parse.inc.c - parsers for the Wireless app (no I/O, no UI), shared by the shell and
 * by the PC tests (tests/wireless_test.c). Prefix wlp_.
 *   - wpa_supplicant control interface replies: STATUS / SIGNAL_POLL (key=value), SCAN_RESULTS,
 *     LIST_NETWORKS, GET_NETWORK <id> ssid, unsolicited events ("<3>CTRL-EVENT-...");
 *   - /proc/net/route (default gateway of an interface);
 *   - "dbus-send --print-reply ... org.freedesktop.DBus.ObjectManager.GetManagedObjects" on
 *     org.bluez (adapter + devices with their properties) and dbus-send error lines.
 * Needs <stdio.h> <stdlib.h> <string.h> <ctype.h>. See docs/WIRELESS.md. */
#ifndef WIRELESS_PARSE_INC_C
#define WIRELESS_PARSE_INC_C

enum { WSEC_OPEN, WSEC_WEP, WSEC_PSK, WSEC_SAE, WSEC_EAP };
struct wl_net { char ssid[33]; char bssid[18]; int level, freq, sec, saved_id, current; };
struct wl_saved { int id, current, disabled, temp_disabled; char ssid[33]; };
struct wl_dev {
    char path[64], addr[18], name[64], icon[32];
    unsigned cls;
    int paired, trusted, connected, rssi, has_rssi, audio, blocked;
};
struct wl_adapter { int found, powered, discovering, discoverable; char addr[18], alias[64]; };

/* ------------------------------------------------------------------ texto */
/* bounded copy (always terminated) */
static void wlp_cpy(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src); if (n >= cap) n = cap - 1;
    memcpy(dst, src, n); dst[n] = 0;
}
static int wlp_hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

/* undoes wpa_supplicant's printf_encode (\\ \" \e \n \r \t \xHH); stops at end/tab/newline.
 * Returns the number of bytes written (an embedded NUL ends the SSID: such names are skipped). */
static int wlp_unescape(const char *in, char *out, int cap)
{
    int n = 0;
    for (const char *p = in; *p && *p != '\t' && *p != '\n' && *p != '\r'; p++) {
        int c = (unsigned char)*p;
        if (c == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'e': c = 27; break;
            case 'x':
                if (wlp_hexv(p[1]) >= 0 && wlp_hexv(p[2]) >= 0) { c = wlp_hexv(p[1]) * 16 + wlp_hexv(p[2]); p += 2; }
                else c = 'x';
                break;
            default: c = (unsigned char)*p; break;          /* \\ and \" */
            }
        }
        if (c == 0) break;
        if (n < cap - 1) out[n++] = (char)c;
    }
    if (cap > 0) out[n] = 0;
    return n;
}

/* the SSID as printable UTF-8 for drawing: invalid bytes and control characters become '?' */
static void wlp_display(const char *in, char *out, int cap)
{
    int n = 0;
    const unsigned char *p = (const unsigned char *)in;
    while (*p && n < cap - 5) {
        int len = *p < 0x80 ? 1 : (*p & 0xE0) == 0xC0 ? 2 : (*p & 0xF0) == 0xE0 ? 3 : (*p & 0xF8) == 0xF0 ? 4 : 0;
        int ok = len > 0;
        for (int i = 1; ok && i < len; i++) if ((p[i] & 0xC0) != 0x80) ok = 0;
        if (ok && len == 1 && *p < 32) ok = 0;
        if (!ok) { out[n++] = '?'; p++; continue; }
        for (int i = 0; i < len; i++) out[n++] = (char)p[i];
        p += len;
    }
    out[n] = 0;
}

/* value of "key=" in a key=value reply (STATUS, SIGNAL_POLL); 1 if found */
static int wlp_kv(const char *txt, const char *key, char *out, int cap)
{
    size_t kl = strlen(key);
    for (const char *l = txt; l && *l; ) {
        if (!strncmp(l, key, kl) && l[kl] == '=') {
            const char *v = l + kl + 1; int n = 0;
            while (v[n] && v[n] != '\n' && v[n] != '\r' && n < cap - 1) n++;
            memcpy(out, v, (size_t)n); out[n] = 0;
            return 1;
        }
        l = strchr(l, '\n'); if (l) l++;
    }
    if (cap > 0) out[0] = 0;
    return 0;
}

/* security from the SCAN_RESULTS flags ("[WPA2-PSK-CCMP][ESS]") */
static int wlp_sec(const char *flags)
{
    if (strstr(flags, "-PSK") || strstr(flags, "PSK-")) return WSEC_PSK;   /* also PSK+SAE transition */
    if (strstr(flags, "SAE")) return WSEC_SAE;
    if (strstr(flags, "-EAP") || strstr(flags, "EAP")) return WSEC_EAP;
    if (strstr(flags, "WEP")) return WSEC_WEP;
    if (strstr(flags, "WPA")) return WSEC_PSK;
    return WSEC_OPEN;
}

/* 0..3 bars, as iOS (3 arcs always drawn, the lit ones depend on the level) */
static int wlp_bars(int dbm)
{
    if (dbm >= -60) return 3;
    if (dbm >= -70) return 2;
    if (dbm >= -80) return 1;
    return 0;
}

/* "bssid / frequency / signal level / flags / ssid" lines. Keeps one entry per SSID (the
 * strongest), skips hidden networks, sorts by level (strongest first). Returns the count. */
static int wlp_scan_results(const char *txt, struct wl_net *out, int max)
{
    int n = 0;
    for (const char *l = txt; l && *l; ) {
        const char *nl = strchr(l, '\n');
        char bssid[18] = "", flags[160] = "";
        int freq = 0, level = 0;
        const char *t1 = strchr(l, '\t');
        if (t1 && (!nl || t1 < nl) && t1 - l == 17) {
            memcpy(bssid, l, 17); bssid[17] = 0;
            const char *p = t1 + 1;
            freq = atoi(p); p = strchr(p, '\t');
            if (p && (!nl || p < nl)) { level = atoi(++p); p = strchr(p, '\t'); }
            if (p && (!nl || p < nl)) {
                p++;
                const char *e = strchr(p, '\t');
                if (e && (!nl || e < nl)) {
                    int fl = (int)(e - p); if (fl > (int)sizeof flags - 1) fl = sizeof flags - 1;
                    memcpy(flags, p, (size_t)fl); flags[fl] = 0;
                    char ssid[33];
                    int sl = wlp_unescape(e + 1, ssid, sizeof ssid);
                    if (sl > 0) {
                        int k;
                        for (k = 0; k < n; k++) if (!strcmp(out[k].ssid, ssid)) break;
                        if (k < n) {
                            if (level > out[k].level) { out[k].level = level; out[k].freq = freq; wlp_cpy(out[k].bssid, sizeof out[k].bssid, bssid); }
                        } else if (n < max) {
                            memset(&out[n], 0, sizeof out[n]);
                            wlp_cpy(out[n].ssid, sizeof out[n].ssid, ssid);
                            wlp_cpy(out[n].bssid, sizeof out[n].bssid, bssid);
                            out[n].level = level; out[n].freq = freq; out[n].sec = wlp_sec(flags); out[n].saved_id = -1;
                            n++;
                        }
                    }
                }
            }
        }
        l = nl ? nl + 1 : 0;
    }
    for (int i = 1; i < n; i++) {                      /* insercion: pocas redes */
        struct wl_net t = out[i]; int j = i - 1;
        while (j >= 0 && out[j].level < t.level) { out[j + 1] = out[j]; j--; }
        out[j + 1] = t;
    }
    return n;
}

/* "network id / ssid / bssid / flags": id \t ssid \t bssid \t [CURRENT][DISABLED]... */
static int wlp_list_networks(const char *txt, struct wl_saved *out, int max)
{
    int n = 0;
    for (const char *l = txt; l && *l && n < max; ) {
        const char *nl = strchr(l, '\n');
        if (isdigit((unsigned char)*l)) {
            const char *t1 = strchr(l, '\t');
            if (t1 && (!nl || t1 < nl)) {
                memset(&out[n], 0, sizeof out[n]);
                out[n].id = atoi(l);
                wlp_unescape(t1 + 1, out[n].ssid, sizeof out[n].ssid);
                const char *t2 = strchr(t1 + 1, '\t'), *t3 = t2 ? strchr(t2 + 1, '\t') : 0;
                if (t3 && (!nl || t3 < nl)) {
                    char fl[80]; int k = 0; const char *p = t3 + 1;
                    while (p[k] && p[k] != '\n' && k < 79) { fl[k] = p[k]; k++; }
                    fl[k] = 0;
                    out[n].current = strstr(fl, "[CURRENT]") != 0;
                    out[n].disabled = strstr(fl, "[DISABLED]") != 0;
                    out[n].temp_disabled = strstr(fl, "[TEMP-DISABLED]") != 0;
                }
                n++;
            }
        }
        l = nl ? nl + 1 : 0;
    }
    return n;
}

/* reply of "GET_NETWORK <id> ssid": "\"name\"" or hex bytes; length or -1 */
static int wlp_ssid_value(const char *rep, char *out, int cap)
{
    if (!rep || !*rep || !strncmp(rep, "FAIL", 4)) return -1;
    if (*rep == '"') {
        const char *e = strrchr(rep + 1, '"'); if (!e) return -1;
        int n = (int)(e - rep - 1); if (n > cap - 1) n = cap - 1;
        memcpy(out, rep + 1, (size_t)n); out[n] = 0;
        return n;
    }
    int n = 0;
    for (const char *p = rep; wlp_hexv(p[0]) >= 0 && wlp_hexv(p[1]) >= 0; p += 2) {
        int c = wlp_hexv(p[0]) * 16 + wlp_hexv(p[1]);
        if (!c) return -1;
        if (n < cap - 1) out[n++] = (char)c;
    }
    out[n] = 0;
    return n ? n : -1;
}

/* SSID as hex for "SET_NETWORK <id> ssid <hex>" (no quoting problems with any byte) */
static void wlp_hex(const char *s, char *out, int cap)
{
    static const char hx[] = "0123456789abcdef";
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && n < cap - 2; p++) { out[n++] = hx[*p >> 4]; out[n++] = hx[*p & 15]; }
    out[n] = 0;
}

/* passphrase usable as psk "..." (8..63 characters, no control characters) */
static int wlp_psk_ok(const char *p)
{
    int n = (int)strlen(p);
    if (n == 64) { for (int i = 0; i < 64; i++) if (wlp_hexv(p[i]) < 0) return 0; return 2; }   /* raw PSK */
    if (n < 8 || n > 63) return 0;
    for (int i = 0; i < n; i++) if ((unsigned char)p[i] < 32 || p[i] == 127) return 0;
    return 1;
}

/* unsolicited control interface events */
enum { WE_NONE, WE_CONNECTED, WE_DISCONNECTED, WE_WRONG_KEY, WE_TEMP_DISABLED, WE_NOT_FOUND, WE_SCAN_RESULTS, WE_ASSOC_REJECT };
static int wlp_event(const char *m, int *id)
{
    if (id) *id = -1;
    if (*m == '<') { const char *e = strchr(m, '>'); if (e) m = e + 1; }
    const char *i = strstr(m, "id=");
    if (id && i) *id = atoi(i + 3);
    if (!strncmp(m, "CTRL-EVENT-CONNECTED", 20)) return WE_CONNECTED;
    if (!strncmp(m, "CTRL-EVENT-DISCONNECTED", 23)) return WE_DISCONNECTED;
    if (!strncmp(m, "CTRL-EVENT-SSID-TEMP-DISABLED", 29)) return strstr(m, "reason=WRONG_KEY") ? WE_WRONG_KEY : WE_TEMP_DISABLED;
    if (!strncmp(m, "CTRL-EVENT-NETWORK-NOT-FOUND", 28)) return WE_NOT_FOUND;
    if (!strncmp(m, "CTRL-EVENT-SCAN-RESULTS", 23)) return WE_SCAN_RESULTS;
    if (!strncmp(m, "CTRL-EVENT-ASSOC-REJECT", 23)) return WE_ASSOC_REJECT;
    if (strstr(m, "4-Way Handshake failed")) return WE_WRONG_KEY;
    return WE_NONE;
}

/* default gateway of "ifname" from /proc/net/route (little-endian hex); 1 if found */
static int wlp_route_gw(const char *txt, const char *ifname, char *out, int cap)
{
    for (const char *l = txt; l && *l; ) {
        char ifc[16]; unsigned dst, gw, flags;
        if (sscanf(l, "%15s %x %x %x", ifc, &dst, &gw, &flags) == 4 && !strcmp(ifc, ifname) && dst == 0 && (flags & 2)) {
            snprintf(out, (size_t)cap, "%u.%u.%u.%u", gw & 255, (gw >> 8) & 255, (gw >> 16) & 255, gw >> 24);
            return 1;
        }
        l = strchr(l, '\n'); if (l) l++;
    }
    if (cap > 0) out[0] = 0;
    return 0;
}

/* ------------------------------------------------------------------ bluez por dbus-send */
static const char *wlp_skipsp(const char *s) { while (*s == ' ' || *s == '\t') s++; return s; }
/* text between the first and the last double quote of the line [l, e) */
static int wlp_quoted(const char *l, const char *e, char *out, int cap)
{
    const char *a = memchr(l, '"', (size_t)(e - l)); if (!a) return 0;
    const char *b = e - 1; while (b > a && *b != '"') b--;
    if (b <= a) return 0;
    int n = (int)(b - a - 1); if (n > cap - 1) n = cap - 1;
    memcpy(out, a + 1, (size_t)n); out[n] = 0;
    return 1;
}
/* "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF" -> "AA:BB:CC:DD:EE:FF" (0 if it is not a device) */
static int wlp_dev_addr(const char *path, char *out)
{
    const char *d = strstr(path, "/dev_");
    if (!d || strlen(d + 5) != 17 || strchr(d + 5, '/')) return 0;
    for (int i = 0; i < 17; i++) out[i] = d[5 + i] == '_' ? ':' : d[5 + i];
    out[17] = 0;
    return 1;
}
static int wlp_audio_uuid(const char *u)
{
    /* 110b AudioSink, 110d A2DP, 1108/1112 headset, 111e/111f handsfree */
    return !strncasecmp(u, "0000110b", 8) || !strncasecmp(u, "0000110d", 8) || !strncasecmp(u, "00001108", 8) ||
           !strncasecmp(u, "0000111e", 8);
}
/* GetManagedObjects as printed by dbus-send --print-reply. Fills the first adapter (hci0) and
 * up to max devices of that adapter. Returns the number of devices. */
static int wlp_bluez_objects(const char *txt, struct wl_adapter *ad, struct wl_dev *dv, int max)
{
    enum { O_NONE, O_ADAPTER, O_DEV };
    int n = 0, obj = O_NONE, iface = 0, in_uuids = 0;   /* iface: 1 Adapter1, 2 Device1, 0 other */
    struct wl_dev *cur = 0;
    char key[64] = "", val[128];
    memset(ad, 0, sizeof *ad);
    for (const char *l = txt; l && *l; ) {
        const char *e = strchr(l, '\n'); if (!e) e = l + strlen(l);
        const char *s = wlp_skipsp(l);
        int len = (int)(e - s);
        if (len >= 12 && !strncmp(s, "object path ", 12)) {
            char p[96] = ""; wlp_quoted(s, e, p, sizeof p);
            obj = O_NONE; cur = 0; iface = 0; in_uuids = 0;
            char a[18];
            if (!strcmp(p, "/org/bluez/hci0")) { obj = O_ADAPTER; ad->found = 1; }
            else if (!strncmp(p, "/org/bluez/hci0/", 16) && wlp_dev_addr(p, a)) {
                int k;
                for (k = 0; k < n; k++) if (!strcmp(dv[k].path, p)) break;
                if (k == n && n >= max) {
                    /* full (busy places: dozens of nameless LE beacons, TVs, phones): reuse the
                     * slot of a nameless device that is neither paired nor connected, so a
                     * real one (headphones in pairing mode) is never left out */
                    for (int q = 0; q < n; q++) {
                        const struct wl_dev *o = &dv[q];
                        char a2[18]; memcpy(a2, o->addr, 18); for (int z = 0; z < 17; z++) if (a2[z] == ':') a2[z] = '-';
                        int unnamed = !o->name[0] || !strcmp(o->name, a2) || !strcmp(o->name, o->addr);
                        if (unnamed && !o->paired && !o->connected && o != cur) { k = q; break; }
                    }
                    if (k < n) {
                        memset(&dv[k], 0, sizeof dv[k]);
                        wlp_cpy(dv[k].path, sizeof dv[k].path, p);
                        memcpy(dv[k].addr, a, 18);
                    }
                } else if (k == n) {
                    memset(&dv[n], 0, sizeof dv[n]);
                    wlp_cpy(dv[n].path, sizeof dv[n].path, p);
                    memcpy(dv[n].addr, a, 18);
                    n++;
                }
                if (k < n) { cur = &dv[k]; obj = O_DEV; }
            }
        } else if (len >= 7 && !strncmp(s, "string ", 7)) {
            char q[128] = ""; wlp_quoted(s, e, q, sizeof q);
            if (in_uuids) { if (cur && wlp_audio_uuid(q)) cur->audio = 1; }
            else wlp_cpy(key, sizeof key, q);
        } else if (len >= 7 && !strncmp(s, "array [", 7) && len <= 8) {
            if (!strncmp(key, "org.", 4)) {
                iface = !strcmp(key, "org.bluez.Adapter1") ? 1 : !strcmp(key, "org.bluez.Device1") ? 2 : 0;
                key[0] = 0;
            }
        } else if (len >= 1 && *s == ']') {
            in_uuids = 0;
        } else if (len >= 8 && !strncmp(s, "variant ", 8)) {
            const char *v = wlp_skipsp(s + 8);
            int vb = !strncmp(v, "boolean true", 12);
            long vi = 0;
            const char *sp = strchr(v, ' ');
            if (sp && sp < e && (!strncmp(v, "int", 3) || !strncmp(v, "uint", 4) || !strncmp(v, "byte", 4))) vi = strtol(sp + 1, 0, 10);
            int isstr = !strncmp(v, "string ", 7);
            val[0] = 0; if (isstr) wlp_quoted(v, e, val, sizeof val);
            if (!strncmp(v, "array [", 7) && !strcmp(key, "UUIDs") && iface == 2) in_uuids = 1;
            if (obj == O_ADAPTER && iface == 1) {
                if (!strcmp(key, "Powered")) ad->powered = vb;
                else if (!strcmp(key, "Discovering")) ad->discovering = vb;
                else if (!strcmp(key, "Discoverable")) ad->discoverable = vb;
                else if (!strcmp(key, "Address") && isstr) wlp_cpy(ad->addr, sizeof ad->addr, val);
                else if (!strcmp(key, "Alias") && isstr) wlp_cpy(ad->alias, sizeof ad->alias, val);
            } else if (obj == O_DEV && cur && iface == 2) {
                if (!strcmp(key, "Name") && isstr) wlp_cpy(cur->name, sizeof cur->name, val);
                else if (!strcmp(key, "Alias") && isstr && !cur->name[0]) wlp_cpy(cur->name, sizeof cur->name, val);
                else if (!strcmp(key, "Icon") && isstr) wlp_cpy(cur->icon, sizeof cur->icon, val);
                else if (!strcmp(key, "Class")) cur->cls = (unsigned)vi;
                else if (!strcmp(key, "Paired")) cur->paired = vb;
                else if (!strcmp(key, "Trusted")) cur->trusted = vb;
                else if (!strcmp(key, "Blocked")) cur->blocked = vb;
                else if (!strcmp(key, "Connected")) cur->connected = vb;
                else if (!strcmp(key, "RSSI")) { cur->rssi = (int)vi; cur->has_rssi = 1; }
            }
        }
        l = *e ? e + 1 : 0;
    }
    for (int i = 0; i < n; i++) {
        /* bluez puts the address (with '-') as Alias when there is no name: treat as unnamed */
        char a2[18]; memcpy(a2, dv[i].addr, 18); for (int k = 0; k < 17; k++) if (a2[k] == ':') a2[k] = '-';
        if (!strcmp(dv[i].name, a2) || !strcmp(dv[i].name, dv[i].addr)) dv[i].name[0] = 0;
        if (!strncmp(dv[i].icon, "audio-", 6) || ((dv[i].cls >> 8) & 0x1F) == 4) dv[i].audio = 1;
    }
    return n;
}

/* dbus-send prints "Error org.bluez.Error.X: message" on failure; 1 and the error name */
static int wlp_dbus_error(const char *out, char *name, int cap)
{
    const char *e = strstr(out, "Error ");
    if (!e) { if (cap > 0) name[0] = 0; return 0; }
    e += 6;
    int n = 0; while (e[n] && e[n] != ':' && e[n] != ' ' && e[n] != '\n' && n < cap - 1) n++;
    memcpy(name, e, (size_t)n); name[n] = 0;
    return 1;
}
#endif
