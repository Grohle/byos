/* files.inc.c - "Archivos": explorador estilo Finder/iOS Files para 320x480.
 *  - Ubicaciones: tarjeta SD y una carpeta interna segura (/usr/data/hibymod/archivos).
 *  - Listas con icono por tipo, tamano y fecha; carpetas primero; desplazamiento con inercia.
 *  - Entrar en carpetas con la animacion de iOS; volver con "<", o deslizando desde el borde.
 *  - Pulsacion larga: Informacion, Renombrar, Mover, Eliminar. "+": nueva carpeta.
 *  - Visor de imagenes (anterior/siguiente deslizando) y visor/editor de texto con teclado.
 *  - Desde abajo: volver al inicio. */

#define FS_SD "/usr/data/mnt/sd_0"
#define FS_INT "/usr/data/hibymod/archivos"
#define ROW_H 54
#define LIST_Y 92

struct fent { char *name; int dir; long long size; time_t mtime; };
static struct fent *ents; static int nents, ecap;
static char cwd[512];                 /* "" = ubicaciones */
static float fscroll;
static char move_src[600];            /* modo mover: ruta origen ("" = no) */
static char **move_list; static int move_n;   /* mover varios (seleccion) */
/* seleccion multiple (pulsacion larga): marca por fila */
static unsigned char selmask[5000]; static int sel_mode, nsel;
static int fs_xoff;                    /* desplazamiento del contenido de fila (circulos de seleccion) */
static int bar_h(void) { return move_src[0] || sel_mode ? 60 : 0; }

static int fs_root(void) { return !cwd[0]; }
static const char *base_name(const char *p) { const char *b = strrchr(p, '/'); return b ? b + 1 : p; }
static int sd_mounted(void)
{
    FILE *f = fopen("/proc/mounts", "r"); if (!f) return 0;
    char l[512]; int ok = 0;
    while (fgets(l, sizeof l, f)) if (strstr(l, " " FS_SD " ") || strstr(l, " /data/mnt/sd_0 ")) ok = 1;
    fclose(f); return ok;
}
/* orden "natural" como Archivos de iOS: los numeros se comparan por su valor (pista 2 antes
 * que pista 10); el resto, sin distinguir mayusculas */
static int nat_cmp(const char *a, const char *b)
{
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            const char *a0 = a, *b0 = b;
            while (*a == '0') a++;
            while (*b == '0') b++;
            const char *as = a, *bs = b;
            while (isdigit((unsigned char)*a)) a++;
            while (isdigit((unsigned char)*b)) b++;
            if (a - as != b - bs) return (int)(a - as) - (int)(b - bs);   /* mas cifras = mayor */
            int c = strncmp(as, bs, (size_t)(a - as)); if (c) return c;
            if ((as - a0) != (bs - b0)) return (int)(as - a0) - (int)(bs - b0);   /* "01" despues de "1" */
            continue;
        }
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}
static int ent_cmp(const void *a, const void *b)
{
    const struct fent *x = a, *y = b;
    if (x->dir != y->dir) return y->dir - x->dir;
    int c = nat_cmp(x->name, y->name);
    return c ? c : strcmp(x->name, y->name);
}
static void fs_free(void) { for (int i = 0; i < nents; i++) free(ents[i].name); nents = 0; }
static void fs_load(void)
{
    fs_free(); fscroll = 0;
    if (fs_root()) return;
    DIR *d = opendir(cwd); if (!d) { toast(tr("No se puede abrir la carpeta")); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (nents >= ecap) { ecap = ecap ? ecap * 2 : 128; ents = realloc(ents, ecap * sizeof *ents); }
        char p[800]; snprintf(p, sizeof p, "%s/%s", cwd, e->d_name);
        struct stat s; if (stat(p, &s)) continue;
        ents[nents].name = strdup(e->d_name); ents[nents].dir = S_ISDIR(s.st_mode);
        ents[nents].size = s.st_size; ents[nents].mtime = s.st_mtime; nents++;
        if (nents >= 5000) break;
    }
    closedir(d);
    qsort(ents, nents, sizeof *ents, ent_cmp);
}

/* tipo por extension: 0 otro, 1 audio, 2 imagen, 3 texto */
static int ftype(const char *n)
{
    const char *x = strrchr(n, '.'); if (!x) return 0; x++;
    static const char *au[] = { "flac", "mp3", "wav", "m4a", "aac", "ogg", "opus", "ape", "dsf", "dff", "wv", "aiff", "aif", "alac", "iso", 0 };
    static const char *im[] = { "jpg", "jpeg", "png", "bmp", "gif", 0 };
    static const char *tx[] = { "txt", "lrc", "cue", "m3u", "m3u8", "log", "ini", "conf", "cfg", "md", "json", "csv", "nfo", "xml", "sh", 0 };
    for (int i = 0; au[i]; i++) if (!strcasecmp(x, au[i])) return 1;
    for (int i = 0; im[i]; i++) if (!strcasecmp(x, im[i])) return 2;
    for (int i = 0; tx[i]; i++) if (!strcasecmp(x, tx[i])) return 3;
    return 0;
}
static void human_size(long long s, char *o, int n)
{
    if (s < 1024) snprintf(o, n, "%lld B", s);
    else if (s < 1024 * 1024) snprintf(o, n, "%.0f KB", s / 1024.0);
    else if (s < 1024LL * 1024 * 1024) snprintf(o, n, "%.1f MB", s / 1048576.0);
    else snprintf(o, n, "%.2f GB", s / 1073741824.0);
}
static void human_date(time_t t, char *o, int n) { lang_date(t, o, n); }   /* en el idioma actual (i18n.inc.c) */

/* ------------------------------------------------------------------ iconos dibujados */
static void icon_folder(uint16_t *d, int x, int y)
{
    fill_rrect(d, x + 1, y + 6, 14, 8, 2.5f, rgb(58, 160, 245), 255);
    fill_rrect(d, x, y + 9, 32, 21, 3.5f, rgb(90, 190, 255), 255);
    fill_rrect(d, x, y + 9, 32, 2, 1, rgb(160, 220, 255), 255);
}
static void icon_file(uint16_t *d, int x, int y, int type)
{
    static const uint16_t *dummy; (void)dummy;
    uint16_t tint = type == 1 ? rgb(255, 70, 110) : type == 2 ? rgb(52, 199, 89) : type == 3 ? rgb(142, 142, 147) : rgb(120, 120, 128);
    fill_rrect(d, x + 4, y + 2, 24, 30, 3.5f, rgb(242, 242, 247), 255);
    fill_rrect(d, x + 20, y + 2, 8, 8, 2, rgb(200, 200, 206), 255);
    if (type == 1) { fill_circle(d, x + 12, y + 23, 3.2f, tint, 255); fill_rrect(d, x + 14, y + 11, 2, 12, 1, tint, 255); fill_rrect(d, x + 14, y + 11, 7, 3, 1, tint, 255); }
    else if (type == 2) { fill_circle(d, x + 12, y + 14, 2.5f, rgb(255, 204, 0), 255); fill_rrect(d, x + 8, y + 20, 16, 8, 2, tint, 255); }
    else for (int i = 0; i < 4; i++) fill_rrect(d, x + 9, y + 11 + i * 5, i == 3 ? 9 : 14, 2, 1, tint, 255);
}
static void icon_drive(uint16_t *d, int x, int y, uint16_t col)
{
    fill_rrect(d, x + 2, y + 4, 28, 24, 5, col, 255);
    fill_rrect(d, x + 7, y + 9, 18, 3, 1.5f, rgb(255, 255, 255), 200);
    fill_circle(d, x + 24, y + 21, 2, rgb(255, 255, 255), 220);
}

