/* nowui.inc.c - "Reproduciendo" propio de Musica (Apple Music de iOS 17/18) y el menu
 * contextual de iOS para canciones y albumes. Incluido justo despues de music.inc.c.
 *
 * Reproduciendo (nu_open, desde el mini reproductor): hoja a pantalla completa que sube desde
 * el mini reproductor siguiendo al dedo (o con un muelle al tocarlo; la caratula vuela de la
 * miniatura a su sitio) y se cierra arrastrando hacia abajo (1:1, decide la velocidad al
 * soltar). Caratula grande con sombra que se encoge en pausa (muelle con un poco de rebote),
 * fondo sacado de la caratula (desenfocado y oscurecido, UNA vez por pista), titulo, artista,
 * progreso con tiempo transcurrido / restante, anterior / reproducir-pausa / siguiente con la
 * respuesta al toque de iOS, letra sincronizada (.lrc junto a la cancion) y el menu "...".
 * No hay API para saltar a un punto de la cancion ni para "poner a continuacion": la barra de
 * progreso no se arrastra y el menu no lo ofrece.
 *
 * Capas (todo se calcula al cambiar algo; cada fotograma es casi solo memcpy):
 *   nu.bg     fondo 320x480 (caratula desenfocada)                 al cambiar de pista
 *   nu.page   fondo + barra de estado + textos y botones fijos       al cambiar de pista o de modo
 *   nu.art    caratula 232x232 (la del album en la biblioteca, o la de 128 ampliada)
 *   nu.shm    sombra de la caratula (alfa, 276x276)                  una vez
 *   img_sheet la hoja completa sin caratula (para arrastrar/abrir/cerrar: compose_sheet + caratula)
 *   img_under la pantalla de Musica de debajo
 * En reposo solo se repinta (present_rows) la franja del progreso, 1 vez por segundo, o la de
 * la letra cuando cambia de verso.
 *
 * Menu contextual (nu_menu): fondo desenfocado (cc_blur del fotograma actual, una vez), tarjeta
 * de la fila "levantada" y lista redondeada con iconos al estilo SF Symbols. Se abre con escala
 * y fundido (muelle) y se puede elegir arrastrando el dedo sin soltarlo, como en iOS. */

#define NU_HM (H - MU_MINI_H)           /* borde superior del mini reproductor */
#define NU_S MU_BS                      /* caratula grande (232, la de la pagina del album) */
#define NU_AY 50                        /* y de la caratula grande */
#define NU_PAUSE 0.80f                  /* escala de la caratula en pausa */
#define NU_SP 22                        /* margen de la sombra */
#define NU_SHD 8                        /* desplazamiento de la sombra hacia abajo */
#define NU_TY 324                       /* linea base del titulo */
#define NU_RY 346                       /* linea base del artista */
#define NU_BY 366                       /* barra de progreso */
#define NU_TMY 386                      /* tiempos */
#define NU_CY 422                       /* controles */
#define NU_LBX 52                       /* boton de la letra (abajo a la izquierda) */
#define NU_LBY 466
#define NU_SA 52                        /* caratula pequena (modo letra) */
#define NU_SAX 20
#define NU_SAY 46
/* franjas que se repintan por separado (no se solapan) */
#define NU_AB0 30
#define NU_AB1 304                      /* caratula grande y su sombra (el titulo empieza en ~309) */
#define NU_SB1 108                      /* caratula pequena (modo letra) */
#define NU_LY0 108
#define NU_LY1 354                      /* letra */
#define NU_PB0 354
#define NU_PB1 392                      /* progreso y tiempos */
#define NU_CB0 392
#define NU_CB1 451                      /* controles (con el circulo de pulsacion) */
#define NU_SM (NU_S + 2 * NU_SP)


struct nu_line { int t, off, y, h; };
static struct {
    uint16_t *bg, *page, *art; uint8_t *shm;
    int art_src;                          /* 0 hueco, 1 de 128 ampliada, 2 la nitida de 232 */
    struct np_info ni; uint16_t cov[128 * 128]; int cov_ok; uint32_t cov_sum;
    int trk, alb;                         /* en la biblioteca (-1 si no esta) */
    uint16_t avg, c_sub, c_trk, c_fill;   /* colores sacados del fondo */
    int playing, hold; double hold_t;     /* estado optimista tras tocar reproducir/pausa */
    float sc, sv;                         /* escala de la caratula (muelle) y su velocidad */
    int raw_pos, anc; double anc_t; int shown_sec;
    int pressed;                          /* boton pulsado (0 anterior, 1 play, 2 siguiente) */
    int lyr; float lyt;                   /* modo letra y su transicion (0 grande, 1 pequena) */
    double pend_t; uint32_t pend_id;      /* pista nueva esperando su caratula */
    /* letra */
    char *lbuf; struct nu_line *ll; int nl, synced, lcur, ltot; float lsc, lsc0, lsc1; double lst;
    int loff;                             /* [offset:] de la letra en ms (positivo = antes) */
} nu;
#define NU_LEAD_MS 11                   /* medio fotograma del panel (44 Hz): el verso cambia centrado en su marca */
static int nu_xm[NU_S * 2];

/* ------------------------------------------------------------------ utilidades */
static float nu_clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
/* muelle amortiguado (como spring(response:dampingFraction:) de SwiftUI); dt en segundos */
static void nu_spring(float *x, float *v, float target, float response, float damp, float dt)
{
    float w = 6.2831853f / response, k = w * w, c = 2 * damp * w;
    if (dt > 0.05f) dt = 0.05f;
    while (dt > 0) { float h = dt > 0.004f ? 0.004f : dt; float a = -k * (*x - target) - c * *v; *v += a * h; *x += *v * h; dt -= h; }
}
static int nu_spring_done(float x, float v, float target, float eps) { return fabsf(x - target) < eps && fabsf(v) < eps * 20; }
/* circulo suavizado recortado a las filas [y0, y1) */
static void nu_disc(uint16_t *d, float cx, float cy, float r, uint16_t col, int alpha, int y0, int y1)
{
    int ya = (int)(cy - r - 1), yb = (int)(cy + r + 2); if (ya < y0) ya = y0; if (yb > y1) yb = y1;
    for (int y = ya; y < yb; y++) {
        if (y < 0 || y >= H) continue;
        for (int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++) {
            if (x < 0 || x >= W) continue;
            float dx = x + .5f - cx, dy = y + .5f - cy, c = r + .5f - sqrtf(dx * dx + dy * dy);
            if (c <= 0) continue; if (c > 1) c = 1;
            d[y * W + x] = mix565(col, d[y * W + x], (unsigned)(alpha * c));
        }
    }
}
/* anillo de grosor th; sides: solo los arcos de izquierda y derecha (ondas de radio) */
static void nu_ring(uint16_t *d, float cx, float cy, float r, float th, uint16_t col, int sides)
{
    for (int y = (int)(cy - r - th); y <= (int)(cy + r + th); y++) {
        if (y < 0 || y >= H) continue;
        for (int x = (int)(cx - r - th); x <= (int)(cx + r + th); x++) {
            if (x < 0 || x >= W) continue;
            float dx = x + .5f - cx, dy = y + .5f - cy;
            if (sides && fabsf(dx) < fabsf(dy) * 1.1f) continue;
            float c = th / 2 + .5f - fabsf(sqrtf(dx * dx + dy * dy) - r);
            if (c <= 0) continue; if (c > 1) c = 1;
            d[y * W + x] = mix565(col, d[y * W + x], (unsigned)(255 * c));
        }
    }
}
/* triangulo suavizado: base vertical en xb y punta a w px (dir 1 derecha, -1 izquierda) */
static void nu_tri(uint16_t *d, float xb, float cy, float w, float h, int dir, uint16_t col)
{
    float hh = h / 2, acc[72];
    int xa = (int)floorf(dir > 0 ? xb : xb - w) - 1, xe = (int)ceilf(dir > 0 ? xb + w : xb) + 1, nx = xe - xa;
    if (nx > 72) nx = 72;
    for (int y = (int)floorf(cy - hh) - 1; y <= (int)ceilf(cy + hh) + 1; y++) {
        if (y < 0 || y >= H) continue;
        for (int i = 0; i < nx; i++) acc[i] = 0;
        for (int s = 0; s < 4; s++) {                          /* 4 sub-filas: bordes inclinados suaves */
            float yy = y + (s + .5f) / 4, dy = fabsf(yy - cy); if (dy >= hh) continue;
            float len = w * (1 - dy / hh), a = dir > 0 ? xb : xb - len, b = dir > 0 ? xb + len : xb;
            for (int i = 0; i < nx; i++) { float x0 = (float)(xa + i), o = (b < x0 + 1 ? b : x0 + 1) - (a > x0 ? a : x0); if (o > 0) acc[i] += o; }
        }
        for (int i = 0; i < nx; i++) {
            int x = xa + i; if (acc[i] <= 0 || x < 0 || x >= W) continue;
            float c = acc[i] / 4; if (c > 1) c = 1;
            d[y * W + x] = mix565(col, d[y * W + x], (unsigned)(255 * c));
        }
    }
}
/* mascara de cobertura de un rectangulo redondeado (w x h, radio r) */
static uint8_t *nu_mask_new(int w, int h, float r)
{
    uint8_t *m = malloc(w * h); if (!m) return 0;
    memset(m, 255, w * h);
    int ri = (int)ceilf(r);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        if ((y >= ri && y < h - ri) || (x >= ri && x < w - ri)) continue;
        float px = x + .5f, py = y + .5f;
        float cx = px < r ? r : px > w - r ? w - r : px, cy = py < r ? r : py > h - r ? h - r : py;
        float c = r + .5f - sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy));
        m[y * w + x] = c <= 0 ? 0 : c >= 1 ? 255 : (uint8_t)(c * 255);
    }
    return m;
}
static void nu_fmt(char *b, int n, int ms, int neg)
{
    int s = ms < 0 ? 0 : ms / 1000;
    snprintf(b, n, "%s%d:%02d", neg ? "-" : "", s / 60, s % 60);
}
/* lo que publica el hilo de nowplay; *cov_for = la caratula es de esta pista */
static void nu_snap(struct np_info *o, uint16_t *cov, int *cov_ok, int *cov_for)
{
    pthread_mutex_lock(&npx_mu);
    *o = npx; *cov_for = npx_cov_track == npx.track_id;
    if (cov && *cov_for) memcpy(cov, npx_cov, sizeof npx_cov);
    if (cov_ok) *cov_ok = npx_cov_ok;
    pthread_mutex_unlock(&npx_mu);
}
/* que el hilo vuelva a leer etiquetas y caratula (tras editarlas o buscar otra) */
static void nu_npx_refresh(void)
{
    pthread_mutex_lock(&npx_mu); npx_cov_track = 0xffffffffu; np_mc.key[0] = 0; pthread_mutex_unlock(&npx_mu);
}
/* teclas del reproductor en otro hilo: pc_key espera 80 ms y la animacion no se para */
static void *nu_key_thread(void *a) { int k = (int)(intptr_t)a; if (k == 0) pc_prev(); else if (k == 1) pc_toggle(); else pc_next(); return 0; }
static void nu_key(int k)
{
    pthread_t th;
    if (!pthread_create(&th, 0, nu_key_thread, (void *)(intptr_t)k)) pthread_detach(th); else nu_key_thread((void *)(intptr_t)k);
}
/* perfil de una animacion: dibujo (sin la espera del panel) y fotogramas */
struct nu_prof { double t0, acc, mx; int n; };
static void nu_prof_add(struct nu_prof *p, double t) { if (!p->n) p->t0 = now_ms() - t; p->acc += t; if (t > p->mx) p->mx = t; p->n++; }
static void nu_prof_log(struct nu_prof *p, const char *what)
{
    if (p->n) lg("np: %s %d fot en %.0f ms (%.1f fps), dibujo media %.1f max %.1f ms", what, p->n, now_ms() - p->t0, p->n * 1000.0 / (now_ms() - p->t0 + 1), p->acc / p->n, p->mx);
    memset(p, 0, sizeof *p);
}

/* ------------------------------------------------------------------ capas */
/* fondo: rejilla 6x8 de medias de la caratula, saturada y oscurecida, ampliada con
 * interpolacion suave y tramado (sin bandas en RGB565). Una vez por pista. */
