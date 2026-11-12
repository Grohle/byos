/* hibykb.inc.c - el teclado de ByOS encima del teclado T9 de HiBy (Qobuz, Tidal, contrasenas de
 * HiBy...). HiBy dibuja su teclado por codigo (dialog/keyboard.dlg es solo el fondo, y 225-480) y
 * escribe en sus propios campos: no hay forma de pasarle texto. Asi que ByOS muestra su teclado
 * abajo, deja ver (y tocar) la pantalla de HiBy arriba, y cada tecla se convierte en las
 * pulsaciones T9 equivalentes, inyectadas por un hilo con los tiempos medidos en el aparato:
 *   - multipulsacion: 2 toques a 250 ms = 2.a letra; 700 ms quieto = letra confirmada; ">" confirma;
 *   - ABC = minusculas (siempre), Shift = mayusculas fijas (alterna), 123 = numeros, sym = simbolos;
 *   - "._@/#" (96,258) en letras; numeros con "." (288,322), "0" (160,448) y "-" (224,448);
 *   - sym: !? ()[] /\| $% @#& +-*^ '"`~ _{} :; <=> ., (ver HK_SYM).
 * El modo de HiBy se lee de la etiqueta de la tecla central (160,258): minusculas, MAYUSCULAS,
 * "2" o "()[]" tienen huellas distintas (pixeles claros arriba / en total, calibrado).
 * Desactivar: /usr/data/hibymod/no_hibykb. */

#define HK_BG 0x18E4                 /* fondo del teclado de HiBy */
static int hk_near(uint16_t p, uint16_t q)
{
    return abs((p >> 11) - (q >> 11)) <= 1 && abs(((p >> 5) & 63) - ((q >> 5) & 63)) <= 1 && abs((p & 31) - (q & 31)) <= 1;
}
static int hk_row(const uint16_t *img, int y) { int n = 0; for (int x = 0; x < W; x++) n += hk_near(img[y * W + x], HK_BG); return n; }
/* el teclado de HiBy en el fotograma: 0 falsos en 3727 fotogramas de otras pantallas */
/* Con el tema de ByOS las teclas de HiBy son invisibles: rellenas de RGB (0,0,8) = 0x0001 exacto,
 * negro a la vista pero distinto del negro puro. Se cuentan con igualdad exacta. */
#define HK_MARK 0x0001
static int hk_rowm(const uint16_t *img, int y) { int n = 0; for (int x = 0; x < W; x++) n += img[y * W + x] == HK_MARK; return n; }
static int is_hiby_kb(const uint16_t *img)
{
    if (hk_rowm(img, 258) >= W / 2 && hk_rowm(img, 322) >= W / 2 && hk_rowm(img, 386) >= W / 2 && hk_rowm(img, 215) < W / 10) return 1;
    return hk_row(img, 231) >= W * 95 / 100 && hk_row(img, 290) >= W * 95 / 100 && hk_row(img, 353) >= W * 95 / 100 &&
           hk_row(img, 415) >= W * 80 / 100 && hk_row(img, 215) < W / 2;
}
enum { HM_LOWER, HM_UPPER, HM_DIGIT, HM_SYM, HM_UNKNOWN };
static int hk_mode_of(const uint16_t *img)
{
    int top = 0, tot = 0;
    for (int y = 258 - 12; y < 258 + 12; y++) for (int x = 140; x < 180; x++) {
        uint16_t p = img[y * W + x]; int b = (p >> 11) * 8 + ((p >> 5) & 63) * 4 + (p & 31) * 8 > 400;
        tot += b; if (y < 258 - 3) top += b;
    }
    return tot < 90 ? HM_DIGIT : top >= 65 ? HM_UPPER : top >= 42 ? HM_SYM : HM_LOWER;
}
struct hk_key { short x, y; const char *chars; };
static const struct hk_key HK_LET[] = { {160, 258, "abc"}, {224, 258, "def"}, {96, 322, "ghi"}, {160, 322, "jkl"}, {224, 322, "mno"},
                                        {96, 386, "pqrs"}, {160, 386, "tuv"}, {224, 386, "wxyz"}, {96, 258, "._@/#"}, {192, 448, " "} };
