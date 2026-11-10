/* settings.inc.c - ajustes del inicio ("Pantalla de inicio", vista previa) y selector de fondos
 * con miniaturas de las imagenes de la SD. */
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;

static int run_wall(const char *in, const char *out, int w, int h)
{
    char ws[8], hs[8]; snprintf(ws, sizeof ws, "%d", w); snprintf(hs, sizeof hs, "%d", h);
    char *argv[] = { ASSETS "/hibywall", (char *)in, (char *)out, ws, hs, 0 };
    pid_t pid;
    if (posix_spawn(&pid, argv[0], 0, 0, argv, environ)) return -1;
    int stt; waitpid(pid, &stt, 0);
    return WIFEXITED(stt) ? WEXITSTATUS(stt) : -1;
}

/* ------------------------------------------------------------------ ajustes del inicio */
/* "Pantalla de inicio": pagina de Ajustes (settings_app) que se abre desde la pildora
 * "Ajustes" del modo edicion y desde Personalizar > Inicio. Cabecera con la vista previa del
 * inicio y de la pagina de widgets (miniaturas 96x144 rehechas al cambiar algo). */
static struct sprite s_title;          /* (hoja antigua; relabel_apps la libera) */
static int home_prev_ok;
static uint16_t hp_home[98 * 146], hp_wg[98 * 146];   /* miniatura con filo, ya sobre el fondo negro de Ajustes */
static void wg_compose(void);
static uint8_t *wg_rrmask(int w, int h, float r, float inset);
static int wg_page_on, wg_cache_ok;
static float wg_scroll;
static uint16_t *wg_cache;
static void hp_shrink(const uint16_t *src, uint16_t *dst)
{
    static uint8_t *mo, *mi;
    if (!mo) { mo = wg_rrmask(98, 146, 13, 0); mi = wg_rrmask(96, 144, 12, 0); }
    for (int y = 0; y < 146; y++) for (int x = 0; x < 98; x++) dst[y * 98 + x] = mo ? mix565(rgb(72, 72, 76), 0, mo[y * 98 + x]) : 0;
    for (int y = 0; y < 144; y++) for (int x = 0; x < 96; x++) {
        int xa = x * W / 96, xb = (x + 1) * W / 96, ya = y * H / 144, yb = (y + 1) * H / 144, r = 0, g = 0, b = 0, n = 0;
        for (int yy = ya; yy < yb; yy++) for (int xx = xa; xx < xb; xx++) { uint16_t q = src[yy * W + xx]; r += q >> 11; g += (q >> 5) & 63; b += q & 31; n++; }
        uint16_t c = (uint16_t)((r / n) << 11 | (g / n) << 5 | (b / n)), *o = &dst[(y + 1) * 98 + x + 1];
        *o = mi ? mix565(c, *o, mi[y * 96 + x]) : c;
    }
}
static void home_prev_build(void)
{
    float s = V.scroll; int e = V.edit, pa = V.pressed_app, da = V.drag_app, ha = hud_active;
    V.scroll = 0; V.edit = 0; V.pressed_app = -1; V.drag_app = -1; hud_active = 0;
    render_launcher(img_sheet, now_ms());
    V.scroll = s; V.edit = e; V.pressed_app = pa; V.drag_app = da; hud_active = ha;
    hp_shrink(img_sheet, hp_home);
    float ws = wg_scroll; wg_scroll = 0; wg_compose(); wg_scroll = ws;
    if (wg_cache) { memcpy(wg_cache, img_sheet, 30 * W * 2); hp_shrink(wg_cache, hp_wg); }
    wg_cache_ok = 0; have_under = 0;                   /* img_sheet reutilizado */
    home_prev_ok = 1;
}
static void hp_blit(uint16_t *d, const uint16_t *src, int x0, int y0, int dim)
{
    for (int y = 0; y < 146; y++) {
        int Y = y0 + y; if (Y < 0 || Y >= H) continue;
        if (!dim) { memcpy(d + Y * W + x0, src + y * 98, 98 * 2); continue; }
        for (int x = 0; x < 98; x++) d[Y * W + x0 + x] = dim565(src[y * 98 + x], 12);
    }
}
static void home_hero(uint16_t *d, int y, int h)
{
    (void)h;
    if (!home_prev_ok) home_prev_build();
    int x1 = W / 2 - 98 - 15, x2 = W / 2 + 15, ty = y + 10;
    hp_blit(d, hp_home, x1, ty, 0);
    hp_blit(d, hp_wg, x2, ty, !wg_page_on);
    const char *a = tr("Inicio"), *b = wg_page_on ? tr("Widgets") : tr("Desactivado");
    int aw = tw_n(a, 13, -1), bw = tw_n(b, 13, -1); if (aw > 110) aw = 110; if (bw > 110) bw = 110;
    draw_text(d, x1 + 49 - aw / 2, ty + 146 + 21, a, 13, rgb(142, 142, 147), 110, 0, H);
    draw_text(d, x2 + 49 - bw / 2, ty + 146 + 21, b, 13, rgb(142, 142, 147), 110, 0, H);
}

