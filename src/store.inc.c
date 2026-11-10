/* store.inc.c - "Apps": instalador de paquetes .hpk de la SD (estilo App Store) y ejecucion
 * de apps externas. Formato y garantias del instalador: docs/FORMATO_APPS.md, hpk.inc.c.
 *
 * Al abrir una app externa: el reproductor se retiene (su pantalla queda quieta, el audio
 * sigue), la app dibuja en /dev/fb0 y lee la tactil de un dispositivo virtual que alimenta
 * el shell; deslizar desde la barra de inicio la cierra (SIGTERM; SIGKILL al segundo). */
#include <linux/uinput.h>
#include <sys/wait.h>

#define SD_APPS "/usr/data/mnt/sd_0/Apps"

/* ------------------------------------------------------------------ registro de apps instaladas */
static struct hpk_info inst[24]; static int ninst;
static void register_installed_apps(void)
{
    static int nbuiltin = -1; if (nbuiltin < 0) { nbuiltin = 0; while (apps[nbuiltin].id) nbuiltin++; }
    napps = nbuiltin;
    hpk_recover();
    ninst = hpk_list_installed(inst, 24); if (ninst < 0) ninst = 0;
    for (int i = 0; i < ninst && napps < 40; i++) {
        struct lapp *a = &apps[napps++];
        memset(a, 0, sizeof *a);
        static char ids[24][HPK_ID_MAX + 4];
        snprintf(ids[i], sizeof ids[i], "x.%s", inst[i].id);      /* prefijo: no choca con las internas */
        a->id = ids[i]; a->label = inst[i].name[0]; a->act = ACT_EXT;
        for (int l = 0; l < 3; l++) a->names3[l] = inst[i].name[l];
        a->ext = &inst[i];
    }
}

/* ------------------------------------------------------------------ tactil virtual para apps */
static int app_uin = -1; static char app_ts[32];
static int app_touch_create(void)
{
    if (app_uin >= 0) return 1;
    int u = open("/dev/uinput", O_WRONLY | O_NONBLOCK); if (u < 0) return 0;
    struct uinput_user_dev d; memset(&d, 0, sizeof d);
    snprintf(d.name, sizeof d.name, "hibymod-app-ts");
    d.id.bustype = BUS_VIRTUAL; d.id.vendor = 0x4859; d.id.product = 0x4150;
    ioctl(u, UI_SET_EVBIT, EV_SYN); ioctl(u, UI_SET_EVBIT, EV_KEY); ioctl(u, UI_SET_EVBIT, EV_ABS);
    ioctl(u, UI_SET_KEYBIT, BTN_TOUCH);
    int ax[] = { ABS_MT_TOUCH_MAJOR, ABS_MT_WIDTH_MAJOR, ABS_MT_POSITION_X, ABS_MT_POSITION_Y, ABS_MT_TRACKING_ID };
    int mx[] = { 255, 255, W - 1, H - 1, 9 };
    for (int i = 0; i < 5; i++) { ioctl(u, UI_SET_ABSBIT, ax[i]); d.absmax[ax[i]] = mx[i]; }
    if (write(u, &d, sizeof d) != sizeof d || ioctl(u, UI_DEV_CREATE)) { close(u); return 0; }
    char sys[64] = ""; ioctl(u, UI_GET_SYSNAME(sizeof sys), sys);
    char dir[96]; snprintf(dir, sizeof dir, "/sys/class/input/%s", sys);
    for (int k = 0; k < 100 && !app_ts[0]; k++) {
        DIR *dd = opendir(dir); struct dirent *e;
        while (dd && (e = readdir(dd))) if (!strncmp(e->d_name, "event", 5)) snprintf(app_ts, sizeof app_ts, "/dev/input/%s", e->d_name);
        if (dd) closedir(dd);
        if (!app_ts[0]) msleep(10);
    }
    app_uin = u;
    return app_ts[0] != 0;
}

/* pulsa la tecla de reproducir/pausa del reproductor (para apps que necesitan la memoria) */
static void player_pause(void)
{
    for (int i = 0; i < 8; i++) {
        char p[32], name[64] = ""; snprintf(p, sizeof p, "/dev/input/event%d", i);
        int f = open(p, O_RDWR); if (f < 0) continue;
        ioctl(f, EVIOCGNAME(sizeof name), name);
        if (!strcmp(name, "jz adc keyboard")) {
            struct input_event e[2]; memset(e, 0, sizeof e);
            e[0].type = EV_KEY; e[0].code = KEY_PLAYPAUSE; e[0].value = 1; e[1].type = EV_SYN;
            write(f, e, sizeof e); msleep(60); e[0].value = 0; write(f, e, sizeof e);
        }
        close(f);
    }
}

