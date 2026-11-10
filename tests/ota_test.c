/* ota_test.c - PC tests (x86, WSL) of src/ota_net.inc.c and src/lang/ota_tr.inc.c.
 * Built and driven by tests/ota_test.sh (fixtures from tests/ota_fixtures.py).
 *   ota_test unit FIXDIR            hashes, Ed25519, signatures, JSON, versions, tar, manifests,
 *                                   translation table
 *   ota_test op load|check|install|install-down|rollback [pause-at BYTES]
 *                                   runs one worker operation (paths from OTA_* variables) and
 *                                   prints "state=... err=... restart=N installed=V prev=P got=G" */
#define _GNU_SOURCE
#define OTA_TEST 1
#include <stdarg.h>
#include <stdio.h>
static void lg(const char *fmt, ...)
{
    va_list a; va_start(a, fmt);
    fprintf(stderr, "  [lg] "); vfprintf(stderr, fmt, a); fputc('\n', stderr);
    va_end(a);
}
#include "ota_net.inc.c"
enum { LANG_ES, LANG_EN, LANG_FR, LANG_DE, LANG_IT, LANG_JA, LANG_KO, LANG_PL, LANG_RU, LANG_ZHS, LANG_TH, LANG_ZHT, LANG_UK, LANG_N };
#include "lang/ota_tr.inc.c"

static int fails, passes;
#define CHECK(c, ...) do { if (c) passes++; else { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int unhex_any(const char *s, uint8_t *out, int cap)
{
    if (!strcmp(s, "-")) return 0;
    int n = (int)strlen(s) / 2; if (n > cap || ota_unhex(s, out, n)) return -1;
    return n;
}

/* ------------------------------------------------------------------ vectors.txt */
static void t_vectors(const char *dir)
{
    char p[600]; snprintf(p, sizeof p, "%s/vectors.txt", dir);
    FILE *f = fopen(p, "r"); CHECK(f, "no %s", p); if (!f) return;
    static char line[40000]; static uint8_t msg[8192];
    int nsha = 0, ned = 0, ngood = 0;
    while (fgets(line, sizeof line, f)) {
        char kind[16], a[20000], b[20000], c[200], d[8];
        int k = sscanf(line, "%15s %19999s %19999s %199s %7s", kind, a, b, c, d);
        if (k >= 3 && (!strcmp(kind, "sha256") || !strcmp(kind, "sha512"))) {
            int n = unhex_any(a, msg, sizeof msg); char hx[129];
            if (kind[3] == '2') { struct ota_sha256 s; uint8_t o[32]; ota_sha256_init(&s);
                /* in two pieces: exercises the buffering */
                ota_sha256_update(&s, msg, (size_t)n / 3); ota_sha256_update(&s, msg + n / 3, (size_t)(n - n / 3)); ota_sha256_final(&s, o); ota_hex(o, 32, hx); }
            else { struct ota_sha512 s; uint8_t o[64]; ota_sha512_init(&s);
                ota_sha512_update(&s, msg, (size_t)n / 2); ota_sha512_update(&s, msg + n / 2, (size_t)(n - n / 2)); ota_sha512_final(&s, o); ota_hex(o, 64, hx); }
            CHECK(!strcmp(hx, b), "%s of %d bytes: %s != %s", kind, n, hx, b);
            nsha++;
        } else if (k == 5 && !strcmp(kind, "ed")) {
            uint8_t pk[32], sig[64];
            int n = unhex_any(b, msg, sizeof msg);
            CHECK(!ota_unhex(a, pk, 32) && !ota_unhex(c, sig, 64) && n >= 0, "bad vector line");
            int ok = !ota_ed25519_verify(sig, msg, (size_t)n, pk), want = atoi(d);
            CHECK(ok == want, "ed25519 pk %.16s... msg %d bytes: got %d want %d", a, n, ok, want);
            ned++; ngood += want;
        }
    }
    fclose(f);
    printf("  vectors: %d hashes, %d ed25519 (%d valid, %d forged)\n", nsha, ned, ngood, ned - ngood);
    CHECK(nsha >= 40 && ned >= 100, "too few vectors");
}