static void wallpaper_picker(void);
static void sync_from_state(void);
struct spage;
static void page_home(struct spage *p);
static void open_page(void (*build)(struct spage *), const char *back);
static void fs_transition(const uint16_t *from, const uint16_t *to, int dir, float start);
/* desde la pildora "Ajustes" del modo edicion del inicio */
static void launcher_settings(void)
{
    V.edit = 0; V.drag_app = -1; V.pressed_app = -1; lconf_save();
    sync_from_state(); home_prev_ok = 0;
    open_page(page_home, "Atrás");               /* entra desde la derecha sobre el inicio */
    memcpy(img_app, img_under, HM_FRAME_BYTES);  /* open_page deja su ultima imagen en img_under */
    render_launcher(img_base, now_ms());
    fs_transition(img_app, img_base, -1, 0);
    drain_until_up();
    launcher_present();
}

/* ------------------------------------------------------------------ selector de fondos */
#define MAXIMG 30
#define TW 96
#define TH 144
static char img_path[MAXIMG][200];
static int nimg;
static uint16_t *thumbs[MAXIMG + 1];   /* [0] = Aurora (por defecto) */
static volatile int thumb_ready[MAXIMG + 1];

/* por defecto la carpeta Pictures de la SD (se crea si no existe); "Toda la SD" busca
 * tambien en la raiz y en las carpetas habituales de imagenes */
static int pick_all;
static void find_images(void)
{
    nimg = 0;
    mkdir("/usr/data/mnt/sd_0/Pictures", 0755);
    const char *mine[] = { "/usr/data/mnt/sd_0/Pictures", "/usr/data/hibymod/fondos", 0 };
    const char *all[] = { "/usr/data/mnt/sd_0/Pictures", "/usr/data/mnt/sd_0", "/usr/data/mnt/sd_0/Fondos", "/usr/data/mnt/sd_0/Wallpapers",
                          "/usr/data/mnt/sd_0/DCIM", "/usr/data/mnt/sd_0/DCIM/Camera", "/usr/data/mnt/sd_0/Download", "/usr/data/hibymod/fondos", 0 };
    const char **dirs = pick_all ? all : mine;
    for (int d = 0; dirs[d] && nimg < MAXIMG; d++) {
        DIR *dd = opendir(dirs[d]); if (!dd) continue;
        struct dirent *e;
        while ((e = readdir(dd)) && nimg < MAXIMG) {
            const char *n = e->d_name, *ext = strrchr(n, '.');
            if (!ext || n[0] == '.') continue;
            if (strcasecmp(ext, ".jpg") && strcasecmp(ext, ".jpeg") && strcasecmp(ext, ".png")) continue;
            snprintf(img_path[nimg++], 200, "%s/%s", dirs[d], n);
        }
        closedir(dd);
    }
}
static volatile int thumbs_busy;
static void *thumb_thread(void *arg)
{
    (void)arg; thumbs_busy = 1; thread_nice(10);
    mkdir("/tmp/hibymod/thumbs", 0755);
    for (int i = 0; i < nimg; i++) {
        char out[64]; snprintf(out, sizeof out, "/tmp/hibymod/thumbs/%d.raw", i);
        if (run_wall(img_path[i], out, TW, TH) == 0) {
            int f = open(out, O_RDONLY);
            if (f >= 0) { if (!thumbs[i + 1]) thumbs[i + 1] = malloc(TW * TH * 2); read(f, thumbs[i + 1], TW * TH * 2); close(f); thumb_ready[i + 1] = 1; }
            unlink(out);
        } else thumb_ready[i + 1] = -1;
    }
    thumbs_busy = 0;
    return 0;
}

static struct sprite s_pick_title, s_nosd, s_cancel;
static int pick_off;   /* 1: modo "elegir imagen" (sin la casilla Aurora) */
static void render_picker(uint16_t *dst, int scroll, int sel)
{
    for (int i = 0; i < W * H; i++) dst[i] = rgb(0, 0, 0);
    int n = nimg + 1 - pick_off;
    for (int k = 0; k < n; k++) {
        int i = k + pick_off, col = k % 3, row = k / 3;
        int x = 8 + col * (TW + 4), y = 64 + row * (TH + 4) - scroll;
        if (y + TH < 56 || y > H) continue;
        if (thumb_ready[i] == 1 && thumbs[i]) {
            for (int yy = 0; yy < TH; yy++) { int dy = y + yy; if (dy >= 56 && dy < H) memcpy(dst + dy * W + x, thumbs[i] + yy * TW, TW * 2); }
        } else fill_rrect(dst, x, y, TW, TH, 6, rgb(44, 44, 48), 255);
        if (i == sel) { fill_rrect(dst, x, y, TW, TH, 6, rgb(255, 255, 255), 70); }
    }
    for (int y = 0; y < 56; y++) for (int x = 0; x < W; x++) dst[y * W + x] = rgb(18, 18, 20);
    memcpy(dst, fr1, 0);
    blit(dst, &s_pick_title, (W - s_pick_title.w) / 2, 22, 32, 255);
    blit(dst, &s_cancel, W - 14 - s_cancel.w, 22, 32, 255);
    draw_text(dst, 14, 37, pick_all ? tr("Pictures") : tr("Toda la SD"), 15, rgb(10, 132, 255), 0, 0, H);
    if (!nimg) blit(dst, &s_nosd, (W - s_nosd.w) / 2, 260, 32, 255);
}