static void run_ext_app(int a, int ix, int iy)
{
    struct hpk_info *pk = apps[a].ext;
    if (!pk) return;
    if (pk->stop_player && music_on) {
        if (!confirm(tr("Esta app necesita la memoria de la música: se pausará."), tr("Abrir"))) { launcher_present(); return; }
        player_pause();
    }
    if (!app_touch_create()) { toast(tr("No se pudo abrir")); launcher_present(); return; }
    /* el icono crece hasta negro (la app dibujara su primera imagen) */
    render_launcher(img_base, now_ms());
    double t0 = now_ms();
    static uint16_t *black; if (!black) black = calloc(W * H, 2);
    for (;;) {
        double t = (now_ms() - t0) / 260.0; if (t > 1) t = 1; double e = ease_out(t);
        int w = (int)(L.icon + (W - L.icon) * e), h = (int)(L.icon + (H - L.icon) * e);
        int x = (int)(ix * (1 - e)), y = (int)(iy * (1 - e));
        for (int i = 0; i < W * H; i++) fr0[i] = dim565(img_base[i], 32 - (int)(8 * e));
        draw_scaled(fr0, black, x, y, w, h, win_radius(w, h, e));
        int ia = (int)(255 * (1 - e * 2.2)); if (ia > 0) blit_scaled(fr0, &apps[a].icon[sz_idx()], x, y, w, h, ia);
        present(); if (t >= 1) break;
    }
    /* retener al reproductor y ceder la pantalla; su barra de estado se guarda para despues */
    static uint16_t sbar[30 * W]; memcpy(sbar, fr1, sizeof sbar);
    cmd('H');
    double th = now_ms(); while (st->capture && now_ms() - th < 200) msleep(2);
    char exe[400], ddir[200], env_dir[240], env_data[240], env_lang[16], env_touch[48], env_keys[48];
    snprintf(exe, sizeof exe, "%s/%s", pk->path, pk->exec);
    snprintf(ddir, sizeof ddir, "/usr/data/hibymod/appdata/%s", pk->id); mkdir("/usr/data/hibymod/appdata", 0755); mkdir(ddir, 0755);
    snprintf(env_dir, sizeof env_dir, "HM_APP_DIR=%s", pk->path);
    snprintf(env_data, sizeof env_data, "HM_DATA_DIR=%s", ddir);
    snprintf(env_lang, sizeof env_lang, "HM_LANG=%s", lang_code());
    snprintf(env_touch, sizeof env_touch, "HM_TOUCH=%s", app_ts);
    snprintf(env_keys, sizeof env_keys, "HM_KEYS=/dev/input/event3");
    char *envp[] = { env_dir, env_data, env_lang, env_touch, env_keys, "HM_FB=/dev/fb0", "PATH=/usr/bin:/bin:/usr/sbin:/sbin", 0 };
    char *argv[] = { exe, 0 };
    pid_t pid;
    lg("abrir app externa %s", pk->id);
    if (posix_spawn(&pid, exe, 0, 0, argv, envp)) { lg("no se pudo lanzar %s", exe); toast(tr("No se pudo abrir")); pid = -1; }
    /* reenviar la tactil a la app; la barra de inicio la cierra */
    struct pkt p; double term_t = 0;
    while (pid > 0) {
        int stt; if (waitpid(pid, &stt, WNOHANG) == pid) break;
        if (term_t && now_ms() - term_t > 1000) { kill(pid, SIGKILL); waitpid(pid, &stt, 0); break; }
        if (!read_pkt(&p, 50)) continue;
        if (p.down && p.y >= 456 && !term_t) {
            struct pkt held[16]; int nh = 0; held[nh++] = p; int sy = p.y, quit = 0;
            while (read_pkt(&p, 600)) { if (!p.down) break; if (sy - p.y > 30) { quit = 1; break; } if (nh < 16) held[nh++] = p; }
            if (quit) { kill(pid, SIGTERM); term_t = now_ms(); drain_until_up(); continue; }
            for (int i = 0; i < nh; i++) write(app_uin, held[i].ev, held[i].n * sizeof held[i].ev[0]);
        }
        if (p.n) write(app_uin, p.ev, p.n * sizeof p.ev[0]);
    }
    lg("app externa cerrada");
    /* volver: la ultima imagen de la app se encoge hasta su icono */
    memcpy(img_sheet, visible(), HM_FRAME_BYTES); have_under = 0;
    for (int i = 0; i < W * H; i++) img_under[i] = 0;
    cmd('R');
    double tr0 = now_ms(); while (st->held && now_ms() - tr0 < 200) msleep(2);
    launcher_on = 0;
    launcher_enter_ex(1);
    memcpy(fr1, sbar, sizeof sbar);          /* la app piso los bufers: barra de estado anterior */
    render_launcher(img_base, now_ms());
    t0 = now_ms();
    for (;;) {
        double t = (now_ms() - t0) / 280.0; if (t > 1) t = 1; double e = 1 - ease_out(t);
        int w = (int)(L.icon + (W - L.icon) * e), h = (int)(L.icon + (H - L.icon) * e);
        for (int i = 0; i < W * H; i++) fr0[i] = dim565(img_base[i], 32 - (int)(8 * e));
        draw_scaled(fr0, img_sheet, (int)(ix * (1 - e)), (int)(iy * (1 - e)), w, h, win_radius(w, h, e));
        present(); if (t >= 1) break;
    }
    launcher_present();
}