/* ------------------------------------------------------------------ json/ */
static void t_json(const char *dir)
{
    char p[600], kp[600]; uint8_t keys[2][32]; char hx[80] = "";
    for (int i = 0; i < 2; i++) {
        snprintf(kp, sizeof kp, "%s/pub%d.hex", dir, i + 1);
        FILE *k = fopen(kp, "r"); CHECK(k && fgets(hx, sizeof hx, k) && !ota_unhex(hx, keys[i], 32), "pub%d", i + 1); if (k) fclose(k);
    }
    snprintf(p, sizeof p, "%s/json/expect.txt", dir);
    FILE *f = fopen(p, "r"); CHECK(f, "no %s", p); if (!f) return;
    char line[1200]; int n = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        char name[100], sg[8], lang[16] = "", cl[800] = ""; int parse;
        int k = sscanf(line, "%99s %7s %d %15s %799[^\n]", name, sg, &parse, lang, cl);
        if (k < 3) continue;
        char fp[800]; snprintf(fp, sizeof fp, "%s/json/%s", dir, name);
        size_t len; char *b = ota_read_file(fp, OTA_JSON_MAX, &len); CHECK(b, "read %s", name); if (!b) continue;
        int got = ota_sig_check_keys(b, len, keys, 1);
        if (!strcmp(sg, "R")) {
            CHECK(got == OTA_SIG_BAD, "%s: with key 1 only: %d", name, got);
            CHECK(ota_sig_check_keys(b, len, keys, 2) == OTA_SIG_OK, "%s: rotation (keys 1+2) must accept", name);
        } else CHECK(got == atoi(sg), "%s: signature %d, want %s", name, got, sg);
        CHECK(ota_sig_check_keys(b, len, keys, 0) == OTA_SIG_NOKEY, "%s: no keys -> NOKEY", name);
        struct ota_info in; int pr = ota_parse(b, &in);
        CHECK(pr == parse, "%s: parse %d, want %d", name, pr, parse);
        if (!strcmp(name, "good.json") && !pr) {
            CHECK(!strcmp(in.version, "0.9.1") && !strcmp(in.min_version, "0.6.0") && in.size == 123456 && !in.needs_firmware && in.restart == OTA_RS_SHELL,
                  "good.json fields: %s %s %lld %d %d", in.version, in.min_version, in.size, in.needs_firmware, in.restart);
            CHECK(!strcmp(in.name, "hibyos-payload-0.9.1.tar") && !strncmp(in.url, "https://github.com/", 19) && strlen(in.sha256) == 64, "good.json payload");
        }
        if (!strcmp(name, "upper_sha.json") && !pr) CHECK(!strcmp(in.sha256, "abababababababababababababababababababababababababababababababab"), "sha256 lower-cased");
        if (!strcmp(name, "restart_none.json") && !pr) CHECK(in.restart == OTA_RS_NONE, "restart none");
        if (lang[0]) {
            char out[800]; ota_changelog(b, lang, out, sizeof out);
            CHECK(!strcmp(out, cl), "%s: changelog[%s] = '%s', want '%s'", name, lang, out, cl);
        }
        free(b); n++;
    }
    fclose(f);
    printf("  json: %d documents\n", n);
    /* truncation is reported, never overflows */
    char small[6]; int r = oj_str("\"abcdefghij\"", small, sizeof small);
    CHECK(r == -2 && !strcmp(small, "abcde"), "oj_str truncation: %d '%s'", r, small);
    CHECK(oj_str("\"a\\u00e9\\\"\"", small, sizeof small) == 4 && !strcmp(small, "a\xc3\xa9\""), "oj_str escapes");
    CHECK(!oj_valid("{\"a\":") && !oj_valid("{\"a\":1,}") && !oj_valid("[1]") && oj_valid(" {\"a\":[1,2,{\"b\":null}]} \n"), "oj_valid");
    CHECK(!oj_valid("{\"a\":\"x\ny\"}"), "control characters inside strings are refused");
}

/* ------------------------------------------------------------------ versions */
static void t_versions(void)
{
    static const struct { const char *a, *b; int c; } V[] = {
        { "0.6.1", "0.6.0", 1 }, { "0.6.0", "0.6.1", -1 }, { "0.6.0", "0.6.0", 0 }, { "0.6.10", "0.6.9", 1 },
        { "1.0", "1.0.0", 0 }, { "1.0.0-beta", "1.0.0", -1 }, { "1.0.0", "1.0.0-rc.1", 1 }, { "1.0.0-beta.2", "1.0.0-beta.1", 1 },
        { "2.0.0", "10.0.0", -1 }, { "0.7.0", "0.6.99", 1 }, { "", "0.1", -1 },
    };
    for (unsigned i = 0; i < sizeof V / sizeof V[0]; i++) {
        int c = ota_vercmp(V[i].a, V[i].b);
        CHECK(c == V[i].c, "vercmp(%s, %s) = %d, want %d", V[i].a, V[i].b, c, V[i].c);
    }
    CHECK(ota_ver_ok("0.6.1") && ota_ver_ok("1.2.3-beta.1") && ota_ver_ok("10.0"), "ver_ok good");
    CHECK(!ota_ver_ok("1") && !ota_ver_ok("1.") && !ota_ver_ok(".1") && !ota_ver_ok("1..2") && !ota_ver_ok("1.2-") && !ota_ver_ok("1.2 ") && !ota_ver_ok("v1.2"), "ver_ok bad");
}