static const struct hk_key HK_DIG[] = { {96, 258, "1"}, {160, 258, "2"}, {224, 258, "3"}, {96, 322, "4"}, {160, 322, "5"}, {224, 322, "6"},
                                        {96, 386, "7"}, {160, 386, "8"}, {224, 386, "9"}, {160, 448, "0"} };
/* (el "." y el "-" del modo numerico son el punto decimal y el signo negativo: solo valen una vez
 * / al principio de un numero (medido: "-5-" salia "-5"). Se escriben desde letras o simbolos.) */
static const struct hk_key HK_SYM[] = { {96, 258, "!?"}, {160, 258, "()[]"}, {224, 258, "/\\|"}, {96, 322, "$%"}, {160, 322, "@#&"},
                                        {224, 322, "+-*^"}, {288, 322, "'\"`~"}, {96, 386, "_{}"}, {160, 386, ":;"}, {224, 386, "<=>"},
                                        {160, 448, ".,"}, {224, 448, " "} };
/* donde esta c en el modo m: tecla y numero de toques (0 = no esta) */
static int hk_find(int m, char c, int *x, int *y)
{
    const struct hk_key *t; int n;
    if (m == HM_DIGIT) { t = HK_DIG; n = sizeof HK_DIG / sizeof *HK_DIG; }
    else if (m == HM_SYM) { t = HK_SYM; n = sizeof HK_SYM / sizeof *HK_SYM; }
    else { t = HK_LET; n = sizeof HK_LET / sizeof *HK_LET; }
    char lc = m == HM_UPPER && c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
    if (m == HM_LOWER && c >= 'A' && c <= 'Z') return 0;
    if (m == HM_UPPER && c >= 'a' && c <= 'z') return 0;
    for (int i = 0; i < n; i++) { const char *s = strchr(t[i].chars, lc); if (s && lc) { *x = t[i].x; *y = t[i].y; return (int)(s - t[i].chars) + 1; } }
    return 0;
}