static int picker_run(char *out, int cap);
static void sprite_free(struct sprite *s);
static void wallpaper_picker(void) { picker_run(0, 0); }
/* elegir una imagen de la SD (devuelve 1 y la ruta en out) */
static int image_picker(char *out, int cap) { sprite_free(&s_pick_title); text_sprite(&s_pick_title, tr("Elegir imagen"), 16.f, 0xFFFF, 0); int r = picker_run(out, cap); sprite_free(&s_pick_title); return r; }
static int picker_run(char *out, int cap)
{
    pick_off = out ? 1 : 0;
    if (!s_pick_title.w) {
        text_sprite(&s_pick_title, tr("Fondo de pantalla"), 16.f, 0xFFFF, 0);
        text_sprite(&s_cancel, tr("Cerrar"), 15.f, rgb(10, 132, 255), 0);
        text_sprite(&s_nosd, tr("Sin imágenes en /Pictures"), 13.f, rgb(152, 152, 159), 0);
    }
    pick_all = 0;
reload:
    while (thumbs_busy) { msleep(20); st->shell_beat++; }
    find_images();
    lg("fondos: %d imagenes", nimg);
    if (!thumbs[0]) thumbs[0] = malloc(TW * TH * 2);
    for (int y = 0; y < TH; y++) for (int x = 0; x < TW; x++) thumbs[0][y * TW + x] = 0;
    {   /* Aurora: reducir wall.raw */
        static uint16_t def[W * H]; int f = open(ASSETS "/wall.raw", O_RDONLY);
        if (f >= 0) { read(f, def, sizeof def); close(f); for (int y = 0; y < TH; y++) for (int x = 0; x < TW; x++) thumbs[0][y * TW + x] = def[(y * H / TH) * W + x * W / TW]; }
        thumb_ready[0] = 1;
    }
    for (int i = 1; i <= MAXIMG; i++) thumb_ready[i] = 0;
    if (!thumbs_busy) { pthread_t th; pthread_create(&th, 0, thumb_thread, 0); pthread_detach(th); }
    int scroll = 0, rows = (nimg + 1 - pick_off + 2) / 3, maxs = rows * (TH + 4) + 64 - H; if (maxs < 0) maxs = 0;
    struct pkt p;
    for (;;) {
        render_picker(fr0, scroll, -1); present();
        if (!read_pkt(&p, 120) || !p.down) continue;
        int sx = p.x, sy = p.y, s0 = scroll, moved = 0;
        while (cur_down && read_pkt(&p, 40)) {
            if (abs(cur_y - sy) > 8) moved = 1;
            if (moved) { scroll = s0 - (cur_y - sy); if (scroll < 0) scroll = 0; if (scroll > maxs) scroll = maxs; render_picker(fr0, scroll, -1); present(); }
        }
        if (moved) continue;
        if (sy < 56) {
            if (sx > W - 90) { if (out) return 0; break; }
            if (sx < 110) { pick_all = !pick_all; goto reload; }
            continue;
        }
        int col = (sx - 8) / (TW + 4), row = (sy - 64 + scroll) / (TH + 4), i = row * 3 + col + pick_off;
        if (col < 0 || col > 2 || i < pick_off || i > nimg) continue;
        render_picker(fr0, scroll, i); present();
        if (out) { snprintf(out, cap, "%s", img_path[i - 1]); return 1; }
        if (i == 0) { L.wall[0] = 0; unlink(ASSETS "/wall_custom.raw"); }
        else {
            toast(tr("Aplicando fondo…"));
            int rc = run_wall(img_path[i - 1], ASSETS "/wall_custom.raw.tmp", W, H);
            if (rc != 0) { toast(rc == 3 ? tr("Imagen demasiado grande") : tr("No se pudo leer la imagen")); continue; }
            rename(ASSETS "/wall_custom.raw.tmp", ASSETS "/wall_custom.raw");
            snprintf(L.wall, sizeof L.wall, "%s", img_path[i - 1]);
        }
        load_wall(); lconf_save();
        break;
    }
    /* esperar a que termine el hilo de miniaturas antes de liberar nada (no se libera: se reutiliza) */
    return 0;
}
