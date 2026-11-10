/* store.inc.c - "Apps": instalador de paquetes .hpk de la SD (estilo App Store) y ejecucion
 * de apps externas. Formato y garantias del instalador: docs/FORMATO_APPS.md, hpk.inc.c.
 *
 * Tienda con repositorios (index.json por HTTPS con hibynet) y apps instaladas en el HiBy o en la
 * tarjeta SD: docs/TIENDA.md.
 *
 * Al abrir una app externa: el reproductor se retiene (su pantalla queda quieta, el audio
 * sigue), la app dibuja en /dev/fb0 y lee la tactil de un dispositivo virtual que alimenta
 * el shell; deslizar desde la barra de inicio la cierra (SIGTERM; SIGKILL al segundo). */
#include <linux/uinput.h>
#include <sys/wait.h>

#define SD_ROOT "/usr/data/mnt/sd_0"
#define SD_APPS SD_ROOT "/Apps"
static int app_sd_missing(const struct hpk_info *in);
static void app_real_dir(const struct hpk_info *in, char *out, int cap);

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

/* apps que el shell no abre aunque esten instaladas (en pruebas: pueden bloquear el aparato) */
static const char *const app_blocked_ids[] = { "org.hibymod.navegador", 0 };
static int app_blocked(const char *id) { for (int k = 0; app_blocked_ids[k]; k++) if (!strcmp(id, app_blocked_ids[k])) return 1; return 0; }
static void run_ext_app(int a, int ix, int iy)
{
    struct hpk_info *pk = apps[a].ext;
    if (!pk) return;
    if (app_blocked(pk->id)) { toast(tr("Esta app está en pruebas y todavía no se puede abrir")); launcher_present(); return; }
    /* apps en la tarjeta SD: el talon de /usr/data apunta a la ruta real */
    if (app_sd_missing(pk)) { toast(tr("Esta app está en la tarjeta SD: insértala para abrirla")); launcher_present(); return; }
    char adir[256]; app_real_dir(pk, adir, sizeof adir);
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
    snprintf(exe, sizeof exe, "%s/%s", adir, pk->exec);
    snprintf(ddir, sizeof ddir, "/usr/data/hibymod/appdata/%s", pk->id); mkdir("/usr/data/hibymod/appdata", 0755); mkdir(ddir, 0755);
    snprintf(env_dir, sizeof env_dir, "HM_APP_DIR=%s", adir);
    snprintf(env_data, sizeof env_data, "HM_DATA_DIR=%s", ddir);
    snprintf(env_lang, sizeof env_lang, "HM_LANG=%s", lang_code());
    snprintf(env_touch, sizeof env_touch, "HM_TOUCH=%s", app_ts);
    snprintf(env_keys, sizeof env_keys, "HM_KEYS=/dev/input/event3");
    char *envp[] = { env_dir, env_data, env_lang, env_touch, env_keys, "HM_FB=/dev/fb0", "PATH=/usr/bin:/bin:/usr/sbin:/sbin", 0 };
    char *argv[] = { exe, 0 };
    pid_t pid;
    int hold = pk->refresh_full; if (hold) hz_hold++;   /* refresh=full: sin refresco de reposo */
    lcd_boost();
    lg("abrir app externa %s", pk->id);
    if (posix_spawn(&pid, exe, 0, 0, argv, envp)) { lg("no se pudo lanzar %s", exe); toast(tr("No se pudo abrir")); pid = -1; }
    /* reenviar la tactil a la app; la barra de inicio la cierra */
    struct pkt p; double term_t = 0; int stt = 0;
    while (pid > 0) {
        if (waitpid(pid, &stt, WNOHANG) == pid) break;
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
    if (hold) hz_hold--;
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
    /* 99 = la app se cerro sola por falta de memoria (vigilante del Navegador) */
    if (pid > 0 && !term_t && WIFEXITED(stt) && WEXITSTATUS(stt) == 99) toast(tr("La app se cerró por falta de memoria"));
    launcher_present();
}

/* ------------------------------------------------------------------ paquetes de la tarjeta SD */
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
static const char *pkg_name(const struct hpk_info *p) { return p->name[lang3()][0] ? p->name[lang3()] : p->name[0]; }   /* manifiestos: es/en/fr */
static const char *pkg_desc(const struct hpk_info *p) { return p->desc[lang3()][0] ? p->desc[lang3()] : p->desc[0]; }
static int pkg_state(const struct hpk_info *p) { return !p->installed[0] ? 0 : hpk_vercmp(p->version, p->installed) > 0 ? 1 : 2; } /* obtener/actualizar/abrir */
static const char *pill_txt(int s) { return s == 0 ? tr("OBTENER") : s == 1 ? tr("ACTUALIZAR") : tr("ABRIR"); }

static void app_real_dir(const struct hpk_info *in, char *out, int cap);
/* ------------------------------------------------------------------ copia de seguridad (.hpk) */
/* Empaqueta en ustar el directorio instalado (sin los icon_*.ov generados): el resultado es
 * un .hpk reinstalable. Los datos de la app (appdata/<id>) van aparte en <id>-datos.tar. */
static int tar_oct(char *dst, int n, unsigned long long v) { snprintf(dst, n, "%0*llo", n - 1, v); return 0; }
static int tar_hdr(int fd, const char *name, unsigned long long size, int dir, int mode)
{
    unsigned char h[512]; memset(h, 0, sizeof h);
    if (strlen(name) >= 100) return -1;
    snprintf((char *)h, 100, "%s%s", name, dir ? "/" : "");
    tar_oct((char *)h + 100, 8, mode); tar_oct((char *)h + 108, 8, 0); tar_oct((char *)h + 116, 8, 0);
    tar_oct((char *)h + 124, 12, dir ? 0 : size); tar_oct((char *)h + 136, 12, (unsigned long long)time(0));
    h[156] = dir ? '5' : '0'; memcpy(h + 257, "ustar", 6); memcpy(h + 263, "00", 2);
    memset(h + 148, ' ', 8); unsigned sum = 0; for (int i = 0; i < 512; i++) sum += h[i];
    snprintf((char *)h + 148, 8, "%06o", sum); h[155] = ' ';
    return write(fd, h, 512) == 512 ? 0 : -1;
}
static int tar_tree(int fd, const char *base, const char *rel, int skip_ov)
{
    char path[512]; snprintf(path, sizeof path, "%s%s%s", base, rel[0] ? "/" : "", rel);
    DIR *d = opendir(path); if (!d) return -1;
    struct dirent *e; int r = 0;
    while (!r && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (skip_ov && !rel[0] && !strncmp(e->d_name, "icon_", 5) && strstr(e->d_name, ".ov")) continue;
        char r2[256], full[600]; snprintf(r2, sizeof r2, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        snprintf(full, sizeof full, "%s/%s", base, r2);
        struct stat sb; if (lstat(full, &sb)) continue;
        if (S_ISDIR(sb.st_mode)) { r = tar_hdr(fd, r2, 0, 1, 0755); if (!r) r = tar_tree(fd, base, r2, skip_ov); continue; }
        if (!S_ISREG(sb.st_mode)) continue;
        int in = open(full, O_RDONLY); if (in < 0) { r = -1; break; }
        r = tar_hdr(fd, r2, sb.st_size, 0, (sb.st_mode & 0111) ? 0755 : 0644);
        char buf[8192]; long long left = sb.st_size; ssize_t n;
        while (!r && left > 0 && (n = read(in, buf, sizeof buf)) > 0) { if (write(fd, buf, n) != n) r = -1; left -= n; st->shell_beat++; }
        close(in);
        if (!r && sb.st_size % 512) { char z[512] = {0}; if (write(fd, z, 512 - sb.st_size % 512) < 0) r = -1; }
    }
    closedir(d);
    return r;
}
static int tar_write(const char *out, const char *dir, int skip_ov)
{
    char tmp[300]; snprintf(tmp, sizeof tmp, "%s.part", out);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (fd < 0) return -1;
    int r = tar_tree(fd, dir, "", skip_ov);
    char z[1024] = {0}; if (!r && write(fd, z, 1024) != 1024) r = -1;
    if (fsync(fd)) r = -1; close(fd);
    if (r || rename(tmp, out)) { unlink(tmp); return -1; }
    return 0;
}
static void app_backup(const struct hpk_info *in)
{
    char dir[200], out[300], dd[200]; mkdir(SD_APPS, 0755);
    snprintf(out, sizeof out, "%s/Copias", SD_APPS); mkdir(out, 0755);
    app_real_dir(in, dir, sizeof dir);
    snprintf(out, sizeof out, "%s/Copias/%s-%s.hpk", SD_APPS, in->id, in->installed[0] ? in->installed : in->version);
    progress_card(0, 0, -1); progress_card(tr("Copiando…"), 0, 0.3);
    int r = tar_write(out, dir, 1);
    snprintf(dd, sizeof dd, "/usr/data/hibymod/appdata/%s", in->id);
    if (!r && hpk_isdir(dd) && hpk_du(dd) > 0) {
        progress_card(tr("Copiando…"), 0, 0.7);
        snprintf(out, sizeof out, "%s/Copias/%s-datos.tar", SD_APPS, in->id);
        r = tar_write(out, dd, 0);
    }
    sync();
    toast(r ? tr("No se pudo hacer la copia") : tr("Copia guardada en Apps/Copias"));
}
/* ------------------------------------------------------------------ apps en la tarjeta SD
 * La SD (exfat, fmask 0022, sin noexec) admite ejecutar binarios. Una app "en la SD" se instala
 * con el mismo instalador atomico pero con la raiz en SD_APPROOT (SD/HiByOS/apps/<id>/), y en
 * /usr/data/hibymod/apps/<id>/ queda un "talon" pequeno: manifest.ini + icon_*.ov + sd_location
 * (ruta real). Asi el launcher y la lista de instaladas no cambian; run_ext_app() lanza desde la
 * ruta real. Sus datos (appdata/<id>) siguen en /usr/data. Ver docs/TIENDA.md. */
#define SD_APPROOT SD_ROOT "/HiByOS"
static int sd_present(void)
{
    struct stat a, b;
    return !stat(SD_ROOT, &a) && !stat("/usr/data/mnt", &b) && a.st_dev != b.st_dev;   /* montada */
}
static long long free_bytes(const char *p) { struct statvfs v; return statvfs(p, &v) ? -1 : (long long)v.f_bavail * v.f_frsize; }
static void fmt_bytes(long long b, char *o, int cap)
{
    if (b >= (1LL << 30)) snprintf(o, cap, "%.1f GB", b / 1073741824.0);
    else if (b >= (1 << 20)) snprintf(o, cap, "%.1f MB", b / 1048576.0);
    else snprintf(o, cap, "%.0f KB", b / 1024.0);
}
/* 1 si la app instalada es un talon de la SD (sd = ruta real) */
static int app_sd_path(const struct hpk_info *in, char *sd, int cap)
{
    char p[300]; snprintf(p, sizeof p, "%s/sd_location", in->path);
    int fd = open(p, O_RDONLY); if (fd < 0) return 0;
    char b[256]; ssize_t n = read(fd, b, sizeof b - 1); close(fd);
    if (n <= 0) return 0;
    b[n] = 0; b[strcspn(b, "\r\n")] = 0;
    if (sd) snprintf(sd, cap, "%s", b);
    return 1;
}
/* directorio real de la app (talon -> SD) */
static void app_real_dir(const struct hpk_info *in, char *out, int cap)
{
    if (!app_sd_path(in, out, cap)) snprintf(out, cap, "%s", in->path);
}
static int app_sd_missing(const struct hpk_info *in)
{
    char sd[256]; if (!app_sd_path(in, sd, sizeof sd)) return 0;
    char m[300]; snprintf(m, sizeof m, "%s/manifest.ini", sd);
    return !sd_present() || access(m, R_OK);
}
/* copia un arbol (ficheros normales y directorios), con fsync; 0 si va bien */
static int copy_tree(const char *src, const char *dst)
{
    if (mkdir(dst, 0755) && errno != EEXIST) return -1;
    DIR *d = opendir(src); if (!d) return -1;
    struct dirent *e; int r = 0;
    while (!r && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char s[600], t[600]; snprintf(s, sizeof s, "%s/%s", src, e->d_name); snprintf(t, sizeof t, "%s/%s", dst, e->d_name);
        struct stat sb; if (lstat(s, &sb)) { r = -1; break; }
        if (S_ISDIR(sb.st_mode)) { r = copy_tree(s, t); continue; }
        if (!S_ISREG(sb.st_mode)) continue;
        int in = open(s, O_RDONLY), out = in < 0 ? -1 : open(t, O_WRONLY | O_CREAT | O_TRUNC, (sb.st_mode & 0111) ? 0755 : 0644);
        if (in < 0 || out < 0) { if (in >= 0) close(in); r = -1; break; }
        static char buf[32768]; ssize_t n;
        while ((n = read(in, buf, sizeof buf)) > 0) { if (write(out, buf, n) != n) { r = -1; break; } st->shell_beat++; }
        if (n < 0 || fsync(out)) r = -1;
        close(in); close(out);
    }
    closedir(d);
    return r;
}
/* cambio atomico en <apps>: tmp -> <id> (la version anterior pasa por .old-<id>, que
 * hpk_recover() restaura si se corta la luz entre los dos rename) */
static int swap_in(const char *apps, const char *id, const char *tmp)
{
    char cur[320], old[320]; snprintf(cur, sizeof cur, "%s/%s", apps, id); snprintf(old, sizeof old, "%s/.old-%s", apps, id);
    hpk_rmrf(old);
    if (hpk_isdir(cur) && rename(cur, old)) return -1;
    if (rename(tmp, cur)) { rename(old, cur); return -1; }
    hpk_fsync_dir(apps);
    hpk_rmrf(old);
    return 0;
}
/* escribe el talon de /usr/data para una app instalada en sddir */
static int make_stub(const char *id, const char *sddir)
{
    char apps[256], tmp[320]; snprintf(apps, sizeof apps, "%s/apps", hpk_root()); mkdir(apps, 0755);
    snprintf(tmp, sizeof tmp, "%s/.tmp-%s", apps, id); hpk_rmrf(tmp);
    if (mkdir(tmp, 0755)) return -1;
    static const char *f[] = { "manifest.ini", "icon_48.ov", "icon_56.ov", "icon_64.ov" };
    for (int k = 0; k < 4; k++) {
        char s[400], t[400]; snprintf(s, sizeof s, "%s/%s", sddir, f[k]); snprintf(t, sizeof t, "%s/%s", tmp, f[k]);
        if (copy_file(s, t) && k == 0) { hpk_rmrf(tmp); return -1; }
    }
    char loc[400]; snprintf(loc, sizeof loc, "%s/sd_location", tmp);
    int fd = open(loc, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, sddir, strlen(sddir)) != (ssize_t)strlen(sddir) || fsync(fd)) { if (fd >= 0) close(fd); hpk_rmrf(tmp); return -1; }
    close(fd);
    if (swap_in(apps, id, tmp)) { hpk_rmrf(tmp); return -1; }
    return 0;
}
/* instala un .hpk en la SD + talon */
static int hpk_install_sd(const char *path, void (*cb)(long, long, void *), char *err, int errlen, const char *id)
{
    mkdir(SD_APPROOT, 0755);
    hpk_set_root(SD_APPROOT);
    int r = hpk_install(path, cb, 0, err, errlen);
    hpk_set_root(NULL);
    if (r) return r;
    char sddir[256]; snprintf(sddir, sizeof sddir, SD_APPROOT "/apps/%s", id);
    return make_stub(id, sddir) ? -HPK_E_SWAP : 0;
}
/* desinstalar: la copia de la SD (si esta) y el talon */
static int app_uninstall_any(const struct hpk_info *in, int flags)
{
    char id[HPK_ID_MAX + 1]; snprintf(id, sizeof id, "%s", in->id);
    if (app_sd_path(in, 0, 0) && sd_present()) { hpk_set_root(SD_APPROOT); hpk_uninstall_ex(id, 0); hpk_set_root(NULL); }
    return hpk_uninstall_ex(id, flags);
}
static void move_progress(const char *t, double f) { progress_card(t, 0, f); }
/* mover entre la SD y el HiBy (to_sd = 1: a la SD) */
static int app_move(const struct hpk_info *in, int to_sd)
{
    char id[HPK_ID_MAX + 1], src[256]; snprintf(id, sizeof id, "%s", in->id);
    int on_sd = app_sd_path(in, src, sizeof src);
    if (on_sd == to_sd) return 0;
    if (!sd_present()) { toast(tr("No hay tarjeta SD")); return -1; }
    if (!on_sd) snprintf(src, sizeof src, "%s", in->path);
    long long need = hpk_du(src);
    if (free_bytes(to_sd ? SD_ROOT : "/usr/data") < need + HPK_SPACE_MARGIN) { toast(hpk_msg("err.space", ui_lang)); return -1; }
    progress_card(0, 0, -1); move_progress(tr("Moviendo…"), 0.2);
    int r;
    if (to_sd) {
        char apps[256], tmp[320], dst[320]; snprintf(apps, sizeof apps, SD_APPROOT "/apps"); mkdir(SD_APPROOT, 0755); mkdir(apps, 0755);
        snprintf(tmp, sizeof tmp, "%s/.tmp-%s", apps, id); hpk_rmrf(tmp);
        r = copy_tree(src, tmp);
        move_progress(tr("Moviendo…"), 0.8);
        if (!r) r = swap_in(apps, id, tmp);
        if (r) hpk_rmrf(tmp);
        snprintf(dst, sizeof dst, "%s/%s", apps, id);
        if (!r) r = make_stub(id, dst);                   /* sustituye la copia de /usr/data */
    } else {
        char apps[256], tmp[320], del[320]; snprintf(apps, sizeof apps, "%s/apps", hpk_root());
        snprintf(tmp, sizeof tmp, "%s/.tmp-%s", apps, id); hpk_rmrf(tmp);
        r = copy_tree(src, tmp);
        move_progress(tr("Moviendo…"), 0.8);
        char loc[400]; snprintf(loc, sizeof loc, "%s/sd_location", tmp); unlink(loc);
        if (!r) r = swap_in(apps, id, tmp);               /* sustituye el talon */
        if (r) hpk_rmrf(tmp);
        else { snprintf(del, sizeof del, SD_APPROOT "/apps/.del-%s", id); hpk_rmrf(del); if (!rename(src, del)) hpk_rmrf(del); }
    }
    sync();
    toast(r ? tr("No se pudo mover la app") : to_sd ? tr("Movida a la tarjeta SD") : tr("Movida al HiBy"));
    return r;
}
/* restos de operaciones cortadas en la SD */
static void sd_recover(void)
{
    if (!sd_present()) return;
    hpk_set_root(SD_APPROOT); hpk_recover(); hpk_set_root(NULL);
}

static int st_open_after = -1;      /* app que abrir al salir de la tienda */
static void installed_detail(int i)
{
    struct hpk_info *in = &inst[i];
    char t[300]; snprintf(t, sizeof t, "%s %s", pkg_name(in), in->installed[0] ? in->installed : in->version);
    int on_sd = app_sd_path(in, 0, 0), sd = sd_present();
    const char *o[5]; int map[5], n = 0;
    o[n] = tr("Abrir"); map[n++] = 0;
    if (sd || on_sd) { o[n] = on_sd ? tr("Mover al HiBy") : tr("Mover a la tarjeta SD"); map[n++] = 4; }
    o[n] = tr("Copia de seguridad"); map[n++] = 1;
    o[n] = tr("Desinstalar"); map[n++] = 2;
    o[n] = tr("Desinstalar y borrar datos"); map[n++] = 3;
    int k = action_sheet(t, o, n, n - 2);
    if (k < 0 || k >= n) return;
    int a = map[k];
    char id[HPK_ID_MAX + 4]; snprintf(id, sizeof id, "x.%s", in->id);
    int ai = app_by_id(id);
    if (a == 0) {
        if (app_sd_missing(in)) { toast(tr("Esta app está en la tarjeta SD: insértala para abrirla")); return; }
        if (ai >= 0) st_open_after = ai;
        return;
    }
    if (a == 4) { if (app_sd_missing(in)) { toast(tr("No hay tarjeta SD")); return; } app_move(in, !on_sd); register_installed_apps(); load_icons(); relabel_apps(); return; }
    if (a == 1) { if (app_sd_missing(in)) { toast(tr("No hay tarjeta SD")); return; } app_backup(in); return; }
    if (a == 2 || a == 3) {
        if (!confirm(tr(a == 2 ? "¿Desinstalar la app? Sus datos se conservan." : "¿Desinstalar y borrar sus datos? No se puede deshacer."), tr("Desinstalar"))) return;
        if (ai >= 0) remove_app_everywhere(ai);
        int r = app_uninstall_any(in, a == 3 ? HPK_PURGE_DATA : 0);
        toast(r ? hpk_msg(hpk_errkey(r), ui_lang) : tr("Desinstalada"));
        register_installed_apps(); load_icons(); relabel_apps(); lconf_save();
    }
}

/* ================================================================== tienda con repositorios
 * Un repositorio es la URL de un index.json (formato en docs/TIENDA.md). Se descarga con
 * hibynet a cache/store/<hash>.json; los iconos (PNG pequenos) a cache/store/icons/ y los .hpk a
 * cache/store/dl/ (se verifica sha256 y tamano antes de instalar y se borra despues).
 * Todo lo de red va en procesos hibynet en segundo plano: la interfaz nunca se bloquea. */
#define ST_DIR "/usr/data/hibymod/cache/store"
#define ST_REPOS_CONF "/usr/data/hibymod/store_repos.conf"
#define ST_DEFAULT_REPO "https://raw.githubusercontent.com/Grohle/hibyos/main/repo/index.json"
#define ST_MAXREPO 8
#define ST_MAXAPP 64
#define ST_INDEX_CAP (256 * 1024)
#define ST_REFRESH_MS (10 * 60 * 1000.0)

struct st_repo { char url[256], name[48]; int napps, err, busy; double fetched; };
struct st_app {
    int repo, featured, ram_kb, stop_player, unavailable;   /* unavailable: "available": false (en pruebas) */
    char id[HPK_ID_MAX + 1], version[HPK_VER_MAX], min[HPK_VER_MAX], sha[65], author[48], category[24];
    char name[48], desc[200], lname[48], ldesc[200];      /* l*: en minusculas, para buscar */
    char icon_url[256], hpk_url[256];
    long long size;
    struct sprite ic; int ic_state;                       /* 0 sin pedir, 1 en cola/listo, 2 fallo */
    uint16_t tint;                                        /* color medio del icono (tarjetas de Hoy) */
};
static struct st_repo st_repos[ST_MAXREPO]; static int st_nrepo;
static struct st_app *st_apps; static int st_napp;

/* ------------------------------------------------------------------ sha256 (FIPS 180-4) */
struct sha256 { uint32_t h[8]; uint64_t len; uint8_t buf[64]; int n; };
static const uint32_t sha_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
#define SHR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(struct sha256 *s, const uint8_t *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = SHR(w[i - 15], 7) ^ SHR(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = SHR(w[i - 2], 17) ^ SHR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (SHR(e, 6) ^ SHR(e, 11) ^ SHR(e, 25)) + ((e & f) ^ (~e & g)) + sha_k[i] + w[i];
        uint32_t t2 = (SHR(a, 2) ^ SHR(a, 13) ^ SHR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}
static void sha_init(struct sha256 *s)
{
    static const uint32_t h0[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    memcpy(s->h, h0, sizeof h0); s->len = 0; s->n = 0;
}
static void sha_update(struct sha256 *s, const uint8_t *p, size_t n)
{
    s->len += n;
    while (n) {
        if (!s->n && n >= 64) { sha_block(s, p); p += 64; n -= 64; continue; }
        int k = 64 - s->n; if ((size_t)k > n) k = (int)n;
        memcpy(s->buf + s->n, p, k); s->n += k; p += k; n -= k;
        if (s->n == 64) { sha_block(s, s->buf); s->n = 0; }
    }
}
static void sha_hex(struct sha256 *s, char out[65])
{
    uint64_t bits = s->len * 8; uint8_t pad = 0x80, z = 0;
    sha_update(s, &pad, 1); while (s->n != 56) sha_update(s, &z, 1);
    uint8_t L[8]; for (int i = 0; i < 8; i++) L[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha_update(s, L, 8);
    for (int i = 0; i < 8; i++) sprintf(out + 8 * i, "%08x", s->h[i]);
}

/* ------------------------------------------------------------------ JSON minimo (recorrido) */
static const char *sj_w(const char *p) { while (p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; return p; }
static const char *sj_skip_str(const char *p)
{
    if (!p || *p != '"') return 0;
    for (p++; *p; p++) { if (*p == '\\') { if (!*++p) return 0; } else if (*p == '"') return p + 1; }
    return 0;
}
static const char *sj_skip(const char *p)
{
    p = sj_w(p); if (!p || !*p) return 0;
    if (*p == '"') return sj_skip_str(p);
    if (*p == '{' || *p == '[') {
        int d = 0;
        while (*p) {
            if (*p == '"') { p = sj_skip_str(p); if (!p) return 0; continue; }
            if (*p == '{' || *p == '[') d++; else if ((*p == '}' || *p == ']') && --d == 0) return p + 1;
            p++;
        }
        return 0;
    }
    while (*p && !strchr(",}] \t\r\n", *p)) p++;
    return p;
}
static void sj_utf8(char **o, char *end, unsigned cp)
{
    char b[4]; int n;
    if (cp < 0x80) { b[0] = cp; n = 1; }
    else if (cp < 0x800) { b[0] = 0xC0 | (cp >> 6); b[1] = 0x80 | (cp & 63); n = 2; }
    else if (cp < 0x10000) { b[0] = 0xE0 | (cp >> 12); b[1] = 0x80 | ((cp >> 6) & 63); b[2] = 0x80 | (cp & 63); n = 3; }
    else { b[0] = 0xF0 | (cp >> 18); b[1] = 0x80 | ((cp >> 12) & 63); b[2] = 0x80 | ((cp >> 6) & 63); b[3] = 0x80 | (cp & 63); n = 4; }
    if (*o + n <= end) { memcpy(*o, b, n); *o += n; }
}
/* valor -> texto (strings con escapes; numeros y booleanos tal cual; null -> "") */
static void sj_str(const char *v, char *out, int cap)
{
    char *o = out, *end = out + cap - 1; *out = 0;
    v = sj_w(v); if (!v || !*v || cap < 2) return;
    if (*v != '"') {
        if (!strncmp(v, "null", 4)) return;
        const char *e = sj_skip(v); if (!e) return;
        int n = (int)(e - v); if (n > cap - 1) n = cap - 1; memcpy(out, v, n); out[n] = 0; return;
    }
    for (v++; *v && *v != '"'; v++) {
        if (*v != '\\') { if (o < end) *o++ = *v; continue; }
        v++;
        if (*v == 'n' || *v == 't' || *v == 'r') { if (o < end) *o++ = ' '; }
        else if (*v == 'u') {
            unsigned cp = 0; int ok = 1;
            for (int i = 1; i <= 4; i++) { char c = v[i]; cp <<= 4; if (c >= '0' && c <= '9') cp |= c - '0'; else if ((c | 32) >= 'a' && (c | 32) <= 'f') cp |= (c | 32) - 'a' + 10; else ok = 0; }
            if (!ok) break;
            v += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && v[1] == '\\' && v[2] == 'u') {
                unsigned lo = (unsigned)strtoul((char[5]){ v[3], v[4], v[5], v[6], 0 }, 0, 16);
                if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); v += 6; }
            }
            sj_utf8(&o, end, cp);
        } else if (!*v) break;
        else if (*v != 'b' && *v != 'f' && o < end) *o++ = *v;
    }
    /* no cortar a mitad de un caracter UTF-8 */
    while (o > out && ((unsigned char)o[-1] & 0xC0) == 0x80) { char *q = o - 1; while (q > out && ((unsigned char)*q & 0xC0) == 0x80) q--; if (o - q < ((((unsigned char)*q) >= 0xF0) ? 4 : (((unsigned char)*q) >= 0xE0) ? 3 : 2)) o = q; break; }
    *o = 0;
}
static const char *sj_get(const char *obj, const char *key)
{
    obj = sj_w(obj); if (!obj || *obj != '{') return 0;
    const char *p = sj_w(obj + 1); char k[48];
    while (p && *p == '"') {
        sj_str(p, k, sizeof k);
        p = sj_w(sj_skip_str(p)); if (!p || *p != ':') return 0;
        p = sj_w(p + 1);
        if (!strcmp(k, key)) return p;
        p = sj_w(sj_skip(p)); if (!p) return 0;
        if (*p == ',') p = sj_w(p + 1); else break;
    }
    return 0;
}
static const char *sj_first(const char *a) { a = sj_w(a); if (!a || *a != '[') return 0; a = sj_w(a + 1); return (!*a || *a == ']') ? 0 : a; }
static const char *sj_next(const char *el) { const char *p = sj_w(sj_skip(el)); if (!p || *p != ',') return 0; p = sj_w(p + 1); return (!*p || *p == ']') ? 0 : p; }
/* texto localizado: "clave": "texto" o "clave": {"es": "...", "en": "...", ...};
 * idioma actual -> ingles -> espanol -> el primero que haya */
static void sj_lang(const char *obj, const char *key, char *out, int cap)
{
    const char *v = sj_get(obj, key); *out = 0; if (!v) return;
    if (*v != '{') { sj_str(v, out, cap); return; }
    const char *t = sj_get(v, lang_code());
    if (!t || *t != '"') t = sj_get(v, "en");
    if (!t || *t != '"') t = sj_get(v, "es");
    if (!t || *t != '"') { const char *p = sj_w(v + 1); p = p && *p == '"' ? sj_w(sj_skip_str(p)) : 0; t = p && *p == ':' ? sj_w(p + 1) : 0; }
    if (t) sj_str(t, out, cap);
}

/* ------------------------------------------------------------------ repositorios */
static unsigned st_hash(const char *s) { unsigned h = 2166136261u; while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; } return h; }
static void st_cache_path(const char *url, const char *dir, const char *ext, char *out, int cap)
{
    snprintf(out, cap, ST_DIR "%s/%08x%s", dir, st_hash(url), ext);
}
static void st_mkdirs(void)
{
    mkdir("/usr/data/hibymod/cache", 0755); mkdir(ST_DIR, 0755); mkdir(ST_DIR "/icons", 0755); mkdir(ST_DIR "/dl", 0755);
}
static int st_url_ok(const char *u) { return (!strncmp(u, "https://", 8) || !strncmp(u, "http://", 7)) && strlen(u) > 10 && !strpbrk(u, " \t\r\n\"'"); }
static void st_host(const char *url, char *out, int cap)
{
    const char *p = strstr(url, "://"); p = p ? p + 3 : url;
    int n = (int)strcspn(p, "/:?"); if (n >= cap) n = cap - 1; memcpy(out, p, n); out[n] = 0;
}
static void st_repos_load(void)
{
    st_nrepo = 0;
    struct st_repo *r = &st_repos[st_nrepo++]; memset(r, 0, sizeof *r); snprintf(r->url, sizeof r->url, "%s", ST_DEFAULT_REPO);
    FILE *f = fopen(ST_REPOS_CONF, "r"); char l[300];
    while (f && fgets(l, sizeof l, f) && st_nrepo < ST_MAXREPO) {
        l[strcspn(l, "\r\n")] = 0;
        if (!l[0] || l[0] == '#' || !st_url_ok(l) || !strcmp(l, ST_DEFAULT_REPO)) continue;
        int dup = 0; for (int i = 0; i < st_nrepo; i++) if (!strcmp(st_repos[i].url, l)) dup = 1;
        if (dup) continue;
        r = &st_repos[st_nrepo++]; memset(r, 0, sizeof *r); snprintf(r->url, sizeof r->url, "%s", l);
    }
    if (f) fclose(f);
}
static void st_repos_save(void)
{
    FILE *f = fopen(ST_REPOS_CONF ".tmp", "w"); if (!f) return;
    fprintf(f, "# repositorios de la tienda (uno por linea; el de HiByOS siempre esta)\n");
    for (int i = 1; i < st_nrepo; i++) fprintf(f, "%s\n", st_repos[i].url);
    fflush(f); fsync(fileno(f)); fclose(f);
    rename(ST_REPOS_CONF ".tmp", ST_REPOS_CONF);
}
/* URL relativa al index.json -> absoluta */
static void st_join(const char *base, const char *rel, char *out, int cap)
{
    if (!strncmp(rel, "https://", 8) || !strncmp(rel, "http://", 7)) { snprintf(out, cap, "%s", rel); return; }
    while (!strncmp(rel, "./", 2)) rel += 2;
    char b[256]; snprintf(b, sizeof b, "%s", base);
    if (rel[0] == '/') { char *p = strstr(b, "://"); p = p ? strchr(p + 3, '/') : 0; if (p) *p = 0; snprintf(out, cap, "%s%s", b, rel); return; }
    char *q = strrchr(b, '/'); if (q) q[1] = 0;
    snprintf(out, cap, "%s%s", b, rel);
}
static void st_lower(const char *s, char *o, int cap)
{
    int n = 0;
    for (; *s && n < cap - 1; s++) {
        unsigned char c = (unsigned char)*s;
        if (c >= 'A' && c <= 'Z') c += 32;
        /* vocales acentuadas frecuentes (UTF-8 de 2 bytes C3 xx) -> sin acento, para buscar */
        if (c == 0xC3 && s[1]) {
            unsigned char d = (unsigned char)s[1] | 0x20; const char *from = "\xa0\xa1\xa2\xa4\xa8\xa9\xaa\xab\xac\xad\xae\xaf\xb2\xb3\xb4\xb6\xb9\xba\xbb\xbc\xb1\xa7", *to = "aaaaeeeeiiiiooooouuuunc";
            const char *k = strchr(from, d);
            if (k) { o[n++] = to[k - from]; s++; continue; }
        }
        o[n++] = (char)c;
    }
    o[n] = 0;
}
static int st_app_find(const char *id) { for (int i = 0; i < st_napp; i++) if (!strcmp(st_apps[i].id, id)) return i; return -1; }
static int st_hex64(const char *s) { if (strlen(s) != 64) return 0; for (; *s; s++) if (!isxdigit((unsigned char)*s)) return 0; return 1; }