/* ---- cola de pulsaciones (hilo) */
enum { HK_CHAR, HK_BACK, HK_ENTER, HK_RESET };
static struct { char kind, c; } hk_q[512];
static int hk_qh, hk_qt; static pthread_mutex_t hk_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t hk_cv = PTHREAD_COND_INITIALIZER;
static volatile int hk_busy, hk_run; static pthread_t hk_th;
static int hk_mode = HM_UNKNOWN, hk_lx = -1, hk_ly = -1; static double hk_lt;
static void hk_push(int kind, char c)
{
    pthread_mutex_lock(&hk_mu);
    if ((hk_qt + 1) % 512 != hk_qh) { hk_q[hk_qt].kind = (char)kind; hk_q[hk_qt].c = c; hk_qt = (hk_qt + 1) % 512; }
    pthread_cond_signal(&hk_cv); pthread_mutex_unlock(&hk_mu);
}
static void hk_tap(int x, int y) { inj_tap(x, y); hk_lt = now_ms(); }
static void hk_set_mode(int m)
{
    if (hk_mode == m) return;
    if (m == HM_DIGIT) hk_tap(32, 258);
    else if (m == HM_SYM) hk_tap(32, 386);
    else { hk_tap(32, 322); msleep(120); if (m == HM_UPPER) { hk_tap(288, 322); } }
    msleep(140);
    int seen = hk_mode_of(fr1);                    /* comprobar en su fotograma */
    if (m == HM_UPPER && seen == HM_LOWER) { hk_tap(288, 322); msleep(140); }
    if (m == HM_LOWER && seen == HM_UPPER) { hk_tap(288, 322); msleep(140); }
    hk_mode = m; hk_lx = -1;
}
/* letras sin tilde: el T9 de HiBy no las tiene */
static char hk_plain(unsigned char c0, unsigned char c1)
{
    static const char lat[65] = "AAAAAAACEEEEIIIIDNOOOOO-OUUUUYTs" "aaaaaaaceeeeiiiidnooooo-ouuuuyty";
    if (c0 == 0xC3 && c1 >= 0x80 && c1 <= 0xBF) return lat[c1 - 0x80];
    /* Latin extendido A (U+0100..017F: polaco, checo, rumano...): la letra base */
    static const char ext[129] = "AaAaAaCcCcCcCcDd" "DdEeEeEeEeEeGgGg" "GgGgHhHhIiIiIiIi" "IiIiJjKkkLlLlLlL"
                                 "lLlNnNnNnnNnOoOo" "OoOoRrRrRrSsSsSs" "SsTtTtTtUuUuUuUu" "UuUuWwYyYZzZzZzs";
    if ((c0 == 0xC4 || c0 == 0xC5) && (c1 & 0xC0) == 0x80) { int cp = ((c0 & 0x1F) << 6) | (c1 & 0x3F); if (cp >= 0x100 && cp < 0x180) return ext[cp - 0x100]; }
    return 0;
}
static void hk_type(char c)
{
    int order[4], no = 0, x = 0, y = 0;
    if (c >= 'a' && c <= 'z') order[no++] = HM_LOWER;
    else if (c >= 'A' && c <= 'Z') order[no++] = HM_UPPER;
    else if (c >= '0' && c <= '9') order[no++] = HM_DIGIT;
    else {                                         /* simbolos: mejor sin cambiar de modo */
        if (hk_mode != HM_UNKNOWN) order[no++] = hk_mode;
        int pref[] = { HM_LOWER, HM_DIGIT, HM_SYM };
        for (int i = 0; i < 3; i++) if (pref[i] != hk_mode) order[no++] = pref[i];
    }
    int n = 0, m = -1;
    for (int i = 0; i < no && !n; i++) { n = hk_find(order[i], c, &x, &y); if (n) m = order[i]; }
    if (!n) { lg("teclado de HiBy: '%c' no esta en su teclado", c); return; }
    if (hk_mode == HM_UPPER && m == HM_LOWER && !(c >= 'a' && c <= 'z')) m = HM_UPPER;   /* simbolo de letras: vale en mayusculas */
    hk_set_mode(m);
    /* misma tecla que la letra anterior sin confirmar: esperar a que se confirme */
    if (x == hk_lx && y == hk_ly) { double w = 780 - (now_ms() - hk_lt); if (w > 0) msleep((int)w); }
    for (int i = 0; i < n; i++) { hk_tap(x, y); if (i + 1 < n) msleep(110); }
    /* tecla de varios caracteres: la letra queda pendiente ~700 ms (otra pulsacion la cambiaria) */
    const struct hk_key *t = m == HM_DIGIT ? HK_DIG : m == HM_SYM ? HK_SYM : HK_LET; int nt = m == HM_DIGIT ? (int)(sizeof HK_DIG / sizeof *HK_DIG) : m == HM_SYM ? (int)(sizeof HK_SYM / sizeof *HK_SYM) : (int)(sizeof HK_LET / sizeof *HK_LET);
    int multi = 0; for (int i = 0; i < nt; i++) if (t[i].x == x && t[i].y == y) multi = strlen(t[i].chars) > 1;
    hk_lx = multi ? x : -1; hk_ly = y;
}
static void *hk_thread(void *a)
{
    (void)a;
    while (hk_run) {
        pthread_mutex_lock(&hk_mu);
        while (hk_run && hk_qh == hk_qt) { hk_busy = 0; pthread_cond_wait(&hk_cv, &hk_mu); }
        if (!hk_run) { pthread_mutex_unlock(&hk_mu); break; }
        int kind = hk_q[hk_qh].kind; char c = hk_q[hk_qh].c; hk_qh = (hk_qh + 1) % 512; hk_busy = 1;
        pthread_mutex_unlock(&hk_mu);
        if (kind == HK_RESET) { hk_mode = HM_UNKNOWN; hk_set_mode(HM_LOWER); }
        else if (kind == HK_BACK) { if (hk_lx >= 0) { double w = 780 - (now_ms() - hk_lt); if (w > 0) msleep((int)w); } hk_tap(288, 258); msleep(60); hk_lx = -1; }
        else if (kind == HK_ENTER) { if (hk_lx >= 0) { double w = 780 - (now_ms() - hk_lt); if (w > 0) msleep((int)w); } hk_tap(288, 418); hk_lx = -1; }
        else hk_type(c);
    }
    hk_busy = 0;
    return 0;
}
/* lo escrito en ByOS (antes -> ahora) en pulsaciones para HiBy: borrar hasta el prefijo comun y
 * escribir el resto (el cursor de HiBy esta al final) */