/* ------------------------------------------------------------------ dibujo de la lista */
static void fs_header(uint16_t *d, const char *title, int back, const char *right)
{
    memcpy(d, fr1, 30 * W * 2);
    for (int y = 30; y < LIST_Y; y++) for (int x = 0; x < W; x++) d[y * W + x] = rgb(0, 0, 0);
    if (back) {
        /* "<" fino estilo iOS + "Atras" */
        for (int i = 0; i < 9; i++) { fill_rrect(d, 12 + i, 58 - i, 3, 3, 1.2f, rgb(10, 132, 255), 255); fill_rrect(d, 12 + i, 58 + i, 3, 3, 1.2f, rgb(10, 132, 255), 255); }
        draw_text(d, 28, 64, tr("Atrás"), 16, rgb(10, 132, 255), 0, 0, H);
        int tw = tw_n(title, 17, -1); if (tw > 170) tw = 170;
        draw_text(d, (W - tw) / 2, 64, title, 17, 0xFFFF, 170, 0, H);
    } else draw_text(d, 16, 76, title, 28, 0xFFFF, 0, 0, H);
    if (right) { int w = tw_n(right, 24, -1); draw_text(d, W - 16 - w, 66, right, 24, rgb(10, 132, 255), 0, 0, H); }
    for (int x = 0; x < W; x++) d[(LIST_Y - 1) * W + x] = rgb(38, 38, 40);
}
static void fs_row(uint16_t *d, int y, int icon, int type, const char *name, const char *sub, int chevron, int sel)
{
    int y0 = LIST_Y, y1 = H - bar_h();
    if (y + ROW_H < y0 || y > y1) return;
    if (sel) for (int yy = y; yy < y + ROW_H; yy++) if (yy >= y0 && yy < y1) for (int x = 0; x < W; x++) d[yy * W + x] = rgb(44, 44, 48);
    /* el icono se dibuja recortado a la zona de la lista */
    static uint16_t ib[40 * 40];
    for (int i = 0; i < 40 * 40; i++) ib[i] = 0;
    {
        static uint16_t tmp[W * 40];
        for (int yy = 0; yy < 40; yy++) for (int x = 0; x < 48; x++) { int sy = y + 8 + yy - 4; tmp[yy * W + x] = (sy >= 0 && sy < H) ? d[sy * W + x] : 0; }
        uint16_t *t2 = tmp - 0;
        if (icon == 0) icon_folder(t2, 12, 4); else if (icon == 1) icon_file(t2, 12, 4, type);
        else icon_drive(t2, 12, 4, icon == 2 ? rgb(255, 159, 10) : rgb(94, 92, 230));
        for (int yy = 0; yy < 40; yy++) { int sy = y + 8 + yy - 4; if (sy >= y0 && sy < y1) memcpy(d + sy * W + fs_xoff, tmp + yy * W, 48 * 2); }
    }
    draw_text(d, 60 + fs_xoff, y + (sub ? 24 : 33), name, 15, 0xFFFF, (chevron ? 228 : 244) - fs_xoff, y0, y1);
    if (sub) draw_text(d, 60 + fs_xoff, y + 42, sub, 12, rgb(142, 142, 147), 244 - fs_xoff, y0, y1);
    if (chevron) for (int i = 0; i < 6; i++) {
        int cy = y + ROW_H / 2;
        for (int k = 0; k < 2; k++) { int yy = cy - 6 + i + k * (12 - 2 * i) - k; (void)yy; }
        int ya = cy - 6 + i, yb = cy + 6 - i;
        if (ya >= y0 && ya < y1) { d[ya * W + W - 22 + i] = rgb(90, 90, 96); d[ya * W + W - 21 + i] = rgb(90, 90, 96); }
        if (yb >= y0 && yb < y1) { d[yb * W + W - 22 + i] = rgb(90, 90, 96); d[yb * W + W - 21 + i] = rgb(90, 90, 96); }
    }
    int sy = y + ROW_H - 1;
    if (sy >= y0 && sy < y1) for (int x = 60 + fs_xoff; x < W; x++) d[sy * W + x] = rgb(38, 38, 40);
}
static int fs_count(void) { return fs_root() ? 2 : nents; }
static void fs_render(uint16_t *d, int sel)
{
    for (int i = LIST_Y * W; i < W * H; i++) d[i] = rgb(0, 0, 0);
    if (fs_root()) {
        fs_header(d, tr("Archivos"), 0, 0);
        draw_text(d, 16, LIST_Y + 20, tr("UBICACIONES"), 12, rgb(142, 142, 147), 0, 0, H);
        int sd = sd_mounted();
        fs_row(d, LIST_Y + 28, 2, 0, tr("Tarjeta SD"), sd ? 0 : tr("En uso por el ordenador (USB)"), sd, sel == 0);
        fs_row(d, LIST_Y + 28 + ROW_H, 3, 0, tr("En el HiBy"), 0, 1, sel == 1);
    } else {
        const char *title = !strcmp(cwd, FS_SD) ? tr("Tarjeta SD") : !strcmp(cwd, FS_INT) ? tr("En el HiBy") : base_name(cwd);
        fs_header(d, title, 1, move_src[0] || sel_mode ? 0 : "+");
        if (sel_mode) { const char *dn = tr("Listo"); draw_text(d, W - 16 - tw_n(dn, 16, -1), 64, dn, 16, rgb(10, 132, 255), 0, 0, H); }
        fs_xoff = sel_mode ? 30 : 0;
        if (!nents) draw_text(d, (W - tw_n(tr("Carpeta vacía"), 15, -1)) / 2, 220, tr("Carpeta vacía"), 15, rgb(142, 142, 147), 0, 0, H);
        int first = (int)(fscroll / ROW_H); if (first < 0) first = 0;
        for (int i = first; i < nents && i < first + (H - LIST_Y) / ROW_H + 2; i++) {
            int y = LIST_Y + i * ROW_H - (int)fscroll;
            char sub[64], a[24], b[24];
            human_date(ents[i].mtime, b, sizeof b);
            if (ents[i].dir) snprintf(sub, sizeof sub, "%s", b);
            else { human_size(ents[i].size, a, sizeof a); snprintf(sub, sizeof sub, "%s · %s", a, b); }
            fs_row(d, y, ents[i].dir ? 0 : 1, ftype(ents[i].name), ents[i].name, sub, ents[i].dir && !sel_mode, sel == i);
            if (sel_mode && y + ROW_H / 2 >= LIST_Y + 12 && y + ROW_H / 2 < H - bar_h() - 12) {   /* circulo de seleccion */
                float cx = 20, cy = y + ROW_H / 2.f;
                if (selmask[i]) {
                    fill_circle(d, cx, cy, 11, rgb(10, 132, 255), 255);
                    for (int k = 0; k < 5; k++) fill_rrect(d, (int)cx - 6 + k, (int)cy + k - 2, 2, 2, 1, 0xFFFF, 255);
                    for (int k = 0; k < 8; k++) fill_rrect(d, (int)cx - 2 + k, (int)cy + 2 - k, 2, 2, 1, 0xFFFF, 255);
                } else { fill_circle(d, cx, cy, 11, rgb(100, 100, 106), 255); fill_circle(d, cx, cy, 9.5f, rgb(0, 0, 0), 255); }
            }
        }
        fs_xoff = 0;
    }
    if (sel_mode) {   /* barra de acciones de la seleccion */
        for (int y = H - 60; y < H; y++) for (int x = 0; x < W; x++) d[y * W + x] = rgb(22, 22, 24);
        for (int x = 0; x < W; x++) d[(H - 60) * W + x] = rgb(44, 44, 48);
        char c[48]; snprintf(c, sizeof c, tr("%d seleccionados"), nsel);
        int cw = tw_n(c, 12, -1); draw_text(d, (W - cw) / 2, H - 42, c, 12, rgb(142, 142, 147), 0, 0, H);
        int audio = nsel > 0; for (int i = 0; i < nents && i < 5000; i++) if (selmask[i] && ftype(ents[i].name) != 1) audio = 0;
        const char *lb[4] = { tr("Editar"), tr("Renombrar"), tr("Mover"), tr("Eliminar") };
        int en[4] = { audio, nsel == 1, nsel > 0, nsel > 0 };
        for (int k = 0; k < 4; k++) {
            int w = tw_n(lb[k], 14, -1), x = k * 80 + (80 - w) / 2;
            draw_text(d, x, H - 14, lb[k], 14, !en[k] ? rgb(70, 70, 76) : k == 3 ? rgb(255, 69, 58) : rgb(10, 132, 255), 0, 0, H);
        }
    }
    if (move_src[0]) {
        for (int y = H - 60; y < H; y++) for (int x = 0; x < W; x++) d[y * W + x] = rgb(22, 22, 24);
        char m[80];
        if (move_n > 1) snprintf(m, sizeof m, tr("Mover %d elementos"), move_n); else snprintf(m, sizeof m, tr("Mover «%s»"), base_name(move_src));
        draw_text(d, 16, H - 38, m, 12, rgb(142, 142, 147), W - 32, 0, H);
        draw_text(d, 16, H - 14, tr("Cancelar"), 15, rgb(10, 132, 255), 0, 0, H);
        if (!fs_root()) { int w = tw_n(tr("Mover aquí"), 15, -1); fill_rrect(d, W - 28 - w, H - 34, w + 20, 28, 14, rgb(10, 132, 255), 255); draw_text(d, W - 18 - w, H - 15, tr("Mover aquí"), 15, 0xFFFF, 0, 0, H); }
    }
    draw_toast(d);
}
static void fs_present(int sel) { fs_render(fr0, sel); present(); }

