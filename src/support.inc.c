/* support.inc.c - enlaces del proyecto en Ajustes > General > Informacion.
 *
 * Cada enlace (el repositorio y las donaciones) es una fila; al tocarla se abre una pagina con
 * su codigo QR para escanearlo con el movil: el reproductor no tiene navegador de fiar ni
 * falta que hace. Para anadir un enlace basta una linea en SUP_LINKS (y, si es de donaciones,
 * que vaya despues del repositorio): la fila y su QR salen solos. Incluido desde sysset.inc.c. */
#include "qr.inc.c"

struct sup_link { const char *name, *val, *url; };
static const struct sup_link SUP_LINKS[] = {
    { "GitHub",          "Grohle/hibyos", "https://github.com/Grohle/hibyos" },      /* [0]: el proyecto */
    { "Buy Me a Coffee", "grohle",        "https://buymeacoffee.com/grohle" },       /* [1..]: donaciones */
};
#define SUP_N ((int)(sizeof SUP_LINKS / sizeof SUP_LINKS[0]))
#define SUP_MAX 7                               /* filas de donacion que caben en una seccion (8) */

static int sup_cur;
static struct qr sup_qr; static int sup_qr_for = -1;
#define SUP_CARD 236
#define SUP_HERO 352
static void sup_hero(uint16_t *d, int y, int h)
{
    (void)h;
    const struct sup_link *l = &SUP_LINKS[sup_cur];
    if (sup_qr_for != sup_cur) sup_qr_for = qr_encode(l->url, (int)strlen(l->url), &sup_qr, -1) ? sup_cur : -1;
    int cx = (W - SUP_CARD) / 2, cy = y + 6;
    fill_rrect(d, cx, cy, SUP_CARD, SUP_CARD, 22, 0xFFFF, 255);                    /* tarjeta blanca: la zona de silencio */
    if (sup_qr_for == sup_cur) {
        int n = sup_qr.n, px = SUP_CARD / (n + 8), o = (SUP_CARD - n * px) / 2;
        for (int my = 0; my < n; my++) for (int mx = 0; mx < n; mx++) {
            if (!sup_qr.m[my][mx]) continue;
            int x0 = cx + o + mx * px, y0 = cy + o + my * px;
            for (int yy = y0 < 0 ? 0 : y0; yy < y0 + px && yy < H; yy++) for (int xx = x0; xx < x0 + px; xx++) d[yy * W + xx] = 0;
        }
    }
    const char *u = strstr(l->url, "://"); u = u ? u + 3 : l->url;                 /* sin "https://" */
    int sz = 15; while (sz > 11 && tw_n(u, sz, -1) > W - 40) sz--;
    int tw = tw_n(u, sz, -1); if (tw > W - 40) tw = W - 40;
    draw_text(d, (W - tw) / 2, cy + SUP_CARD + 30, u, sz, 0xFFFF, W - 40, NAVH + 30, H);
    wrap_cw = W - 64; draw_wrapped(d, 32, cy + SUP_CARD + 44, W - 64, tr("QR_HELP"), 12, rgb(142, 142, 147), 16, 1); wrap_cw = 0;
}
static void page_sup(struct spage *p)
{
    *p = (struct spage){ SUP_LINKS[sup_cur].name };
    p->hero = SUP_HERO; p->hero_fn = sup_hero;
    p->nsec = 0;
}
static void sup_open(int i) { if (i < 0 || i >= SUP_N) return; sup_cur = i; open_page(page_sup, "Información"); }
/* las filas no llevan argumento: una funcion por enlace */
#define SUP_ROW(i) static void sup_nav##i(void) { sup_open(i); } static const char *sup_val##i(void) { return i < SUP_N ? SUP_LINKS[i].val : ""; }
SUP_ROW(0) SUP_ROW(1) SUP_ROW(2) SUP_ROW(3) SUP_ROW(4) SUP_ROW(5) SUP_ROW(6) SUP_ROW(7)
#undef SUP_ROW
static void (*const sup_nav[8])(void) = { sup_nav0, sup_nav1, sup_nav2, sup_nav3, sup_nav4, sup_nav5, sup_nav6, sup_nav7 };
static const char *(*const sup_val[8])(void) = { sup_val0, sup_val1, sup_val2, sup_val3, sup_val4, sup_val5, sup_val6, sup_val7 };
static struct srow sup_row(int i) { return (struct srow){ SUP_LINKS[i].name, R_NAV, 0, 0, 0, sup_nav[i], sup_val[i], 0, 0, 0, 0, 0 }; }
/* seccion "Apoyar el proyecto" de Informacion: los enlaces de donacion (del 1 en adelante) */
static int sup_donate_sec(struct ssec *sc)
{
    int n = SUP_N - 1; if (n > SUP_MAX) n = SUP_MAX;
    if (n <= 0) return 0;
    *sc = (struct ssec){ "APOYAR EL PROYECTO", "DONATE_HELP", { { 0 } }, n };
    for (int i = 0; i < n; i++) sc->rows[i] = sup_row(i + 1);
    return 1;
}
