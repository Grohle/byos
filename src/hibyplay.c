/* hibyplay - prueba de nowplaying.inc.c y playctl.inc.c (lo que suena + control)
 *   hibyplay status [-w MS]      pista, estado, posicion (con -w repite cada MS ms)
 *   hibyplay raw                 volcado de hm_state.np_* (libreria v5)
 *   hibyplay toggle|play|pause|next|prev
 *   hibyplay cover [RUTA] OUT.raw  caratula 128x128 RGB565 LE (por defecto, la que suena)
 * Compilar: tools/build_hibyplay.sh (musl estatico, hard-float). */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hm_shared.h"
#define HT_API static
#define HIBYTAG_NO_DB
#include "hibytag.c"
#include "nowplaying.inc.c"
#include "playctl.inc.c"

static const char *srcname(int s)
{
    return s == NP_SRC_SHIM ? "shim" : s == NP_SRC_PROC ? "proc" : s == NP_SRC_LAST ? "user.ini" : "none";
}

static void show(void)
{
    struct np_info i;
    if (!np_get(&i)) { printf("nada\n"); return; }
    printf("src=%s %s%s pos=%d/%d ms rate=%d bits=%d ch=%d id=%08x\n  %s\n  title=%s | artist=%s | album=%s\n",
           srcname(i.source), i.playing ? "PLAYING" : "paused", i.stopped ? " (stopped)" : "",
           i.position_ms, i.duration_ms, i.rate, i.bits, i.channels, i.track_id, i.path, i.title, i.artist, i.album);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    if (argc < 2) goto usage;
    const char *c = argv[1];
    if (!strcmp(c, "status")) {
        int w = argc >= 4 && !strcmp(argv[2], "-w") ? atoi(argv[3]) : 0;
        do { show(); if (w) usleep(w * 1000); } while (w);
        return 0;
    }
    if (!strcmp(c, "raw")) {
        int f = open(HM_STATE, O_RDONLY); if (f < 0) { perror(HM_STATE); return 1; }
        struct hm_state *s = mmap(0, sizeof *s, PROT_READ, MAP_SHARED, f, 0);
        if (s == MAP_FAILED) { perror("mmap"); return 1; }
        printf("np_magic=%08x (%s) gen=%u now=%u\n", s->np_magic, s->np_magic == HM_NP_MAGIC ? "v5" : "sin v5", s->np_gen, np_ms());
        if (s->np_magic != HM_NP_MAGIC) return 0;
        printf("pcm open=%u frames=%u last=%u rate=%u bits=%u ch=%u drops=%u(@%u) prepares=%u paused=%u\n",
               s->pcm_open, s->pcm_frames, s->pcm_last_ms, s->pcm_rate, s->pcm_bits, s->pcm_channels,
               s->pcm_drops, s->pcm_drop_ms, s->pcm_prepares, s->pcm_paused);
        printf("rt tid=%u prio=%u err=%u count=%u\n", s->rt_tid, s->rt_prio, s->rt_err, s->rt_count);
        for (int k = 0; k < HM_NP_SLOTS; k++) {
            const struct hm_np_open *e = &s->np_open[k];
            if (!e->gen) continue;
            printf("[%d] gen=%u fd=%d tid=%u open=%u close=%u fr0=%u seeks=%u last=%llu@%u %s\n", k, e->gen, e->fd, e->tid,
                   e->open_ms, e->close_ms, e->frames_at_open, e->nseeks,
                   (unsigned long long)e->seek_hi << 32 | e->seek_lo, e->seek_ms, e->path);
        }
        return 0;
    }
    if (!strcmp(c, "cover") && argc >= 3) {
        const char *in = argc >= 4 ? argv[2] : 0, *out = argv[argc - 1];
        struct np_info i;
        if (!in) { if (!np_get(&i)) { fprintf(stderr, "no suena nada\n"); return 1; } in = i.path; }
        static uint16_t px[NP_COVER * NP_COVER];
        if (np_cover(in, px)) { fprintf(stderr, "sin caratula: %s\n", in); return 1; }
        FILE *f = fopen(out, "wb"); if (!f) { perror(out); return 1; }
        fwrite(px, 2, NP_COVER * NP_COVER, f); fclose(f);
        printf("%s -> %s\n", in, out);
        return 0;
    }
    int r = 1;
    if (!strcmp(c, "toggle")) r = pc_toggle();
    else if (!strcmp(c, "play")) r = pc_play();
    else if (!strcmp(c, "pause")) r = pc_pause();
    else if (!strcmp(c, "next")) r = pc_next();
    else if (!strcmp(c, "prev")) r = pc_prev();
    else goto usage;
    if (r < 0) { fprintf(stderr, "%s: %s\n", c, strerror(-r)); return 1; }
    if (r == 1) printf("%s: ya estaba asi\n", c);
    return 0;
usage:
    fprintf(stderr, "uso: hibyplay status [-w MS] | raw | toggle | play | pause | next | prev | cover [RUTA] OUT.raw\n");
    return 2;
}
