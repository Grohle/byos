/* hibysetctl - device test tool for hibyset.inc.c (hibymod_disp.so v7, command 'S').
 *   hibysetctl check                 signatures + settings loaded? (calls nothing in the player)
 *   hibysetctl list                  every option with its current value
 *   hibysetctl get NAME              one value
 *   hibysetctl set NAME VAL [force]  apply like the stock Control Centre; prints the value now
 *   hibysetctl raw                   last response file as is
 * Exit code 0 = ok. Names: bt wifi dlna airplay hibylink gain mseb lineout usbmode balance
 * gapless fade filter sleep sleepmin playmode volume (volmax, speed: read only).
 * Build: tools/build_hibysetctl.sh (static musl, hard-float). */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hm_shared.h"
#include "hibyset.inc.c"

static const char *ename(int rc)
{
    switch (rc) {
    case 0: return "ok";
    case -ENXIO: return "ENXIO (library v7 not loaded in hiby_player)";
    case -ETIMEDOUT: return "ETIMEDOUT (no answer; older library or busy)";
    case -ENOEXEC: return "ENOEXEC (different firmware: nothing was called)";
    case -EAGAIN: return "EAGAIN (settings not loaded / refused or busy)";
    case -ENOENT: return "ENOENT (unknown option)";
    case -ERANGE: return "ERANGE";
    case -EPERM: return "EPERM (not allowed now)";
    case -EACCES: return "EACCES (needs 'force')";
    case -EROFS: return "EROFS (read only)";
    default: return "error";
    }
}

static int run(int id, int show_extra)
{
    struct hs_res r;
    char ex[1024] = "";
    int rc = hs_wait_ex(id, &r, ex, sizeof ex, HS_TIMEOUT_MS);
    if (rc == -ETIMEDOUT || id < 0) { printf("rc=%d %s\n", rc, ename(rc)); return 1; }
    printf("rc=%d key=%s val=%d ms=%d %s  [%s]\n", r.rc, r.key, r.val, r.ms, r.msg, ename(r.rc));
    if (show_extra) fputs(ex, stdout);
    return r.rc ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) goto usage;
    if (!strcmp(argv[1], "check")) return run(hs_write("check", 0, 0, 0), 0);
    if (!strcmp(argv[1], "list")) return run(hs_write("list", 0, 0, 0), 1);
    if (!strcmp(argv[1], "get") && argc == 3) return run(hs_get_send(argv[2]), 0);
    if (!strcmp(argv[1], "set") && (argc == 4 || argc == 5)) {
        char *end; long v = strtol(argv[3], &end, 0);
        if (*end || v < -1000 || v > 1000) { fprintf(stderr, "bad value\n"); return 2; }
        int fl = argc == 5 && !strcmp(argv[4], "force") ? HS_FORCE : 0;
        if (argc == 5 && !fl) goto usage;
        return run(hs_set_send(argv[2], (int)v, fl), 0);
    }
    if (!strcmp(argv[1], "raw")) {
        FILE *f = fopen(HS_RES, "r");
        if (!f) { perror(HS_RES); return 1; }
        char b[1024]; size_t n = fread(b, 1, sizeof b, f); fclose(f);
        fwrite(b, 1, n, stdout);
        return 0;
    }
usage:
    fprintf(stderr, "usage: hibysetctl check | list | get NAME | set NAME VAL [force] | raw\n");
    return 2;
}