/* transicion de pila estilo iOS: dir>0 entrar (la nueva desde la derecha), dir<0 volver */
static void fs_transition(const uint16_t *from, const uint16_t *to, int dir, float start)
{
    double t0 = now_ms(); int dur = 300;
    for (;;) {
        double t = (now_ms() - t0) / dur; if (t > 1) t = 1;
        float e = start + (1 - start) * (float)ease_out(t);
        int off = (int)(W * e);
        /* dir>0: "to" entra por la derecha en x = W-off; "from" se va a -off/3 oscureciendose */
        const uint16_t *top = dir > 0 ? to : from, *bot = dir > 0 ? from : to;
        int tx = dir > 0 ? W - off : off;            /* x de la pantalla de encima */
        int bx = dir > 0 ? -off / 3 : -(W - off) / 3;
        int k = dir > 0 ? 32 - (int)(10 * e) : 22 + (int)(10 * e);
        memcpy(fr0, from, 30 * W * 2);               /* barra de estado fija */
        for (int y = 30; y < H; y++) {
            uint16_t *dr = fr0 + y * W; const uint16_t *tr = top + y * W, *br = bot + y * W;
            for (int x = 0; x < W; x++) {
                if (x >= tx) dr[x] = tr[x - tx];
                else { int sx = x - bx; dr[x] = sx >= 0 && sx < W ? dim565(br[sx], k) : 0; if (x >= tx - 6) dr[x] = dim565(dr[x], 24 + (x - tx + 6)); }
            }
        }
        present();
        if (t >= 1) break;
    }
}
static void fs_go(const char *path, int dir)
{
    fs_render(img_base, -1);
    if (path) snprintf(cwd, sizeof cwd, "%s", path); else {
        char *s = strrchr(cwd, '/');
        if (!strcmp(cwd, FS_SD) || !strcmp(cwd, FS_INT) || !s) cwd[0] = 0; else *s = 0;
    }
    fs_load();
    fs_render(img_app, -1);
    fs_transition(img_base, img_app, dir, 0);
    fs_present(-1);
}

/* ------------------------------------------------------------------ hojas de acciones */
static int action_sheet(const char *title, const char **opts, int n, int destructive_idx)
{
    static uint16_t base[W * H]; memcpy(base, fr0, sizeof base);
    int bh = 46, sh = 40 + n * bh + 12 + bh + 16, y0 = H - sh;
    for (int s = 0; s <= 6; s++) {
        float e = (float)ease_out(s / 6.0); int yy = y0 + (int)((1 - e) * sh);
        dim_buf(fr0, base, W * H, 32 - (int)(14 * e));
        fill_rrect(fr0, 8, yy, W - 16, 40 + n * bh, 13, rgb(44, 44, 48), 250);
        int tw = tw_n(title, 12, -1); if (tw > W - 40) tw = W - 40;
        draw_text(fr0, (W - tw) / 2, yy + 25, title, 12, rgb(142, 142, 147), W - 40, 0, H);
        for (int i = 0; i < n; i++) {
            int by = yy + 40 + i * bh;
            for (int x = 8; x < W - 8; x++) if (by < H) fr0[by * W + x] = rgb(64, 64, 68);
            int w = tw_n(opts[i], 17, -1);
            draw_text(fr0, (W - w) / 2, by + 29, opts[i], 17, i == destructive_idx ? rgb(255, 69, 58) : rgb(10, 132, 255), 0, 0, H);
        }
        int cy = yy + 40 + n * bh + 12;
        fill_rrect(fr0, 8, cy, W - 16, bh, 13, rgb(44, 44, 48), 255);
        int w = tw_n(tr("Cancelar"), 17, -1);
        draw_text(fr0, (W - w) / 2, cy + 29, tr("Cancelar"), 17, rgb(10, 132, 255), 0, 0, H);
        present();
    }
    struct pkt p;
    /* solo cuenta un toque NUEVO: descartar lo que quede del que abrio la hoja */
    while (read_pkt(&p, 0)) {}
    while (cur_down && read_pkt(&p, 3000)) {}
    for (;;) {
        if (!read_pkt(&p, 300) || !p.down) continue;
        int y = p.y; while (cur_down && read_pkt(&p, 3000)) {}
        if (y >= y0 + 40 && y < y0 + 40 + n * bh) return (y - y0 - 40) / bh;
        return -1;
    }
}
static int confirm(const char *title, const char *button)
{
    const char *o[1] = { button };
    return action_sheet(title, o, 1, 0) == 0;
}
static int rm_rf(const char *p)
{
    struct stat s; if (lstat(p, &s)) return -1;
    if (S_ISDIR(s.st_mode)) {
        DIR *d = opendir(p); if (!d) return -1;
        struct dirent *e; int r = 0;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char c[900]; snprintf(c, sizeof c, "%s/%s", p, e->d_name);
            r |= rm_rf(c); st->shell_beat++;
        }
        closedir(d);
        return r | rmdir(p);
    }
    return unlink(p);
}

