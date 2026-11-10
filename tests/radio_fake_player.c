/* radio_fake_player.c - stands in for the shell + hiby_player in tests/radio_helper_test.sh: waits
 * for the helper's state file to say st=ready, opens the announced file and reads it like the
 * stock engine does (fread at the stream's rate, a small read-ahead), opens the new file when the
 * helper announces a new generation (what the shell does by sending the list again), then pauses
 * (file open, no reads), then closes it and stays alive (so only "the file was closed" tells the
 * helper that other music started).
 *   radio_fake_player STATE RATE READ_S PAUSE_S [OTHER]
 * OTHER: an audio file opened (and kept open) when it closes ours: the user picked other music.
 * Prints "opened FILE gen=N" per generation, one "t=<s> pos=<bytes> phase=<read|pause>" line per
 * second and "closed gens=N". */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
/* gen of a ready state (0 = not ready); its file in file */
static int ready_gen(const char *state, char *file, int cap)
{
    FILE *f = fopen(state, "r"); if (!f) return 0;
    char l[1024]; int ready = 0, gen = 0; file[0] = 0;
    while (fgets(l, sizeof l, f)) {
        l[strcspn(l, "\n")] = 0;
        if (!strcmp(l, "st=ready")) ready = 1;
        if (!strncmp(l, "gen=", 4)) gen = atoi(l + 4);
        if (!strncmp(l, "file=", 5) && strlen(l + 5) < (size_t)cap) memcpy(file, l + 5, strlen(l + 5) + 1);
    }
    fclose(f);
    return ready && file[0] ? gen : 0;
}
int main(int argc, char **argv)
{
    if (argc < 5) return 2;
    const char *state = argv[1]; int rate = atoi(argv[2]); double rd = atof(argv[3]), ps = atof(argv[4]);
    char file[512]; double t0 = now(); int gen;
    while (!(gen = ready_gen(state, file, sizeof file))) { if (now() - t0 > 20) { printf("never ready\n"); return 1; } usleep(100000); }
    FILE *f = fopen(file, "rb"); if (!f) { printf("cannot open %s\n", file); return 1; }
    printf("opened %s gen=%d\n", file, gen); fflush(stdout);
    double t1 = now(), tg = t1, last = 0, tchk = 0; long long pos = 0, total = 0; char buf[4096]; int gens = 1;
    while (now() - t1 < rd + ps) {
        double t = now() - t1;
        if (t - tchk > 0.3) {                                   /* the shell: a new generation -> new list */
            tchk = t; char nf[512]; int g = ready_gen(state, nf, sizeof nf);
            if (g && g != gen) {
                FILE *n = fopen(nf, "rb");
                if (n) { fclose(f); f = n; gen = g; gens++; pos = 0; tg = now(); snprintf(file, sizeof file, "%s", nf); printf("opened %s gen=%d\n", file, gen); fflush(stdout); }
            }
        }
        if (t < rd && pos < (long long)((now() - tg) * rate) + 32 * 1024) {
            size_t r = fread(buf, 1, sizeof buf, f);
            if (r > 0) { pos += r; total += r; } else { clearerr(f); usleep(20000); }
        } else usleep(20000);
        if (t - last >= 1) { last = t; printf("t=%.0f pos=%lld phase=%s\n", t, pos, t < rd ? "read" : "pause"); fflush(stdout); }
    }
    FILE *o = argc > 5 ? fopen(argv[5], "rb") : 0;
    fclose(f);
    printf("closed gens=%d total=%lld%s\n", gens, total, o ? " other" : ""); fflush(stdout);
    sleep(12);
    if (o) fclose(o);
    return 0;
}