/* lee cache/store/<hash>.json del repositorio r y anade sus apps */
static void st_parse_repo(int r)
{
    struct st_repo *R = &st_repos[r]; R->napps = 0;
    char path[160]; st_cache_path(R->url, "", ".json", path, sizeof path);
    int fd = open(path, O_RDONLY); if (fd < 0) return;
    struct stat sb; if (fstat(fd, &sb) || sb.st_size <= 0 || sb.st_size > ST_INDEX_CAP) { close(fd); if (!R->err) R->err = 4; return; }
    char *buf = malloc(sb.st_size + 1); if (!buf) { close(fd); return; }
    int n = hpk_rd_all(fd, buf, (long)sb.st_size, 0) ? -1 : (int)sb.st_size; close(fd);
    if (n < 0) { free(buf); return; }
    buf[n] = 0;
    char host[48]; st_host(R->url, host, sizeof host);
    sj_lang(buf, "name", R->name, sizeof R->name); if (!R->name[0]) snprintf(R->name, sizeof R->name, "%s", host);
    const char *arr = sj_get(buf, "apps");
    for (const char *o = sj_first(arr); o && st_napp < ST_MAXAPP; o = sj_next(o)) {
        if (*o != '{') continue;
        struct st_app a; memset(&a, 0, sizeof a); a.repo = r;
        char t[300];
        sj_str(sj_get(o, "id"), a.id, sizeof a.id);
        sj_str(sj_get(o, "version"), a.version, sizeof a.version);
        sj_str(sj_get(o, "min_hibymod"), a.min, sizeof a.min); if (!a.min[0]) snprintf(a.min, sizeof a.min, "0.0.0");
        sj_str(sj_get(o, "sha256"), a.sha, sizeof a.sha); for (char *q = a.sha; *q; q++) *q = (char)tolower((unsigned char)*q);
        sj_str(sj_get(o, "hpk"), t, sizeof t); if (t[0]) st_join(R->url, t, a.hpk_url, sizeof a.hpk_url);
        sj_str(sj_get(o, "icon"), t, sizeof t); if (t[0]) st_join(R->url, t, a.icon_url, sizeof a.icon_url);
        sj_lang(o, "name", a.name, sizeof a.name);
        sj_lang(o, "desc", a.desc, sizeof a.desc);
        sj_str(sj_get(o, "author"), a.author, sizeof a.author);
        sj_str(sj_get(o, "category"), a.category, sizeof a.category);
        sj_str(sj_get(o, "size"), t, 32); a.size = atoll(t);
        sj_str(sj_get(o, "ram_kb"), t, 32); a.ram_kb = atoi(t);
        sj_str(sj_get(o, "stop_player"), t, 32); a.stop_player = !strcmp(t, "true") || atoi(t) > 0;
        sj_str(sj_get(o, "featured"), t, 32); a.featured = !strcmp(t, "true") || atoi(t) > 0;
        sj_str(sj_get(o, "available"), t, 32); a.unavailable = !strcmp(t, "false") || !strcmp(t, "0") || app_blocked(a.id);
        /* lo que el instalador no comprueba hasta descargar se valida ya (si no, no se muestra) */
        long sv[3];
        if (!hpk_valid_id(a.id) || hpk_semver(a.version, sv) || !a.name[0]) continue;
        if (!a.unavailable && (!st_url_ok(a.hpk_url) || !st_hex64(a.sha))) continue;
        int dup = st_app_find(a.id);
        if (dup >= 0) continue;            /* el repositorio anterior (el de HiByOS primero) manda */
        st_lower(a.name, a.lname, sizeof a.lname); st_lower(a.desc, a.ldesc, sizeof a.ldesc);
        st_apps[st_napp++] = a; R->napps++;
    }
    free(buf);
}
static void st_apps_reload(void)
{
    for (int i = 0; i < st_napp; i++) sprite_free(&st_apps[i].ic);
    st_napp = 0;
    for (int r = 0; r < st_nrepo; r++) st_parse_repo(r);
}

