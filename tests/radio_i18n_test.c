/* radio_i18n_test.c - PC check of lang/radio_tr.inc.c (app Radio): every key has a
 * non-empty text in the 13 languages, tr_lang() returns it, same %s/%d sequence as the Spanish
 * text, and every character has a glyph in the firmware fonts. Every tr("...") literal used in
 * src/radio*.inc.c must resolve in all 13 languages (from any table).
 * Run: bash tests/radio_test.sh */
#define _GNU_SOURCE
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define W 320
#define H 480
struct sprite { int w, h; uint16_t *rgb; uint8_t *a; };
#include "i18n.inc.c"
#include "text.inc.c"

static int fails;
#define FAIL(...) do { fails++; printf("FALLO: "); printf(__VA_ARGS__); printf("\n"); } while (0)
static void fmt_seq(const char *s, char *o, int n)
{
    int k = 0;
    for (; *s && k < n - 1; s++) if (*s == '%' && s[1]) { if (strchr("sdiufxXcpl", s[1])) o[k++] = s[1]; if (s[1] == '%' || strchr("sdiufxXcpl", s[1])) s++; }
    o[k] = 0;
}
int main(int argc, char **argv)
{
    (void)ui_lang_sys; (void)kb_cfg_height; (void)kb_cfg_numrow; (void)kb_cfg_layout; (void)kb_cfg_layouts;
    if (!text_init()) { printf("no se pudo cargar default.otf (TEXT_FONT_DIRS)\n"); return 2; }
    unsigned n = sizeof RATR / sizeof RATR[0];
    for (unsigned i = 0; i < n; i++) {
        const char *k = RATR[i][0]; char f0[16], f1[16];
        for (unsigned j = 0; j < i; j++) if (!strcmp(RATR[j][0], k)) FAIL("clave repetida: \"%s\"", k);
        fmt_seq(RATR[i][1], f0, sizeof f0);
        for (int l = 0; l < LANG_N; l++) {
            const char *v = RATR[i][1 + l];
            if (!v || !*v) { FAIL("[%s] sin traduccion: \"%s\"", LANG_CODES[l], k); continue; }
            const char *got = tr_lang(k, l);
            if (!got) FAIL("[%s] tr_lang sin texto para \"%s\"", LANG_CODES[l], k);
            else if (strcmp(got, v) && l == LANG_EN) printf("aviso: \"%s\" ya existe en otra tabla (se usa \"%s\")\n", k, got);
            fmt_seq(v, f1, sizeof f1);
            if (strcmp(f0, f1)) FAIL("[%s] formato distinto (%s / %s): \"%s\"", LANG_CODES[l], f0, f1, k);
            const char *s = v; int c;
            while ((c = utf8_next(&s))) {
                if (c <= ' ' || c == 0xA0 || c == 0x202F) continue;
                int gi; font_glyph(c, &gi);
                if (!gi) FAIL("[%s] sin glifo U+%04X en \"%s\"", LANG_CODES[l], c, v);
            }
        }
    }
    /* claves usadas en radio*.inc.c (una por linea en argv[1]) */
    if (argc > 1) {
        FILE *f = fopen(argv[1], "r"); char l[512]; int nk = 0;
        while (f && fgets(l, sizeof l, f)) {
            l[strcspn(l, "\n")] = 0; if (!*l) continue; nk++;
            for (int k = 0; k < LANG_N; k++) {
                const char *v = tr_lang(l, k);
                if (!v || !*v) FAIL("[%s] clave sin traducir: \"%s\"", LANG_CODES[k], l);
                else if (k != LANG_ES && !strncmp(l, "RA_", 3) && !strcmp(v, l)) FAIL("[%s] id sin texto: \"%s\"", LANG_CODES[k], l);
            }
        }
        if (f) fclose(f);
        printf("%d claves usadas en radio*.inc.c comprobadas\n", nk);
    }
    printf("%u claves x %d idiomas en RATR; %d fallos\n", n, LANG_N, fails);
    return fails != 0;
}
