/* i18n_test.c - pruebas en PC (x86, WSL) de los textos de la interfaz en los 13 idiomas.
 * Compilar/ejecutar: bash tests/i18n_test.sh  (usa las fuentes del firmware de ~/hiby/rootfs)
 * Comprueba:
 *   - cada clave de TR tiene texto no vacio en los 13 idiomas (sin recurrir a la reserva);
 *   - las tablas lang/<idioma>.inc.c no tienen claves inexistentes ni repetidas;
 *   - los %s/%d de cada traduccion coinciden con los del original;
 *   - todos los caracteres tienen glifo en la fuente principal o en una de reserva;
 *   - los mensajes del instalador (hpk_msg) existen en los 13 idiomas;
 *   - (aviso) etiquetas cortas cuya traduccion mide a 15 px mas de 1,25x la referencia, que es
 *     la mas ancha entre la inglesa y la espanola original (la interfaz se diseno para esta). */
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
#define HPK_TEST 1
#include "hpk.h"
#include "hpk.inc.c"

static int fails;
#define FAIL(...) do { fails++; printf("FALLO: "); printf(__VA_ARGS__); printf("\n"); } while (0)

static int is_id(const char *k) { for (; *k; k++) if (!((*k >= 'A' && *k <= 'Z') || *k == '_')) return 0; return 1; }
static int is_long_id(const char *k) { return is_id(k) && strchr(k, '_'); }
/* secuencia de especificadores de formato ("sd" para "%s ... %d") */
static void fmt_seq(const char *s, char *o, int n)
{
    int k = 0;
    for (; *s && k < n - 1; s++) if (*s == '%' && s[1]) { if (strchr("sdiufxXcpl", s[1])) o[k++] = s[1]; if (s[1] == '%' || strchr("sdiufxXcpl", s[1])) s++; }
    o[k] = 0;
}
static int in_tr(const char *k) { for (unsigned i = 0; i < sizeof TR / sizeof TR[0]; i++) if (!strcmp(TR[i][0], k)) return 1; return 0; }