/* ------------------------------------------------------------------ tar/ */
static int count_files(const char *d)
{
    int n = 0; DIR *dd = opendir(d); struct dirent *e;
    while (dd && (e = readdir(dd))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char c[900]; snprintf(c, sizeof c, "%s/%s", d, e->d_name);
        struct stat s; if (!lstat(c, &s)) n += S_ISDIR(s.st_mode) ? count_files(c) : 1;
    }
    if (dd) closedir(dd);
    return n;
}
static void t_tar(const char *dir)
{
    char p[600], work[600]; snprintf(p, sizeof p, "%s/tar/expect.txt", dir);
    snprintf(work, sizeof work, "%s/tarwork", dir);
    FILE *f = fopen(p, "r"); CHECK(f, "no %s", p); if (!f) return;
    char name[100]; int want, n = 0;
    while (fscanf(f, "%99s %d", name, &want) == 2) {
        char tp[800], dest[800], evil[900];
        snprintf(tp, sizeof tp, "%s/tar/%s", dir, name);
        ota_rm_rf(work); ota_mkdirs(work);
        snprintf(dest, sizeof dest, "%s/dest", work); ota_mkdirs(dest);
        volatile double fr = 0;
        int rc = ota_tar_extract(tp, dest, "payload/", &fr, 0);
        CHECK(rc == want, "%s: rc %d, want %d", name, rc, want);
        snprintf(evil, sizeof evil, "%s/evil", work);
        CHECK(!ota_exists(evil) && !ota_exists("/payload"), "%s: wrote outside the destination", name);
        snprintf(evil, sizeof evil, "%s/other", dest);
        CHECK(!ota_exists(evil), "%s: entry outside payload/ extracted", name);
        if (!strcmp(name, "good.tar")) {
            char a[900]; snprintf(a, sizeof a, "%s/payload/a/b/c.txt", dest);
            size_t l; char *b = ota_read_file(a, 100, &l);
            CHECK(b && l == 5 && !memcmp(b, "hello", 5), "good.tar content"); free(b);
            snprintf(a, sizeof a, "%s/payload/bin/hibyshell", dest); CHECK(ota_fsize(a) == 4 + 256 * 9, "good.tar size");
            snprintf(a, sizeof a, "%s/payload/deep/er/c.txt", dest); CHECK(ota_fsize(a) == 1, "ustar prefix field");
            snprintf(a, sizeof a, "%s/payload/empty", dest); CHECK(ota_fsize(a) == 0, "empty file");
            CHECK(fr > 0.99, "progress reaches 1 (%f)", fr);
            CHECK(count_files(dest) == 5, "good.tar: %d files", count_files(dest));
        }
        if (!strcmp(name, "garbage_after_end.tar")) { char a[900]; snprintf(a, sizeof a, "%s/payload/late", dest); CHECK(!ota_exists(a), "nothing after the end blocks"); }
        n++;
    }
    fclose(f);
    ota_rm_rf(work);
    printf("  tar: %d archives\n", n);
    volatile int cancel = 1; char tp[800], dest[800];
    snprintf(tp, sizeof tp, "%s/tar/good.tar", dir); snprintf(dest, sizeof dest, "%s/tarwork", dir); ota_mkdirs(dest);
    CHECK(ota_tar_extract(tp, dest, "payload/", 0, &cancel) == -2, "cancel");
    ota_rm_rf(dest);
}