/* ------------------------------------------------------------------ tienda (lista + detalle) */
static struct hpk_info pkgs[32]; static int npkgs;
static struct sprite pkg_icon[32];
static void store_scan(void)
{
    mkdir(SD_APPS, 0755);
    npkgs = hpk_scan(SD_APPS, pkgs, 32); if (npkgs < 0) npkgs = 0;
    for (int i = 0; i < npkgs; i++) {
        sprite_free(&pkg_icon[i]);
        pkg_icon[i].w = pkg_icon[i].h = 48;
        pkg_icon[i].rgb = malloc(48 * 48 * 2); pkg_icon[i].a = malloc(48 * 48);
        if (hpk_icon(&pkgs[i], 48, pkg_icon[i].rgb, pkg_icon[i].a)) { sprite_free(&pkg_icon[i]); }
    }
}
static const char *pkg_name(const struct hpk_info *p) { return p->name[ui_lang][0] ? p->name[ui_lang] : p->name[0]; }
static const char *pkg_desc(const struct hpk_info *p) { return p->desc[ui_lang][0] ? p->desc[ui_lang] : p->desc[0]; }
static int pkg_state(const struct hpk_info *p) { return !p->installed[0] ? 0 : hpk_vercmp(p->version, p->installed) > 0 ? 1 : 2; } /* obtener/actualizar/abrir */
static const char *pill_txt(int s) { return s == 0 ? tr("OBTENER") : s == 1 ? tr("ACTUALIZAR") : tr("ABRIR"); }