int main(void)
{
    (void)ui_lang_sys; (void)kb_cfg_height; (void)kb_cfg_numrow; (void)kb_cfg_layout; (void)kb_cfg_layouts;
    if (!text_init()) { printf("no se pudo cargar default.otf (TEXT_FONT_DIRS)\n"); return 2; }
    unsigned ntr = sizeof TR / sizeof TR[0];
    int nkeys = 0, nwarn = 0, nmiss_gl = 0;
    /* claves repetidas en TR (gana la primera) */
    for (unsigned i = 0; i < ntr; i++) for (unsigned j = 0; j < i; j++)
        if (!strcmp(TR[i][0], TR[j][0])) { printf("aviso: clave repetida en TR: \"%s\" (gana la primera)\n", TR[i][0]); break; }
    for (unsigned i = 0; i < ntr; i++) {
        const char *k = TR[i][0]; int dup = 0;
        for (unsigned j = 0; j < i; j++) if (!strcmp(TR[j][0], k)) dup = 1;
        if (dup) continue;
        nkeys++;
        if (is_long_id(k) && !tr_es_long(k)) FAIL("[es] falta el texto largo de %s", k);
        const char *en = tr_lang(k, LANG_EN);
        char f0[16], f1[16]; fmt_seq(is_long_id(k) ? (en ? en : "") : k, f0, sizeof f0);
        int wen = en ? text_width(en, 15) : 0, wes = text_width(tr_lang(k, LANG_ES), 15);
        if (wes > wen) wen = wes;
        for (int l = 0; l < LANG_N; l++) {
            const char *v = tr_lang(k, l);
            if (!v || !*v) { FAIL("[%s] sin traduccion: \"%s\"", LANG_CODES[l], k); continue; }
            fmt_seq(v, f1, sizeof f1);
            if (strcmp(f0, f1)) FAIL("[%s] formato distinto (%s / %s): \"%s\" -> \"%s\"", LANG_CODES[l], f0, f1, k, v);
            const char *s = v; int c;
            while ((c = utf8_next(&s))) {
                if (c <= ' ' || c == 0xA0 || c == 0x202F) continue;
                int gi; font_glyph(c, &gi);
                if (!gi) { FAIL("[%s] sin glifo U+%04X en \"%s\"", LANG_CODES[l], c, v); nmiss_gl++; }
            }
            if (!is_long_id(k) && wen > 0 && wen < 250 && l != LANG_EN && l != LANG_ES) {
                int w = text_width(v, 15);
                if (w > wen * 1.25 && w > wen + 6) { nwarn++; printf("ancho [%s] %3d px vs ref %3d px (x%.2f): \"%s\"\n", LANG_CODES[l], w, wen, (double)w / wen, v); }
            }
        }
    }
    /* nombres de las apps bajo el icono (12,5 px): como mucho lo que ya mide «Reproduciendo» */
    static const char *app_names[] = { "Música", "Ajustes", "Inalámbrico", "Libros", "DAC USB", "Reproduciendo", "Archivos", "Radio", "Personalizar", "Apps", 0 };
    for (int i = 0; app_names[i]; i++) for (int l = 0; l < LANG_N; l++) {
        const char *v = tr_lang(app_names[i], l); if (!v || !*v) continue;
        int w = text_width(v, 12.5f);
        if (w > 90) { nwarn++; printf("icono [%s] %3d px > 90 px: \"%s\"\n", LANG_CODES[l], w, v); }
    }
    /* Ajustes del sistema (lang/settings_tr.inc.c): 13 idiomas en una fila */
    for (unsigned i = 0; i < sizeof TRS / sizeof TRS[0]; i++) {
        const char *k = TRS[i][0]; char f0[16], f1[16];
        for (unsigned j = 0; j < i; j++) if (!strcmp(TRS[j][0], k)) FAIL("[TRS] clave repetida: \"%s\"", k);
        if (in_tr(k)) printf("aviso: \"%s\" esta en TR y en TRS (gana TRS)\n", k);
        fmt_seq(TRS[i][1], f0, sizeof f0);
        int wref = text_width(TRS[i][1], 15), wen = text_width(TRS[i][2], 15); if (wen > wref) wref = wen;
        for (int l = 0; l < LANG_N; l++) {
            const char *v = TRS[i][1 + l];
            if (!v || !*v) { FAIL("[%s] TRS sin traduccion: \"%s\"", LANG_CODES[l], k); continue; }
            if (strcmp(tr_lang(k, l), v)) FAIL("[%s] tr_lang no usa TRS para \"%s\"", LANG_CODES[l], k);
            fmt_seq(v, f1, sizeof f1);
            if (strcmp(f0, f1)) FAIL("[%s] formato distinto (%s / %s): \"%s\" -> \"%s\"", LANG_CODES[l], f0, f1, k, v);
            const char *s = v; int c;
            while ((c = utf8_next(&s))) {
                if (c <= ' ' || c == 0xA0 || c == 0x202F) continue;
                int gi; font_glyph(c, &gi);
                if (!gi) { FAIL("[%s] sin glifo U+%04X en \"%s\"", LANG_CODES[l], c, v); nmiss_gl++; }
            }
            if (!is_long_id(k) && wref < 250 && l > LANG_EN) {
                int w = text_width(v, 15);
                if (w > wref * 1.25 && w > wref + 6) { nwarn++; printf("ancho [%s] %3d px vs ref %3d px (x%.2f): \"%s\"\n", LANG_CODES[l], w, wref, (double)w / wref, v); }
            }
        }
        nkeys++;
    }
    /* tablas por idioma: claves que no existen o repetidas */
    for (int l = 0; l < LANG_N; l++) {
        if (!TRX[l]) continue;
        int n = 0;
        for (const struct trp *p = TRX[l]; p->k; p++, n++) {
            if (!in_tr(p->k)) FAIL("[%s] clave que no esta en TR: \"%s\"", LANG_CODES[l], p->k);
            for (const struct trp *q = TRX[l]; q < p; q++) if (!strcmp(q->k, p->k)) { FAIL("[%s] clave repetida: \"%s\"", LANG_CODES[l], p->k); break; }
        }
        printf("tabla %-8s %d entradas\n", LANG_CODES[l], n);
    }
    /* mensajes del instalador */
    for (int e = 0; e < HPK_E_COUNT; e++) for (int l = 0; l < LANG_N; l++) {
        const char *k = hpk_errkey(e), *v = hpk_msg(k, l);
        if (!v || !*v || !strcmp(v, k)) FAIL("[%s] hpk_msg(%s) vacio", LANG_CODES[l], k);
    }
    static const char *st[] = { "st.verifying", "st.installing", "st.icons", "st.installed", "st.notinst", "st.update", "st.install", "st.current",
                                "st.older", "st.uninstall", "st.uninstalling", "st.uninstalled", "st.keepdata", "st.damaged", "st.stopplayer", "st.noapps", 0 };
    for (int i = 0; st[i]; i++) for (int l = 0; l < LANG_N; l++) {
        const char *v = hpk_msg(st[i], l);
        if (!v || !*v || !strcmp(v, st[i])) FAIL("[%s] hpk_msg(%s) vacio", LANG_CODES[l], st[i]);
        const char *s = v; int c;
        while ((c = utf8_next(&s))) { int gi; if (c > ' ') { font_glyph(c, &gi); if (!gi) FAIL("[%s] sin glifo U+%04X en \"%s\"", LANG_CODES[l], c, v); } }
    }
    if (strcmp(hpk_msg("err.space", 99), hpk_msg("err.space", HPK_LANG_EN))) FAIL("hpk_msg con idioma fuera de rango no cae en ingles");
    /* reservas: idioma fuera de rango y clave desconocida */
    ui_lang = LANG_N + 3; if (strcmp(tr("Música"), "Music")) FAIL("tr() con idioma fuera de rango no cae en ingles");
    ui_lang = LANG_DE; if (strcmp(tr("clave inexistente"), "clave inexistente")) FAIL("tr() con clave desconocida no devuelve la clave");
    /* nombres de idioma, fechas y mapa del reproductor */
    int seen[LANG_N] = { 0 };
    for (int i = 0; i < 13; i++) { if (!lang_ok(HIBY_LANG[i]) || seen[HIBY_LANG[i]]++) FAIL("HIBY_LANG no es una permutacion"); }
    for (int l = 0; l < LANG_N; l++) {
        char d[64]; ui_lang = l; lang_date(1767398400, d, sizeof d);     /* 3 ene 2026 */
        const char *s = LANG_NAMES[l]; int c;
        while ((c = utf8_next(&s))) { int gi; font_glyph(c, &gi); if (!gi) FAIL("sin glifo U+%04X en el nombre %s", c, LANG_NAMES[l]); }
        s = d; while ((c = utf8_next(&s))) { int gi; if (c > ' ') { font_glyph(c, &gi); if (!gi) FAIL("sin glifo U+%04X en la fecha %s", c, d); } }
        printf("fecha %-8s %-22s nombre %s\n", LANG_CODES[l], d, LANG_NAMES[l]);
    }
    printf("fuentes de reserva mapeadas:");
    for (int i = 0; i < FONT_NFB; i++) printf(" %s=%s", font_fb[i].file, font_fb[i].st > 0 ? "si" : font_fb[i].st < 0 ? "FALTA" : "no usada");
    printf("\n%d claves x %d idiomas; %d avisos de ancho; %d glifos ausentes; %d fallos\n", nkeys, LANG_N, nwarn, nmiss_gl, fails);
    return fails ? 1 : 0;
}