static void hk_diff(const char *o, int ol, const char *n, int nl)
{
    int p = 0; while (p < ol && p < nl && o[p] == n[p]) p++;
    while (p > 0 && ((unsigned char)n[p] & 0xC0) == 0x80) p--;      /* no partir un caracter UTF-8 */
    int back = 0; for (int i = p; i < ol; i++) if (((unsigned char)o[i] & 0xC0) != 0x80) back++;
    for (int i = 0; i < back; i++) hk_push(HK_BACK, 0);
    for (int i = p; i < nl; i++) {
        unsigned char c = (unsigned char)n[i];
        if (c < 0x80) { hk_push(HK_CHAR, (char)c); continue; }
        char pl = i + 1 < nl ? hk_plain(c, (unsigned char)n[i + 1]) : 0;
        if (pl) hk_push(HK_CHAR, pl);
        while (i + 1 < nl && ((unsigned char)n[i + 1] & 0xC0) == 0x80) i++;
    }
}

/* la pantalla: arriba la de HiBy en directo, abajo el teclado de ByOS */
static void hk_redraw(uint16_t *dst, int pressed)
{
    int top = KB_Y; if (top > H) top = H; if (top < 0) top = 0;
    int keep = top < 225 ? top : 225;
    memcpy(dst, fr1, (size_t)keep * W * 2);
    for (int i = keep * W; i < top * W; i++) dst[i] = 0;
    kb_draw(dst, pressed);
}
static int hk_session(void)
{
    if (!access("/usr/data/hibymod/no_hibykb", F_OK)) return 0;
    st->ov_on = 0; ov_cur = 0; cmd('C');
    double tc = now_ms(); while (!st->capture && now_ms() - tc < 200) msleep(2);
    if (!st->capture) return 0;
    lg("teclado de HiBy: el de ByOS encima");
    if (!hk_run) { hk_run = 1; if (pthread_create(&hk_th, 0, hk_thread, 0)) { hk_run = 0; cmd('R'); return 0; } }
    hk_mode = HM_UNKNOWN; hk_lx = -1; hk_push(HK_RESET, 0);
    static char buf[256]; buf[0] = 0;
    struct ebuf e = { buf, 0, sizeof buf, 0 };
    /* el T9 de HiBy solo escribe latin: aqui solo los teclados latinos (kb_end lo quita) */
    kb_latin_only = 1;
    kb_layer = 0; kb_shift = 0; kb_layout(tr("Intro"));
    kb_begin(&e, fr0, hk_redraw);
    uint32_t last_cap = st->cap_frames; int gone = 0, home = 0, typed = 0;
    struct pkt p;
    for (;;) {
        if (!read_pkt(&p, 60)) {
            st->shell_beat++;
            if (st->cap_frames != last_cap) {                    /* HiBy redibujo (lo escrito, su cursor) */
                last_cap = st->cap_frames;
                if (!is_hiby_kb(fr1)) { if (++gone >= 3 && !hk_busy) break; } else gone = 0;
                hk_redraw(fr0, -1); kb_present_rows(0, KB_Y > H ? H : KB_Y);
            }
            continue;
        }
        if (!p.down) continue;
        kb_map(&p);
        if (p.y >= KB_Y) {
            int k = kb_hit(p.x, p.y);
            int back_empty = k >= 0 && kbk[k].code == K_BACK && e.len == 0;
            char old[256]; int ol = e.len; memcpy(old, buf, (size_t)ol + 1);
            int r = kb_touch(&p, &e, fr0, hk_redraw, 0);
            if (back_empty) hk_push(HK_BACK, 0);
            else hk_diff(old, ol, buf, e.len);
            typed += e.len != ol || back_empty;
            if (r == K_ENTER) hk_push(HK_ENTER, 0);
            if (r == K_HOME) { home = 1; break; }
            if (e.len > 200) { e.len = e.cur = 0; buf[0] = 0; }    /* solo hace falta lo ultimo para editar */
            continue;
        }
        /* arriba: la pantalla de HiBy (campos, Cancelar, Entrar, el ojo) */
        fwd(&p); while (cur_down && read_pkt(&p, 5000)) fwd(&p);
        player_poke();
    }
    for (int i = 0; i < 500 && hk_busy; i++) msleep(10);         /* que termine de escribir */
    kb_end();
    lg("teclado de HiBy: fuera (%d cambios%s)", typed, home ? ", al inicio" : "");
    if (home) { nav_start(NAV_HOME); nav_wait(3000); if (!launcher_resume()) cmd('R'); }
    else cmd('R');
    return 1;
}