/* Mover entre la SD y la memoria interna: rename() no cruza sistemas de ficheros (EXDEV).
 * Se copia (64 KB a la vez, con suma de cada fichero), se fuerza a disco, se vuelve a leer la
 * copia y se compara la suma; solo si todo cuadra se borra el original. Con progreso. */
struct fs_mv { long long done, tot; double last; const char *name; int err; };
static long long fs_du(const char *p)
{
    struct stat s; if (lstat(p, &s)) return 0;
    if (!S_ISDIR(s.st_mode)) return s.st_size;
    long long t = 0; DIR *d = opendir(p); struct dirent *e;
    while (d && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char c[900]; snprintf(c, sizeof c, "%s/%s", p, e->d_name); t += fs_du(c);
    }
    if (d) closedir(d);
    return t;
}
static void fs_mv_progress(struct fs_mv *m)
{
    double t = now_ms(); if (t - m->last < 120 && m->done < m->tot) return;
    m->last = t; st->shell_beat++;
    progress_card(tr("Moviendo…"), m->name, m->tot > 0 ? (double)m->done / m->tot : 0);
}
static uint32_t fs_sum(uint32_t a, const uint8_t *b, ssize_t n)   /* Adler-32 */
{
    uint32_t s1 = a & 0xFFFF, s2 = a >> 16;
    for (ssize_t i = 0; i < n; i++) { s1 = (s1 + b[i]) % 65521; s2 = (s2 + s1) % 65521; }
    return s2 << 16 | s1;
}
static int fs_copy_file(const char *src, const char *dst, mode_t mode, struct fs_mv *m)
{
    static uint8_t *buf; if (!buf) buf = malloc(65536);
    if (!buf) return -1;
    int fi = open(src, O_RDONLY); if (fi < 0) return -1;
    int fo = open(dst, O_WRONLY | O_CREAT | O_EXCL, mode & 0777); if (fo < 0) { close(fi); return -1; }
    uint32_t a = 1; ssize_t n; int bad = 0;
    while ((n = read(fi, buf, 65536)) > 0) {
        if (write(fo, buf, n) != n) { bad = 1; break; }
        a = fs_sum(a, buf, n); m->done += n; fs_mv_progress(m);
    }
    if (n < 0) bad = 1;
    close(fi);
    if (fsync(fo)) bad = 1;
    if (close(fo)) bad = 1;
    if (!bad) {                                       /* verificar: releer la copia */
        uint32_t b = 1; int fv = open(dst, O_RDONLY);
        if (fv < 0) bad = 1; else { while ((n = read(fv, buf, 65536)) > 0) b = fs_sum(b, buf, n); if (n < 0) bad = 1; close(fv); }
        if (b != a) bad = 1;
    }
    if (bad) unlink(dst);
    return bad ? -1 : 0;
}
static int fs_copy_tree(const char *src, const char *dst, struct fs_mv *m)
{
    struct stat s; if (lstat(src, &s)) return -1;
    if (S_ISLNK(s.st_mode)) return -1;                 /* (FAT de la SD: no hay enlaces) */
    if (!S_ISDIR(s.st_mode)) return fs_copy_file(src, dst, s.st_mode, m);
    if (mkdir(dst, 0755) && errno != EEXIST) return -1;
    DIR *d = opendir(src); if (!d) return -1;
    struct dirent *e; int r = 0;
    while (!r && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char a[900], b[900]; snprintf(a, sizeof a, "%s/%s", src, e->d_name); snprintf(b, sizeof b, "%s/%s", dst, e->d_name);
        r = fs_copy_tree(a, b, m);
    }
    closedir(d);
    return r;
}
/* 0 movido; -1 no se pudo (el original queda intacto; una copia a medias se borra) */
static int fs_move(const char *src, const char *dst)
{
    if (!rename(src, dst)) return 0;
    if (errno != EXDEV) return -1;
    struct fs_mv m = { 0, fs_du(src), 0, base_name(src), 0 };
    struct statvfs v;
    { char dir[900]; snprintf(dir, sizeof dir, "%s", dst); char *sl = strrchr(dir, '/'); if (sl) *sl = 0;
      if (!statvfs(dir, &v) && (long long)v.f_bavail * v.f_frsize < m.tot + (1 << 20)) { toast(tr("No hay espacio suficiente")); return -1; } }
    fs_mv_progress(&m);
    if (fs_copy_tree(src, dst, &m)) { rm_rf(dst); return -1; }
    return rm_rf(src) ? -1 : 0;   /* copia completa y verificada: ya se puede borrar el original */
}