/* ------------------------------------------------------------------ manifests -> restart */
static void wfile(const char *p, const char *s) { FILE *f = fopen(p, "w"); if (f) { fputs(s, f); fclose(f); } }
static void t_restart(const char *dir)
{
    char a[600], b[600]; snprintf(a, sizeof a, "%s/m_old", dir); snprintf(b, sizeof b, "%s/m_new", dir);
    const char *base = "11111111111111111111111111111111  VERSION\n22222222222222222222222222222222  bin/hibyshell\n"
                       "33333333333333333333333333333333  bin/hibymod_disp.so\n44444444444444444444444444444444  shell/home_btn.ov\n"
                       "55555555555555555555555555555555  theme/layout/theme3/x.view\n66666666666666666666666666666666  Pictures/A.png\n";
    wfile(a, base);
    struct { const char *nw; int k; const char *what; } C[] = {
        { "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  VERSION\n22222222222222222222222222222222  bin/hibyshell\n33333333333333333333333333333333  bin/hibymod_disp.so\n44444444444444444444444444444444  shell/home_btn.ov\n55555555555555555555555555555555  theme/layout/theme3/x.view\n66666666666666666666666666666666  Pictures/A.png\n", OTA_RS_NONE, "only VERSION" },
        { "11111111111111111111111111111111  VERSION\nbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb  bin/hibyshell\n33333333333333333333333333333333  bin/hibymod_disp.so\n44444444444444444444444444444444  shell/home_btn.ov\n55555555555555555555555555555555  theme/layout/theme3/x.view\n66666666666666666666666666666666  Pictures/A.png\n", OTA_RS_SHELL, "shell binary" },
        { "11111111111111111111111111111111  VERSION\n22222222222222222222222222222222  bin/hibyshell\ncccccccccccccccccccccccccccccccc  bin/hibymod_disp.so\n44444444444444444444444444444444  shell/home_btn.ov\n55555555555555555555555555555555  theme/layout/theme3/x.view\n66666666666666666666666666666666  Pictures/A.png\n", OTA_RS_REBOOT, "display library" },
        { "11111111111111111111111111111111  VERSION\n22222222222222222222222222222222  bin/hibyshell\n33333333333333333333333333333333  bin/hibymod_disp.so\n44444444444444444444444444444444  shell/home_btn.ov\n66666666666666666666666666666666  Pictures/A.png\n", OTA_RS_REBOOT, "theme file removed" },
        { "11111111111111111111111111111111  VERSION\n22222222222222222222222222222222  bin/hibyshell\n33333333333333333333333333333333  bin/hibymod_disp.so\n44444444444444444444444444444444  shell/home_btn.ov\n55555555555555555555555555555555  theme/layout/theme3/x.view\n66666666666666666666666666666666  Pictures/A.png\n77777777777777777777777777777777  shell/new.ov\n88888888888888888888888888888888  Pictures/B.png\n", OTA_RS_SHELL, "new shell asset + wallpaper" },
        { "11111111111111111111111111111111  VERSION\n22222222222222222222222222222222  bin/hibyshell\n33333333333333333333333333333333  bin/hibymod_disp.so\n44444444444444444444444444444444  shell/home_btn.ov\n55555555555555555555555555555555  theme/layout/theme3/x.view\n66666666666666666666666666666666  Pictures/A.png\n99999999999999999999999999999999  sys/hibyos_install.sh\n", OTA_RS_NONE, "installer override" },
        { "11111111111111111111111111111111  VERSION\n22222222222222222222222222222222  bin/hibyshell\n33333333333333333333333333333333  bin/hibymod_disp.so\n44444444444444444444444444444444  shell/home_btn.ov\n55555555555555555555555555555555  theme/layout/theme3/x.view\n66666666666666666666666666666666  Pictures/A.png\n99999999999999999999999999999999  sys/hiby_player.sh\n", OTA_RS_REBOOT, "supervisor override" },
    };
    for (unsigned i = 0; i < sizeof C / sizeof C[0]; i++) {
        wfile(b, C[i].nw);
        int k = ota_restart_kind(a, b);
        CHECK(k == C[i].k, "restart (%s): %d, want %d", C[i].what, k, C[i].k);
    }
    char missing[600]; snprintf(missing, sizeof missing, "%s/m_missing", dir);
    CHECK(ota_restart_kind(missing, b) == OTA_RS_REBOOT, "no old manifest -> reboot");
    unlink(a); unlink(b);
}

