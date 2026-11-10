/* settings.inc.c - hoja "Inicio" del launcher (tamano de iconos, columnas, fondo) y selector
 * de fondos con miniaturas de las imagenes de la SD. */
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

/* ------------------------------------------------------------------ hoja de ajustes */
static struct sprite s_title, s_done, s_icons, s_cols, s_wall, s_reset, s_choose;
static struct sprite s_opt_icon[3], s_opt_col[3];
#define SHEET_Y 150

static void seg(uint16_t *dst, int x, int y, int w, int n, int sel, struct sprite *labels)
{
    fill_rrect(dst, x, y, w, 32, 9, rgb(118, 118, 128), 60);
    int cw = w / n;
    fill_rrect(dst, x + sel * cw + 2, y + 2, cw - 4, 28, 7, rgb(99, 99, 102), 255);
    for (int i = 0; i < n; i++) blit(dst, &labels[i], x + i * cw + (cw - labels[i].w) / 2, y + (32 - labels[i].h) / 2, 32, 255);
}

static void render_settings(uint16_t *dst, int sheet_y)
{
    render_launcher(dst, now_ms());
    for (int i = 0; i < W * H; i++) dst[i] = dim565(dst[i], 16);
    int y = sheet_y;
    fill_rrect(dst, 0, y, W, H - y + 20, 16, rgb(28, 28, 30), 250);
    fill_rrect(dst, W / 2 - 18, y + 6, 36, 5, 2.5f, rgb(90, 90, 96), 255);
    blit(dst, &s_title, (W - s_title.w) / 2, y + 16, 32, 255);
    blit(dst, &s_done, W - 16 - s_done.w, y + 16, 32, 255);
    blit(dst, &s_icons, 18, y + 58, 32, 255);
    seg(dst, 18, y + 80, W - 36, 3, sz_idx(), s_opt_icon);
    blit(dst, &s_cols, 18, y + 124, 32, 255);
    seg(dst, 18, y + 146, W - 36, 3, L.cols - 3, s_opt_col);
    blit(dst, &s_wall, 18, y + 190, 32, 255);
    /* miniatura del fondo actual + boton elegir */
    static uint16_t th[48 * 72];
    for (int yy = 0; yy < 72; yy++) for (int xx = 0; xx < 48; xx++) th[yy * 48 + xx] = wall[(yy * H / 72) * W + xx * W / 48];
    for (int yy = 0; yy < 72; yy++) { int dy = y + 212 + yy; if (dy < H) memcpy(dst + dy * W + 18, th + yy * 48, 96); }
    fill_rrect(dst, 78, y + 232, W - 96, 34, 10, rgb(44, 44, 48), 255);
    blit(dst, &s_choose, 78 + (W - 96 - s_choose.w) / 2, y + 232 + (34 - s_choose.h) / 2, 32, 255);
    blit(dst, &s_reset, (W - s_reset.w) / 2, y + 296, 32, 255);
}

static void wallpaper_picker(void);

static void launcher_settings(void)
{
    if (!s_title.w) {
        text_sprite(&s_title, tr("Inicio"), 16.f, 0xFFFF, 0);
        text_sprite(&s_done, tr("Listo"), 15.f, rgb(10, 132, 255), 0);
        text_sprite(&s_icons, tr("Tamaño de iconos"), 13.f, rgb(152, 152, 159), 0);
        text_sprite(&s_cols, tr("Columnas"), 13.f, rgb(152, 152, 159), 0);
        text_sprite(&s_wall, tr("Fondo de pantalla"), 13.f, rgb(152, 152, 159), 0);
        text_sprite(&s_reset, tr("Restablecer inicio"), 15.f, rgb(255, 69, 58), 0);
        text_sprite(&s_choose, tr("Elegir de la SD…"), 14.f, 0xFFFF, 0);
        const char *oi[3] = { tr("Pequeños"), tr("Normales"), tr("Grandes") }, *oc[3] = { "3", "4", "5" };
        for (int i = 0; i < 3; i++) { text_sprite(&s_opt_icon[i], oi[i], 13.f, 0xFFFF, 0); text_sprite(&s_opt_col[i], oc[i], 13.f, 0xFFFF, 0); }
    }
    /* sube la hoja */
    double t0 = now_ms();
    for (;;) { double t = (now_ms() - t0) / 240; if (t > 1) t = 1; render_settings(fr0, SHEET_Y + (int)((H - SHEET_Y) * (1 - ease_out(t)))); present(); if (t >= 1) break; }
    struct pkt p;
    for (;;) {
        if (!read_pkt(&p, 300)) continue;
        if (!p.down) continue;
        int x = p.x, y = p.y - SHEET_Y;
        while (cur_down && read_pkt(&p, 3000)) {}
        if (y < 0 || (y < 44 && x > W - 90)) break;          /* fuera de la hoja o "Listo" */
        if (y >= 80 && y < 112) { int i = (x - 18) * 3 / (W - 36); if (i < 0) i = 0; if (i > 2) i = 2; L.icon = 48 + 8 * i; }
        else if (y >= 146 && y < 178) { int i = (x - 18) * 3 / (W - 36); if (i < 0) i = 0; if (i > 2) i = 2; L.cols = 3 + i; }
        else if (y >= 212 && y < 284) { wallpaper_picker(); }
        else if (y >= 288 && y < 320) { lconf_default(); unlink(ASSETS "/wall_custom.raw"); load_wall(); cur_page = 0; V.scroll = 0; }
        /* reacomodar si ahora caben menos por pagina */
        int pp = per_page();
        for (int pg = 0; pg < L.npages; pg++) while (L.pn[pg] > pp && L.npages <= MAXPAGES) {
            int last = L.page[pg][--L.pn[pg]];
            if (pg + 1 >= L.npages) { if (L.npages >= MAXPAGES) { L.pn[pg]++; break; } L.pn[L.npages++] = 0; }
            memmove(&L.page[pg + 1][1], &L.page[pg + 1][0], L.pn[pg + 1] * sizeof(int)); L.page[pg + 1][0] = last; L.pn[pg + 1]++;
        }
        lconf_save();
        render_settings(fr0, SHEET_Y); present();
    }
    t0 = now_ms();
    for (;;) { double t = (now_ms() - t0) / 200; if (t > 1) t = 1; render_settings(fr0, SHEET_Y + (int)((H - SHEET_Y) * ease_out(t))); present(); if (t >= 1) break; }
    lconf_save();
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
    (void)arg; thumbs_busy = 1;
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
static void render_picker(uint16_t *dst, int scroll, int sel)
{
    for (int i = 0; i < W * H; i++) dst[i] = rgb(0, 0, 0);
    int n = nimg + 1;
    for (int i = 0; i < n; i++) {
        int col = i % 3, row = i / 3;
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

static void wallpaper_picker(void)
{
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
    int scroll = 0, rows = (nimg + 1 + 2) / 3, maxs = rows * (TH + 4) + 64 - H; if (maxs < 0) maxs = 0;
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
            if (sx > W - 90) break;
            if (sx < 110) { pick_all = !pick_all; goto reload; }
            continue;
        }
        int col = (sx - 8) / (TW + 4), row = (sy - 64 + scroll) / (TH + 4), i = row * 3 + col;
        if (col < 0 || col > 2 || i < 0 || i > nimg) continue;
        render_picker(fr0, scroll, i); present();
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
}