/* ------------------------------------------------------------------ visor de imagenes */
static void image_viewer(int idx)
{
    uint16_t *img = img_under;                                  /* reutilizado */
    for (;;) {
        char path[800]; snprintf(path, sizeof path, "%s/%s", cwd, ents[idx].name);
        for (int i = 0; i < W * H; i++) fr0[i] = 0;
        draw_text(fr0, (W - tw_n(tr("Cargando…"), 14, -1)) / 2, 240, tr("Cargando…"), 14, rgb(142, 142, 147), 0, 0, H);
        present();
        char *argv[] = { ASSETS "/hibywall", path, "/tmp/hibymod/view.raw", "320", "480", "fit", 0 };
        pid_t pid; int ok = 0, stt;
        if (!posix_spawn(&pid, argv[0], 0, 0, argv, environ)) {
            while (waitpid(pid, &stt, WNOHANG) == 0) { msleep(20); st->shell_beat++; }
            ok = WIFEXITED(stt) && WEXITSTATUS(stt) == 0;
        }
        int f = ok ? open("/tmp/hibymod/view.raw", O_RDONLY) : -1;
        if (f >= 0) { read(f, img, W * H * 2); close(f); unlink("/tmp/hibymod/view.raw"); }
        else { for (int i = 0; i < W * H; i++) img[i] = 0; }
        int chrome = 1;
        for (;;) {
            memcpy(fr0, img, W * H * 2);
            if (!ok) draw_text(fr0, (W - tw_n(tr("No se pudo abrir la imagen"), 14, -1)) / 2, 240, tr("No se pudo abrir la imagen"), 14, rgb(142, 142, 147), 0, 0, H);
            if (chrome) {
                for (int y = 0; y < 30; y++) for (int x = 0; x < W; x++) fr0[y * W + x] = fr1[y * W + x];
                for (int y = 30; y < 74; y++) for (int x = 0; x < W; x++) fr0[y * W + x] = dim565(fr0[y * W + x], 10);
                for (int i = 0; i < 9; i++) { fill_rrect(fr0, 12 + i, 52 - i, 3, 3, 1.2f, rgb(10, 132, 255), 255); fill_rrect(fr0, 12 + i, 52 + i, 3, 3, 1.2f, rgb(10, 132, 255), 255); }
                draw_text(fr0, 30, 58, ents[idx].name, 14, 0xFFFF, W - 46, 0, H);
            }
            present();
            struct pkt p;
            if (!read_pkt(&p, 300) || !p.down) continue;
            int sx = p.x, sy = p.y;
            while (cur_down && read_pkt(&p, 3000)) {}
            int dx = cur_x - sx;
            if (abs(dx) > 50) {   /* imagen anterior / siguiente */
                int n = idx;
                do { n += dx < 0 ? 1 : -1; } while (n >= 0 && n < nents && ftype(ents[n].name) != 2);
                if (n >= 0 && n < nents) { idx = n; break; }
                continue;
            }
            if (sy >= 456 || (chrome && sy < 74 && sx < 60)) return;
            chrome = !chrome;
        }
    }
}

/* ------------------------------------------------------------------ visor / editor de texto */
#define TXT_CAP (96 * 1024)
static struct ebuf tx; static int tx_edit, tx_ro, tx_top;   /* tx_top: primera linea visible */
static const char *tx_name;
static int tx_lines[8192], tx_nl;                           /* inicio de cada linea visual */
static void tx_layout(int maxw)
{
    tx_nl = 0; int i = 0;
    while (i <= tx.len && tx_nl < 8192) {
        tx_lines[tx_nl++] = i;
        int w = 0, last_sp = -1, j = i;
        while (j < tx.len && tx.b[j] != '\n') {
            const char *s = tx.b + j; int c = utf8_next(&s);
            struct glyph *g = glyph_get(c, 14); int gw = g ? g->adv : 7 << 6;   /* 26.6, como draw_text */
            if ((w + gw) >> 6 > maxw) break;                                     /* (antes: >>6 por glifo, las lineas largas se salian) */
            if (c == ' ') last_sp = (int)(s - tx.b);
            w += gw; j = (int)(s - tx.b);
        }
        if (j >= tx.len) break;
        if (tx.b[j] == '\n') i = j + 1;
        else i = last_sp > i ? last_sp : j;
    }
}
static int tx_line_of(int pos) { int l = 0; for (int k = 0; k < tx_nl; k++) if (tx_lines[k] <= pos) l = k; return l; }
static void tx_redraw(uint16_t *d, int pressed)
{
    int bottom = tx_edit ? KB_Y : H;
    for (int i = 0; i < W * H; i++) d[i] = rgb(0, 0, 0);
    memcpy(d, fr1, 30 * W * 2);
    for (int i = 0; i < 9; i++) { fill_rrect(d, 12 + i, 58 - i, 3, 3, 1.2f, rgb(10, 132, 255), 255); fill_rrect(d, 12 + i, 58 + i, 3, 3, 1.2f, rgb(10, 132, 255), 255); }
    int tw = tw_n(tx_name, 15, -1); if (tw > 160) tw = 160;
    draw_text(d, (W - tw) / 2, 64, tx_name, 15, 0xFFFF, 160, 0, H);
    const char *rb = tx_ro ? tr("Solo lectura") : tx_edit ? tr("Guardar") : tr("Editar");
    draw_text(d, W - 14 - tw_n(rb, 15, -1), 64, rb, 15, tx_ro ? rgb(142, 142, 147) : rgb(10, 132, 255), 0, 0, H);
    for (int x = 0; x < W; x++) d[85 * W + x] = rgb(38, 38, 40);
    tx_layout(W - 24);
    int lh = 20, vis = (bottom - 92) / lh;
    if (tx_edit) { int cl = tx_line_of(tx.cur); if (cl < tx_top) tx_top = cl; if (cl >= tx_top + vis) tx_top = cl - vis + 1; }
    if (tx_top > tx_nl - 1) tx_top = tx_nl - 1; if (tx_top < 0) tx_top = 0;
    char line[1024];
    for (int l = tx_top; l < tx_nl && l < tx_top + vis + 1; l++) {
        int a = tx_lines[l], b = l + 1 < tx_nl ? tx_lines[l + 1] : tx.len;
        int n = b - a; if (n > (int)sizeof line - 1) n = sizeof line - 1;
        memcpy(line, tx.b + a, n); line[n] = 0;
        for (int k = 0; k < n; k++) if (line[k] == '\n' || line[k] == '\r' || line[k] == '\t') line[k] = line[k] == '\t' ? ' ' : 0;
        int base = 92 + (l - tx_top) * lh + 15;
        draw_text(d, 12, base, line, 14, rgb(230, 230, 235), 0, 88, bottom);
        if (tx_edit && tx.cur >= a && (tx.cur < b || (l == tx_nl - 1)) ) {
            int cx = 12 + tw_n(tx.b + a, 14, tx.cur - a);
            for (int y = base - 14; y < base + 4; y++) if (y >= 88 && y < bottom) for (int x = cx; x < cx + 2 && x < W; x++) d[y * W + x] = rgb(10, 132, 255);
            kb_caret(cx, base - 14, base + 4);
        }
    }
    if (tx_edit) kb_draw(d, pressed);
    draw_toast(d);
}
/* trackpad del teclado: linea visual arriba/abajo con la misma x */
static int tx_vmove(int cur, int dir)
{
    int l = tx_line_of(cur), nl = l + dir;
    if (nl < 0 || nl >= tx_nl) return cur;
    int x = tw_n(tx.b + tx_lines[l], 14, cur - tx_lines[l]);
    int a = tx_lines[nl], b = nl + 1 < tx_nl ? tx_lines[nl + 1] : tx.len, best = a, bd = 1 << 30;
    for (int i = a; i <= b; i++) {
        if (i < tx.len && ((unsigned char)tx.b[i] & 0xC0) == 0x80) continue;
        int dd = abs(tw_n(tx.b + a, 14, i - a) - x); if (dd < bd) { bd = dd; best = i; }
    }
    if (best > a && best == b && b < tx.len && tx.b[b - 1] == '\n') best = b - 1;
    return best;
}
static int fs_gone;          /* salio con el gesto de inicio: el inicio ya esta en pantalla */
static void text_editor(const char *path, const char *name)
{
    static char *buf; if (!buf) buf = malloc(TXT_CAP);
    if (!buf) { toast(tr("No se pudo abrir")); return; }
    int f = open(path, O_RDONLY);
    int n = f >= 0 ? (int)read(f, buf, TXT_CAP - 1) : 0; if (n < 0) n = 0;
    struct stat s; tx_ro = f < 0 || (fstat(f, &s) == 0 && s.st_size > TXT_CAP / 2);
    if (f >= 0) close(f);
    buf[n] = 0;
    tx = (struct ebuf){ buf, n, TXT_CAP / 2 + 1024, 0 }; if (tx_ro) tx.cap = n + 1;
    tx_name = name; tx_edit = 0; tx_top = 0;
    kb_layer = 0; kb_shift = 0; kb_layout(tr("intro"));
    int dirty = 0;
    tx_redraw(fr0, -1); present();
    struct pkt p;
    for (;;) {
        if (!read_pkt(&p, 300) || !p.down) continue;
        kb_map(&p);
        int hx = p.x, hy = p.y - kb_pg, home = 0;
        if (tx_edit && p.y >= KB_Y) {
            int ol = tx.len, r = kb_touch(&p, &tx, fr0, tx_redraw, 1); if (tx.len != ol) dirty = 1;
            if (r != K_HOME) continue;
            home = 1;                                     /* inicio con el teclado abierto */
        } else if (p.y >= 456) {                          /* desde abajo: inicio (como en la lista) */
            double tw = now_ms();
            while (cur_down && !home && now_ms() - tw < 400) { read_pkt(&p, 20); if (hy - cur_y > 12) home = 1; }
        }
        if (home) {
            if (dirty && !confirm(tr("Hay cambios sin guardar"), tr("Descartar cambios"))) { drain_until_up(); tx_redraw(fr0, -1); present(); continue; }
            tx_redraw(fr0, -1);
            if (own_home_gesture(hx, hy, app_by_id("files"))) { if (tx_edit) kb_end(); fs_gone = 1; return; }
            tx_redraw(fr0, -1); present(); continue;
        }
        int sx = p.x, sy = p.y, top0 = tx_top, moved = 0;
        while (cur_down && read_pkt(&p, 40)) {
            if (abs(cur_y - sy) > 8) moved = 1;
            if (moved && sy > 86) { tx_top = top0 - (cur_y - sy) / 20; if (tx_top < 0) tx_top = 0; if (tx_top > tx_nl - 1) tx_top = tx_nl - 1; tx_redraw(fr0, -1); present(); }
        }
        if (moved) continue;
        if (sy < 86 && sx < 90) {   /* atras */
            if (dirty && !confirm(tr("Hay cambios sin guardar"), tr("Descartar cambios"))) { tx_redraw(fr0, -1); present(); continue; }
            if (tx_edit) kb_end();
            return;
        }
        if (sy < 86 && sx > W - 110 && !tx_ro) {
            if (!tx_edit) { tx_edit = 1; tx.cur = tx.len; kb_scroll_area(88); kb_set_vmove(tx_vmove); kb_begin(&tx, fr0, tx_redraw); }
            else {   /* guardar: fichero temporal y renombrado (nunca se queda a medias) */
                char tmp[820]; snprintf(tmp, sizeof tmp, "%s.hibymod_tmp", path);
                int o = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (o >= 0 && write(o, tx.b, tx.len) == tx.len && !fsync(o) && !close(o) && !rename(tmp, path)) { dirty = 0; toast(tr("Guardado")); }
                else { if (o >= 0) close(o); unlink(tmp); toast(tr("No se pudo guardar")); }
                kb_ctx(&tx, fr0, tx_redraw); kb_hide(); kb_end();   /* el teclado se va deslizandose */
                tx_edit = 0;
            }
            tx_redraw(fr0, -1); present(); continue;
        }
        if (tx_edit && sy > 86 && sy < KB_Y) {   /* colocar el cursor */
            int l = tx_top + (sy - 92) / 20; if (l >= tx_nl) l = tx_nl - 1; if (l < 0) l = 0;
            int a = tx_lines[l], b = l + 1 < tx_nl ? tx_lines[l + 1] : tx.len, best = a, bd = 1 << 30;
            for (int i = a; i <= b; i++) {
                if (i < tx.len && ((unsigned char)tx.b[i] & 0xC0) == 0x80) continue;
                int d = abs(12 + tw_n(tx.b + a, 14, i - a) - sx); if (d < bd) { bd = d; best = i; }
            }
            if (best > a && best == b && b < tx.len && tx.b[b - 1] == '\n') best = b - 1;
            tx.cur = best; if (kb_off) kb_reveal(fr0, tx_redraw); else { tx_redraw(fr0, -1); present(); }
        }
    }
}