/* ------------------------------------------------------------------ translations */
static int specs(const char *s, char *out, int cap)
{
    int n = 0;
    for (; *s; s++) if (*s == '%') { s++; if (!*s) break; if (n < cap - 1) out[n++] = *s; }
    out[n] = 0; return n;
}
static void t_i18n(void)
{
    int rows = (int)(sizeof OTR / sizeof OTR[0]);
    for (int i = 0; i < rows; i++) {
        char want[16], got[16]; specs(OTR[i][1 + LANG_EN], want, sizeof want);
        for (int l = 0; l < LANG_N; l++) {
            const char *v = OTR[i][1 + l];
            CHECK(v && *v, "OTR %s: language %d empty", OTR[i][0], l);
            if (!v) continue;
            specs(v, got, sizeof got);
            CHECK(!strcmp(got, want), "OTR %s lang %d: formats '%s' vs en '%s'", OTR[i][0], l, got, want);
            char buf[1200];                                       /* formats with the real arguments */
            if (strchr(want, 'd')) snprintf(buf, sizeof buf, v, 30); else snprintf(buf, sizeof buf, v, "1.2 MB", "4.0 MB");
            CHECK(strlen(buf) > 0, "OTR %s: empty after formatting", OTR[i][0]);
        }
        if (strncmp(OTR[i][0], "OTA_", 4)) CHECK(!strcmp(OTR[i][0], OTR[i][1]), "OTR %s: key must be the Spanish text", OTR[i][0]);
        for (int j = 0; j < i; j++) CHECK(strcmp(OTR[i][0], OTR[j][0]), "OTR duplicate key %s", OTR[i][0]);
    }
    printf("  translations: %d strings x %d languages\n", rows, LANG_N);
}

/* ------------------------------------------------------------------ worker */
static const char *SN[] = { "IDLE", "CHECKING", "UPTODATE", "AVAILABLE", "NEWER", "DOWNLOADING", "PAUSED", "VERIFYING", "EXTRACTING", "INSTALLING", "RESTORING", "DONE", "ERROR" };
static const char *EN[] = { "NONE", "NOKEY", "NOSD", "NET", "SIG", "FORMAT", "SHA", "SPACE_SD", "SPACE_DATA", "BATT", "TAR", "INSTALL", "IO", "MINVER", "FIRMWARE", "DOWNGRADE", "NOPREV", "NONET" };
static void wait_idle(long long pause_at)
{
    int seen[16] = { 0 };
    while (ota_st.busy) {
        seen[ota_st.state & 15] = 1;
        if (pause_at > 0 && ota_st.state == OS_DOWNLOADING && ota_st.got >= pause_at) { ota_cancel(); pause_at = 0; }
        usleep(2000);
    }
    fprintf(stderr, "  states seen:");
    for (int i = 0; i < 13; i++) if (seen[i]) fprintf(stderr, " %s", SN[i]);
    fprintf(stderr, "\n");
}
static int run_op(int argc, char **argv)
{
    const char *op = argv[2]; long long pause_at = 0;
    if (argc > 4 && !strcmp(argv[3], "pause-at")) pause_at = atoll(argv[4]);
    ota_paths_init(); ota_versions_refresh(); ota_conf_load();
    if (strcmp(op, "check")) { ota_start(OTA_OP_LOAD); wait_idle(0); }   /* like opening the page */
    if (!strcmp(op, "check")) ota_start(OTA_OP_CHECK);
    else if (!strcmp(op, "install") || !strcmp(op, "install-down")) { ota_st.allow_down = !strcmp(op, "install-down"); ota_start(OTA_OP_INSTALL); }
    else if (!strcmp(op, "rollback")) ota_start(OTA_OP_ROLLBACK);
    else if (strcmp(op, "load")) { fprintf(stderr, "unknown op %s\n", op); return 3; }
    wait_idle(pause_at);
    printf("state=%s err=%s restart=%d installed=%s prev=%s got=%lld\n", SN[ota_st.state], EN[ota_st.err], ota_st.restart,
           ota_st.installed[0] ? ota_st.installed : "-", ota_st.prev[0] ? ota_st.prev : "-", ota_st.got);
    pthread_mutex_lock(&ota_st.mu); free(ota_st.json); ota_st.json = 0; pthread_mutex_unlock(&ota_st.mu);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "op")) return run_op(argc, argv);
    if (argc < 3 || strcmp(argv[1], "unit")) { fprintf(stderr, "usage: ota_test unit FIXDIR | op OPERATION\n"); return 3; }
    t_vectors(argv[2]);
    t_json(argv[2]);
    t_versions();
    t_tar(argv[2]);
    t_restart(argv[2]);
    t_i18n();
    uint8_t k[2][32];
    CHECK(ota_keys(k) == 1, "the test build has exactly one compiled key");
    printf("%d passed, %d failed\n", passes, fails);
    return fails != 0;
}
