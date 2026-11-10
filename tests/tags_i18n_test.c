/* tags_i18n_test.c - PC check of lang/tags_tr.inc.c (editor de etiquetas): every key has a
 * non-empty text in the 13 languages, tr_lang() returns it, same %s/%d sequence as the Spanish
 * text, and every character has a glyph in the firmware fonts. Every tr("...") / N_("...")
 * literal of src/tageditor.inc.c must resolve in es, en and de (list in argv[1]).
 * Run: bash tests/tags_i18n_test.sh */
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
    (void)ui_lang_sys; (void)kb_cfg_height; (void)kb_cfg_numrow; (void)kb_cfg_layout;
    if (!text_init()) { printf("no se pudo cargar default.otf (TEXT_FONT_DIRS)\n"); return 2; }
    unsigned n = sizeof TGTR / sizeof TGTR[0];
    int nwarn = 0;
    for (unsigned i = 0; i < n; i++) {
        const char *k = TGTR[i][0]; char f0[16], f1[16];
        for (unsigned j = 0; j < i; j++) if (!strcmp(TGTR[j][0], k)) FAIL("clave repetida: \"%s\"", k);
        fmt_seq(TGTR[i][1], f0, sizeof f0);
        int wref = text_width(TGTR[i][1], 15), wen = text_width(TGTR[i][2], 15); if (wen > wref) wref = wen;
        for (int l = 0; l < LANG_N; l++) {
            const char *v = TGTR[i][1 + l];
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
            if (strncmp(k, "TAGS_", 5) && wref < 250 && l > LANG_EN) {
                int w = text_width(v, 15);
                if (w > wref * 1.35 && w > wref + 10) { nwarn++; printf("ancho [%s] %3d px vs ref %3d px: \"%s\"\n", LANG_CODES[l], w, wref, v); }
            }
        }
    }
    if (argc > 1) {
        FILE *f = fopen(argv[1], "r"); char l[512]; int nk = 0;
        while (f && fgets(l, sizeof l, f)) {
            l[strcspn(l, "\n")] = 0; if (!*l) continue; nk++;
            const char *es = tr_lang(l, LANG_ES), *en = tr_lang(l, LANG_EN), *de = tr_lang(l, LANG_DE);
            if (!en || !*en || !de || !*de) FAIL("clave sin traducir: \"%s\"", l);
            if (!es || !*es) FAIL("clave sin texto en espanol: \"%s\"", l);
        }
        if (f) fclose(f);
        printf("%d claves usadas en tageditor.inc.c comprobadas\n", nk);
    }
    printf("%u claves x %d idiomas en TGTR; %d avisos de ancho; %d fallos\n", n, LANG_N, nwarn, fails);
    return fails != 0;
}
