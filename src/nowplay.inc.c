/* nowplay.inc.c - estado compartido de "lo que suena" para el centro de control, los
 * widgets y Musica. Un hilo lo actualiza cada segundo (np_get ~20 ms) y, al cambiar de pista,
 * pide la caratula (np_cover, hasta ~0,3 s sin cache): nunca dentro de una animacion. */
static pthread_mutex_t npx_mu = PTHREAD_MUTEX_INITIALIZER;
static struct np_info npx;                 /* copia publicada */
static uint16_t npx_cov[128 * 128]; static int npx_cov_ok; static uint32_t npx_cov_track = 0xffffffffu;
static volatile int npx_changed;           /* algo nuevo para pintar */
static void *npx_thread(void *a)
{
    (void)a;
    static uint16_t cov[128 * 128];
    for (;;) {
        struct np_info i; memset(&i, 0, sizeof i);
        int r = np_get(&i);
        pthread_mutex_lock(&npx_mu);
        int ch = r > 0 && (i.track_id != npx.track_id || i.playing != npx.playing || strcmp(i.title, npx.title));
        if (r > 0) npx = i; else npx.source = 0;
        pthread_mutex_unlock(&npx_mu);
        if (r > 0 && i.track_id != npx_cov_track) {
            int ok = i.path[0] && np_cover(i.path, cov) == 0;
            pthread_mutex_lock(&npx_mu);
            if (ok) memcpy(npx_cov, cov, sizeof cov);
            npx_cov_ok = ok; npx_cov_track = i.track_id;
            pthread_mutex_unlock(&npx_mu);
            ch = 1;
        }
        if (ch) npx_changed = 1;
        msleep(1000);
    }
    return 0;
}
static void npx_start(void) { pthread_t th; if (!pthread_create(&th, 0, npx_thread, 0)) pthread_detach(th); }
static void npx_snapshot(struct np_info *o, uint16_t *cov, int *cov_ok)
{
    pthread_mutex_lock(&npx_mu);
    *o = npx; if (cov) memcpy(cov, npx_cov, sizeof npx_cov); if (cov_ok) *cov_ok = npx_cov_ok;
    pthread_mutex_unlock(&npx_mu);
}
/* caratula 128x128 escalada (caja) a s x s en (x, y), esquinas redondeadas */
static void npx_blit_cover(uint16_t *d, const uint16_t *cov, int x, int y, int s, float rad)
{
    for (int yy = 0; yy < s; yy++) {
        int dy = y + yy; if (dy < 0 || dy >= H) continue;
        int sy = yy * 128 / s;
        for (int xx = 0; xx < s; xx++) {
            int dx = x + xx; if (dx < 0 || dx >= W) continue;
            /* esquina redondeada simple */
            float cx = xx < rad ? rad - xx - .5f : xx >= s - rad ? xx - (s - rad) + .5f : 0;
            float cy = yy < rad ? rad - yy - .5f : yy >= s - rad ? yy - (s - rad) + .5f : 0;
            if (cx > 0 && cy > 0 && cx * cx + cy * cy > rad * rad) continue;
            d[dy * W + dx] = cov[sy * 128 + xx * 128 / s];
        }
    }
}