static float st_scroll;
static void store_render(uint16_t *d, int sel)
{
    for (int i = 0; i < W * H; i++) d[i] = 0;
    int y = 110 - (int)st_scroll;
    draw_text(d, 16, y + 4, tr("EN LA TARJETA SD (/Apps)"), 12, rgb(142, 142, 147), 0, 94, H); y += 14;
    if (!npkgs) { y += draw_wrapped(d, 16, y + 6, W - 32, tr("HPK_HELP"), 13, rgb(142, 142, 147), 18, 1) + 12; }
    for (int i = 0; i < npkgs; i++, y += 64) {
        if (y + 64 < 94 || y > H) continue;
        if (i == sel) fill_rrect(d, 0, y, W, 64, 0, rgb(28, 28, 30), 255);
        if (pkg_icon[i].w) blit(d, &pkg_icon[i], 16, y + 8, 32, 255); else fill_rrect(d, 16, y + 8, 48, 48, 11, rgb(60, 60, 66), 255);
        draw_text(d, 76, y + 28, pkg_name(&pkgs[i]), 15, 0xFFFF, 150, 94, H);
        char sub[64];
        if (pkgs[i].status) snprintf(sub, sizeof sub, "%s", hpk_msg(hpk_errkey(pkgs[i].status), ui_lang));
        else snprintf(sub, sizeof sub, "%s · %.0f KB", pkgs[i].version, pkgs[i].size / 1024.0);
        draw_text(d, 76, y + 46, sub, 12, pkgs[i].status ? rgb(255, 69, 58) : rgb(142, 142, 147), 150, 94, H);
        if (!pkgs[i].status) {
            const char *pt = pill_txt(pkg_state(&pkgs[i])); int pw = tw_n(pt, 12, -1) + 24;
            fill_rrect(d, W - 16 - pw, y + 18, pw, 28, 14, rgb(44, 44, 48), 255);
            draw_text(d, W - 16 - pw + 12, y + 37, pt, 12, rgb(10, 132, 255), 0, 94, H);
        }
        if (i < npkgs - 1) for (int x = 76; x < W; x++) if (y + 63 >= 94 && y + 63 < H) d[(y + 63) * W + x] = rgb(38, 38, 40);
    }
    memcpy(d, fr1, 30 * W * 2);
    for (int yy = 30; yy < 94; yy++) for (int x = 0; x < W; x++) d[yy * W + x] = 0;
    draw_text(d, 16, 80, tr("Apps"), 26, 0xFFFF, 0, 0, H);
    for (int x = 0; x < W; x++) d[93 * W + x] = rgb(38, 38, 40);
    draw_toast(d);
}
static void install_progress(long done, long total, void *ctx)
{
    (void)ctx; st->shell_beat++;
    if (now_ms() - te_last_draw < 30 && done < total) return;
    progress_card(tr("Instalando…"), 0, total ? (double)done / total : 0);
}
static void store_detail(int i)
{
    struct hpk_info *p = &pkgs[i];
    char t[300]; snprintf(t, sizeof t, "%s — %s", pkg_name(p), pkg_desc(p));
    int s = pkg_state(p);
    const char *o[2] = { s == 2 ? tr("Abrir") : s == 1 ? tr("Actualizar") : tr("Instalar"), tr("Desinstalar") };
    int a = action_sheet(t, o, p->installed[0] ? 2 : 1, 1);
    if (a == 0 && s == 2) {
        for (int k = 0; k < napps; k++) if (apps[k].ext && !strcmp(apps[k].ext->id, p->id)) { run_ext_app(k, W / 2 - L.icon / 2, H / 2 - L.icon / 2); return; }
    } else if (a == 0) {
        char err[160] = "";
        progress_card(0, 0, -1); progress_card(tr("Instalando…"), 0, 0);
        int r = hpk_install(p->path, install_progress, 0, err, sizeof err);
        if (r) toast(hpk_msg(hpk_errkey(r), ui_lang));
        else {
            toast(tr("Instalada"));
            register_installed_apps(); load_icons(); relabel_apps();
            /* la app nueva va al final de la ultima pagina del inicio */
            char id[HPK_ID_MAX + 4]; snprintf(id, sizeof id, "x.%s", p->id);
            int ai = app_by_id(id), found = 0;
            for (int q = 0; q < L.npages; q++) for (int k = 0; k < L.pn[q]; k++) if (L.page[q][k] == ai) found = 1;
            for (int k = 0; k < L.dn; k++) if (L.dock[k] == ai) found = 1;
            if (ai >= 0 && !found) {
                int q = L.npages - 1; if (L.pn[q] >= per_page() && L.npages < MAXPAGES) { q = L.npages++; L.pn[q] = 0; }
                L.page[q][L.pn[q]++] = ai;
            }
            lconf_save();
        }
    } else if (a == 1) {
        if (confirm(tr("¿Desinstalar la app? Sus datos se conservan."), tr("Desinstalar"))) {
            char id[HPK_ID_MAX + 4]; snprintf(id, sizeof id, "x.%s", p->id);
            int ai = app_by_id(id);
            if (ai >= 0) remove_app_everywhere(ai);
            int r = hpk_uninstall(p->id);
            toast(r ? hpk_msg(hpk_errkey(r), ui_lang) : tr("Desinstalada"));
            register_installed_apps(); load_icons(); relabel_apps(); lconf_save();
        }
    }
    store_scan();
}
static void store_app_ui(int ix, int iy)
{
    (void)ix; (void)iy;
    store_scan(); st_scroll = 0;
    store_render(fr0, -1); present();
    struct pkt p;
    for (;;) {
        if (!read_pkt(&p, now_ms() < toast_until + 40 ? 30 : 400)) { if (now_ms() < toast_until + 40) { store_render(fr0, -1); present(); } continue; }
        if (!p.down) continue;
        int sx = p.x, sy = p.y, moved = 0; float s0 = st_scroll;
        if (sy >= 456) { while (cur_down && read_pkt(&p, 1000)) {} if (sy - cur_y > 20) break; continue; }
        int i = (sy - 124 + (int)st_scroll) / 64;
        if (i >= 0 && i < npkgs && sy > 94) { store_render(fr0, i); present(); }
        while (cur_down && read_pkt(&p, 30)) {
            if (abs(cur_y - sy) > 8) moved = 1;
            if (moved) { float maxs = 124 + npkgs * 64 - H; if (maxs < 0) maxs = 0; st_scroll = s0 - (cur_y - sy); if (st_scroll < 0) st_scroll = 0; if (st_scroll > maxs) st_scroll = maxs; store_render(fr0, -1); present(); }
        }
        (void)sx;
        if (moved || sy <= 94) { store_render(fr0, -1); present(); continue; }
        if (i >= 0 && i < npkgs && !pkgs[i].status) store_detail(i);
        store_render(fr0, -1); present();
    }
    drain_until_up();
    launcher_present();
}