/* ------------------------------------------------------------------ app */
static void file_info(const char *path, const struct fent *e)
{
    char sz[32], dt[32], t[200];
    human_size(e->size, sz, sizeof sz); human_date(e->mtime, dt, sizeof dt);
    if (e->dir) snprintf(t, sizeof t, tr("Carpeta · modificada el %s"), dt);
    else snprintf(t, sizeof t, tr("%s · modificado el %s"), sz, dt);
    const char *o[1] = { t };
    (void)path;
    action_sheet(e->name, o, 1, -1);
}

static void files_app(int ix, int iy)
{
    have_under = 0;   /* Archivos reutiliza img_under */
    cwd[0] = 0; move_src[0] = 0; fs_load();
    mkdir(FS_INT, 0755);
    /* apertura con zoom desde el icono */
    render_launcher(img_base, now_ms());
    fs_render(img_app, -1);
    double t0 = now_ms();
    for (;;) {
        double t = (now_ms() - t0) / 300; if (t > 1) t = 1;
        double e = ease_out(t);
        zoom_frame_app(fr0, img_base, 32 - (int)(8 * e), img_app, (int)(ix * (1 - e)), (int)(iy * (1 - e)), (int)(L.icon + (W - L.icon) * e), (int)(L.icon + (H - L.icon) * e), win_radius((int)(L.icon + (W - L.icon) * e), (int)(L.icon + (H - L.icon) * e), e));
        present();
        if (t >= 1) break;
    }
    fs_present(-1);
    float vel = 0;
    struct pkt p;
    for (;;) {
        /* inercia */
        if (fabsf(vel) > 0.02f) {
            static double lt; double now = now_ms(); if (!lt || now - lt > 100) lt = now - 16;
            fscroll -= vel * (float)(now - lt); vel *= powf(0.9975f, (float)(now - lt)); lt = now;   /* deceleracion comun (Musica, Ajustes) */
            float maxs = fs_count() * ROW_H - (H - LIST_Y - (move_src[0] ? 60 : 0)); if (maxs < 0) maxs = 0;
            if (fscroll < 0) { fscroll = 0; vel = 0; } if (fscroll > maxs) { fscroll = maxs; vel = 0; }
            fs_present(-1);
            if (!read_pkt(&p, 0)) continue;
            vel = 0;
        } else if (!read_pkt(&p, now_ms() < toast_until + 40 ? 30 : 300)) { if (now_ms() < toast_until + 40) fs_present(-1); continue; }
        if (!p.down) continue;
        int sx = p.x, sy = p.y; double ts = now_ms();
        /* desde abajo: inicio (la ventana sigue al dedo; mantener 1 s: multitarea) */
        if (sy >= 456) {
            int up = 0; double tw = now_ms();
            while (cur_down && !up && now_ms() - tw < 400) { read_pkt(&p, 20); if (sy - cur_y > 12) up = 1; }
            if (up) { fs_render(fr0, -1); if (own_home_gesture(sx, sy, app_by_id("files"))) { fs_gone = 1; break; } fs_present(-1); continue; }
            while (cur_down && read_pkt(&p, 1000)) {}
            if (!bar_h()) continue;          /* con barra de acciones, un toque es para sus botones */
        }
        /* desde un borde: volver (burbuja, como en el resto del sistema) */
        if (!fs_root() && back_zone(sx, sy)) {
            int bk = app_back_gesture(sx, sy);
            if (bk > 0) { sel_mode = 0; nsel = 0; memset(selmask, 0, sizeof selmask); fs_go(0, -1); continue; }
            if (bk < 0) continue;
        }
        if (0) {
            uint16_t *cur_img = img_under, *prev_img = img_sheet;   /* reutilizados */
            fs_render(cur_img, -1);
            char saved[512]; snprintf(saved, sizeof saved, "%s", cwd);
            float sv = fscroll;
            char *sl = strrchr(cwd, '/');
            if (!strcmp(cwd, FS_SD) || !strcmp(cwd, FS_INT) || !sl) cwd[0] = 0; else *sl = 0;
            fs_load(); fs_render(prev_img, -1);
            int lastx = sx; double lt = now_ms(); float v = 0;
            while (cur_down) {
                if (read_pkt(&p, 20) && !p.down) break;
                double tn = now_ms(); if (tn > lt) v = 0.6f * v + 0.4f * (cur_x - lastx) / (float)(tn - lt); lastx = cur_x; lt = tn;
                int off = cur_x - sx; if (off < 0) off = 0;
                memcpy(fr0, cur_img, 30 * W * 2);
                for (int y = 30; y < H; y++) for (int x = 0; x < W; x++) {
                    int bx = x + (W - off) / 3;
                    fr0[y * W + x] = x >= off ? cur_img[y * W + x - off] : (bx < W ? dim565(prev_img[y * W + bx], 22 + 10 * off / W) : 0);
                }
                present();
            }
            int off = cur_x - sx;
            if (off > 90 || v > 0.4f) { fs_transition(cur_img, prev_img, -1, off / (float)W); fs_present(-1); }
            else { snprintf(cwd, sizeof cwd, "%s", saved); fs_load(); fscroll = sv; fs_present(-1); }
            continue;
        }
        /* cabecera */
        if (sy < LIST_Y) {
            while (cur_down && read_pkt(&p, 3000)) {}
            if (sel_mode && sx > W - 80) { sel_mode = 0; nsel = 0; memset(selmask, 0, sizeof selmask); fs_present(-1); continue; }
            if (!fs_root() && sx < 100) { sel_mode = 0; nsel = 0; memset(selmask, 0, sizeof selmask); fs_go(0, -1); continue; }
            if (!fs_root() && sx > W - 60 && !move_src[0]) {
                char name[256] = "";
                if (line_edit(tr("Nueva carpeta"), tr("Nombre"), name, sizeof name)) {
                    char np[800]; snprintf(np, sizeof np, "%s/%s", cwd, name);
                    if (mkdir(np, 0755)) toast(tr("No se pudo crear")); else { fs_load(); toast(tr("Carpeta creada")); }
                }
                fs_present(-1);
            }
            continue;
        }
        /* barra de mover */
        if (move_src[0] && sy >= H - 60) {
            while (cur_down && read_pkt(&p, 3000)) {}
            if (sx < 120) { move_src[0] = 0; for (int i = 0; i < move_n; i++) free(move_list[i]); move_n = 0; fs_present(-1); continue; }
            if (!fs_root()) {
                int nlist = move_n ? move_n : 1, bad = 0, ok = 0;
                for (int i = 0; i < nlist; i++) {
                    const char *src = move_n ? move_list[i] : move_src;
                    char dst[900]; snprintf(dst, sizeof dst, "%s/%s", cwd, base_name(src));
                    if (!strncmp(cwd, src, strlen(src)) && (cwd[strlen(src)] == '/' || !cwd[strlen(src)])) { bad++; continue; }
                    if (!access(dst, F_OK) || fs_move(src, dst)) bad++; else ok++;
                }
                if (!bad) toast(tr("Movido")); else toast(nlist == 1 ? tr("No se pudo mover") : tr("Algunos no se pudieron mover"));
                if (ok || !bad) { move_src[0] = 0; for (int i = 0; i < move_n; i++) free(move_list[i]); move_n = 0; }
                fs_load(); fs_present(-1);
            }
            continue;
        }
        if (sel_mode && sy >= H - 60) {
            while (cur_down && read_pkt(&p, 3000)) {}
            int k = sx / 80;
            int audio = nsel > 0; for (int i = 0; i < nents && i < 5000; i++) if (selmask[i] && ftype(ents[i].name) != 1) audio = 0;
            int first = -1; for (int i = 0; i < nents && i < 5000; i++) if (selmask[i]) { first = i; break; }
            if (k == 0 && audio) {
                char **pp = calloc(nsel, sizeof(char *)); int n = 0;
                for (int i = 0; i < nents && i < 5000; i++) if (selmask[i]) { char b[900]; snprintf(b, sizeof b, "%s/%s", cwd, ents[i].name); pp[n++] = strdup(b); }
                tag_editor(pp, n);
                for (int i = 0; i < n; i++) free(pp[i]); free(pp);
                sel_mode = 0; nsel = 0; memset(selmask, 0, sizeof selmask); fs_load();
            } else if (k == 1 && nsel == 1 && first >= 0) {
                struct fent *e = &ents[first]; char path[900]; snprintf(path, sizeof path, "%s/%s", cwd, e->name);
                char name[256]; snprintf(name, sizeof name, "%s", e->name);
                if (line_edit(tr("Renombrar"), tr("Nombre"), name, sizeof name) && strcmp(name, e->name)) {
                    char np[900]; snprintf(np, sizeof np, "%s/%s", cwd, name);
                    if (!access(np, F_OK)) toast(tr("Ya existe uno con ese nombre"));
                    else if (rename(path, np)) toast(tr("No se pudo renombrar")); else toast(tr("Renombrado"));
                }
                sel_mode = 0; nsel = 0; memset(selmask, 0, sizeof selmask); fs_load();
            } else if (k == 2 && nsel > 0) {
                move_list = realloc(move_list, nsel * sizeof(char *)); move_n = 0;
                for (int i = 0; i < nents && i < 5000; i++) if (selmask[i]) { char b[900]; snprintf(b, sizeof b, "%s/%s", cwd, ents[i].name); move_list[move_n++] = strdup(b); }
                snprintf(move_src, sizeof move_src, "%s", move_list[0]);
                sel_mode = 0; nsel = 0; memset(selmask, 0, sizeof selmask);
                toast(tr("Elige la carpeta de destino"));
            } else if (k == 3 && nsel > 0) {
                char q[200]; snprintf(q, sizeof q, tr("¿Eliminar %d elementos? No se puede deshacer."), nsel);
                if (nsel == 1 && first >= 0) snprintf(q, sizeof q, tr("¿Eliminar «%s»? No se puede deshacer."), ents[first].name);
                if (confirm(q, tr("Eliminar"))) {
                    progress_card(0, 0, -1);
                    int done = 0, bad = 0;
                    for (int i = 0; i < nents && i < 5000; i++) if (selmask[i]) {
                        char b[900]; snprintf(b, sizeof b, "%s/%s", cwd, ents[i].name);
                        char sub[48]; snprintf(sub, sizeof sub, tr("%d de %d"), done + 1, nsel);
                        progress_card(tr("Eliminando…"), sub, (double)done / nsel);
                        if (rm_rf(b)) bad++;
                        done++;
                    }
                    toast(bad ? tr("No se pudo eliminar del todo") : tr("Eliminado"));
                    sel_mode = 0; nsel = 0; memset(selmask, 0, sizeof selmask); fs_load();
                }
            }
            fs_present(-1);
            continue;
        }
        /* lista: tocar, pulsacion larga o arrastrar */
        int row = fs_root() ? (sy - LIST_Y - 28) / ROW_H : (int)((sy - LIST_Y + fscroll) / ROW_H);
        int valid = row >= 0 && row < fs_count() && !(fs_root() && sy < LIST_Y + 28);
        float s0 = fscroll; int moved = 0, lastyy = sy; double lt = now_ms(); vel = 0;
        if (valid) fs_present(row);
        int longp = 0;
        while (cur_down) {
            int got = read_pkt(&p, 30);
            if (got && !p.down) break;
            double tn = now_ms();
            if (abs(cur_y - sy) > 8) moved = 1;
            if (moved && !fs_root()) {
                if (tn > lt) vel = 0.6f * vel + 0.4f * (cur_y - lastyy) / (float)(tn - lt);
                lastyy = cur_y; lt = tn;
                float maxs = nents * ROW_H - (H - LIST_Y - bar_h()); if (maxs < 0) maxs = 0;
                float prev = fscroll;
                fscroll = s0 - (cur_y - sy); if (fscroll < 0) fscroll = 0; if (fscroll > maxs) fscroll = maxs;
                if (fscroll != prev) fs_present(-1);          /* lista corta o en el tope: nada que pintar */
            }
            if (!moved && valid && !fs_root() && !move_src[0] && !sel_mode && tn - ts > 450) { longp = 1; break; }
        }
        if (moved) { if (fs_root() || now_ms() - lt > 60) vel = 0; continue; }   /* el dedo se paro antes de soltar: sin inercia (como Ajustes) */
        vel = 0;
        if (!valid) { fs_present(-1); continue; }
        if (fs_root()) {
            if (row == 0 && !sd_mounted()) { toast(tr("La SD está conectada al ordenador")); fs_present(-1); continue; }
            fs_go(row == 0 ? FS_SD : FS_INT, 1); continue;
        }
        struct fent *e = &ents[row];
        char path[900]; snprintf(path, sizeof path, "%s/%s", cwd, e->name);
        if (sel_mode || (longp && !move_src[0])) {   /* seleccion multiple (como en Fotos de iOS) */
            while (cur_down && read_pkt(&p, 3000)) {}
            if (!sel_mode) { sel_mode = 1; nsel = 0; memset(selmask, 0, sizeof selmask); }
            if (row < 5000) { selmask[row] = !selmask[row]; nsel += selmask[row] ? 1 : -1; }
            fs_present(-1);
            continue;
        }
        if (longp) {
            while (cur_down && read_pkt(&p, 3000)) {}   /* soltar antes de la hoja */
            const char *o[4] = { tr("Información"), tr("Renombrar"), tr("Mover"), tr("Eliminar") };
            int a = action_sheet(e->name, o, 4, 3);
            if (a == 0) file_info(path, e);
            else if (a == 1) {
                char name[256]; snprintf(name, sizeof name, "%s", e->name);
                if (line_edit(tr("Renombrar"), tr("Nombre"), name, sizeof name) && strcmp(name, e->name)) {
                    char np[900]; snprintf(np, sizeof np, "%s/%s", cwd, name);
                    if (!access(np, F_OK)) toast(tr("Ya existe uno con ese nombre"));
                    else if (rename(path, np)) toast(tr("No se pudo renombrar")); else { fs_load(); toast(tr("Renombrado")); }
                }
            } else if (a == 2) { snprintf(move_src, sizeof move_src, "%s", path); toast(tr("Elige la carpeta de destino")); }
            else if (a == 3) {
                char t[300]; snprintf(t, sizeof t, tr("¿Eliminar «%s»? No se puede deshacer."), e->name);
                if (confirm(t, e->dir ? tr("Eliminar carpeta") : tr("Eliminar"))) {
                    toast(tr("Eliminando…")); fs_present(-1);
                    if (rm_rf(path)) toast(tr("No se pudo eliminar del todo")); else toast(tr("Eliminado"));
                    fs_load();
                }
            }
            fs_present(-1);
            continue;
        }
        if (e->dir) { fs_go(path, 1); continue; }
        int ty = ftype(e->name);
        if (ty == 2) { image_viewer(row); fs_present(-1); }
        else if (ty == 3) { text_editor(path, e->name); if (fs_gone) break; fs_load(); fs_present(-1); }
        else if (ty == 1) { toast(tr("Para reproducir, usa Música › Carpetas")); fs_present(-1); }
        else { file_info(path, e); fs_present(-1); }
    }
    /* salir: la ventana se encoge hacia su icono (si no lo hizo ya el gesto de inicio) */
    if (!fs_gone) { fs_render(img_app, -1); render_launcher(img_base, now_ms()); }
    t0 = now_ms();
    if (!fs_gone) for (;;) {
        double t = (now_ms() - t0) / 280; if (t > 1) t = 1;
        double e = 1 - ease_out(t);
        zoom_frame_app(fr0, img_base, 32 - (int)(8 * e), img_app, (int)(ix * (1 - e)), (int)(iy * (1 - e)), (int)(L.icon + (W - L.icon) * e), (int)(L.icon + (H - L.icon) * e), win_radius((int)(L.icon + (W - L.icon) * e), (int)(L.icon + (H - L.icon) * e), e));
        present();
        if (t >= 1) break;
    }
    fs_gone = 0;
    fs_free();
    drain_until_up();
    launcher_present();
}