static void nu_make_bg(void)
{
    double t0 = now_ms();
    float g[8][6][3], sr = 0, sg = 0, sb = 0;
    for (int gy = 0; gy < 8; gy++) for (int gx = 0; gx < 6; gx++) {
        float r = 58, gg = 58, b = 64;
        if (nu.cov_ok) {
            unsigned ar = 0, ag = 0, ab = 0;
            for (int y = gy * 16; y < gy * 16 + 16; y += 2) for (int x = 16 + gx * 16; x < 32 + gx * 16; x += 2) {
                uint16_t p = nu.cov[y * 128 + x]; ar += p >> 11; ag += (p >> 5) & 63; ab += p & 31;
            }
            r = ar * 255.f / (64 * 31); gg = ag * 255.f / (64 * 63); b = ab * 255.f / (64 * 31);
        }
        float l = .299f * r + .587f * gg + .114f * b;
        r = l + (r - l) * 1.3f; gg = l + (gg - l) * 1.3f; b = l + (b - l) * 1.3f;   /* mas color (iOS) */
        float l2 = .299f * r + .587f * gg + .114f * b, k = l2 > 118 ? 118 / l2 : 1;  /* portadas claras: que el texto blanco se lea */
        k *= 0.66f;
        g[gy][gx][0] = nu_clampf(r * k, 0, 255); g[gy][gx][1] = nu_clampf(gg * k, 0, 255); g[gy][gx][2] = nu_clampf(b * k, 0, 255);
        sr += g[gy][gx][0]; sg += g[gy][gx][1]; sb += g[gy][gx][2];
    }
    nu.avg = rgb((int)(sr / 48), (int)(sg / 48), (int)(sb / 48));
    nu.c_sub = blend565(nu.avg, 0xFFFF, 19); nu.c_trk = blend565(nu.avg, 0xFFFF, 8); nu.c_fill = blend565(nu.avg, 0xFFFF, 27);
    static const int bay[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
    static short x0[W]; static float xt[W];
    for (int x = 0; x < W; x++) {
        float f = (x + .5f) * 6 / W - .5f; if (f < 0) f = 0; if (f > 5) f = 5;
        int i = (int)f; if (i > 4) i = 4; float t = f - i; x0[x] = (short)i; xt[x] = t * t * (3 - 2 * t);
    }
    for (int y = 0; y < H; y++) {
        float f = (y + .5f) * 8 / H - .5f; if (f < 0) f = 0; if (f > 7) f = 7;
        int i = (int)f; if (i > 6) i = 6; float t = f - i; t = t * t * (3 - 2 * t);
        float v = 1.f - 0.28f * y / H, row[6][3];               /* abajo algo mas oscuro */
        for (int gx = 0; gx < 6; gx++) for (int c = 0; c < 3; c++) row[gx][c] = (g[i][gx][c] * (1 - t) + g[i + 1][gx][c] * t) * v;
        uint16_t *d = nu.bg + y * W;
        for (int x = 0; x < W; x++) {
            int a = x0[x]; float u = xt[x], dt = bay[y & 3][x & 3] / 16.f;
            int r = (int)((row[a][0] * (1 - u) + row[a + 1][0] * u) / 8.225f + dt);
            int gg = (int)((row[a][1] * (1 - u) + row[a + 1][1] * u) / 4.048f + dt);
            int b = (int)((row[a][2] * (1 - u) + row[a + 1][2] * u) / 8.225f + dt);
            d[x] = (uint16_t)((r > 31 ? 31 : r) << 11 | (gg > 63 ? 63 : gg) << 5 | (b > 31 ? 31 : b));
        }
    }
    lg("np: fondo en %.1f ms", now_ms() - t0);
}
/* Caratula nitida sin la biblioteca (Reproduciendo abierto desde el gesto, el Centro de
 * control o el icono, con Musica cerrada): un hilo (nice 10) la saca de la pista con la misma
 * cache de Musica (mu_cover_make, por carpeta del fichero) y la interfaz la cambia al llegar. */
static pthread_mutex_t nu_bc_mu = PTHREAD_MUTEX_INITIALIZER;
static uint16_t nu_bc_px[NU_S * NU_S];
static char nu_bc_path[600]; static uint32_t nu_bc_trk, nu_bc_want; static volatile int nu_bc_ready, nu_bc_busy;
static void *nu_bc_thread(void *a)
{
    (void)a; thread_nice(10);
    for (;;) {
        pthread_mutex_lock(&nu_bc_mu); char path[600]; snprintf(path, sizeof path, "%s", nu_bc_path); uint32_t id = nu_bc_want; pthread_mutex_unlock(&nu_bc_mu);
        uint16_t *px = malloc(NU_S * NU_S * 2), *tmp = malloc(MU_GS * MU_GS * 2);
        int r = px && tmp ? mu_cover_make(path, MC_B, px, tmp) : -1;
        pthread_mutex_lock(&nu_bc_mu);
        if (!r && id == nu_bc_want) { memcpy(nu_bc_px, px, sizeof nu_bc_px); nu_bc_trk = id; nu_bc_ready = 1; }
        int again = id != nu_bc_want;                 /* cambio de pista mientras tanto */
        if (!again) nu_bc_busy = 0;
        pthread_mutex_unlock(&nu_bc_mu);
        free(px); free(tmp);
        if (!again) break;
    }
    return 0;
}
static void nu_bc_request(const char *path, uint32_t id)
{
    if (!path[0]) return;
    pthread_mutex_lock(&nu_bc_mu);
    if (nu_bc_ready && nu_bc_trk == id) { pthread_mutex_unlock(&nu_bc_mu); return; }
    nu_bc_ready = 0; nu_bc_want = id; snprintf(nu_bc_path, sizeof nu_bc_path, "%s", path);
    int start = !nu_bc_busy; if (start) nu_bc_busy = 1;
    pthread_mutex_unlock(&nu_bc_mu);
    if (start) { pthread_t th; if (!pthread_create(&th, 0, nu_bc_thread, 0)) pthread_detach(th); else nu_bc_busy = 0; }
}
/* 1 si la nitida de esta pista ya esta (se copia en nu.art) */
static int nu_bc_take(void)
{
    int ok = 0;
    pthread_mutex_lock(&nu_bc_mu);
    if (nu_bc_ready && nu_bc_trk == nu.ni.track_id && nu.art) { memcpy(nu.art, nu_bc_px, sizeof nu_bc_px); ok = 1; }
    pthread_mutex_unlock(&nu_bc_mu);
    return ok;
}
/* caratula 232: la nitida de la biblioteca si ya esta; si no, la de 128 ampliada (bilineal) */
static void nu_make_art(void)
{
    if (nu.alb >= 0) {
        double t; int none; const uint16_t *b = mu_cover(nu.alb, MC_B, &t, &none);
        if (b) { memcpy(nu.art, b, NU_S * NU_S * 2); nu.art_src = 2; return; }
    }
    if (nu.alb < 0 && nu.cov_ok) {                    /* sin biblioteca: la nitida en segundo plano */
        if (nu_bc_take()) { nu.art_src = 2; return; }
        nu_bc_request(nu.ni.path, nu.ni.track_id);
    }
    if (nu.cov_ok) {
        for (int y = 0; y < NU_S; y++) {
            float sy = (y + .5f) * 128 / NU_S - .5f; if (sy < 0) sy = 0;
            int y0 = (int)sy, y1 = y0 + 1 < 128 ? y0 + 1 : y0; unsigned fy = (unsigned)((sy - y0) * 255);
            for (int x = 0; x < NU_S; x++) {
                float sx = (x + .5f) * 128 / NU_S - .5f; if (sx < 0) sx = 0;
                int xa = (int)sx, xb = xa + 1 < 128 ? xa + 1 : xa; unsigned fx = (unsigned)((sx - xa) * 255);
                uint16_t a = mix565(nu.cov[y0 * 128 + xb], nu.cov[y0 * 128 + xa], fx), b = mix565(nu.cov[y1 * 128 + xb], nu.cov[y1 * 128 + xa], fx);
                nu.art[y * NU_S + x] = mix565(b, a, fy);
            }
        }
        nu.art_src = 1; return;
    }
    /* sin caratula: el hueco gris con la nota (se dibuja en una pantalla de trabajo) */
    for (int y = 0; y < NU_S; y++) for (int x = 0; x < NU_S; x++) img_app[y * W + x] = rgb(58, 58, 62);
    mu_note(img_app, NU_S / 2 + NU_S / 14, NU_S / 2, NU_S * 2 / 5, rgb(104, 104, 108));
    for (int y = 0; y < NU_S; y++) memcpy(nu.art + y * NU_S, img_app + y * W, NU_S * 2);
    nu.art_src = 0;
}
/* sombra: distancia al rectangulo redondeado de la caratula con caida cubica (una vez) */
static void nu_make_shadow(void)
{
    if (nu.shm) return;
    nu.shm = malloc(NU_SM * NU_SM); if (!nu.shm) return;
    float h = NU_S / 2.f, r = 12;
    for (int y = 0; y < NU_SM; y++) for (int x = 0; x < NU_SM; x++) {
        float px = fabsf(x + .5f - NU_SM / 2.f) - (h - r), py = fabsf(y + .5f - NU_SM / 2.f) - (h - r);
        float ox = px > 0 ? px : 0, oy = py > 0 ? py : 0, dd = sqrtf(ox * ox + oy * oy) - r;
        float a = dd <= 0 ? 1 : dd >= NU_SP ? 0 : (1 - dd / NU_SP) * (1 - dd / NU_SP) * (1 - dd / NU_SP);
        nu.shm[y * NU_SM + x] = (uint8_t)(a * 255);
    }
}
static void nu_dots_btn(uint16_t *d, int cx, int cy)
{
    nu_disc(d, cx, cy, 14, nu.c_trk, 255, 0, H);
    for (int i = -1; i <= 1; i++) nu_disc(d, cx + i * 6, cy, 1.8f, 0xFFFF, 255, 0, H);
}
/* globo de cita (letra) */
static void nu_quote_icon(uint16_t *d, int cx, int cy, uint16_t col, uint16_t hole)
{
    fill_rrect(d, cx - 10, cy - 8, 21, 15, 4.5f, col, 255);
    for (int i = 0; i < 4; i++) fill_rrect(d, cx - 6, cy + 6 + i, 4 - i, 1, 0, col, 255);
    nu_disc(d, cx - 3.5f, cy - 1.5f, 2, hole, 255, 0, H); nu_disc(d, cx + 3, cy - 1.5f, 2, hole, 255, 0, H);
    mu_line(d, cx - 2.2f, cy - 1, cx - 3.8f, cy + 2.5f, 1.4f, hole); mu_line(d, cx + 4.3f, cy - 1, cx + 2.7f, cy + 2.5f, 1.4f, hole);
}
static void nu_lyr_btn(uint16_t *d)
{
    if (nu.lyr) { nu_disc(d, NU_LBX, NU_LBY, 15, 0xFFFF, 235, 0, H); nu_quote_icon(d, NU_LBX, NU_LBY, nu.avg, 0xFFFF); }
    else { uint16_t c = nu.nl ? 0xFFFF : nu.c_trk; nu_quote_icon(d, NU_LBX, NU_LBY, c, mix565(c, nu.avg, 0)); }
    /* el hueco de las comillas: el fondo de esa zona (con el boton blanco, el blanco) */
}
/* barra de estado del sistema sobre el fondo: su brillo hace de alfa del blanco */
static void nu_page_status(void)
{
    mu_statusbar(nu.page);
    for (int i = 0; i < 30 * W; i++) { uint16_t q = nu.page[i]; int a = (q >> 5) & 63; nu.page[i] = a ? mix565(0xFFFF, nu.bg[i], a * 255 / 63) : nu.bg[i]; }
}
/* pagina fija: fondo, barra de estado, asa, textos y botones que no cambian solos */
static void nu_make_page(void)
{
    memcpy(nu.page, nu.bg, HM_FRAME_BYTES);
    nu_page_status();
    fill_rrect(nu.page, W / 2 - 18, 36, 36, 5, 2.5f, nu.c_trk, 255);
    const char *ti = nu.ni.title[0] ? nu.ni.title : tr("Reproduciendo"), *ar = nu.ni.artist;
    if (!nu.lyr) {
        mu_text_bold(nu.page, 24, NU_TY, ti, 19, 0xFFFF, W - 24 - 64);
        draw_text(nu.page, 24, NU_RY, ar, 17, nu.c_sub, W - 24 - 64, 0, H);
        nu_dots_btn(nu.page, W - 40, NU_TY - 7);
    } else {
        int x = NU_SAX + NU_SA + 14;
        mu_text_bold(nu.page, x, 68, ti, 16, 0xFFFF, W - x - 56);
        draw_text(nu.page, x, 89, ar, 14, nu.c_sub, W - x - 56, 0, H);
        nu_dots_btn(nu.page, W - 40, 72);
    }
    nu_lyr_btn(nu.page);
}

/* ------------------------------------------------------------------ elementos dinamicos */
/* posicion extrapolada desde la ultima lectura del hilo de nowplay (que lee cada ~1 s). Con el
 * estado optimista de reproducir/pausa (nu.hold) para que la letra se pare al tocar, no 1 s
 * despues; el tope de 4 s solo protege si el hilo se queda sin publicar. */
/* ultima posicion conocida de la pista: en pausa la libreria publica -1 ("desconocida") y la
 * pagina mostraba "--:--"; se queda la ultima (tambien al volver a abrir Reproduciendo) */
static uint32_t nu_last_trk; static int nu_last_ms = -1;
static int nu_pos(void)
{
    if (nu.anc < 0) return nu_last_trk == nu.ni.track_id ? nu_last_ms : -1;
    int p = nu.anc;
    if (nu.hold ? nu.playing : nu.ni.playing) { double e = now_ms() - nu.anc_t; p += (int)(e > 4000 ? 4000 : e); }
    if (nu.ni.duration_ms > 0 && p > nu.ni.duration_ms) p = nu.ni.duration_ms;
    nu_last_trk = nu.ni.track_id; nu_last_ms = p;
    return p;
}
static void nu_anchor_init(double now)
{
    nu.raw_pos = nu.ni.position_ms; nu.anc = nu.ni.position_ms; nu.anc_t = now;
    if (nu.anc < 0 && nu_last_trk == nu.ni.track_id) nu.anc = nu_last_ms;
}
/* lectura barata de la posicion publicada (cada vuelta del bucle): el ancla se toma a <=25 ms de
 * que el hilo la publique, no a <=100 ms (sin deriva entre lecturas) */
static void nu_pos_sync(void)
{
    pthread_mutex_lock(&npx_mu); int p = npx.position_ms; uint32_t id = npx.track_id; int src = npx.source; pthread_mutex_unlock(&npx_mu);
    if (src == NP_SRC_NONE || id != nu.ni.track_id || p == nu.raw_pos || p < 0) return;
    nu.raw_pos = p; nu.anc = p; nu.anc_t = now_ms();
}
static void nu_draw_prog(uint16_t *d)
{
    int x0 = 24, w = W - 48, pos = nu_pos(), dur = nu.ni.duration_ms;
    fill_rrect(d, x0, NU_BY, w, 4, 2, nu.c_trk, 255);
    if (dur > 0 && pos > 0) { int fw = (int)((long long)w * pos / dur); if (fw > w) fw = w; if (fw >= 2) fill_rrect(d, x0, NU_BY, fw < 4 ? 4 : fw, 4, 2, nu.c_fill, 255); }
    char a[16], b[16];
    if (pos >= 0) nu_fmt(a, sizeof a, pos, 0); else snprintf(a, sizeof a, "--:--");
    draw_text(d, x0, NU_TMY, a, 11, nu.c_sub, 0, 0, H);
    if (dur > 0) {
        int rem = dur / 1000 - (pos > 0 ? pos / 1000 : 0); nu_fmt(b, sizeof b, rem * 1000, 1);
        draw_text(d, x0 + w - tw_n(b, 11, -1), NU_TMY, b, 11, nu.c_sub, 0, 0, H);
    }
    nu.shown_sec = pos / 1000;
}
static void nu_draw_ctl(uint16_t *d)
{
    for (int i = 0; i < 3; i++) {
        float cx = 80 + i * 80.f, cy = NU_CY, k = nu.pressed == i ? 0.86f : 1;
        if (nu.pressed == i) nu_disc(d, cx, cy, 28, 0xFFFF, 40, NU_CB0, NU_CB1);
        if (i == 1) {
            if (nu.playing) { float bw = 8 * k, bh = 28 * k; fill_rrect(d, (int)(cx - 4 * k - bw), (int)(cy - bh / 2), (int)(bw + .5f), (int)(bh + .5f), 2, 0xFFFF, 255); fill_rrect(d, (int)(cx + 4 * k), (int)(cy - bh / 2), (int)(bw + .5f), (int)(bh + .5f), 2, 0xFFFF, 255); }
            else nu_tri(d, cx - 10 * k, cy, 27 * k, 30 * k, 1, 0xFFFF);
        } else {
            float w = 15 * k, h = 19 * k;
            if (i == 0) { nu_tri(d, cx, cy, w, h, -1, 0xFFFF); nu_tri(d, cx + w, cy, w, h, -1, 0xFFFF); }
            else { nu_tri(d, cx - w, cy, w, h, 1, 0xFFFF); nu_tri(d, cx, cy, w, h, 1, 0xFFFF); }
        }
    }
}
/* rectangulo de la caratula: m 0 = en el mini reproductor, 1 = en la hoja (con borde en top) */
static void nu_art_rect(float m, int top, float *x, float *y, float *s, float *r)
{
    float bs = NU_S * nu.sc, bx = W / 2.f - bs / 2, by = top + NU_AY + NU_S / 2.f - bs / 2;
    float lt = nu.lyt, ls = NU_SA, lx = NU_SAX, ly = top + NU_SAY;
    float fs = bs + (ls - bs) * lt, fx = bx + (lx - bx) * lt, fy = by + (ly - by) * lt, fr = 10 + (6 - 10) * lt;
    float ms = 40, mx = 16, my = NU_HM + 6;
    *s = ms + (fs - ms) * m; *x = mx + (fx - mx) * m; *y = my + (fy - my) * m; *r = 5 + (fr - 5) * m;
}
/* caratula escalada (vecino mas cercano) con esquinas suavizadas, en las filas [y0, y1) */
static void nu_art_blit(uint16_t *d, int x, int y, int s, float r, int y0, int y1)
{
    if (s < 2) return;
    if (s > NU_S * 2) s = NU_S * 2;
    for (int i = 0; i < s; i++) nu_xm[i] = i * NU_S / s;
    if (r > s / 2.f) r = s / 2.f;
    for (int j = 0; j < s; j++) {
        int yy = y + j; if (yy < y0 || yy >= y1 || yy < 0 || yy >= H) continue;
        const uint16_t *sr = nu.art + (j * NU_S / s) * NU_S; uint16_t *dr = d + yy * W;
        float edge = 0, dy = j + .5f < r ? r - (j + .5f) : (j + .5f > s - r ? (j + .5f) - (s - r) : 0);
        if (dy > 0) edge = r - sqrtf(r * r - dy * dy > 0 ? r * r - dy * dy : 0);
        int full = (int)ceilf(edge), xa = full, xb = s - full;
        if (x + xa < 0) xa = -x; if (x + xb > W) xb = W - x;
        if (s == NU_S && xb > xa) memcpy(dr + x + xa, sr + xa, (xb - xa) * 2);
        else for (int i = xa; i < xb; i++) dr[x + i] = sr[nu_xm[i]];
        if (edge > 0) {
            int e0 = (int)edge; unsigned a = (unsigned)((1.f - (edge - e0)) * 255);
            if (e0 < s / 2) {
                int xl = x + e0, xr = x + s - 1 - e0;
                if (xl >= 0 && xl < W) dr[xl] = mix565(sr[nu_xm[e0]], dr[xl], a);
                if (xr >= 0 && xr < W && xr != xl) dr[xr] = mix565(sr[nu_xm[s - 1 - e0]], dr[xr], a);
            }
        }
    }
}
/* sombra escalada al tamano de la caratula; alpha 0..255 */
static void nu_shadow(uint16_t *d, float cx, float cy, float s, int alpha, int y0, int y1)
{
    if (!nu.shm || alpha <= 0 || s < 8) return;
    int ms = (int)(NU_SM * s / NU_S), x = (int)(cx - ms / 2.f), y = (int)(cy - ms / 2.f + NU_SHD * s / NU_S);
    if (ms < 2) return;
    int amax = alpha * 16 / 255;                                   /* hasta un 50 % mas oscuro */
    for (int j = 0; j < ms; j++) {
        int yy = y + j; if (yy < y0 || yy >= y1 || yy < 0 || yy >= H) continue;
        const uint8_t *m = nu.shm + (j * NU_SM / ms) * NU_SM; uint16_t *dr = d + yy * W;
        for (int i = 0; i < ms; i++) {
            int xx = x + i; if (xx < 0 || xx >= W) continue;
            int a = m[i * NU_SM / ms]; if (!a) continue;
            int k = a * amax >> 8; if (k) dr[xx] = dim565(dr[xx], 32 - k);
        }
    }
}
/* caratula con su sombra; clip0: primera fila (borde de la hoja) */
static void nu_art_draw(uint16_t *d, float m, int top, int clip0, int clip1)
{
    float x, y, s, r; nu_art_rect(m, top, &x, &y, &s, &r);
    int shadow_end = top + (int)(NU_AB1 + (NU_SB1 - NU_AB1) * nu.lyt);
    if (shadow_end > clip1) shadow_end = clip1;
    float sa = m * (1 - 0.5f * nu.lyt) * nu_clampf((nu.sc - 0.55f) / 0.45f, 0.3f, 1.1f);
    nu_shadow(d, x + s / 2, y + s / 2, s, (int)(255 * sa > 255 ? 255 : 255 * sa), clip0, shadow_end);
    nu_art_blit(d, (int)(x + .5f), (int)(y + .5f), (int)(s + .5f), r, clip0, clip1);
}

/* ------------------------------------------------------------------ letra (.lrc) */
/* El .lrc se lee con lrc_parse (lrc_core.inc.c): todas las variantes de marcas, [offset:],
 * LRC mejorado, CRLF, BOM, UTF-16. El verso i es el actual desde t_i - loff (offset positivo =
 * antes) hasta la marca siguiente; el bucle se despierta justo en ese instante (nu_lyrics_next_ms). */
static void nu_lyrics_free(void) { free(nu.lbuf); free(nu.ll); nu.lbuf = 0; nu.ll = 0; nu.nl = 0; nu.synced = 0; nu.lcur = -1; nu.ltot = 0; nu.loff = 0; }
/* toma una letra ya leida (se queda con su texto) */
static void nu_lyrics_from(struct lrc *L)
{
    nu_lyrics_free();
    if (!L->n) { lrc_free(L); return; }
    nu.ll = malloc(L->n * sizeof *nu.ll);
    if (!nu.ll) { lrc_free(L); return; }
    nu.lbuf = L->buf; L->buf = 0;
    nu.nl = L->n; nu.synced = L->synced; nu.loff = L->offset;
    int y = 0;
    for (int i = 0; i < nu.nl; i++) {
        nu.ll[i].t = L->ln[i].t; nu.ll[i].off = L->ln[i].off;
        nu.ll[i].y = y; nu.ll[i].h = draw_wrapped(0, 0, 0, W - 48, nu.lbuf + nu.ll[i].off, 20, 0, 26, 0) + 14; y += nu.ll[i].h;
    }
    lrc_free(L);
    nu.ltot = y; nu.lcur = -1; nu.lsc = nu.lsc0 = nu.lsc1 = 0; nu.lst = 0;
}
static void nu_lyrics_load(void)
{
    nu_lyrics_free();
    if (!nu.ni.path[0]) return;
    char p[600]; lx_lrc_of(nu.ni.path, p, sizeof p);
    struct lrc L; if (lx_load(p, &L)) return;
    nu_lyrics_from(&L);
    lg("np: letra %s: %d versos%s offset %d", p, nu.nl, nu.synced ? " (sincronizada)" : "", nu.loff);
}
/* el verso actual en pos (ultimo con t - loff <= pos; -1 antes del primero) */
static int nu_lyrics_at(int pos)
{
    int a = 0, b = nu.nl - 1, r = -1;
    while (a <= b) { int m = (a + b) / 2; if (nu.ll[m].t - nu.loff <= pos) { r = m; a = m + 1; } else b = m - 1; }
    return r;
}
/* ms hasta el siguiente cambio de verso (-1: ninguno o no suena) */
static int nu_lyrics_next_ms(void)
{
    if (!nu.lyr || !nu.nl || !nu.synced || !(nu.hold ? nu.playing : nu.ni.playing)) return -1;
    int pos = nu_pos(); if (pos < 0) return -1;
    pos += NU_LEAD_MS;
    int i = nu_lyrics_at(pos) + 1; if (i >= nu.nl) return -1;
    int d = nu.ll[i].t - nu.loff - pos;
    return d > 0 ? d : 0;
}
/* verso actual y desplazamiento objetivo; 1 = cambio (empieza la animacion) */
static int nu_lyrics_tick(void)
{
    if (!nu.nl) return 0;
    int pos = nu_pos(), cur = -1; float target;
    if (nu.synced) {
        if (pos >= 0) cur = nu_lyrics_at(pos + NU_LEAD_MS);
        /* una lectura nueva un poco por detras (< 150 ms) no devuelve al verso anterior */
        if (cur < nu.lcur && nu.lcur >= 0 && nu.lcur < nu.nl && pos >= 0 && nu.ll[nu.lcur].t - nu.loff - (pos + NU_LEAD_MS) < 150) cur = nu.lcur;
        target = cur < 0 ? 0 : nu.ll[cur].y - 34;
    } else {
        float k = nu.ni.duration_ms > 0 && pos > 0 ? (float)pos / nu.ni.duration_ms : 0;
        target = (nu.ltot - (NU_LY1 - NU_LY0) + 60) * k; cur = -2;
    }
    float mx = nu.ltot - (NU_LY1 - NU_LY0) / 2.f; if (target > mx) target = mx; if (target < 0) target = 0;
    if (cur == nu.lcur && fabsf(target - nu.lsc1) < 1) return 0;
    if (!nu.synced && fabsf(target - nu.lsc1) < 6 && nu.lcur == cur) return 0;
    nu.lcur = cur; nu.lsc0 = nu.lsc; nu.lsc1 = target; nu.lst = now_ms();
    return 1;
}
/* 1 si la animacion de desplazamiento sigue */
static int nu_lyrics_step(void)
{
    if (!nu.lst) return 0;
    double t = (now_ms() - nu.lst) / 420; if (t >= 1) { nu.lsc = nu.lsc1; nu.lst = 0; return 1; }
    nu.lsc = nu.lsc0 + (nu.lsc1 - nu.lsc0) * (float)ease_out(t);
    return 1;
}
/* sin letra: "Sin letra" y el boton "Buscar letra" en la franja (nu_nl_btn: su rectangulo) */
static int nu_nl_btn[4];
static void nu_draw_nolyrics(uint16_t *d)
{
    memcpy(d + NU_LY0 * W, nu.page + NU_LY0 * W, (NU_LY1 - NU_LY0) * W * 2);
    nu_quote_icon(d, W / 2, 178, nu.c_sub, d[178 * W + W / 2]);
    const char *t1 = tr("Sin letra"), *t2 = tr("Busca la letra de esta canción"), *b = tr("Buscar letra");
    int w = tw_n(t1, 20, -1); mu_text_bold(d, (W - w) / 2, 220, t1, 20, 0xFFFF, W - 40);
    w = tw_n(t2, 13, -1); draw_text(d, (W - w > 40 ? (W - w) / 2 : 20), 242, t2, 13, nu.c_sub, W - 40, 0, H);
    int bw = tw_n(b, 15, -1) + 44, bx = (W - bw) / 2, by = 260;
    fill_rrect(d, bx, by, bw, 38, 19, blend565(nu.avg, 0xFFFF, 7), 255);
    mu_text_bold(d, bx + 22, by + 25, b, 15, 0xFFFF, bw - 30);
    nu_nl_btn[0] = bx; nu_nl_btn[1] = by; nu_nl_btn[2] = bw; nu_nl_btn[3] = 38;
}
/* la letra en su franja (de la pagina), con los bordes fundidos; se compone en img_app */
static void nu_draw_lyrics(uint16_t *d)
{
    if (!nu.nl) { nu_draw_nolyrics(d); return; }
    uint16_t *s = img_app;
    memcpy(s + NU_LY0 * W, nu.page + NU_LY0 * W, (NU_LY1 - NU_LY0) * W * 2);
    int base = NU_LY0 + 18 - (int)nu.lsc;
    uint16_t dim = blend565(nu.avg, 0xFFFF, 13);
    int ct = nu.synced && nu.lcur >= 0 && nu.lcur < nu.nl ? nu.ll[nu.lcur].t : -2;   /* versos con la misma marca: todos */
    for (int i = 0; i < nu.nl; i++) {
        int y = base + nu.ll[i].y;
        if (y + nu.ll[i].h < NU_LY0 - 30) continue;
        if (y > NU_LY1) break;
        int on = nu.synced && nu.ll[i].t == ct;
        const char *t = nu.lbuf + nu.ll[i].off;
        /* draw_wrapped dibuja sin recorte: lo que se sale va a filas de img_app que no se copian */
        if (y < 30 || y + nu.ll[i].h > H - 20) continue;
        draw_wrapped(s, 24, y - 6, W - 48, t, 20, on || !nu.synced ? (on ? 0xFFFF : nu.c_sub) : dim, 26, 1);
        if (on) draw_wrapped(s, 25, y - 6, W - 48, t, 20, 0xFFFF, 26, 1);
    }
    for (int y = NU_LY0; y < NU_LY1; y++) {          /* fundido arriba y abajo (como iOS) */
        int k = y - NU_LY0 < 22 ? (y - NU_LY0) * 32 / 22 : NU_LY1 - 1 - y < 26 ? (NU_LY1 - 1 - y) * 32 / 26 : 32;
        uint16_t *o = d + y * W; const uint16_t *a = s + y * W, *b = nu.page + y * W;
        if (k >= 32) memcpy(o, a, W * 2); else for (int x = 0; x < W; x++) o[x] = blend565(b[x], a[x], k);
    }
}

/* ------------------------------------------------------------------ composicion */
/* pantalla completa en reposo (hoja arriba); art: con la caratula */
static void nu_render(uint16_t *d, int art)
{
    memcpy(d, nu.page, HM_FRAME_BYTES);
    if (nu.lyr && nu.lyt > 0.5f) nu_draw_lyrics(d);
    nu_draw_prog(d);
    nu_draw_ctl(d);
    if (art) nu_art_draw(d, 1, 0, 0, H);
}
/* repinta solo las filas [y0, y1) de fr0 (franjas que no se solapan) */
static void nu_band(int y0, int y1)
{
    memcpy(fr0 + y0 * W, nu.page + y0 * W, (y1 - y0) * W * 2);
    if (y0 == NU_LY0 && nu.lyr) nu_draw_lyrics(fr0);
    if (y0 == NU_PB0) nu_draw_prog(fr0);
    if (y0 == NU_CB0) nu_draw_ctl(fr0);
    if (y0 == NU_AB0 || (nu.lyr && y0 == 30)) nu_art_draw(fr0, 1, 0, y0, y1);
}
static void nu_sheet_build(void) { nu_render(img_sheet, 0); }
static int nu_loose;     /* abierto suelto (sin mini reproductor debajo): se cierra hacia abajo entero */
/* fotograma de la hoja con su borde en top; m: caratula (0 en el mini, 1 en la hoja) */
static void nu_sheet_frame(int top, float m)
{
    if (top < 0) top = 0;
    int k = 32 - (12 * (NU_HM - (top > NU_HM ? NU_HM : top))) / NU_HM;
    compose_sheet(fr0, img_under, img_sheet, top, k);
    /* el mini reproductor se funde en la hoja al principio del recorrido */
    float a = nu_loose ? 0 : 1 - (NU_HM - top) / 110.f;
    if (a > 0) {
        unsigned al = (unsigned)(nu_clampf(a, 0, 1) * 255);
        for (int y = top; y < top + MU_MINI_H && y < H; y++) {
            int sy = NU_HM + (y - top); if (sy >= H) break;
            uint16_t *o = fr0 + y * W; const uint16_t *u = img_under + sy * W;
            for (int x = 0; x < W; x++) o[x] = mix565(u[x], o[x], al);
        }
    }
    nu_art_draw(fr0, m, top, top, H);
}

/* ------------------------------------------------------------------ datos */
static uint32_t nu_sum(const uint16_t *c) { uint32_t h = 2166136261u; for (int i = 0; i < 128 * 128; i += 7) h = (h ^ c[i]) * 16777619u; return h; }
static void nu_lookup(void)
{
    nu.trk = nu.ni.path[0] && ML ? mu_find_sys(ML, nu.ni.path) : -1;
    nu.alb = nu.trk >= 0 ? ML->T[nu.trk].alb : -1;
}
/* todas las capas de la pista actual */
static void nu_build_all(void)
{
    double t0 = now_ms();
    nu_lookup();
    nu_make_bg(); nu_make_art(); nu_lyrics_load(); nu_make_page();
    nu.cov_sum = nu.cov_ok ? nu_sum(nu.cov) : 0;
    lg("np: capas en %.1f ms (caratula %s, pista %d)", now_ms() - t0, nu.art_src == 2 ? "nitida" : nu.art_src ? "ampliada" : "ninguna", nu.trk);
}
static int nu_init(void)
{
    memset(&nu, 0, sizeof nu);
    nu.bg = malloc(HM_FRAME_BYTES); nu.page = malloc(HM_FRAME_BYTES); nu.art = malloc(NU_S * NU_S * 2);
    nu_make_shadow();
    if (!nu.bg || !nu.page || !nu.art || !nu.shm) return 0;
    int cf; nu_snap(&nu.ni, nu.cov, &nu.cov_ok, &cf);
    if (!cf) nu.cov_ok = 0;
    if (!cf && mu_npcov_ok) { memcpy(nu.cov, mu_npcov, sizeof nu.cov); nu.cov_ok = 1; }   /* la del mini reproductor */
    nu.playing = nu.ni.playing; nu.sc = nu.playing ? 1 : NU_PAUSE; nu.pressed = -1; nu.lcur = -1;
    nu_anchor_init(now_ms());
    nu_build_all();
    return 1;
}
static void nu_free(void)
{
    free(nu.bg); free(nu.page); free(nu.art); free(nu.shm); nu_lyrics_free();
    memset(&nu, 0, sizeof nu); nu.trk = nu.alb = -1;
}

/* ------------------------------------------------------------------ animaciones de la hoja */
/* lleva el borde de la hoja a "to" con un muelle (velocidad inicial v px/s); m: 0 abrir/cerrar
 * siguiendo la posicion, 1 caratula fija en la hoja; close_from: borde al empezar a cerrar */
static void nu_sheet_anim(float top, float v, float to, float response, int mmode, float from)
{
    struct nu_prof pr = { 0, 0, 0, 0 }; double last = now_ms(); mu_busy = 1;
    for (;;) {
        double t = now_ms(); float dt = (float)(t - last) / 1000; last = t;
        nu_spring(&top, &v, to, response, 1.0f, dt);
        if (to > 0 && top > to) { top = to; v = 0; }          /* sin pasarse del mini reproductor */
        int done = nu_spring_done(top, v, to, 0.5f);
        if (done) top = to;
        float m = mmode == 1 ? 1 : mmode == 2 ? nu_clampf((NU_HM - top) / (NU_HM - from + .01f), 0, 1) : nu_clampf(1 - top / NU_HM, 0, 1);
        double t1 = now_ms();
        if (done && to >= NU_HM) memcpy(fr0, img_under, HM_FRAME_BYTES);
        else if (done && to <= 0) nu_render(fr0, 1);
        else nu_sheet_frame((int)(top + .5f), m);
        nu_prof_add(&pr, now_ms() - t1);
        present();
        if (done) break;
    }
    nu_prof_log(&pr, to > 0 ? "cerrar" : "abrir");
    mu_busy = 0;
}
/* arrastre de la hoja (abierta: hacia abajo; 1:1). Devuelve 1 si se cierra */
static int nu_drag(int sy)
{
    nu_sheet_build();
    struct pkt p; float v = 0; int lasty = sy; double lt = now_ms(); struct nu_prof pr = { 0, 0, 0, 0 };
    mu_busy = 1;
    int top = 0;
    while (cur_down) {
        if (read_pkt(&p, 20) && !p.down) break;
        double tn = now_ms(); if (tn > lt) v = 0.6f * v + 0.4f * (cur_y - lasty) * 1000.f / (float)(tn - lt);
        lasty = cur_y; lt = tn;
        top = cur_y - sy; if (top < 0) top = 0;
        double t1 = now_ms(); nu_sheet_frame(top, 1); nu_prof_add(&pr, now_ms() - t1); present();
    }
    nu_prof_log(&pr, "arrastrar");
    if (now_ms() - lt > 80) v = 0;                         /* se paro antes de soltar */
    if (top > 110 || (v > 600 && top > 12)) {
        if (nu_loose) nu_sheet_anim((float)top, v, H, 0.30f, 1, (float)top);   /* entera hacia abajo */
        else nu_sheet_anim((float)top, v, NU_HM, 0.36f, 2, (float)top);
        return 1;
    }
    nu_sheet_anim((float)top, v, 0, 0.32f, 1, 0);
    return 0;
}

/* ------------------------------------------------------------------ bucle */
static int nu_hit(int x, int y)
{
    if (y >= NU_CB0 && y < NU_CB1 + 4) { int i = (x - 40) / 80; if (x >= 40 && i >= 0 && i < 3) return i; }
    if (!nu.lyr && abs(x - (W - 40)) < 24 && abs(y - (NU_TY - 7)) < 22) return 3;
    if (nu.lyr && abs(x - (W - 40)) < 24 && abs(y - 72) < 24) return 3;
    if (abs(x - NU_LBX) < 30 && abs(y - NU_LBY) < 22) return 4;
    if (nu.lyr && y < NU_SB1 && y >= 36 && x < W - 64) return 5;   /* cabecera de la letra: volver */
    if (nu.lyr && !nu.nl && x >= nu_nl_btn[0] - 8 && x < nu_nl_btn[0] + nu_nl_btn[2] + 8 && y >= nu_nl_btn[1] - 8 && y < nu_nl_btn[1] + nu_nl_btn[3] + 8) return 7;   /* "Buscar letra" */
    if (nu.lyr && y >= NU_LY0 && y < NU_LY1) return 6;               /* la letra (pulsacion larga: ajustar) */
    return -1;
}
/* fundido de toda la pantalla al nuevo estado (cambio de pista) */
static void nu_crossfade(void)
{
    memcpy(img_base, fr0, HM_FRAME_BYTES);
    nu_render(img_sheet, 1);
    struct nu_prof pr = { 0, 0, 0, 0 }; double t0 = now_ms(); mu_busy = 1;
    for (;;) {
        double t = (now_ms() - t0) / 280; if (t > 1) t = 1;
        int k = (int)(ease_out(t) * 32 + .5);
        double t1 = now_ms();
        if (k >= 32) memcpy(fr0, img_sheet, HM_FRAME_BYTES); else for (int i = 0; i < W * H; i++) fr0[i] = blend565(img_base[i], img_sheet[i], k);
        nu_prof_add(&pr, now_ms() - t1);
        present();
        if (t >= 1) break;
    }
    nu_prof_log(&pr, "fundido de pista");
    mu_busy = 0;
}
/* cambio entre caratula grande y letra: la caratula vuela con un muelle */
static void nu_toggle_lyrics(void)
{
    nu.lyr = !nu.lyr;                     /* sin letra: la vista de letra ofrece buscarla */ nu_make_page(); nu.lcur = -1; nu_lyrics_tick(); nu.lsc = nu.lsc1; nu.lst = 0;
    float v = 0, to = nu.lyr ? 1 : 0; struct nu_prof pr = { 0, 0, 0, 0 }; double last = now_ms(); mu_busy = 1;
    for (;;) {
        double t = now_ms(); float dt = (float)(t - last) / 1000; last = t;
        nu_spring(&nu.lyt, &v, to, 0.42f, 0.86f, dt);
        int done = nu_spring_done(nu.lyt, v, to, 0.002f); if (done) nu.lyt = to;
        double t1 = now_ms();
        memcpy(fr0, nu.page, HM_FRAME_BYTES);
        if (nu.lyr && nu.lyt > 0.35f) {                                /* la letra aparece al final */
            nu_draw_lyrics(img_base);
            int k = (int)nu_clampf((nu.lyt - 0.35f) / 0.5f * 32, 0, 32);
            for (int y = NU_LY0; y < NU_LY1; y++) for (int x = 0; x < W; x++) fr0[y * W + x] = blend565(fr0[y * W + x], img_base[y * W + x], k);
        }
        nu_draw_prog(fr0); nu_draw_ctl(fr0);
        nu_art_draw(fr0, 1, 0, 0, H);
        nu_prof_add(&pr, now_ms() - t1);
        present();
        if (done) break;
    }
    nu_prof_log(&pr, nu.lyr ? "a la letra" : "de la letra");
    mu_busy = 0;
}
/* datos nuevos del reproductor: devuelve las franjas a repintar (bits) o -1 = todo con fundido */
enum { NB_ART = 1, NB_PROG = 2, NB_CTL = 4, NB_LYR = 8, NB_STAT = 16, NB_PAGE = 32 };
static int nu_poll(void)
{
    static double last_full, last_stat;
    double now = now_ms(); int r = 0, cf;
    struct np_info n; nu_snap(&n, 0, 0, &cf);
    if (n.source == NP_SRC_NONE) return 0;
    if (n.track_id != nu.ni.track_id) {
        if (nu.pend_id != n.track_id) { nu.pend_id = n.track_id; nu.pend_t = now; }
        if (cf || now - nu.pend_t > 1500) {                    /* con su caratula (o sin ella tras 1,5 s) */
            int ok; nu_snap(&nu.ni, nu.cov, &ok, &cf); nu.cov_ok = cf && ok;
            nu_anchor_init(now); nu.pend_id = 0;
            if (!nu.hold) nu.playing = nu.ni.playing;
            nu_build_all(); nu.lcur = -1;         /* sin letra se queda en la vista de letra (ofrece buscarla) */
            if (nu.lyr) { nu_lyrics_tick(); nu.lsc = nu.lsc1; nu.lst = 0; }
            last_full = now;
            return -1;
        }
        return 0;
    }
    /* reproducir/pausa (respetando lo que se acaba de tocar) */
    int held = nu.hold;                   /* reproducir/pausa tocado aqui: ya se anclo al tocar */
    if (nu.hold && (n.playing == nu.playing || now - nu.hold_t > 2500)) nu.hold = 0;
    if (!nu.hold && n.playing != nu.playing) { nu.playing = n.playing; r |= NB_CTL; }
    /* posicion: se ancla cuando el hilo publica un valor nuevo y se extrapola mientras suena */
    if (!nu.ni.playing && n.playing && !held) { nu.anc = nu_pos(); nu.anc_t = now; }   /* vuelve a sonar: extrapolar desde ahora */
    if (n.position_ms != nu.raw_pos && n.position_ms >= 0) { nu.raw_pos = n.position_ms; nu.anc = n.position_ms; nu.anc_t = now; }   /* -1 (en pausa): se queda la ultima */
    if (nu.ni.playing && !n.playing && !held) { nu.anc = nu_pos(); nu.anc_t = now; }   /* se paro: congelar donde iba */
    nu.ni.playing = n.playing; nu.ni.duration_ms = n.duration_ms;
    if (strcmp(n.title, nu.ni.title) || strcmp(n.artist, nu.ni.artist)) {   /* etiquetas editadas */
        snprintf(nu.ni.title, sizeof nu.ni.title, "%s", n.title); snprintf(nu.ni.artist, sizeof nu.ni.artist, "%s", n.artist); snprintf(nu.ni.album, sizeof nu.ni.album, "%s", n.album);
        nu_make_page(); r |= NB_PAGE;
    }
    { int p = nu_pos(); if (p >= 0 && p / 1000 != nu.shown_sec) r |= NB_PROG; }
    if (nu.lyr && nu_lyrics_tick()) r |= NB_LYR;
    /* caratula: la nitida de la biblioteca cuando llega, o una nueva (buscada) */
    if (nu.art_src != 2 && nu.alb >= 0 && mu_cov_new) {
        mu_cov_new = 0; double t; int none; const uint16_t *b = mu_cover(nu.alb, MC_B, &t, &none);
        if (b) { memcpy(nu.art, b, NU_S * NU_S * 2); nu.art_src = 2; r |= NB_ART; lg("np: caratula nitida"); }
    }
    if (nu.art_src == 1 && nu.alb < 0 && nu_bc_ready && nu_bc_take()) { nu.art_src = 2; r |= NB_ART; lg("np: caratula nitida (sin biblioteca)"); }
    if (now - last_full > 1000) {
        last_full = now;
        if (cf) {
            static uint16_t c2[128 * 128]; int ok; struct np_info n2; nu_snap(&n2, c2, &ok, &cf);
            if (cf && n2.track_id == nu.ni.track_id && (ok != nu.cov_ok || (ok && nu_sum(c2) != nu.cov_sum))) {
                memcpy(nu.cov, c2, sizeof c2); nu.cov_ok = ok;
                if (nu.alb >= 0) mu_cover_forget(nu.alb);
                nu_build_all(); return -1;
            }
        }
    }
    if (now - last_stat > 5000) { last_stat = now; nu_page_status(); r |= NB_STAT; }
    return r;
}
static void nu_present_bands(int r)
{
    int y0 = H, y1 = 0;
    if (r & NB_PAGE) { nu_render(fr0, 1); present(); return; }
    if (r & NB_STAT) { memcpy(fr0, nu.page, 30 * W * 2); y0 = 0; y1 = 30; }
    if (r & NB_ART) { if (nu.lyr) { nu_band(30, NU_SB1); if (30 < y0) y0 = 30; if (NU_SB1 > y1) y1 = NU_SB1; } else { nu_band(NU_AB0, NU_AB1); if (NU_AB0 < y0) y0 = NU_AB0; if (NU_AB1 > y1) y1 = NU_AB1; } }
    if ((r & NB_LYR) && nu.lyr) { nu_band(NU_LY0, NU_LY1); if (NU_LY0 < y0) y0 = NU_LY0; if (NU_LY1 > y1) y1 = NU_LY1; }
    if (r & NB_PROG) { nu_band(NU_PB0, NU_PB1); if (NU_PB0 < y0) y0 = NU_PB0; if (NU_PB1 > y1) y1 = NU_PB1; }
    if (r & NB_CTL) { nu_band(NU_CB0, NU_CB1); if (NU_CB0 < y0) y0 = NU_CB0; if (NU_CB1 > y1) y1 = NU_CB1; }
    if (y1 > y0) present_rows(y0, y1);
}


/* ------------------------------------------------------------------ buscar / ajustar la letra
 * Integrado en la vista de letra de Reproduciendo (sin salir a otra pantalla): una hoja de iOS
 * sube sobre la letra con los resultados de LRCLIB para la cancion que suena (artista, titulo,
 * album y duracion del reproductor; la cabecera con la caratula se queda). Al tocar uno, la hoja
 * baja hasta abajo y deja ver ESA letra sincronizada con la cancion (es la que suena: misma
 * posicion, mismo desplazamiento que la letra normal), con el ajuste de sincronizacion (-/+ 0,5 s
 * y regla deslizable) y "Usar esta letra": guarda el .lrc junto a la cancion con [offset:] y la
 * hoja baja del todo con la letra ya puesta. "‹ Resultados" vuelve a subirla.
 * adjust = 1: solo el ajuste de la letra actual (pulsacion larga sobre la letra o menu "...").
 * Usa img_app (nu_draw_lyrics) y fr0; ningun otro bufer grande. */
#define NLX_LT 104                      /* borde de la hoja de resultados */
#define NLX_PT (H - 168)                /* borde de la hoja de vista previa / ajuste */
#define NLX_HDR 70                      /* cabecera de la hoja de resultados */
#define NI_LYRSYNC 40                   /* menu "...": ajustar sincronizacion (icono reloj) */
enum { NLX_LIST, NLX_PREV, NLX_ADJ };
static struct { uint16_t sb, sub, dimq, btn, sel; } nlx;
/* fondo opaco de la hoja (borde superior en top) en las filas [y0, y1), esquinas redondeadas */
static void nu_lx_fill(uint16_t *d, int top, int y0, int y1)
{
    if (y0 < 0) y0 = 0; if (y1 > H) y1 = H;
    for (int y = y0; y < y1; y++) {
        uint16_t *r = d + y * W; int j = y - top;
        if (j >= 14 || j < 0) { for (int x = 0; x < W; x++) r[x] = nlx.sb; continue; }
        float dy = 14 - (j + .5f), e = 14 - sqrtf(196 - dy * dy);
        int e0 = (int)e; unsigned a = (unsigned)((1 - (e - e0)) * 255);
        for (int x = e0 + 1; x < W - e0 - 1; x++) r[x] = nlx.sb;
        r[e0] = mix565(nlx.sb, r[e0], a); r[W - 1 - e0] = mix565(nlx.sb, r[W - 1 - e0], a);
    }
}
static void nu_lx_sheet(uint16_t *d, int top)
{
    if (top >= H) return;
    for (int k = 1; k <= 12; k++) {                      /* sombra suave encima del borde */
        int y = top - k; if (y < 0) break;
        uint16_t *r = d + y * W; int dk = 32 - (13 - k) * 10 / 12;
        for (int x = 0; x < W; x++) r[x] = dim565(r[x], dk);
    }
    nu_lx_fill(d, top, top, H);
    fill_rrect(d, W / 2 - 18, top + 6, 36, 5, 2.5f, blend565(nlx.sb, 0xFFFF, 9), 255);
}
static void nu_lx_render(int mode, int top, float sc, int ci, int press, int loading)
{
    memcpy(fr0, nu.page, HM_FRAME_BYTES);
    if (top > NU_LY0 + 8) nu_draw_lyrics(fr0);
    if (top > NU_PB0) nu_draw_prog(fr0);
    if (top > NU_CB0) nu_draw_ctl(fr0);
    nu_art_draw(fr0, 1, 0, 30, NU_SB1);
    if (top < H) {
        nu_lx_sheet(fr0, top);
        if (mode == NLX_LIST) {
            int ly = top + NLX_HDR;
            if (lc_rc == -1) lx_spinner_msg(fr0, ly + 96, tr("Buscando letras"));
            else if (!nlc) {
                const char *m = lc_rc == 2 ? tr("No se pudo conectar. Comprueba la Wi-Fi.") : lc_rc == 99 ? tr("Falta el componente de red (hibynet)") : tr("No se encontraron letras. Prueba a cambiar la búsqueda.");
                lx_empty(fr0, ly + 50, m, lc_rc == 2 ? tr("Reintentar") : tr("Editar búsqueda"), nlx.sub, nlx.btn);
            } else {
                int y = ly - (int)sc;
                for (int i = 0; i < nlc; i++, y += LX_ROW) {
                    if (y + LX_ROW < ly || y >= H) continue;
                    uint16_t bg = nlx.sb;
                    if (i == press) { bg = nlx.sel; for (int yy = y < ly ? ly : y; yy < y + LX_ROW && yy < H; yy++) for (int x = 0; x < W; x++) fr0[yy * W + x] = bg; }
                    else if (i && y >= ly) for (int x = 16; x < W; x++) fr0[y * W + x] = blend565(nlx.sb, 0xFFFF, 3);
                    lx_row(fr0, 0, y, W, &lc[i], lc_want, bg, 0xFFFF, nlx.sub, nlx.dimq, loading == i);
                }
                nu_lx_fill(fr0, top, top, ly);                /* la cabecera tapa las filas que suben */
                fill_rrect(fr0, W / 2 - 18, top + 6, 36, 5, 2.5f, blend565(nlx.sb, 0xFFFF, 9), 255);
                for (int yy = ly; yy < ly + 10 && yy < H; yy++) { int k = (yy - ly) * 32 / 10; uint16_t *r = fr0 + yy * W; for (int x = 0; x < W; x++) r[x] = blend565(nlx.sb, r[x], k); }
            }
            const char *tt = tr("Letras"); int w = tw_n(tt, 17, -1);
            mu_text_bold(fr0, (W - w) / 2, top + 37, tt, 17, 0xFFFF, W - 130);
            /* buscar a mano (lupa) y cerrar (x) */
            uint16_t lc_ = press == -11 ? blend565(LX_BLUE, 0, 10) : LX_BLUE;
            nu_ring(fr0, 27, top + 29, 6.5f, 2.2f, lc_, 0); mu_line(fr0, 32, top + 34, 37, top + 39, 2.4f, lc_);
            nu_disc(fr0, W - 30, top + 32, 14, press == -10 ? blend565(nlx.btn, 0xFFFF, 6) : nlx.btn, 255, 0, H);
            mu_line(fr0, W - 34.5f, top + 27.5f, W - 25.5f, top + 36.5f, 2, nlx.sub); mu_line(fr0, W - 25.5f, top + 27.5f, W - 34.5f, top + 36.5f, 2, nlx.sub);
            char s2[96] = "";
            if (nlc) {
                int k = snprintf(s2, sizeof s2, tr("%d resultados"), nlc);
                if (lc_want > 0) snprintf(s2 + k, sizeof s2 - k, " \xC2\xB7 %d:%02d", lc_want / 60, lc_want % 60);
            }
            if (s2[0]) { w = tw_n(s2, 12, -1); draw_text(fr0, (W - w) / 2, top + 57, s2, 12, nlx.sub, W - 40, 0, H); }
        } else {
            uint16_t bl = press == 5 ? blend565(LX_BLUE, 0, 10) : LX_BLUE;
            if (mode == NLX_PREV) {
                mu_line(fr0, 23, top + 21, 17, top + 27, 2.2f, bl); mu_line(fr0, 17, top + 27, 23, top + 33, 2.2f, bl);
                draw_text(fr0, 29, top + 33, tr("Resultados"), 16, bl, 130, 0, H);
                if (ci >= 0 && ci < nlc) {
                    char dt[24]; int k = lrc_dur_badge(lc[ci].dur, lc_want, dt, sizeof dt), xr = W - 16;
                    if (dt[0]) xr = lx_pill(fr0, xr, top + 18, dt, k, k == 1 ? blend565(nlx.sb, LX_GREEN, 9) : k == 2 ? blend565(nlx.sb, LX_ORANGE, 9) : nlx.btn, k == 1 ? LX_GREEN : k == 2 ? LX_ORANGE : nlx.sub) - 6;
                    if (lc[ci].synced) lx_pill(fr0, xr, top + 18, "LRC", 0, blend565(nlx.sb, LX_GREEN, 9), LX_GREEN);
                    else lx_pill(fr0, xr, top + 18, tr("Texto"), 0, nlx.btn, nlx.sub);
                }
            } else {
                draw_text(fr0, 16, top + 33, tr("Cancelar"), 16, bl, 100, 0, H);
                const char *tt = tr("Sincronización"); int w = tw_n(tt, 16, -1); if (w > 150) w = 150;
                mu_text_bold(fr0, (W - w) / 2, top + 33, tt, 16, 0xFFFF, 150);
            }
            if (nu.synced) lx_adjust(fr0, top + 44, nu.loff, press, nlx.btn, 0xFFFF, nlx.sub);
            else { const char *m = tr("Letra sin sincronizar"); int w = tw_n(m, 14, -1); draw_text(fr0, (W - w) / 2, top + 80, m, 14, nlx.sub, W - 40, 0, H); }
            lx_button(fr0, 16, top + 116, W - 32, 44, mode == NLX_PREV ? tr("Usar esta letra") : tr("Guardar"), LX_BLUE, 0xFFFF, press == 4);
        }
    }
    draw_toast(fr0);
}
static void nu_lx_snap(void) { nu.lcur = -1; nu_lyrics_tick(); nu.lsc = nu.lsc1; nu.lst = 0; }
/* arrastre de la hoja hacia abajo desde su cabecera; 1 = soltada para cerrarla */
static int nu_lx_drag(int mode, float base, float *st, float *sv, int sy, float sc, int ci, int loading)
{
    struct pkt p; float v = 0; int lasty = sy; double lt = now_ms();
    while (cur_down) {
        if (read_pkt(&p, 20) && !p.down) break;
        double tn = now_ms(); if (tn > lt) v = 0.6f * v + 0.4f * (cur_y - lasty) * 1000.f / (float)(tn - lt);
        lasty = cur_y; lt = tn;
        float dy = (float)(cur_y - sy); if (dy < 0) dy /= 4;          /* hacia arriba: con resistencia */
        *st = base + dy;
        nu_lx_render(mode, (int)(*st + .5f), sc, ci, -1, loading); present();
    }
    if (now_ms() - lt > 80) v = 0;
    *sv = v;
    return *st - base > 110 || (v > 700 && *st - base > 12);
}
static void nu_lx(int adjust)
{
    if (adjust && (!nu.nl || !nu.synced)) return;
    nlx.sb = blend565(rgb(20, 20, 22), nu.avg, 7); nlx.sub = blend565(nlx.sb, 0xFFFF, 19);
    nlx.dimq = blend565(nlx.sb, 0xFFFF, 13); nlx.btn = blend565(nlx.sb, 0xFFFF, 4); nlx.sel = blend565(nlx.sb, 0xFFFF, 3);
    int want = nu.ni.duration_ms > 0 ? (nu.ni.duration_ms + 500) / 1000 : lx_dur_s(nu.ni.path);
    char a[128], t[160], b[128], path[460];
    snprintf(path, sizeof path, "%s", nu.ni.path);
    snprintf(a, sizeof a, "%s", nu.trk >= 0 && ML ? ML->T[nu.trk].artist : nu.ni.artist);
    snprintf(t, sizeof t, "%s", nu.ni.title);
    snprintf(b, sizeof b, "%s", nu.trk >= 0 && nu.alb >= 0 && ML ? ML->A[nu.alb].title : nu.ni.album);
    if (!adjust) { if (access(NETBIN, X_OK)) { toast(tr("Falta el componente de red (hibynet)")); return; } lc_start(a, t, b, want); }
    int mode = adjust ? NLX_ADJ : NLX_LIST, ci = -1, loading = -1, press = -1, closing = 0, restore = 0, saved = 0, dirty = 1;
    int off0 = nu.loff;
    float st = H, sv = 0, sc = 0, scv = 0;
    double last = now_ms(), lpoll = now_ms(); struct pkt p;
    struct nu_prof pr = { 0, 0, 0, 0 };
    mu_busy = 1;
    lg("np: letra %s (%d s)", adjust ? "ajuste" : "buscar", want);
    for (;;) {
        double now = now_ms(); float dt = (float)(now - last) / 1000; last = now;
        if (now - lpoll >= 100) { lpoll = now; if (nu_poll() < 0) { lc_cancel(); nu_crossfade(); mu_busy = 0; return; } }   /* otra cancion */
        nu_pos_sync();
        if (lc_poll()) dirty = 1;
        if (loading >= 0) {
            int r = lc_ready(loading);
            if (r) {
                int i = loading; loading = -1; dirty = 1;
                struct lrc L; char src[160]; lc_path(i, src, sizeof src);
                int ok = r > 0 && !lx_load(src, &L);
                if (ok && !L.n) { lrc_free(&L); ok = 0; }
                if (!ok) toast(tr("No se pudo descargar"));
                else { nu_lyrics_from(&L); nu_lx_snap(); ci = i; mode = NLX_PREV; restore = 0; }
            }
        }
        /* la hoja (muelle) y el desplazamiento con inercia de la lista */
        float tgt = closing ? H + 14 : mode == NLX_LIST ? NLX_LT : NLX_PT;
        int moving = !nu_spring_done(st, sv, tgt, 0.5f);
        if (moving) { nu_spring(&st, &sv, tgt, 0.40f, 0.92f, dt); if (nu_spring_done(st, sv, tgt, 0.5f)) { st = tgt; sv = 0; } dirty = 1; }
        else if (closing) break;
        if (scv) {
            float mx = nlc * LX_ROW - (H - NLX_LT - NLX_HDR) + 8; if (mx < 0) mx = 0;
            sc += scv * (dt > 0.05f ? 0.05f : dt); scv *= powf(0.135f, dt > 0.05f ? 0.05f : dt);
            if (sc < 0) { sc = 0; scv = 0; } if (sc > mx) { sc = mx; scv = 0; }
            if (fabsf(scv) < 15) scv = 0;
            dirty = 1;
        }
        if (restore && st <= NU_LY0 + 2) { restore = 0; nu_lyrics_load(); nu_lx_snap(); }   /* ya tapada: la letra de verdad */
        if (nu_lyrics_tick()) dirty = 1;
        if (nu_lyrics_step()) dirty = 1;
        int spin = (lc_rc == -1 && mode == NLX_LIST) || loading >= 0, toast_on = now < toast_until + 40;
        if (dirty || spin || toast_on) { double t1 = now_ms(); nu_lx_render(mode, (int)(st + .5f), sc, ci, press, loading); nu_prof_add(&pr, now_ms() - t1); present(); dirty = 0; }
        int anim = moving || nu.lst || spin || toast_on || scv;
        int wait = 100;
        if (mode != NLX_LIST) { int nx = nu_lyrics_next_ms(); if (nx >= 0 && nx < wait) wait = nx; if (nu.synced && (nu.hold ? nu.playing : nu.ni.playing) && wait > 25) wait = 25; }
        if (!read_pkt(&p, anim ? 0 : wait) || !p.down) continue;
        mu_last_input = now_ms(); scv = 0;
        int sx = p.x, sy = p.y, top = (int)(st + .5f);
        if (back_zone(sx, sy)) {
            int bk = app_back_gesture(sx, sy);
            if (bk) {
                if (bk > 0) { if (mode == NLX_PREV) { mode = NLX_LIST; restore = 1; } else { if (mode == NLX_ADJ) nu.loff = off0; closing = 1; } }
                last = now_ms(); dirty = 1; continue;
            }
        }
        if (sy < top - 4 || closing) { drain_until_up(); continue; }       /* fuera de la hoja */
        if (mode == NLX_LIST) {
            if (sy < top + 56) {                                           /* cabecera */
                int h = sx < 64 ? -11 : sx > W - 64 ? -10 : 0;
                if (h) {
                    press = h; nu_lx_render(mode, top, sc, ci, press, loading); present(); drain_until_up(); press = -1; dirty = 1;
                    if (abs(cur_x - sx) > 40 || abs(cur_y - sy) > 40) continue;
                    if (h == -10) { lc_cancel(); if (restore) { restore = 0; nu_lyrics_load(); nu_lx_snap(); } closing = 1; }
                    else if (line_edit(tr("Título"), tr("Título"), t, sizeof t) && line_edit(tr("Artista"), tr("Artista"), a, sizeof a)) { lc_start(a, t, "", want); sc = 0; }
                    last = now_ms(); continue;
                }
                if (nu_lx_drag(mode, NLX_LT, &st, &sv, sy, sc, ci, loading)) { lc_cancel(); if (restore) { restore = 0; nu_lyrics_load(); nu_lx_snap(); } closing = 1; }
                last = now_ms(); dirty = 1; continue;
            }
            int ly = top + NLX_HDR;
            if (lc_rc == -1 || loading >= 0) { drain_until_up(); continue; }
            if (!nlc) {                                                    /* vacio / sin red: su boton */
                drain_until_up();
                if (sy < ly + 30 || sy > ly + 220) continue;
                if (lc_rc == 2) lc_start(a, t, b, want);
                else if (lc_rc != 99 && line_edit(tr("Título"), tr("Título"), t, sizeof t) && line_edit(tr("Artista"), tr("Artista"), a, sizeof a)) lc_start(a, t, "", want);
                last = now_ms(); dirty = 1; continue;
            }
            int row = sy >= ly ? (int)((sy - ly + sc) / LX_ROW) : -1; if (row >= nlc) row = -1;
            press = row; nu_lx_render(mode, top, sc, ci, press, loading); present();
            int moved = 0, lasty = sy; float sc0 = sc, v = 0; double lt = now_ms();
            float mx = nlc * LX_ROW - (H - NLX_LT - NLX_HDR) + 8; if (mx < 0) mx = 0;
            while (cur_down) {
                if (read_pkt(&p, 16) && !p.down) break;
                if (!moved && abs(cur_y - sy) > 8) { moved = 1; press = -1; }
                if (!moved) continue;
                double tn = now_ms(); if (tn > lt) v = 0.6f * v + 0.4f * (lasty - cur_y) * 1000.f / (float)(tn - lt);
                lasty = cur_y; lt = tn;
                sc = sc0 - (cur_y - sy);
                if (sc < 0) sc = sc / 3; if (sc > mx) sc = mx + (sc - mx) / 3;    /* bordes con resistencia */
                nu_lx_render(mode, top, sc, ci, -1, loading); present();
                while (read_pkt(&p, 0)) {}
            }
            press = -1; dirty = 1;
            if (moved) {
                if (now_ms() - lt > 80) v = 0;
                if (sc < 0) sc = 0; else if (sc > mx) sc = mx; else scv = v;
                last = now_ms(); continue;
            }
            if (row >= 0 && abs(cur_y - sy) < 20) loading = row;           /* abrir: lc_ready lo trae si falta */
            last = now_ms(); continue;
        }
        /* vista previa / ajuste */
        int h = -1;
        if (sy < top + 44) h = sx < 150 ? 5 : -2;
        else if (sy >= top + 108) h = 4;
        else if (nu.synced) h = lx_adjust_hit(sx, sy, top + 44);
        if (h == -2) {
            if (mode == NLX_ADJ && nu_lx_drag(mode, NLX_PT, &st, &sv, sy, sc, ci, loading)) { nu.loff = off0; closing = 1; }
            else drain_until_up();
            last = now_ms(); dirty = 1; continue;
        }
        if (h < 0) { drain_until_up(); continue; }
        press = h; nu_lx_render(mode, top, sc, ci, press, loading); present();
        int moved = 0, offd = nu.loff;
        while (cur_down) {
            if (read_pkt(&p, 16) && !p.down) break;
            if (h != 2 || (!moved && abs(cur_x - sx) <= 3)) continue;
            moved = 1;
            int o = lx_adjust_drag(offd, cur_x - sx);
            if (o != nu.loff) { nu.loff = o; nu_lyrics_tick(); }               /* la letra se mueve en vivo */
            nu_lyrics_step(); nu_pos_sync();
            nu_lx_render(mode, top, sc, ci, 2, loading); present();
            while (read_pkt(&p, 0)) {}
        }
        press = -1; dirty = 1; last = now_ms();
        if (moved) continue;
        if (h != 2 && (abs(cur_x - sx) > 30 || abs(cur_y - sy) > 30)) continue;   /* se fue del boton */
        if (h == 0 || h == 1) { nu.loff = lx_adjust_step(nu.loff, h ? 1 : -1); nu_lyrics_tick(); }
        else if (h == 5) { if (mode == NLX_PREV) { mode = NLX_LIST; restore = 1; } else { nu.loff = off0; nu_lyrics_tick(); closing = 1; } }
        else if (h == 4) {
            char src[600];
            if (mode == NLX_PREV) lc_path(ci, src, sizeof src); else lx_lrc_of(path, src, sizeof src);
            if (!lx_save(src, path, nu.synced ? nu.loff : 0)) { saved = 1; closing = 1; lc_cancel(); toast(mode == NLX_PREV ? tr("Letra guardada") : tr("Sincronización guardada")); }
            else toast(tr("No se pudo guardar"));
        }
    }
    nu_prof_log(&pr, "hoja de letras");
    lc_cancel();
    if (restore || (mode == NLX_PREV && !saved)) nu_lyrics_load();
    if (mode == NLX_ADJ && !saved) nu.loff = off0;
    nu_lx_snap(); nu_make_page();
    nu_render(fr0, 1); draw_toast(fr0); present();
    mu_busy = 0;
}

/* el menu "..." de la cancion que suena; devuelve NU_* si hay que salir de la hoja */
static int nu_song_menu(int *arg)
{
    struct nu_mi it[8]; int n = 0, trk = nu.trk;
    if (trk >= 0) {
        it[n++] = (struct nu_mi){ tr("Crear radio"), NI_RADIO, 0 };
        it[n++] = (struct nu_mi){ tr("Ir al álbum"), NI_ALB, 1 };
        it[n++] = (struct nu_mi){ tr("Ir al artista"), NI_ART, 0 };
    }
    { int g = n > 0; it[n++] = (struct nu_mi){ tr("Editar canción"), NI_TAGS, g }; }
    it[n++] = (struct nu_mi){ tr("Editar álbum"), NI_ALBTAGS, 0 };
    it[n++] = (struct nu_mi){ nu.nl ? tr("Cambiar letra") : tr("Buscar letra"), NI_LYR, 0 };
    if (nu.nl && nu.synced) it[n++] = (struct nu_mi){ tr("Ajustar sincronización"), NI_LYRSYNC, 0 };
    it[n++] = (struct nu_mi){ tr("Buscar carátula"), NI_COV, 0 };
    char sub[300]; snprintf(sub, sizeof sub, "%s%s%s", nu.ni.artist, nu.ni.artist[0] && nu.ni.album[0] ? " · " : "", nu.ni.album);
    int a = nu_menu(it, n, nu.ni.title, sub, nu.alb, nu.cov_ok ? nu.cov : 0, nu.lyr ? 46 : NU_TY - 30, 64);
    int act = a >= 0 ? it[a].ic : -1;
    char path[460]; snprintf(path, sizeof path, "%s", nu.ni.path);
    const char *artist = trk >= 0 ? ML->A[nu.alb].artist : nu.ni.artist, *album = trk >= 0 ? ML->A[nu.alb].title : nu.ni.album;
    if (act == NI_RADIO) { *arg = trk; return NU_RADIO; }
    if (act == NI_ALB) { *arg = nu.alb; return NU_ALBUM; }
    if (act == NI_ART) { *arg = ML->A[nu.alb].art; return NU_ARTIST; }
    if (act == NI_TAGS) { char *v[1] = { path }; tag_editor(v, 1); nu_npx_refresh(); }
    else if (act == NI_ALBTAGS) {   /* todas las canciones del album: las de la biblioteca o, si no, las de su carpeta */
        char **v = 0; int nv = 0;
        if (trk >= 0 && nu.alb >= 0 && ML) v = mu_album_paths(nu.alb, &nv);
        if (!v || !nv) { if (v) free_list(v, nv); v = album_files(path, &nv); }
        if (v && nv) tag_editor(v, nv);
        if (v) free_list(v, nv);
        nu_npx_refresh();
    }
    else if (act == NI_LYR || act == NI_LYRSYNC) { if (!nu.lyr) nu_toggle_lyrics(); nu_lx(act == NI_LYRSYNC); return NU_NONE; }   /* en la propia vista de letra */
    else if (act == NI_COV) { char *v[1] = { path }; cover_search(v, 1, artist, album); if (nu.alb >= 0) mu_cover_forget(nu.alb); nu.art_src = 0; nu_npx_refresh(); }
    if ((act == NI_TAGS || act == NI_ALBTAGS || act == NI_COV) && mu_open > 0 && mu_st[mu_sp]) mu_render(img_under, mu_st[mu_sp], 1);   /* esas pantallas reutilizan los bufers */
    if (nu.lyr) { nu.lcur = -1; nu_lyrics_tick(); nu.lsc = nu.lsc1; nu.lst = 0; }
    nu_render(fr0, 1); draw_toast(fr0); present();
    return NU_NONE;
}
static int nu_loop(int *arg)
{
    struct pkt p; double last = now_ms(), lpoll = 0;
    for (;;) {
        double now = now_ms(); float dt = (float)(now - last) / 1000; last = now;
        int bands = 0;
        if (now - lpoll >= 100) { lpoll = now; int r = nu_poll(); if (r < 0) { nu_crossfade(); last = now_ms(); continue; } bands |= r; }
        /* muelle de la caratula (pausa: mas pequena) */
        float tgt = nu.playing ? 1 : NU_PAUSE;
        int art_anim = !nu_spring_done(nu.sc, nu.sv, tgt, 0.0015f);
        if (art_anim) { nu_spring(&nu.sc, &nu.sv, tgt, 0.5f, 0.68f, dt); if (nu_spring_done(nu.sc, nu.sv, tgt, 0.0015f)) { nu.sc = tgt; nu.sv = 0; } bands |= NB_ART; }
        nu_pos_sync();                                               /* ancla fresca: el verso cambia en su marca */
        if (nu.lyr && nu_lyrics_tick()) bands |= NB_LYR;
        if (nu.lyr && nu_lyrics_step()) bands |= NB_LYR;
        int anim = art_anim || nu.lst || now < toast_until + 40;
        mu_busy = anim;
        if (bands) { double t1 = now_ms(); nu_present_bands(bands); static struct nu_prof pr; nu_prof_add(&pr, now_ms() - t1); if (pr.n >= 60) nu_prof_log(&pr, "reposo/franjas"); }
        else if (now < toast_until + 40) { nu_render(fr0, 1); draw_toast(fr0); present(); }
        int wait = 100;                                              /* letra sincronizada: despertar en el siguiente verso */
        if (!anim && nu.lyr) { int nx = nu_lyrics_next_ms(); if (nx >= 0 && nx < wait) wait = nx; if (nu.nl && nu.synced && (nu.hold ? nu.playing : nu.ni.playing) && wait > 25) wait = 25; }
        if (!read_pkt(&p, anim ? 0 : wait)) continue;
        if (!p.down) continue;
        mu_last_input = now_ms();
        int sx = p.x, sy = p.y, hit = nu_hit(sx, sy), moved = 0, lp = 0; double tdown = now_ms();
        /* abajo del todo: subir = al inicio */
        if (sy >= 456) {
            /* en cuanto sube (sin esperar a soltar: antes la respuesta llegaba ~190 ms tarde) */
            while (cur_down && read_pkt(&p, 1000)) if (sy - cur_y > 20) { moved = 1; break; }
            if (moved || sy - cur_y > 20) return NU_HOME;
            if (hit == 4) nu_toggle_lyrics();
            continue;
        }
        /* desde cualquiera de los bordes: el gesto comun de atras (burbuja) cierra la hoja */
        if (back_zone(sx, sy)) {
            int bk = app_back_gesture(sx, sy);
            if (bk > 0) { nu_sheet_build(); if (nu_loose) nu_sheet_anim(0, 0, H, 0.32f, 1, 0); else nu_sheet_anim(0, 0, NU_HM, 0.38f, 2, 0); return NU_CLOSED; }
            if (bk < 0) { last = now_ms(); continue; }
        }
        if (hit >= 0 && hit <= 2) { nu.pressed = hit; nu_band(NU_CB0, NU_CB1); present_rows(NU_CB0, NU_CB1); }
        while (cur_down) {
            if (read_pkt(&p, 30) && !p.down) break;
            int dx = cur_x - sx, dy = cur_y - sy;
            if (dy > 10 && dy > abs(dx)) {                           /* arrastrar hacia abajo: cerrar */
                if (nu.pressed >= 0) { nu.pressed = -1; nu_band(NU_CB0, NU_CB1); }
                if (nu_drag(sy)) return NU_CLOSED;
                moved = 2; break;
            }
            if (abs(dx) > 14 || abs(dy) > 14) moved = 1;
            if (hit == 6 && !moved && nu.nl && nu.synced && now_ms() - tdown > 500) { lp = 1; break; }   /* pulsacion larga: ajustar */
        }
        if (moved == 2) { last = now_ms(); continue; }
        if (lp) { drain_until_up(); nu_lx(1); last = now_ms(); continue; }
        int up_hit = nu_hit(cur_x, cur_y);
        if (nu.pressed >= 0) {
            int b = nu.pressed; nu.pressed = -1;
            if (up_hit == b) {
                nu_key(b);
                if (b == 1) { nu.anc = nu_pos(); nu.anc_t = now_ms(); nu.playing = !nu.playing; nu.hold = 1; nu.hold_t = now_ms(); }   /* la letra se para/sigue ya */
            }
            nu_band(NU_CB0, NU_CB1); present_rows(NU_CB0, NU_CB1);
            continue;
        }
        if (moved || up_hit != hit) continue;
        if (hit == 3) { int r = nu_song_menu(arg); if (r != NU_NONE) { if (mu_open > 0 && mu_st[mu_sp]) mu_render(img_under, mu_st[mu_sp], 1); nu_sheet_build(); nu_sheet_anim(0, 0, NU_HM, 0.36f, 2, 0); return r; } last = now_ms(); continue; }
        if (hit == 4 || hit == 5) { nu_toggle_lyrics(); last = now_ms(); continue; }
        if (hit == 7) { nu_lx(0); last = now_ms(); continue; }      /* sin letra: buscarla aqui mismo */
    }
}
/* abre Reproduciendo desde el mini reproductor (toque o arrastre hacia arriba). sx, sy: donde
 * empezo el toque (el dedo puede seguir abajo). Devuelve NU_* (arg: album/artista/pista). */
static int nu_open(int sx, int sy, int *arg)
{
    double t0 = now_ms();
    memcpy(img_under, fr0, HM_FRAME_BYTES);
    if (!nu_init()) { nu_free(); toast(tr("No se pudo abrir")); return NU_NONE; }
    nu_sheet_build();
    lg("np: preparar %.1f ms", now_ms() - t0);
    /* toque, arrastre hacia arriba o deslizar de lado (no abrir) */
    struct pkt p; int dragging = 0;
    while (cur_down) {
        if (read_pkt(&p, 20) && !p.down) break;
        if (sy - cur_y > 6) { dragging = 1; break; }
        if (abs(cur_x - sx) > 30) { while (cur_down && read_pkt(&p, 3000)) {} nu_free(); return NU_NONE; }
    }
    if (dragging) {
        float v = 0; int lasty = cur_y, top = NU_HM; double lt = now_ms(); struct nu_prof pr = { 0, 0, 0, 0 }; mu_busy = 1;
        while (cur_down) {
            if (read_pkt(&p, 20) && !p.down) break;
            double tn = now_ms(); if (tn > lt) v = 0.6f * v + 0.4f * (cur_y - lasty) * 1000.f / (float)(tn - lt);
            lasty = cur_y; lt = tn;
            top = NU_HM - (sy - cur_y); if (top > NU_HM) top = NU_HM; if (top < 0) top = 0;
            double t1 = now_ms(); nu_sheet_frame(top, nu_clampf(1 - (float)top / NU_HM, 0, 1)); nu_prof_add(&pr, now_ms() - t1); present();
        }
        nu_prof_log(&pr, "abrir con el dedo");
        if (now_ms() - lt > 80) v = 0;
        if (v > 300 || (top > NU_HM * 0.7f && v > -300)) { nu_sheet_anim((float)top, v, NU_HM, 0.34f, 0, 0); nu_free(); return NU_NONE; }
        nu_sheet_anim((float)top, v, 0, 0.40f, 0, 0);
    } else nu_sheet_anim(NU_HM, 0, 0, 0.44f, 0, 0);
    lg("musica -> Reproduciendo propio (%.0f ms)", now_ms() - t0);
    top_gesture_off++; int r = nu_loop(arg); top_gesture_off--;
    nu_free();
    mu_busy = 0;
    return r;
}

/* ------------------------------------------------------------------ menu contextual (iOS) */
/* layout: tarjeta de la fila arriba y la lista debajo; devuelve el indice o -1 */
static void nu_icon(uint16_t *d, int ic, int cx, int cy, uint16_t col, uint16_t bgc)
{
    switch (ic) {
    case NI_PLAY: nu_tri(d, cx - 5, cy, 13, 15, 1, col); break;
    case NI_SHUF: mu_shuffle_icon(d, cx - 9, cy, col); break;
    case NI_RADIO: nu_disc(d, cx, cy, 2.6f, col, 255, 0, H); nu_ring(d, cx, cy, 6.5f, 1.7f, col, 1); nu_ring(d, cx, cy, 10.5f, 1.7f, col, 1); break;
    case NI_ALB:
        fill_rrect(d, cx - 9, cy - 9, 18, 2, 1, col, 255); fill_rrect(d, cx - 9, cy + 7, 18, 2, 1, col, 255);
        fill_rrect(d, cx - 9, cy - 9, 2, 18, 1, col, 255); fill_rrect(d, cx + 7, cy - 9, 2, 18, 1, col, 255);
        nu_ring(d, cx, cy, 4, 1.6f, col, 0); nu_disc(d, cx, cy, 1.2f, col, 255, 0, H); break;
    case NI_ART: nu_disc(d, cx, cy - 4.5f, 4.2f, col, 255, 0, H); nu_disc(d, cx, cy + 10, 8.5f, col, 255, 0, cy + 9); break;
    case NI_TAGS: case NI_ALBTAGS:
        mu_line(d, cx - 5, cy + 5, cx + 5, cy - 5, 3.4f, col);
        nu_disc(d, cx - 7.2f, cy + 7.2f, 1.6f, col, 255, 0, H);
        mu_line(d, cx + 2.6f, cy - 6.4f, cx + 6.4f, cy - 2.6f, 1.2f, bgc); break;
    case NI_LYR: nu_quote_icon(d, cx, cy, col, bgc); break;
    case NI_LYRSYNC: nu_ring(d, cx, cy, 9, 1.7f, col, 0); mu_line(d, cx, cy, cx, cy - 5.5f, 1.7f, col); mu_line(d, cx, cy, cx + 4, cy + 2, 1.7f, col); break;
    case NI_COV:
        fill_rrect(d, cx - 10, cy - 8, 20, 2, 1, col, 255); fill_rrect(d, cx - 10, cy + 6, 20, 2, 1, col, 255);
        fill_rrect(d, cx - 10, cy - 8, 2, 16, 1, col, 255); fill_rrect(d, cx + 8, cy - 8, 2, 16, 1, col, 255);
        nu_disc(d, cx + 3.5f, cy - 2.5f, 1.8f, col, 255, 0, H);
        for (int i = 0; i < 6; i++) fill_rrect(d, cx - 6 + i, cy + 5 - i, 12 - 2 * i, 1, 0, col, 255);
        break;
    }
}
/* capa escalada desde (ax, ay) (esquina de anclaje) con su mascara y opacidad */
static void nu_layer(uint16_t *d, const uint16_t *src, const uint8_t *mk, int lw, int lh, int ax, int ay, float s, int alpha)
{
    int dw = (int)(lw * s + .5f), dh = (int)(lh * s + .5f);
    if (dw < 2 || dh < 2 || alpha <= 0) return;
    static int xm[W + 64]; if (dw > W + 64) dw = W + 64;
    for (int i = 0; i < dw; i++) { int k = i * lw / dw; xm[i] = k < lw ? k : lw - 1; }
    for (int j = 0; j < dh; j++) {
        int yy = ay + j; if (yy < 0 || yy >= H) continue;
        int sy = j * lh / dh; if (sy >= lh) sy = lh - 1;
        const uint16_t *sr = src + sy * lw; const uint8_t *mr = mk + sy * lw; uint16_t *dr = d + yy * W;
        for (int i = 0; i < dw; i++) {
            int xx = ax + i; if (xx < 0 || xx >= W) continue;
            unsigned a = mr[xm[i]] * alpha / 255; if (!a) continue;
            dr[xx] = a >= 255 ? sr[xm[i]] : mix565(sr[xm[i]], dr[xx], a);
        }
    }
}
static int nu_menu(const struct nu_mi *it, int n, const char *t1, const char *t2, int alb, const uint16_t *cov128, int ry, int rh)
{
    (void)rh;
    double tp = now_ms();
    const int cw = W - 32, chh = 64, mw = 244, ih = 44, gap = 8;
    int ngap = 0; for (int i = 1; i < n; i++) if (it[i].grp) ngap++;
    int mh = n * ih + ngap * gap, total = chh + 10 + mh;
    int cy0 = ry < 0 ? (H - total) / 2 : ry; if (cy0 < 36) cy0 = 36; if (cy0 > H - 12 - total) cy0 = H - 12 - total;
    int cy1 = cy0, cx = 16, mx = 16, my = cy1 + chh + 10;
    int iy[12]; { int y = my; for (int i = 0; i < n && i < 12; i++) { if (i && it[i].grp) y += gap; iy[i] = y; y += ih; } }
    /* fondo: desenfoque del fotograma actual (una vez) */
    uint16_t *under = img_base, *blur = img_sheet, *fin = img_app;
    memcpy(under, fr0, HM_FRAME_BYTES);
    cc_blur(under, blur);
    memcpy(fin, blur, HM_FRAME_BYTES);
    uint8_t *cmk = nu_mask_new(cw, chh, 14), *mmk = nu_mask_new(mw, mh, 13);
    uint16_t *cly = malloc(cw * chh * 2), *mly = malloc(mw * mh * 2);
    if (!cmk || !mmk || !cly || !mly) { free(cmk); free(mmk); free(cly); free(mly); return -1; }
    /* tarjeta: la fila "levantada" */
    for (int y = 0; y < chh; y++) for (int x = 0; x < cw; x++) { int a = cmk[y * cw + x]; if (a) { uint16_t *o = &fin[(cy1 + y) * W + cx + x]; *o = mix565(rgb(30, 30, 32), *o, a); } }
    if (alb >= 0) { double t; int none; const uint16_t *s = mu_cover(alb, MC_S, &t, &none); if (s) mu_blit(fin, s, MU_SS, cx + 10, cy1 + 8, 6, 255); else mu_placeholder(fin, cx + 10, cy1 + 8, MU_SS, 6); }
    else if (cov128) npx_blit_cover(fin, cov128, cx + 10, cy1 + 8, 48, 6);
    else mu_placeholder(fin, cx + 10, cy1 + 8, MU_SS, 6);
    mu_text_bold(fin, cx + 70, cy1 + 29, t1 ? t1 : "", 16, 0xFFFF, cw - 84);
    draw_text(fin, cx + 70, cy1 + 49, t2 ? t2 : "", 13, MU_GRAY, cw - 84, 0, H);
    /* lista: material oscuro translucido sobre el desenfoque */
    for (int y = 0; y < mh; y++) {
        int yy = my + y, ingap = 0;
        for (int i = 1; i < n; i++) if (it[i].grp && yy >= iy[i] - gap && yy < iy[i]) ingap = 1;
        for (int x = 0; x < mw; x++) {
            if (!mmk[y * mw + x]) continue;
            uint16_t *o = &fin[yy * W + mx + x];
            uint16_t c = ingap ? blend565(*o, rgb(10, 10, 11), 24) : blend565(*o, rgb(37, 37, 40), 25);
            *o = mix565(c, *o, mmk[y * mw + x]);
        }
    }
    for (int i = 0; i < n; i++) {
        if (i && !it[i].grp) for (int x = mx; x < mx + mw; x++) fin[iy[i] * W + x] = blend565(fin[iy[i] * W + x], rgb(84, 84, 88), 14);
        uint16_t bgc = fin[(iy[i] + ih / 2) * W + mx + mw - 14];
        draw_text(fin, mx + 16, iy[i] + 28, it[i].t, 16, 0xFFFF, mw - 64, 0, H);
        nu_icon(fin, it[i].ic, mx + mw - 26, iy[i] + ih / 2, 0xFFFF, bgc);
    }
    for (int y = 0; y < chh; y++) memcpy(cly + y * cw, fin + (cy1 + y) * W + cx, cw * 2);
    for (int y = 0; y < mh; y++) memcpy(mly + y * mw, fin + (my + y) * W + mx, mw * 2);
    lg("np: menu preparado en %.1f ms", now_ms() - tp);
    /* entrada: el fondo se desenfoca, la tarjeta sube a su sitio y la lista crece desde arriba */
    int from_y = ry < 0 ? cy1 : ry;
    float p = 0, v = 0; double last = now_ms(); struct nu_prof pr = { 0, 0, 0, 0 }; mu_busy = 1;
    for (;;) {
        double t = now_ms(); float dt = (float)(t - last) / 1000; last = t;
        nu_spring(&p, &v, 1, 0.36f, 0.76f, dt);
        int done = nu_spring_done(p, v, 1, 0.002f);
        double t1 = now_ms();
        if (done) memcpy(fr0, fin, HM_FRAME_BYTES);
        else {
            int k = (int)(nu_clampf(p, 0, 1) * 32 + .5f);
            for (int i = 0; i < W * H; i++) fr0[i] = blend565(under[i], blur[i], k);
            float pc = nu_clampf(p, 0, 1.2f);
            nu_layer(fr0, cly, cmk, cw, chh, cx, (int)(from_y + (cy1 - from_y) * pc), 1, 255);
            float s = 0.35f + 0.65f * p;
            nu_layer(fr0, mly, mmk, mw, mh, mx, my, s, (int)(nu_clampf(p * 1.8f, 0, 1) * 255));
        }
        nu_prof_add(&pr, now_ms() - t1);
        present();
        if (done) break;
    }
    nu_prof_log(&pr, "menu abrir");
    /* eleccion: arrastrando sin soltar (iOS) o tocando; fuera de la lista: cerrar */
    struct pkt pk; int sel = -1, hl = -1, first = 1, held = cur_down;   /* held: el dedo de la pulsacion larga */
    while (read_pkt(&pk, 0)) {}
    held = cur_down;
    for (;;) {
        if (!first || !cur_down) {
            first = 0;
            if (!read_pkt(&pk, 300)) continue;
            if (!pk.down) continue;
        }
        first = 0;
        int x0 = cur_x, y0 = cur_y, outside_start = 1;
        for (;;) {
            int h = -1;
            if (cur_x >= mx && cur_x < mx + mw) for (int i = 0; i < n; i++) if (cur_y >= iy[i] && cur_y < iy[i] + ih) h = i;
            if (h >= 0 || (cur_x >= mx && cur_x < mx + mw && cur_y >= my && cur_y < my + mh)) outside_start = 0;
            if (h != hl) {
                hl = h;
                memcpy(fr0 + my * W, fin + my * W, mh * W * 2);
                if (h >= 0) for (int y = iy[h]; y < iy[h] + ih; y++) for (int x = 0; x < mw; x++) { int a = mmk[(y - my) * mw + x]; if (a > 128) fr0[y * W + mx + x] = blend565(fin[y * W + mx + x], 0xFFFF, 5); }
                present_rows(my, my + mh);
            }
            if (!cur_down) break;
            if (!read_pkt(&pk, 3000)) break;
        }
        (void)x0; (void)y0;
        if (hl >= 0) { sel = hl; break; }
        if (outside_start && !held) break;                       /* toque fuera: cerrar */
        held = 0;                                                /* soltar la pulsacion larga: el menu se queda */
    }
    /* salida: se funde todo de vuelta */
    double t0 = now_ms();
    for (;;) {
        double t = (now_ms() - t0) / (sel >= 0 ? 160 : 220); if (t > 1) t = 1;
        int k = 32 - (int)(ease_out(t) * 32 + .5);
        double t1 = now_ms();
        if (k <= 0) memcpy(fr0, under, HM_FRAME_BYTES); else for (int i = 0; i < W * H; i++) fr0[i] = blend565(under[i], fin[i], k);
        nu_prof_add(&pr, now_ms() - t1);
        present();
        if (t >= 1) break;
    }
    nu_prof_log(&pr, "menu cerrar");
    mu_busy = 0;
    free(cmk); free(mmk); free(cly); free(mly);
    drain_until_up();
    return sel;
}


/* ------------------------------------------------------------------ Reproduciendo suelto */
/* La pagina de Reproduciendo como imagen (tarjeta de la multitarea). 0 si no se pudo. */
static int np_card(uint16_t *d)
{
    if (!nu_init()) { nu_free(); return 0; }
    nu_render(d, 1);
    nu_free();
    return 1;
}
/* Reproduciendo como app suelta (multitarea, icono del inicio), sin la biblioteca debajo.
 * home: el inicio (al cerrar la hoja hacia abajo se ve el inicio). ix, iy >= 0: crece desde el
 * icono; si no, la pantalla ya muestra la pagina (la tarjeta acaba de ampliarse). Al terminar
 * deja el inicio en pantalla. Devuelve NU_*. */
static int np_app_run(const uint16_t *home, int ix, int iy)
{
    double t0 = now_ms();
    if (!nu_init()) { nu_free(); toast(tr("No se pudo abrir")); return NU_HOME; }
    memcpy(img_under, home, HM_FRAME_BYTES);
    if (ix >= 0) {
        uint16_t *pg = img_app; nu_render(pg, 1);
        for (;;) {
            double t = (now_ms() - t0) / 300; if (t > 1) t = 1; double e = ease_out(t);
            int w = (int)(L.icon + (W - L.icon) * e), h = (int)(L.icon + (H - L.icon) * e);
            zoom_frame(fr0, home, 32 - (int)(8 * e), pg, (int)(ix * (1 - e)), (int)(iy * (1 - e)), w, h, win_radius(w, h, e));
            present(); if (t >= 1) break;
        }
    }
    nu_render(fr0, 1); present();
    lg("reproduciendo (suelto) en %.0f ms", now_ms() - t0);
    int arg = -1, r;
    nu_loose = 1;
    for (;;) {
        top_gesture_off++; r = nu_loop(&arg); top_gesture_off--;
        if (r != NU_HOME || ix >= 0) break;
        /* abierto sobre otra pantalla: la hoja sube siguiendo al dedo y deja ver lo de debajo */
        uint16_t *sh = img_app; memcpy(sh, fr0, HM_FRAME_BYTES);
        struct pkt p; int off = 0, last = cur_y; double vel = 0, lt = now_ms();
        while (cur_down) {
            int got = read_pkt(&p, 0); while (got && p.down && read_pkt(&p, 0)) {}
            double t = now_ms(); if (t > lt) { vel = 0.6 * vel + 0.4 * (cur_y - last) / (t - lt); last = cur_y; lt = t; }
            off = cur_y - (H - 12); if (off > 0) off = 0; if (off < -H) off = -H;
            compose_slide(fr0, home, sh, 0, off, 32 - 11 * (H + off) / H);
            present();
            if (got && !p.down) break;
            if (!got) msleep(2);
        }
        int close = off < -H / 4 || vel < -0.35, from = off, to = close ? -H : 0; double t0 = now_ms();
        for (;;) {
            double t = (now_ms() - t0) / 220.0; if (t > 1) t = 1;
            int o = from + (int)((to - from) * ease_out(t));
            compose_slide(fr0, home, sh, 0, o, 32 - 11 * (H + o) / H);
            present(); if (t >= 1) break;
        }
        lg("reproduciendo (suelto): %s hacia arriba", close ? "cerrado" : "vuelve");
        if (close) { memcpy(fr0, home, HM_FRAME_BYTES); r = NU_CLOSED; break; }
        nu_render(fr0, 1); present();
    }
    if (r == NU_HOME) {             /* subir desde la barra: la pagina se encoge hacia el centro */
        memcpy(img_app, fr0, HM_FRAME_BYTES);
        card_to_home(home, img_app, 0, 0, W, H);
    } else if (r != NU_CLOSED) sw_reveal_home(home, 22);
    nu_loose = 0;
    nu_free(); mu_busy = 0;
    drain_until_up();
    return r;
}

/* ------------------------------------------------------------------ inicio desde apps propias */
/* Subir desde la barra en una app propia (Musica, Archivos, Ajustes, Apps...), con fr0 = la
 * pantalla de la app: la ventana se encoge siguiendo al dedo; al soltar va al inicio; si se
 * mantiene quieta 1 s, multitarea (la app y el reproductor a su izquierda). Devuelve 0 si hay
 * que seguir en la app (la app repinta) o 1 si la app debe salir SIN su animacion de salida
 * (el inicio ya esta en pantalla). self: indice de la app en apps[]. */
static uint16_t *own_card; static int own_card_app = -1;
static int own_home_gesture(int sx, int sy, int self)
{
    if (!own_card) own_card = malloc(HM_FRAME_BYTES);
    if (!own_card) { drain_until_up(); return 1; }
    memcpy(own_card, fr0, HM_FRAME_BYTES); own_card_app = self; last_app = self;
    uint16_t *home = img_base, *bg = img_app, *bg2 = img_under;
    render_launcher(home, now_ms());
    dim_buf(bg, home, W * H, 22); dim_buf(bg2, home, W * H, 11);
    have_under = 0;
    static uint16_t *phb; if (!phb) phb = lazy_alloc((void **)&phb, HM_FRAME_BYTES, 0);
    const uint16_t *ph = 0;          /* la tarjeta del reproductor se prepara solo si se mantiene el gesto */
    sw_player_first = 0;
    struct pkt p; double vel = 0, lt = now_ms(), still_t = 0, arm_t = 0, prog = 0; int ly = sy, lx = sx, armed = 0;
    double ht[16]; int hy[16], hn = 0;
    for (;;) {
        int got = read_pkt(&p, 0);
        while (got && p.down && read_pkt(&p, 0)) {}
        if (got && !p.down) break;
        double t = now_ms();
        if (t > lt) { vel = 0.6 * vel + 0.4 * (cur_y - ly) / (t - lt); ly = cur_y; lx = cur_x; lt = t; }
        ht[hn % 16] = t; hy[hn % 16] = cur_y; hn++;
        double spd = 0;
        for (int i = 1; i < 16 && i < hn; i++) { int j = (hn - 1 - i) % 16; if (t - ht[j] >= 80) { spd = abs(cur_y - hy[j]) / (t - ht[j]); break; } }
        prog = (sy - cur_y) / 300.0; if (prog < 0) prog = 0; if (prog > 1.2) prog = 1.2;
        if (!armed && prog > 0.12 && hn > 3 && spd < 0.15) { if (!still_t) still_t = t; else if (t - still_t > SW_HOLD_MS) { armed = 1; arm_t = t; lg("multitarea armada (app propia)"); } }
        else if (spd >= 0.15) still_t = 0;
        float s = 1 - 0.40f * (float)(prog > 1 ? 1 : prog);
        double a = armed ? (t - arm_t) / 200.0 : 0; if (a > 1) a = 1;
        if (!ph && phb && (armed || (still_t && t - still_t > 200))) { player_card(phb); ph = phb; }
        int peek = ph && (armed || (still_t && t - still_t > 300));
        switcher_frame(armed && a > 0.5 ? bg2 : bg, own_card, ph, (lx - sx) / (W * CARD_S + 18.0), s, (sy - cur_y) * 0.35, peek, armed && a >= 1);
        if (!cur_down) break;
        if (!got) msleep(2);
    }
    if (armed) {
        double off = 0;
        { double t0 = now_ms(); int ly0 = sy - cur_y; for (;;) { double t = (now_ms() - t0) / 220.0; if (t > 1) t = 1; double e = ease_out(t);
            float s = (float)((1 - 0.40 * (prog > 1 ? 1 : prog)) + (CARD_S - (1 - 0.40 * (prog > 1 ? 1 : prog))) * e);
            switcher_frame(bg2, own_card, ph, off, s, ly0 * 0.35 * (1 - e), ph != 0, 1); if (t >= 1) break; } }
        int r = sw_loop(bg2, own_card, ph, 1, &off);
        if (r == SW_APP) return 0;
        if (r == SW_HOME) { int x, y, w, h; card_geom(off < 0.5 ? 0 : 1, off, CARD_S, 0, &x, &y, &w, &h); card_to_home(home, off < 0.5 ? own_card : ph, x, y, w, h); return 1; }
        if (r == SW_CLOSED) { sw_reveal_home(home, 11); last_app = -1; own_card_app = -1; return 1; }
        np_app_run(home, -1, -1);       /* el reproductor: la app se cierra (solo una a la vez) */
        return 1;
    }
    if (prog > 0.28 || vel < -0.5) {
        float s = 1 - 0.40f * (float)(prog > 1 ? 1 : prog);
        int x, y, w, h; card_geom(0, (lx - sx) / (W * CARD_S + 18.0), s, (sy - cur_y) * 0.35, &x, &y, &w, &h);
        card_to_home(home, own_card, x, y, w, h);
        return 1;
    }
    double t0 = now_ms(), p0 = prog, o0 = (lx - sx) / (W * CARD_S + 18.0);   /* no llega: vuelve */
    for (;;) {
        double t = (now_ms() - t0) / 200.0; if (t > 1) t = 1;
        double e = ease_out(t), pr = p0 * (1 - e);
        switcher_frame(bg, own_card, ph, o0 * (1 - e), 1 - 0.40f * (float)(pr > 1 ? 1 : pr), (sy - cur_y) * 0.35 * (1 - e), 0, 0);
        if (t >= 1) break;
    }
    memcpy(fr0, own_card, HM_FRAME_BYTES);
    return 0;
}

/* Reproduciendo abierto desde un gesto sobre otra pantalla (que ya muestra la pagina: la hoja
 * acaba de entrar). Devuelve 1 si el usuario subio desde la barra (ir al inicio). */
static int np_from_screen(const uint16_t *under)
{
    int r = np_app_run(under, -1, -1);
    return r == NU_HOME;
}