/* ------------------------------------------------------------------ trabajos de red (hibynet) */
enum { J_NONE, J_INDEX, J_ICON, J_HPK };
struct st_job { pid_t pid; int kind, idx, efd; char out[160]; long long got, total; char line[96]; int ln; };
static struct st_job st_bg, st_dl;     /* uno en segundo plano (indices, iconos) y una descarga */
static int st_dl_app = -1;             /* app que se descarga (indice en st_apps) */
static int st_dl_sd;                   /* 1: se instala en la tarjeta SD */
static int st_inst_app = -1; static double st_inst_frac;   /* instalando (bloqueante) */
static int st_dirty;

static pid_t st_spawn(const char *url, const char *out, int *efd)
{
    int fd[2] = { -1, -1 }; if (efd && pipe(fd)) return -1;
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    if (efd) { posix_spawn_file_actions_adddup2(&fa, fd[1], 2); posix_spawn_file_actions_addclose(&fa, fd[0]); }
    else posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    char *argv[] = { NETBIN, "get", (char *)url, (char *)out, 0 };
    pid_t pid; int r = posix_spawn(&pid, NETBIN, &fa, 0, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (efd) { close(fd[1]); if (r) { close(fd[0]); return -1; } fcntl(fd[0], F_SETFL, O_NONBLOCK); *efd = fd[0]; }
    return r ? -1 : pid;
}
static void st_job_kill(struct st_job *j)
{
    if (!j->pid) return;
    kill(j->pid, SIGTERM); int s; waitpid(j->pid, &s, 0);
    if (j->efd > 0) close(j->efd);
    char part[180]; snprintf(part, sizeof part, "%s.part", j->out); unlink(part);
    j->pid = 0; j->kind = J_NONE;
}
/* siguiente trabajo de segundo plano: indices pendientes y despues iconos */
static double st_session_fetch[ST_MAXREPO]; static char st_session_url[ST_MAXREPO][256];
static void st_bg_next(void)
{
    if (st_bg.pid || access(NETBIN, X_OK)) return;
    for (int r = 0; r < st_nrepo; r++) if (st_repos[r].busy == 1) {
        st_repos[r].busy = 2;
        st_cache_path(st_repos[r].url, "", ".json", st_bg.out, sizeof st_bg.out);
        st_bg.pid = st_spawn(st_repos[r].url, st_bg.out, 0);
        if (st_bg.pid <= 0) { st_bg.pid = 0; st_repos[r].busy = 0; st_repos[r].err = 2; continue; }
        st_bg.kind = J_INDEX; st_bg.idx = r; st_bg.efd = -1; st_dirty = 1;
        return;
    }
    for (int i = 0; i < st_napp; i++) {
        struct st_app *a = &st_apps[i];
        if (a->ic_state || !a->icon_url[0]) continue;
        a->ic_state = 1;
        st_cache_path(a->icon_url, "/icons", ".png", st_bg.out, sizeof st_bg.out);
        if (!access(st_bg.out, R_OK)) continue;                   /* ya en cache (se carga al pintar) */
        st_bg.pid = st_spawn(a->icon_url, st_bg.out, 0);
        if (st_bg.pid <= 0) { st_bg.pid = 0; a->ic_state = 2; continue; }
        st_bg.kind = J_ICON; st_bg.idx = i; st_bg.efd = -1;
        return;
    }
}
static void st_refresh(int force)
{
    for (int r = 0; r < st_nrepo; r++) {
        int known = !strcmp(st_session_url[r], st_repos[r].url);
        if (!force && known && st_session_fetch[r] && now_ms() - st_session_fetch[r] < ST_REFRESH_MS) continue;
        if (!st_repos[r].busy) st_repos[r].busy = 1;
    }
    st_bg_next();
}
static int st_busy(void) { for (int r = 0; r < st_nrepo; r++) if (st_repos[r].busy) return 1; return 0; }

/* carga el icono (PNG en cache) en un sprite de s px con la mascara de la casa */
static int st_icon_load(const char *url, int s, struct sprite *sp, uint16_t *tint)
{
    char path[160]; st_cache_path(url, "/icons", ".png", path, sizeof path);
    int fd = open(path, O_RDONLY); if (fd < 0) return -1;
    struct stat sb; unsigned char *png = 0, *img = 0; int w = 0, h = 0;
    if (!fstat(fd, &sb) && sb.st_size > 0 && sb.st_size <= HPK_MAX_ICON_BYTES && (png = malloc(sb.st_size)) && !hpk_rd_all(fd, png, (long)sb.st_size, 0))
        img = hpk_png_decode(png, (int)sb.st_size, &w, &h);
    close(fd); free(png);
    if (!img) { unlink(path); return -1; }
    sp->w = sp->h = s; sp->rgb = malloc(s * s * 2); sp->a = malloc(s * s);
    if (!sp->rgb || !sp->a) { free(img); sprite_free(sp); return -1; }
    hpk_icon_render(img, w, h, s, sp->rgb, sp->a);
    if (tint) {
        long R = 0, G = 0, B = 0, n = 0;
        for (int i = 0; i < w * h; i += 7) { if (img[4 * i + 3] < 128) continue; R += img[4 * i]; G += img[4 * i + 1]; B += img[4 * i + 2]; n++; }
        if (n) *tint = rgb((int)(R / n), (int)(G / n), (int)(B / n)); else *tint = rgb(60, 60, 66);
    }
    free(img);
    return 0;
}
static void st_app_icon(struct st_app *a)
{
    if (a->ic.w || a->ic_state == 2 || !a->icon_url[0]) return;
    if (st_bg.pid && st_bg.kind == J_ICON && &st_apps[st_bg.idx] == a) return;
    if (st_icon_load(a->icon_url, 48, &a->ic, &a->tint) == 0) a->ic_state = 1;
}

static void st_install_downloaded(int i);
/* revisa los trabajos; devuelve 1 si hay que repintar */
static int st_jobs_poll(void)
{
    int ch = st_dirty; st_dirty = 0; int s;
    if (st_bg.pid && waitpid(st_bg.pid, &s, WNOHANG) == st_bg.pid) {
        int ok = WIFEXITED(s) && WEXITSTATUS(s) == 0;
        if (st_bg.kind == J_INDEX) {
            struct st_repo *R = &st_repos[st_bg.idx];
            R->busy = 0; R->err = ok ? 0 : WIFEXITED(s) ? WEXITSTATUS(s) : 2;
            if (ok) { R->fetched = now_ms(); }
            st_session_fetch[st_bg.idx] = now_ms(); snprintf(st_session_url[st_bg.idx], 256, "%s", R->url);
            if (!st_busy()) {
                /* conserva la app que se esta descargando por id */
                char keep[HPK_ID_MAX + 1] = ""; if (st_dl_app >= 0) snprintf(keep, sizeof keep, "%s", st_apps[st_dl_app].id);
                st_apps_reload();
                if (keep[0]) { st_dl_app = st_app_find(keep); if (st_dl_app < 0) st_job_kill(&st_dl); }
            }
        } else if (st_bg.kind == J_ICON && st_bg.idx < st_napp) {
            struct st_app *a = &st_apps[st_bg.idx];
            if (!ok || st_icon_load(a->icon_url, 48, &a->ic, &a->tint)) a->ic_state = 2;
        }
        st_bg.pid = 0; st_bg.kind = J_NONE; ch = 1;
        st_bg_next();
    }
    if (st_dl.pid) {
        char b[256]; ssize_t n;
        while (st_dl.efd > 0 && (n = read(st_dl.efd, b, sizeof b)) > 0)
            for (int k = 0; k < n; k++) {
                if (b[k] != '\n') { if (st_dl.ln < (int)sizeof st_dl.line - 1) st_dl.line[st_dl.ln++] = b[k]; continue; }
                st_dl.line[st_dl.ln] = 0; st_dl.ln = 0;
                long long g, t; if (sscanf(st_dl.line, "progress %lld %lld", &g, &t) == 2) { st_dl.got = g; if (t > 0) st_dl.total = t; ch = 1; }
            }
        if (waitpid(st_dl.pid, &s, WNOHANG) == st_dl.pid) {
            int ok = WIFEXITED(s) && WEXITSTATUS(s) == 0;
            if (st_dl.efd > 0) close(st_dl.efd);
            st_dl.pid = 0; st_dl.kind = J_NONE; ch = 1;
            int i = st_dl_app; st_dl_app = -1;
            if (!ok) { unlink(st_dl.out); toast(tr("No se pudo descargar")); }
            else if (i >= 0) st_install_downloaded(i);
        }
    }
    return ch;
}
static double st_dl_frac(void)
{
    if (st_dl_app < 0) return 0;
    long long t = st_dl.total > 0 ? st_dl.total : st_apps[st_dl_app].size;
    return t > 0 ? (double)st_dl.got / t : 0;
}

/* ------------------------------------------------------------------ estado de cada app */
enum { S_GET, S_UPDATE, S_OPEN, S_LOADING, S_INSTALLING, S_NEWER, S_TESTING };
static const struct hpk_info *st_installed(const char *id) { for (int k = 0; k < ninst; k++) if (!strcmp(inst[k].id, id)) return &inst[k]; return 0; }
static int st_state(int i)
{
    struct st_app *a = &st_apps[i];
    if (i == st_inst_app) return S_INSTALLING;
    if (i == st_dl_app) return S_LOADING;
    if (a->unavailable) return S_TESTING;
    const struct hpk_info *in = st_installed(a->id);
    if (in) return hpk_vercmp(a->version, in->installed[0] ? in->installed : in->version) > 0 ? S_UPDATE : S_OPEN;
    if (hpk_vercmp(a->min, HPK_HIBYMOD_VERSION) > 0) return S_NEWER;
    return S_GET;
}
static void st_place_on_home(const char *id)
{
    char xid[HPK_ID_MAX + 4]; snprintf(xid, sizeof xid, "x.%s", id);
    int ai = app_by_id(xid), found = 0;
    for (int q = 0; q < L.npages; q++) for (int k = 0; k < L.pn[q]; k++) if (L.page[q][k] == ai) found = 1;
    for (int k = 0; k < L.dn; k++) if (L.dock[k] == ai) found = 1;
    if (ai >= 0 && !found) {
        int q = L.npages - 1; if (L.pn[q] >= per_page() && L.npages < MAXPAGES) { q = L.npages++; L.pn[q] = 0; }
        if (L.pn[q] < per_page()) L.page[q][L.pn[q]++] = ai;
    }
    lconf_save();
}
static void st_render_current(void);
static void st_install_cb(long done, long total, void *ctx)
{
    (void)ctx; st->shell_beat++;
    st_inst_frac = total ? (double)done / total : 0;
    if (now_ms() - te_last_draw < 30 && done < total) return;
    st_render_current(); present(); te_last_draw = now_ms();
}
/* comprueba tamano y sha256 del .hpk descargado e instala */
static void st_install_downloaded(int i)
{
    struct st_app *a = &st_apps[i];
    char path[180]; snprintf(path, sizeof path, "%s", st_dl.out);
    st_inst_app = i; st_inst_frac = 0;
    int fd = open(path, O_RDONLY), bad = fd < 0;
    if (!bad) {
        struct sha256 s; sha_init(&s); static uint8_t buf[32768]; ssize_t n; long long tot = 0; char hex[65];
        while ((n = read(fd, buf, sizeof buf)) > 0) { sha_update(&s, buf, n); tot += n; st->shell_beat++; }
        close(fd); sha_hex(&s, hex);
        if (n < 0 || strcmp(hex, a->sha) || (a->size > 0 && tot != a->size)) { bad = 1; lg("tienda: %s sha256 %s (esperado %s) %lld bytes", a->id, hex, a->sha, tot); }
    }
    if (bad) { unlink(path); st_inst_app = -1; toast(tr("La descarga está dañada (sha256)")); return; }
    char xid[HPK_ID_MAX + 4]; snprintf(xid, sizeof xid, "x.%s", a->id);
    char err[160] = "";
    int r = st_dl_sd ? (sd_present() ? hpk_install_sd(path, st_install_cb, err, sizeof err, a->id) : -HPK_E_IO)
                     : hpk_install(path, st_install_cb, 0, err, sizeof err);
    unlink(path);
    st_inst_app = -1;
    if (r) { toast(hpk_msg(hpk_errkey(r), ui_lang)); return; }
    char id[HPK_ID_MAX + 1]; snprintf(id, sizeof id, "%s", a->id);
    register_installed_apps(); load_icons(); relabel_apps();
    st_place_on_home(id);
    toast(tr("Instalada"));
}
static void st_start_download(int i, int to_sd)
{
    if (st_dl.pid) { toast(tr("Espera a que termine la descarga")); return; }
    if (access(NETBIN, X_OK)) { toast(tr("Falta el componente de red (hibynet)")); return; }
    st_mkdirs();
    struct st_app *a = &st_apps[i];
    memset(&st_dl, 0, sizeof st_dl);
    if (to_sd) { mkdir(SD_APPROOT, 0755); mkdir(SD_APPROOT "/dl", 0755); snprintf(st_dl.out, sizeof st_dl.out, SD_APPROOT "/dl/%s.hpk", a->id); }
    else snprintf(st_dl.out, sizeof st_dl.out, ST_DIR "/dl/%s.hpk", a->id);
    st_dl_sd = to_sd;
    st_dl.pid = st_spawn(a->hpk_url, st_dl.out, &st_dl.efd);
    if (st_dl.pid <= 0) { st_dl.pid = 0; toast(tr("No se pudo descargar")); return; }
    st_dl.kind = J_HPK; st_dl_app = i;
    lg("tienda: descargando %s %s", a->id, a->hpk_url);
}

/* ================================================================== interfaz */
#define ST_TOP 94           /* bajo la cabecera */
#define ST_TABY 424         /* barra de pestanas */
#define ST_ROW 64
enum { T_TODAY, T_APPS, T_SEARCH, T_INST, T_N };
enum { HT_APP = 1, HT_PILL, HT_SD, HT_INST, HT_INSTPILL, HT_REPO, HT_ADDREPO, HT_SEARCH, HT_BACK, HT_TAB };
struct st_hit { short x0, y0, x1, y1; int kind, idx; };
static struct st_hit st_hits[96]; static int st_nhit, st_rec;
static int st_tab = T_TODAY, st_detail = -1, st_press_kind, st_press_idx = -1;
static float st_pos[T_N + 1], st_vel; static int st_content_h[T_N + 1];
static char st_q[64], st_lq[64];
static uint16_t *st_pageA, *st_pageB;

static void st_hit_add(int x0, int y0, int x1, int y1, int kind, int idx)
{
    if (!st_rec || st_nhit >= 96) return;
    if (y0 < ST_TOP && kind != HT_BACK && kind != HT_TAB) y0 = ST_TOP;
    if (y1 > ST_TABY && kind != HT_TAB) y1 = ST_TABY;
    if (y1 <= y0) return;
    st_hits[st_nhit++] = (struct st_hit){ (short)x0, (short)y0, (short)x1, (short)y1, kind, idx };
}
static int st_hit_at(int x, int y, int *idx)
{
    for (int k = st_nhit - 1; k >= 0; k--) {
        struct st_hit *h = &st_hits[k];
        if (x >= h->x0 && x < h->x1 && y >= h->y0 && y < h->y1) { *idx = h->idx; return h->kind; }
    }
    return 0;
}
static void st_hline(uint16_t *d, int x0, int x1, int y, uint16_t c) { if (y < ST_TOP || y >= ST_TABY) return; for (int x = x0; x < x1 && x < W; x++) d[y * W + x] = c; }

/* anillo de progreso de iOS (pista gris, arco azul y cuadradito de parar) */
static void st_ring(uint16_t *d, int cx, int cy, float frac, double t, int spin)
{
    float R = 13, w = 2.6f;
    for (int y = cy - 15; y <= cy + 15; y++) {
        if (y < ST_TOP || y >= ST_TABY) continue;
        for (int x = cx - 15; x <= cx + 15; x++) {
            float dx = x + .5f - cx, dy = y + .5f - cy, r = sqrtf(dx * dx + dy * dy);
            float cov = w / 2 + .5f - fabsf(r - R); if (cov <= 0) continue; if (cov > 1) cov = 1;
            float ang = atan2f(dx, -dy); if (ang < 0) ang += 2 * (float)M_PI; float u = ang / (2 * (float)M_PI);
            int on = spin ? fmodf(u - (float)fmod(t / 900.0, 1.0) + 1, 1) < 0.28f : u <= frac;
            uint16_t c = on ? rgb(10, 132, 255) : rgb(58, 58, 62);
            d[y * W + x] = mix565(c, d[y * W + x], (unsigned)(255 * cov));
        }
    }
    if (!spin) fill_rrect(d, cx - 4, cy - 4, 8, 8, 1.5f, rgb(10, 132, 255), 255);
}
/* boton de la fila: pildora gris con texto azul (o el anillo si se descarga) */
static int st_pill(uint16_t *d, int xr, int y, int state, double frac, int filled)
{
    if (state == S_LOADING || state == S_INSTALLING) { st_ring(d, xr - 16, y + 14, (float)frac, now_ms(), state == S_INSTALLING && frac <= 0.01); return 32; }
    const char *t = state == S_TESTING ? tr("EN PRUEBAS") : state == S_GET || state == S_NEWER ? tr("OBTENER") : state == S_UPDATE ? tr("ACTUALIZAR") : tr("ABRIR");
    int pw = tw_n(t, 13, -1) + 30; if (pw < 74) pw = 74;
    int blue = filled == 1 && state != S_OPEN, glass = filled == 2;   /* 2: sobre tarjeta de color */
    if (glass) fill_rrect(d, xr - pw, y, pw, 28, 14, 0xFFFF, 64);
    else fill_rrect(d, xr - pw, y, pw, 28, 14, blue ? rgb(10, 132, 255) : rgb(44, 44, 48), 255);
    draw_text(d, xr - pw + (pw - tw_n(t, 13, -1)) / 2, y + 19, t, 13, blue || glass ? 0xFFFF : state == S_NEWER || state == S_TESTING ? rgb(99, 99, 102) : rgb(10, 132, 255), 0, ST_TOP, ST_TABY);
    return pw;
}
static void st_icon_box(uint16_t *d, const struct sprite *ic, int x, int y, int s)
{
    if (ic && ic->w == s) blit(d, ic, x, y, 32, 255);
    else fill_rrect(d, x, y, s, s, s * 0.2237f, rgb(58, 58, 62), 255);
}
/* hasta "lines" lineas con corte por palabras; la ultima con "…" */
static int st_wrap(uint16_t *d, int x, int y, int w, const char *s, int size, uint16_t col, int lh, int lines)
{
    int n = 0;
    while (*s && n < lines) {
        while (*s == ' ') s++;
        int len = (int)strlen(s), cut = len;
        if (tw_n(s, size, len) > w) {
            cut = 0; int lastsp = -1;
            for (int k = 0; k <= len; k++) {
                if (k < len && ((unsigned char)s[k] & 0xC0) == 0x80) continue;
                if (tw_n(s, size, k) > w) break;
                cut = k; if (s[k] == ' ') lastsp = k;
            }
            if (lastsp > 0 && n < lines - 1) cut = lastsp;
        }
        if (n == lines - 1 || cut >= len) { draw_text(d, x, y + n * lh, s, size, col, w, ST_TOP, ST_TABY); n++; break; }
        char line[256]; int c2 = cut < 255 ? cut : 255; memcpy(line, s, c2); line[c2] = 0;
        draw_text(d, x, y + n * lh, line, size, col, w, ST_TOP, ST_TABY);
        s += cut; n++;
    }
    return n * lh;
}
static void st_section(uint16_t *d, int y, const char *t) { if (y > ST_TOP - 20 && y < ST_TABY + 10) draw_text(d, 16, y, t, 12, rgb(142, 142, 147), 0, ST_TOP, ST_TABY); }

/* fila de app de un repositorio: icono, nombre, descripcion corta y boton */
static void st_app_row(uint16_t *d, int y, int i, int last)
{
    struct st_app *a = &st_apps[i];
    if (y + ST_ROW < ST_TOP || y > ST_TABY) return;
    st_app_icon(a);
    if (st_press_kind == HT_APP && st_press_idx == i) fill_rrect(d, 0, y, W, ST_ROW, 0, rgb(28, 28, 30), 255);
    st_icon_box(d, &a->ic, 16, y + 8, 48);
    int s = st_state(i);
    int pw = st_pill(d, W - 16, y + 18, s, s == S_INSTALLING ? st_inst_frac : st_dl_frac(), 0);
    draw_text(d, 76, y + 28, a->name, 15, 0xFFFF, W - 76 - pw - 26, ST_TOP, ST_TABY);
    draw_text(d, 76, y + 46, a->desc, 12, rgb(142, 142, 147), W - 76 - pw - 26, ST_TOP, ST_TABY);
    st_hit_add(0, y, W - 16 - pw - 6, y + ST_ROW, HT_APP, i);
    st_hit_add(W - 16 - pw - 6, y, W, y + ST_ROW, HT_PILL, i);
    if (!last) st_hline(d, 76, W, y + ST_ROW - 1, rgb(38, 38, 40));
}
static int st_match(int i) { return !st_lq[0] || strstr(st_apps[i].lname, st_lq) || strstr(st_apps[i].ldesc, st_lq); }

/* mensaje de estado (sin conexion, cargando, vacio) */
static int st_status_msg(uint16_t *d, int y)
{
    const char *m = 0;
    if (!st_napp && st_busy()) m = tr("Cargando…");
    else if (!st_napp) {
        int err = 0; for (int r = 0; r < st_nrepo; r++) if (st_repos[r].err) err = 1;
        m = access(NETBIN, X_OK) ? tr("Falta el componente de red (hibynet)") : err ? tr("No se pudo conectar. Comprueba la WiFi y desliza hacia abajo para reintentar.") : tr("No hay apps en los repositorios");
    }
    if (!m) return 0;
    return st_wrap(d, 24, y + 24, W - 48, m, 14, rgb(142, 142, 147), 20, 4) + 30;
}

static int st_render_today(uint16_t *d, int y0)
{
    int y = y0 + 8;
    char date[48]; time_t now = time(0); struct tm tmv; localtime_r(&now, &tmv);
    if (tmv.tm_year > 120) {
        static const char *dn[7] = { "DOMINGO", "LUNES", "MARTES", "MIÉRCOLES", "JUEVES", "VIERNES", "SÁBADO" };
        snprintf(date, sizeof date, "%s %d", tr(dn[tmv.tm_wday]), tmv.tm_mday);
        st_section(d, y + 12, date); y += 22;
    }
    int nfeat = 0; for (int i = 0; i < st_napp; i++) nfeat += st_apps[i].featured;
    int shown = 0;
    for (int i = 0; i < st_napp; i++) {
        struct st_app *a = &st_apps[i];
        if (nfeat && !a->featured) continue;
        int ch = 212;
        if (y + ch > ST_TOP && y < ST_TABY) {
            st_app_icon(a);
            uint16_t base = a->ic.w ? a->tint : rgb(40, 40, 46);
            int press = st_press_kind == HT_APP && st_press_idx == i;
            /* tarjeta: degradado del color del icono hacia negro, esquinas de 16 px */
            for (int yy = 0; yy < ch; yy++) {
                int yy2 = y + yy; if (yy2 < ST_TOP || yy2 >= ST_TABY) continue;
                int k = 20 - yy * 12 / ch; if (press) k -= 4;
                uint16_t c = dim565(base, k);
                int inset = 0;
                if (yy < 16 || yy >= ch - 16) { float dy = yy < 16 ? 16 - yy - .5f : yy - (ch - 16) + .5f; inset = (int)(16 - sqrtf(fmaxf(0, 256 - dy * dy))); }
                for (int x = 16 + inset; x < W - 16 - inset; x++) d[yy2 * W + x] = c;
            }
            const char *cap = a->category[0] ? a->category : "app";
            char up[32]; int n = 0; for (const char *p = cap; *p && n < 30; p++) up[n++] = (char)toupper((unsigned char)*p); up[n] = 0;
            draw_text(d, 32, y + 30, nfeat ? tr("APP DESTACADA") : up, 11, rgb(235, 235, 245), 0, ST_TOP, ST_TABY);
            draw_text(d, 32, y + 60, a->name, 24, 0xFFFF, W - 64, ST_TOP, ST_TABY);
            st_wrap(d, 32, y + 88, W - 64, a->desc, 14, rgb(235, 235, 245), 19, 2);
            /* pie: icono, nombre, autor y boton */
            for (int x = 16; x < W - 16; x++) if (y + 140 >= ST_TOP && y + 140 < ST_TABY) d[(y + 140) * W + x] = dim565(base, 10);
            st_icon_box(d, &a->ic, 32, y + 152, 48);
            int s = st_state(i);
            int pw = st_pill(d, W - 32, y + 162, s, s == S_INSTALLING ? st_inst_frac : st_dl_frac(), 2);
            draw_text(d, 90, y + 172, a->name, 14, 0xFFFF, W - 90 - pw - 40, ST_TOP, ST_TABY);
            draw_text(d, 90, y + 190, a->author[0] ? a->author : st_repos[a->repo].name, 12, rgb(200, 200, 210), W - 90 - pw - 40, ST_TOP, ST_TABY);
            st_hit_add(16, y, W - 16, y + ch, HT_APP, i);
            st_hit_add(W - 32 - pw - 6, y + 150, W - 16, y + 200, HT_PILL, i);
        }
        y += ch + 20; shown++;
    }
    if (!shown) y += st_status_msg(d, y);
    return y - y0 + 10;
}
static int st_render_apps(uint16_t *d, int y0)
{
    int y = y0 + 8, n = 0;
    st_section(d, y + 14, tr("TODAS LAS APPS")); y += 22;
    for (int i = 0; i < st_napp; i++, n++) { st_app_row(d, y, i, i == st_napp - 1); y += ST_ROW; }
    if (!n) y += st_status_msg(d, y);
    /* paquetes .hpk copiados a la tarjeta SD */
    y += 18; st_section(d, y + 14, tr("EN LA TARJETA SD (/Apps)")); y += 22;
    if (!npkgs) y += st_wrap(d, 16, y + 18, W - 32, tr("HPK_HELP"), 13, rgb(142, 142, 147), 18, 6) + 14;
    for (int i = 0; i < npkgs; i++, y += ST_ROW) {
        if (y + ST_ROW < ST_TOP || y > ST_TABY) continue;
        if (st_press_kind == HT_SD && st_press_idx == i) fill_rrect(d, 0, y, W, ST_ROW, 0, rgb(28, 28, 30), 255);
        st_icon_box(d, &pkg_icon[i], 16, y + 8, 48);
        char sub[64];
        if (pkgs[i].status) snprintf(sub, sizeof sub, "%s", hpk_msg(hpk_errkey(pkgs[i].status), ui_lang));
        else snprintf(sub, sizeof sub, "%s · %.0f KB", pkgs[i].version, pkgs[i].size / 1024.0);
        int pw = pkgs[i].status ? 0 : st_pill(d, W - 16, y + 18, pkg_state(&pkgs[i]), 0, 0);
        draw_text(d, 76, y + 28, pkg_name(&pkgs[i]), 15, 0xFFFF, W - 76 - pw - 26, ST_TOP, ST_TABY);
        draw_text(d, 76, y + 46, sub, 12, pkgs[i].status ? rgb(255, 69, 58) : rgb(142, 142, 147), W - 76 - pw - 26, ST_TOP, ST_TABY);
        if (!pkgs[i].status) st_hit_add(0, y, W, y + ST_ROW, HT_SD, i);
        if (i < npkgs - 1) st_hline(d, 76, W, y + ST_ROW - 1, rgb(38, 38, 40));
    }
    return y - y0 + 16;
}
static void st_glyph_search(uint16_t *d, int cx, int cy, float s, uint16_t col, uint16_t bg)
{
    fill_circle(d, cx - s * .12f, cy - s * .12f, s * .36f, col, 255);
    fill_circle(d, cx - s * .12f, cy - s * .12f, s * .36f - s * .11f, bg, 255);
    for (float k = 0; k <= 1; k += .1f) fill_circle(d, cx + s * (.14f + .26f * k), cy + s * (.14f + .26f * k), s * .065f, col, 255);
}
static int st_render_search(uint16_t *d, int y0)
{
    int y = y0 + 10;
    int press = st_press_kind == HT_SEARCH;
    fill_rrect(d, 16, y, W - 32, 36, 10, press ? rgb(44, 44, 48) : rgb(28, 28, 30), 255);
    st_glyph_search(d, 34, y + 18, 16, rgb(142, 142, 147), press ? rgb(44, 44, 48) : rgb(28, 28, 30));
    if (st_q[0]) draw_text(d, 50, y + 24, st_q, 15, 0xFFFF, W - 82, ST_TOP, ST_TABY);
    else draw_text(d, 50, y + 24, tr("Buscar apps"), 15, rgb(142, 142, 147), W - 82, ST_TOP, ST_TABY);
    st_hit_add(16, y, W - 16, y + 36, HT_SEARCH, 0);
    y += 52;
    int n = 0, total = 0; for (int i = 0; i < st_napp; i++) total += st_match(i);
    if (st_lq[0]) { char h[64]; snprintf(h, sizeof h, tr("%d resultados"), total); st_section(d, y + 12, h); y += 20; }
    for (int i = 0; i < st_napp; i++) if (st_match(i)) { st_app_row(d, y, i, ++n == total); y += ST_ROW; }
    if (!n && st_lq[0]) { draw_text(d, 24, y + 30, tr("Sin resultados"), 15, rgb(142, 142, 147), W - 48, ST_TOP, ST_TABY); y += 50; }
    else if (!n) y += st_status_msg(d, y);
    return y - y0 + 10;
}
static int st_render_inst(uint16_t *d, int y0)
{
    int y = y0 + 8;
    st_section(d, y + 14, tr("INSTALADAS")); y += 22;
    if (!ninst) { draw_text(d, 16, y + 24, tr("Ninguna app instalada"), 14, rgb(142, 142, 147), 0, ST_TOP, ST_TABY); y += 44; }
    for (int i = 0; i < ninst; i++, y += ST_ROW) {
        if (y + ST_ROW < ST_TOP || y > ST_TABY) continue;
        if (st_press_kind == HT_INST && st_press_idx == i) fill_rrect(d, 0, y, W, ST_ROW, 0, rgb(28, 28, 30), 255);
        char id[HPK_ID_MAX + 4]; snprintf(id, sizeof id, "x.%s", inst[i].id);
        int ai = app_by_id(id);
        st_icon_box(d, ai >= 0 ? &apps[ai].icon[0] : 0, 16, y + 8, 48);
        int ri = st_app_find(inst[i].id), s = ri >= 0 ? st_state(ri) : S_OPEN, pw = 0;
        if (ri >= 0 && (s == S_UPDATE || s == S_LOADING || s == S_INSTALLING)) {
            pw = st_pill(d, W - 16, y + 18, s, s == S_INSTALLING ? st_inst_frac : st_dl_frac(), 0);
            st_hit_add(W - 16 - pw - 6, y, W, y + ST_ROW, HT_INSTPILL, ri);
        } else for (int k = 0; k < 6; k++) { int cx = W - 24 + k / 2 - 1; if (y + 26 + k < ST_TABY && y + 26 + k > ST_TOP) { d[(y + 26 + k) * W + cx] = rgb(90, 90, 96); d[(y + 37 - k) * W + cx] = rgb(90, 90, 96); } }
        draw_text(d, 76, y + 28, pkg_name(&inst[i]), 15, 0xFFFF, W - 76 - pw - 30, ST_TOP, ST_TABY);
        char sub[96];
        if (s == S_UPDATE) snprintf(sub, sizeof sub, tr("Versión %s disponible"), st_apps[ri].version);
        else if (app_sd_missing(&inst[i])) snprintf(sub, sizeof sub, "%s", tr("Falta la tarjeta SD"));
        else if (app_sd_path(&inst[i], 0, 0)) snprintf(sub, sizeof sub, "%s · %s", inst[i].installed[0] ? inst[i].installed : inst[i].version, tr("Tarjeta SD"));
        else snprintf(sub, sizeof sub, "%s · %.0f KB", inst[i].installed[0] ? inst[i].installed : inst[i].version, inst[i].size / 1024.0);
        draw_text(d, 76, y + 46, sub, 12, app_sd_missing(&inst[i]) ? rgb(255, 159, 10) : rgb(142, 142, 147), W - 76 - pw - 30, ST_TOP, ST_TABY);
        st_hit_add(0, y, W - 16 - pw - 6, y + ST_ROW, HT_INST, i);
        if (i < ninst - 1) st_hline(d, 76, W, y + ST_ROW - 1, rgb(38, 38, 40));
    }
    /* repositorios */
    y += 18; st_section(d, y + 14, tr("REPOSITORIOS")); y += 22;
    for (int r = 0; r < st_nrepo; r++, y += 56) {
        if (y + 56 < ST_TOP || y > ST_TABY) continue;
        struct st_repo *R = &st_repos[r];
        if (st_press_kind == HT_REPO && st_press_idx == r) fill_rrect(d, 0, y, W, 56, 0, rgb(28, 28, 30), 255);
        char host[48]; st_host(R->url, host, sizeof host);
        draw_text(d, 16, y + 24, R->name[0] ? R->name : host, 15, 0xFFFF, W - 60, ST_TOP, ST_TABY);
        char sub[96];
        if (R->busy) snprintf(sub, sizeof sub, "%s · %s", host, tr("Actualizando…"));
        else if (R->err) snprintf(sub, sizeof sub, "%s · %s", host, tr("Sin conexión"));
        else snprintf(sub, sizeof sub, tr("%s · %d apps"), host, R->napps);
        draw_text(d, 16, y + 43, sub, 12, R->err && !R->busy ? rgb(255, 159, 10) : rgb(142, 142, 147), W - 60, ST_TOP, ST_TABY);
        if (R->busy) st_ring(d, W - 30, y + 28, 0, now_ms(), 1);
        st_hit_add(0, y, W, y + 56, HT_REPO, r);
        st_hline(d, 16, W, y + 55, rgb(38, 38, 40));
    }
    if (st_nrepo < ST_MAXREPO) {
        if (st_press_kind == HT_ADDREPO) fill_rrect(d, 0, y, W, 48, 0, rgb(28, 28, 30), 255);
        draw_text(d, 16, y + 30, tr("Añadir repositorio…"), 15, rgb(10, 132, 255), 0, ST_TOP, ST_TABY);
        st_hit_add(0, y, W, y + 48, HT_ADDREPO, 0);
        y += 48;
    }
    return y - y0 + 16;
}

/* iconos de la barra de pestanas (vectoriales, 24 px) */
static void st_tab_glyph(uint16_t *d, int t, int cx, int cy, uint16_t col)
{
    uint16_t bg = rgb(20, 20, 22);
    if (t == T_TODAY) { fill_rrect(d, cx - 10, cy - 11, 20, 22, 4, col, 255); fill_rrect(d, cx - 7, cy - 1, 14, 9, 2, bg, 255); fill_rrect(d, cx - 7, cy - 8, 9, 3, 1.5f, bg, 255); }
    else if (t == T_APPS) { for (int k = 0; k < 4; k++) fill_rrect(d, cx - 10 + (k & 1) * 11, cy - 10 + (k >> 1) * 11, 9, 9, 2.5f, col, 255); }
    else if (t == T_SEARCH) st_glyph_search(d, cx, cy, 24, col, bg);
    else {
        fill_circle(d, cx, cy, 11, col, 255); fill_circle(d, cx, cy, 9, bg, 255);
        fill_rrect(d, cx - 1, cy - 6, 3, 9, 1, col, 255);
        for (int r = 0; r < 5; r++) for (int x = cx - 5 + r; x <= cx + 5 - r + 1; x++) { int yy = cy + 1 + r; if (x >= 0 && x < W) d[yy * W + x] = col; }
    }
}
static void st_chrome(uint16_t *d, const char *title, int back)
{
    memcpy(d, fr1, 30 * W * 2);
    for (int yy = 30; yy < ST_TOP; yy++) for (int x = 0; x < W; x++) d[yy * W + x] = 0;
    if (back) {
        const char *bt = tr(st_tab == T_TODAY ? "Hoy" : st_tab == T_APPS ? "Apps" : st_tab == T_SEARCH ? "Buscar" : "Instaladas");
        int c = st_press_kind == HT_BACK ? rgb(5, 66, 128) : rgb(10, 132, 255);
        for (float k = -1; k <= 1.001f; k += .1f) fill_circle(d, 18 + 8 * fabsf(k), 55 + 9 * k, 1.4f, c, 255);   /* chevron < */
        draw_text(d, 32, 61, bt, 17, c, 120, 0, H);
        st_hit_add(0, 30, 140, ST_TOP, HT_BACK, 0);
    } else {
        draw_text(d, 16, 80, title, 28, 0xFFFF, W - 70, 0, H);
        if (st_busy() || st_bg.kind == J_INDEX) st_ring(d, W - 30, 72, 0, now_ms(), 1);
    }
    for (int x = 0; x < W; x++) d[(ST_TOP - 1) * W + x] = rgb(38, 38, 40);
    /* barra de pestanas */
    for (int yy = ST_TABY; yy < H; yy++) for (int x = 0; x < W; x++) d[yy * W + x] = rgb(20, 20, 22);
    for (int x = 0; x < W; x++) d[ST_TABY * W + x] = rgb(48, 48, 52);
    static const char *tl[T_N] = { "Hoy", "Apps", "Buscar", "Instaladas" };
    for (int t = 0; t < T_N; t++) {
        int cx = W * t / T_N + W / T_N / 2;
        uint16_t col = t == st_tab ? rgb(10, 132, 255) : rgb(142, 142, 147);
        st_tab_glyph(d, t, cx, ST_TABY + 18, col);
        const char *l = tr(tl[t]); int lw = tw_n(l, 10, -1);
        draw_text(d, cx - lw / 2, ST_TABY + 44, l, 10, col, W / T_N - 4, 0, H);
        st_hit_add(W * t / T_N, ST_TABY, W * (t + 1) / T_N, H, HT_TAB, t);
    }
    /* insignia de actualizaciones en "Instaladas" */
    int nup = 0; for (int i = 0; i < ninst; i++) { int ri = st_app_find(inst[i].id); if (ri >= 0 && st_state(ri) == S_UPDATE) nup++; }
    if (nup) { int cx = W * 3 / T_N + W / T_N / 2 + 12; char b[8]; snprintf(b, sizeof b, "%d", nup); fill_circle(d, cx, ST_TABY + 8, 8, rgb(255, 59, 48), 255); draw_text(d, cx - tw_n(b, 11, -1) / 2, ST_TABY + 12, b, 11, 0xFFFF, 0, 0, H); }
    draw_toast(d);
}
static void st_render_tab(uint16_t *d, int rec)
{
    st_rec = rec; if (rec) st_nhit = 0;
    for (int i = ST_TOP * W; i < ST_TABY * W; i++) d[i] = 0;
    int y0 = ST_TOP - (int)st_pos[st_tab], h = 0;
    /* tirar para actualizar: hueco con el anillo girando */
    if (st_pos[st_tab] < -8 && st_tab != T_SEARCH) st_ring(d, W / 2, ST_TOP + (int)(-st_pos[st_tab]) / 2, 0, now_ms(), 1);
    switch (st_tab) {
    case T_TODAY: h = st_render_today(d, y0); break;
    case T_APPS: h = st_render_apps(d, y0); break;
    case T_SEARCH: h = st_render_search(d, y0); break;
    default: h = st_render_inst(d, y0); break;
    }
    st_content_h[st_tab] = h;
    static const char *tl[T_N] = { "Hoy", "Apps", "Buscar", "Instaladas" };
    st_chrome(d, tr(tl[st_tab]), 0);
    st_rec = 0;
}

/* ------------------------------------------------------------------ ficha de una app */
static struct sprite st_big; static char st_big_for[256];
static void st_render_detail(uint16_t *d, int rec)
{
    st_rec = rec; if (rec) st_nhit = 0;
    for (int i = ST_TOP * W; i < ST_TABY * W; i++) d[i] = 0;
    if (st_detail < 0 || st_detail >= st_napp) { st_chrome(d, "", 1); st_rec = 0; return; }
    struct st_app *a = &st_apps[st_detail];
    if (strcmp(st_big_for, a->icon_url) || !st_big.w) {
        sprite_free(&st_big); st_big_for[0] = 0;
        if (a->icon_url[0] && !st_icon_load(a->icon_url, 96, &st_big, 0)) snprintf(st_big_for, sizeof st_big_for, "%s", a->icon_url);
    }
    int y = ST_TOP + 14 - (int)st_pos[T_N];
    st_icon_box(d, st_big.w ? &st_big : 0, 16, y, 96);
    draw_text(d, 126, y + 26, a->name, 21, 0xFFFF, W - 140, ST_TOP, ST_TABY);
    draw_text(d, 126, y + 46, a->author[0] ? a->author : st_repos[a->repo].name, 13, rgb(142, 142, 147), W - 140, ST_TOP, ST_TABY);
    int s = st_state(st_detail);
    if (s == S_LOADING || s == S_INSTALLING) st_ring(d, 126 + 16, y + 78, (float)(s == S_INSTALLING ? st_inst_frac : st_dl_frac()), now_ms(), s == S_INSTALLING && st_inst_frac <= 0.01);
    else {
        const char *t = s == S_TESTING ? tr("EN PRUEBAS") : s == S_UPDATE ? tr("ACTUALIZAR") : s == S_OPEN ? tr("ABRIR") : tr("OBTENER");
        int pw = tw_n(t, 14, -1) + 34; if (pw < 84) pw = 84;
        int press = st_press_kind == HT_PILL;
        uint16_t bg = s == S_OPEN || s == S_NEWER || s == S_TESTING ? rgb(44, 44, 48) : rgb(10, 132, 255);
        if (press) bg = dim565(bg, 22);
        fill_rrect(d, 126, y + 64, pw, 30, 15, bg, 255);
        draw_text(d, 126 + (pw - tw_n(t, 14, -1)) / 2, y + 84, t, 14, s == S_OPEN ? rgb(10, 132, 255) : s == S_NEWER || s == S_TESTING ? rgb(99, 99, 102) : 0xFFFF, 0, ST_TOP, ST_TABY);
        st_hit_add(120, y + 58, 126 + pw + 8, y + 100, HT_PILL, st_detail);
    }
    if (s == S_NEWER) draw_text(d, 126, y + 112, tr("Necesita una versión más nueva de HiByOS"), 11, rgb(255, 159, 10), W - 140, ST_TOP, ST_TABY);
    if (s == S_TESTING) draw_text(d, 126, y + 112, tr("En pruebas: todavía no se puede instalar"), 11, rgb(255, 159, 10), W - 140, ST_TOP, ST_TABY);
    y += 124;
    /* franja de datos: tamano, version, memoria */
    for (int x = 16; x < W - 16; x++) if (y >= ST_TOP && y < ST_TABY) d[y * W + x] = rgb(38, 38, 40);
    char v1[24], v3[24];
    snprintf(v1, sizeof v1, a->size >= 1 << 20 ? "%.1f MB" : "%.0f KB", a->size >= 1 << 20 ? a->size / 1048576.0 : a->size / 1024.0);
    if (a->ram_kb >= 1024) snprintf(v3, sizeof v3, "%.0f MB", a->ram_kb / 1024.0); else if (a->ram_kb > 0) snprintf(v3, sizeof v3, "%d KB", a->ram_kb); else snprintf(v3, sizeof v3, "—");
    const char *lab[3] = { tr("TAMAÑO"), tr("VERSIÓN"), tr("MEMORIA") }; const char *val[3] = { v1, a->version, v3 };
    for (int k = 0; k < 3; k++) {
        int cx = 16 + (W - 32) * k / 3 + (W - 32) / 6;
        draw_text(d, cx - tw_n(lab[k], 10, -1) / 2, y + 22, lab[k], 10, rgb(142, 142, 147), 0, ST_TOP, ST_TABY);
        draw_text(d, cx - tw_n(val[k], 17, -1) / 2, y + 46, val[k], 17, rgb(200, 200, 206), 0, ST_TOP, ST_TABY);
        if (k) for (int yy = y + 12; yy < y + 52; yy++) if (yy >= ST_TOP && yy < ST_TABY) d[yy * W + 16 + (W - 32) * k / 3] = rgb(38, 38, 40);
    }
    y += 62;
    for (int x = 16; x < W - 16; x++) if (y >= ST_TOP && y < ST_TABY) d[y * W + x] = rgb(38, 38, 40);
    y += 16;
    y += st_wrap(d, 16, y + 16, W - 32, a->desc, 15, rgb(235, 235, 245), 21, 12) + 10;
    if (a->stop_player) { y += st_wrap(d, 16, y + 16, W - 32, tr("Al abrirla se pausa la música para dejarle memoria."), 13, rgb(142, 142, 147), 18, 3) + 8; }
    y += 10; st_section(d, y + 12, tr("INFORMACIÓN")); y += 20;
    const char *kl[3] = { tr("Repositorio"), tr("Categoría"), tr("Identificador") };
    const char *kv[3] = { st_repos[a->repo].name, a->category[0] ? a->category : "—", a->id };
    for (int k = 0; k < 3; k++, y += 40) {
        draw_text(d, 16, y + 26, kl[k], 14, rgb(142, 142, 147), 120, ST_TOP, ST_TABY);
        int vw = tw_n(kv[k], 14, -1); if (vw > W - 160) vw = W - 160;
        draw_text(d, W - 16 - vw, y + 26, kv[k], 14, rgb(220, 220, 226), W - 160, ST_TOP, ST_TABY);
        st_hline(d, 16, W - 16, y + 39, rgb(38, 38, 40));
    }
    st_content_h[T_N] = y + 20 - (ST_TOP - (int)st_pos[T_N]);
    st_chrome(d, a->name, 1);
    st_rec = 0;
}
static void st_render_current(void) { if (st_detail >= 0) st_render_detail(fr0, 1); else st_render_tab(fr0, 1); }

/* ------------------------------------------------------------------ transiciones */
/* empujar/sacar la ficha: la de arriba entra desde la derecha con sombra, la de abajo se
 * desplaza un 30 % y se oscurece (como UINavigationController). x = borde izquierdo de la ficha */
static void st_compose(const uint16_t *under, const uint16_t *top, int x)
{
    if (x < 0) x = 0; if (x > W) x = W;
    double e = (double)x / W;
    int ux = -(int)((W - x) * 0.3), k = 32 - (int)(8 * (1 - e));
    for (int y = 0; y < H; y++) {
        uint16_t *o = fr0 + y * W;
        if (y < 30 || y >= ST_TABY) { memcpy(o, (x < W / 2 ? top : under) + y * W, W * 2); continue; }
        const uint16_t *u = under + y * W, *t = top + y * W;
        for (int i = 0; i < x; i++) { int sx = i - ux; o[i] = sx < W ? dim565(u[sx], k) : 0; }
        if (x < W) memcpy(o + x, t, (W - x) * 2);
        for (int s = 1; s <= 8 && x - s >= 0; s++) o[x - s] = dim565(o[x - s], 32 - (9 - s) * 2 * (int)(e * 10 + 1) / 11);
    }
}
static void st_push(int i)
{
    st_render_tab(st_pageA, 0);
    st_detail = i; st_pos[T_N] = 0; st_press_kind = 0;
    st_render_detail(st_pageB, 0);
    double t0 = now_ms();
    for (;;) {
        double t = (now_ms() - t0) / 320.0; if (t > 1) t = 1;
        st_compose(st_pageA, st_pageB, (int)(W * (1 - ease_out(t))));
        present(); if (t >= 1) break;
    }
}
/* sacar la ficha desde x0 (arrastre del borde) */
static void st_pop_from(int x0, int done)
{
    st_render_detail(st_pageB, 0);
    int keep = st_detail; st_detail = -1; st_render_tab(st_pageA, 0); st_detail = keep;
    double t0 = now_ms(), ms = done ? 260.0 * (W - x0) / W + 60 : 220.0 * x0 / W + 60;
    for (;;) {
        double t = (now_ms() - t0) / ms; if (t > 1) t = 1; double e = ease_out(t);
        int x = done ? x0 + (int)((W - x0) * e) : (int)(x0 * (1 - e));
        st_compose(st_pageA, st_pageB, x);
        present(); if (t >= 1) break;
    }
    if (done) st_detail = -1;
}

/* ------------------------------------------------------------------ acciones */
static void st_action_pill(int i, int *quit)
{
    int s = st_state(i);
    if (s == S_LOADING) { st_job_kill(&st_dl); unlink(st_dl.out); st_dl_app = -1; return; }   /* tocar el anillo cancela */
    if (s == S_INSTALLING) return;
    if (s == S_NEWER) { toast(tr("Necesita una versión más nueva de HiByOS")); return; }
    if (s == S_TESTING) { toast(tr("En pruebas: todavía no se puede instalar")); return; }
    const struct hpk_info *in = st_installed(st_apps[i].id);
    if (s == S_OPEN) {
        if (in && app_sd_missing(in)) { toast(tr("Esta app está en la tarjeta SD: insértala para abrirla")); return; }
        char xid[HPK_ID_MAX + 4]; snprintf(xid, sizeof xid, "x.%s", st_apps[i].id);
        int ai = app_by_id(xid); if (ai >= 0) { st_open_after = ai; *quit = 1; }
        return;
    }
    int to_sd = 0;
    long long need = st_apps[i].size > 0 ? st_apps[i].size * 2 + HPK_SPACE_MARGIN : 0;
    if (s == S_UPDATE && in) {                       /* la actualizacion se queda donde esta la app */
        to_sd = app_sd_path(in, 0, 0);
        if (to_sd && app_sd_missing(in)) { toast(tr("No hay tarjeta SD")); return; }
    } else if (sd_present()) {
        /* elegir donde instalar, con el espacio libre de cada sitio */
        char fi[24], fs[24], o1[96], o2[96], t[160];
        fmt_bytes(free_bytes("/usr/data"), fi, sizeof fi); fmt_bytes(free_bytes(SD_ROOT), fs, sizeof fs);
        snprintf(o1, sizeof o1, tr("En el HiBy (%s libres)"), fi);
        snprintf(o2, sizeof o2, tr("En la tarjeta SD (%s libres)"), fs);
        char sz[24]; fmt_bytes(st_apps[i].size, sz, sizeof sz);
        snprintf(t, sizeof t, tr("¿Dónde instalar %s (%s)?"), st_apps[i].name, sz);
        const char *o[2] = { o2, o1 };                 /* la SD primero: la memoria interna es escasa */
        int k = action_sheet(t, o, 2, -1);
        if (k < 0 || k > 1) return;
        to_sd = k == 0;
    }
    if (need && free_bytes(to_sd ? SD_ROOT : "/usr/data") < need) { toast(hpk_msg(hpk_errkey(HPK_E_SPACE), ui_lang)); return; }
    st_start_download(i, to_sd);
}
static void st_add_repo(void)
{
    char url[256] = "https://";
    if (!line_edit(tr("Añadir repositorio"), tr("URL del index.json"), url, sizeof url)) return;
    char *u = url; while (*u == ' ') u++;
    u[strcspn(u, " \r\n")] = 0;
    if (!st_url_ok(u)) { toast(tr("URL no válida")); return; }
    for (int r = 0; r < st_nrepo; r++) if (!strcmp(st_repos[r].url, u)) { toast(tr("Ese repositorio ya está")); return; }
    if (st_nrepo >= ST_MAXREPO) return;
    struct st_repo *R = &st_repos[st_nrepo]; memset(R, 0, sizeof *R); snprintf(R->url, sizeof R->url, "%s", u);
    st_session_url[st_nrepo][0] = 0; st_session_fetch[st_nrepo] = 0;
    st_nrepo++; st_repos_save();
    R->busy = 1; st_bg_next();
    toast(tr("Repositorio añadido"));
}
static void st_repo_menu(int r)
{
    char host[48]; st_host(st_repos[r].url, host, sizeof host);
    const char *o[2] = { tr("Actualizar ahora"), tr("Eliminar repositorio") };
    int a = action_sheet(st_repos[r].url, o, r ? 2 : 1, r ? 1 : -1);
    if (a == 0) { st_repos[r].busy = 1; st_bg_next(); }
    else if (a == 1 && r > 0 && confirm(tr("¿Eliminar el repositorio? Las apps instaladas se conservan."), tr("Eliminar"))) {
        if (st_bg.pid && st_bg.kind == J_INDEX) st_job_kill(&st_bg);
        if (st_dl_app >= 0 && st_apps[st_dl_app].repo == r) { st_job_kill(&st_dl); st_dl_app = -1; }
        char path[160]; st_cache_path(st_repos[r].url, "", ".json", path, sizeof path); unlink(path);
        for (int k = r; k < st_nrepo - 1; k++) { st_repos[k] = st_repos[k + 1]; st_session_fetch[k] = st_session_fetch[k + 1]; memcpy(st_session_url[k], st_session_url[k + 1], 256); }
        st_nrepo--; st_repos_save();
        for (int k = 0; k < st_nrepo; k++) if (st_repos[k].busy == 2) st_repos[k].busy = 1;
        st_dl_app = -1; st_apps_reload(); st_bg_next();
    }
}
static void install_progress(long done, long total, void *ctx)
{
    (void)ctx; st->shell_beat++;
    if (now_ms() - te_last_draw < 30 && done < total) return;
    progress_card(tr("Instalando…"), 0, total ? (double)done / total : 0);
}
/* paquete de la SD: hoja de acciones (instalar / actualizar / abrir / desinstalar) */
static void store_detail(int i, int *quit)
{
    struct hpk_info *p = &pkgs[i];
    char t[300]; snprintf(t, sizeof t, "%s — %s", pkg_name(p), pkg_desc(p));
    int s = pkg_state(p);
    const char *o[2] = { s == 2 ? tr("Abrir") : s == 1 ? tr("Actualizar") : tr("Instalar"), tr("Desinstalar") };
    int a = action_sheet(t, o, p->installed[0] ? 2 : 1, 1);
    if (a == 0 && s == 2) {
        char xid[HPK_ID_MAX + 4]; snprintf(xid, sizeof xid, "x.%s", p->id);
        int ai = app_by_id(xid); if (ai >= 0) { st_open_after = ai; *quit = 1; }
    } else if (a == 0) {
        char err[160] = "";
        progress_card(0, 0, -1); progress_card(tr("Instalando…"), 0, 0);
        int r = hpk_install(p->path, install_progress, 0, err, sizeof err);
        if (r) toast(hpk_msg(hpk_errkey(r), ui_lang));
        else { char id[HPK_ID_MAX + 1]; snprintf(id, sizeof id, "%s", p->id); toast(tr("Instalada")); register_installed_apps(); load_icons(); relabel_apps(); st_place_on_home(id); }
    } else if (a == 1) {
        if (confirm(tr("¿Desinstalar la app? Sus datos se conservan."), tr("Desinstalar"))) {
            char id[HPK_ID_MAX + 4]; snprintf(id, sizeof id, "x.%s", p->id);
            int ai = app_by_id(id);
            if (ai >= 0) remove_app_everywhere(ai);
            const struct hpk_info *in = st_installed(p->id);
            int r = in ? app_uninstall_any(in, 0) : hpk_uninstall(p->id);
            toast(r ? hpk_msg(hpk_errkey(r), ui_lang) : tr("Desinstalada"));
            register_installed_apps(); load_icons(); relabel_apps(); lconf_save();
        }
    }
    store_scan();
}

/* ------------------------------------------------------------------ bucle */
static float st_maxpos(void) { int vis = ST_TABY - ST_TOP; float m = (float)(st_content_h[st_detail >= 0 ? T_N : st_tab] - vis); return m < 0 ? 0 : m; }
static void store_app_ui(int ix, int iy)
{
    st_mkdirs();
    if (!st_apps) st_apps = calloc(ST_MAXAPP, sizeof *st_apps);
    if (!st_apps) { toast(tr("No se pudo abrir")); return; }
    st_pageA = img_under; st_pageB = img_sheet; have_under = 0;     /* bufers compartidos */
    sd_recover(); st_repos_load(); st_apps_reload(); store_scan(); register_installed_apps();
    st_detail = -1; st_press_kind = 0; st_press_idx = -1; st_open_after = -1;
    for (int k = 0; k <= T_N; k++) st_pos[k] = 0;
    st_vel = 0;
    st_refresh(0);
    /* abrir: la tienda crece desde su icono */
    render_launcher(img_base, now_ms());
    st_render_tab(st_pageB, 1);
    double t0 = now_ms();
    for (;;) {
        double t = (now_ms() - t0) / 300.0; if (t > 1) t = 1; double e = ease_out(t);
        int w = (int)(L.icon + (W - L.icon) * e), h = (int)(L.icon + (H - L.icon) * e);
        for (int i = 0; i < W * H; i++) fr0[i] = dim565(img_base[i], 32 - (int)(8 * e));
        draw_scaled(fr0, st_pageB, (int)(ix * (1 - e)), (int)(iy * (1 - e)), w, h, win_radius(w, h, e));
        present(); if (t >= 1) break;
    }
    struct pkt p; int quit = 0, moving = 0; double tlast = now_ms();
    st_render_current(); present();
    while (!quit) {
        int ch = st_jobs_poll();
        int anim = moving || st_dl_app >= 0 || st_busy() || st_bg.kind == J_INDEX || now_ms() < toast_until + 40;
        /* inercia y rebote */
        double tn = now_ms(), dt = tn - tlast; if (dt > 50) dt = 50; tlast = tn;
        float *pos = &st_pos[st_detail >= 0 ? T_N : st_tab], mx = st_maxpos();
        if (moving) {
            if (*pos < 0 || *pos > mx) { float tgt = *pos < 0 ? 0 : mx; *pos += (float)((tgt - *pos) * (1 - exp(-dt / 70.0))); st_vel *= 0.5f; if (fabsf(*pos - tgt) < 0.5f) { *pos = tgt; st_vel = 0; } }
            else { *pos += st_vel * (float)dt; st_vel *= (float)pow(0.998, dt); }
            if (fabsf(st_vel) < 0.02f && *pos >= 0 && *pos <= mx) moving = 0;
            ch = 1;
        }
        if (ch || anim) { st_render_current(); present(); }
        if (!read_pkt(&p, (ch || anim) ? 1 : 400)) continue;
        if (!p.down) continue;
        moving = 0; st_vel = 0;
        int sx = p.x, sy = p.y;
        if (sy >= 456) {                                         /* barra de inicio: salir (o pestana) */
            while (cur_down && read_pkt(&p, 1000)) {}
            if (sy - cur_y > 20) break;
            if (st_detail < 0) { st_tab = sx * T_N / W; }
            continue;
        }
        /* borde izquierdo en la ficha: volver arrastrando */
        if (st_detail >= 0 && sx < 22) {
            st_render_detail(st_pageB, 0);
            int keep = st_detail; st_detail = -1; st_render_tab(st_pageA, 0); st_detail = keep;
            int x = 0; double lt = now_ms(); float vx = 0; int lx = sx;
            while (cur_down && read_pkt(&p, 1000)) {
                x = cur_x - sx; if (x < 0) x = 0;
                double t = now_ms(); if (t > lt) { vx = 0.6f * vx + 0.4f * (float)((cur_x - lx) / (t - lt)); lt = t; lx = cur_x; }
                st_compose(st_pageA, st_pageB, x); present();
            }
            st_pop_from(x, x > W / 3 || vx > 0.35f);
            if (st_detail < 0) st_press_kind = 0;
            continue;
        }
        int idx = -1, kind = st_hit_at(sx, sy, &idx);
        st_press_kind = kind; st_press_idx = idx;
        if (kind && kind != HT_TAB) { st_render_current(); present(); }
        float p0 = *pos; int drag = 0; double t1 = now_ms(); float vy = 0; int ly = sy;
        double hold0 = now_ms();
        while (cur_down && read_pkt(&p, 30)) {
            if (!drag && (abs(cur_y - sy) > 8 || abs(cur_x - sx) > 12)) { drag = 1; st_press_kind = 0; st_press_idx = -1; p0 = *pos + (float)(cur_y - sy); }
            if (drag) {
                float np = p0 - (float)(cur_y - sy);
                mx = st_maxpos();
                if (np < 0) np = np * 0.5f; else if (np > mx) np = mx + (np - mx) * 0.5f;    /* resistencia */
                *pos = np;
                double t = now_ms(); if (t > t1) { vy = 0.6f * vy + 0.4f * (float)((ly - cur_y) / (t - t1)); t1 = t; ly = cur_y; }
                st_jobs_poll();
                st_render_current(); present();
            }
        }
        (void)hold0;
        if (drag) {
            if (now_ms() - t1 > 80) vy = 0;
            st_vel = vy; moving = 1; tlast = now_ms();
            if (st_detail < 0 && st_tab != T_SEARCH && *pos < -60) {    /* tirar para actualizar */
                st_refresh(1);
            }
            continue;
        }
        st_press_kind = 0; st_press_idx = -1;
        switch (kind) {
        case HT_TAB:
            if (st_detail >= 0) { st_pop_from(0, 1); }
            if (idx == st_tab) st_pos[idx] = 0;              /* tocar la pestana activa: arriba */
            st_tab = idx;
            if (st_tab == T_INST) register_installed_apps();
            break;
        case HT_BACK: if (st_detail >= 0) st_pop_from(0, 1); break;
        case HT_APP: st_push(idx); break;
        case HT_PILL: case HT_INSTPILL: st_action_pill(idx, &quit); break;
        case HT_SD: store_detail(idx, &quit); break;
        case HT_INST: installed_detail(idx); if (st_open_after >= 0) quit = 1; store_scan(); break;
        case HT_REPO: st_repo_menu(idx); break;
        case HT_ADDREPO: st_add_repo(); break;
        case HT_SEARCH: {
            char q[64]; snprintf(q, sizeof q, "%s", st_q);
            if (line_edit(tr("Buscar"), tr("Nombre o descripción"), q, sizeof q) || !q[0]) {
                snprintf(st_q, sizeof st_q, "%s", q); st_lower(st_q, st_lq, sizeof st_lq); st_pos[T_SEARCH] = 0;
            }
            break;
        }
        default: break;
        }
    }
    drain_until_up();
    if (st_dl.pid) { st_job_kill(&st_dl); st_dl_app = -1; }        /* salir cancela la descarga */
    if (st_bg.pid) { st_job_kill(&st_bg); for (int r = 0; r < st_nrepo; r++) st_repos[r].busy = 0; }
    for (int i = 0; i < st_napp; i++) if (st_apps[i].ic_state == 1 && !st_apps[i].ic.w) st_apps[i].ic_state = 0;
    sprite_free(&st_big); st_big_for[0] = 0;
    have_under = 0;
    if (st_open_after >= 0) {                                     /* ABRIR: la app sale del centro */
        run_ext_app(st_open_after, W / 2 - L.icon / 2, H / 2 - L.icon / 2);
        return;
    }
    /* cerrar: la tienda se encoge hasta su icono */
    st_detail = -1; st_render_tab(st_pageB, 0);
    memcpy(st_pageB, fr0, sizeof img_sheet);
    render_launcher(img_base, now_ms());
    double t0b = now_ms();
    for (;;) {
        double t = (now_ms() - t0b) / 280.0; if (t > 1) t = 1; double e = 1 - ease_out(t);
        int w = (int)(L.icon + (W - L.icon) * e), h = (int)(L.icon + (H - L.icon) * e);
        for (int i = 0; i < W * H; i++) fr0[i] = dim565(img_base[i], 32 - (int)(8 * e));
        draw_scaled(fr0, st_pageB, (int)(ix * (1 - e)), (int)(iy * (1 - e)), w, h, win_radius(w, h, e));
        present(); if (t >= 1) break;
    }
    launcher_present();
}
