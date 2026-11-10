/* qr.inc.c - codificador de codigos QR (ISO/IEC 18004), escrito para HiByOS.
 *
 * Modo byte, nivel de correccion M, versiones 1 a 10 (hasta 213 bytes: de sobra para una URL).
 * qr_encode() elige la version mas pequena que cabe y la mascara de menor penalizacion (o la
 * que se le pida: lo usa tests/qr_test.c para comparar modulo a modulo con otro codificador).
 * Sin memoria dinamica; unos 4 KB de pila. */
#define QR_MAXV 10
#define QR_MAXN (17 + 4 * QR_MAXV)
struct qr { int n, ver, mask; uint8_t m[QR_MAXN][QR_MAXN]; };   /* m[y][x]: 1 = modulo oscuro */

/* nivel M: codigos de datos totales, de correccion por bloque, bloques cortos y largos (+1) */
static const struct { uint16_t data; uint8_t ec, g1, g2; } QR_M[QR_MAXV + 1] = {
    { 0, 0, 0, 0 }, { 16, 10, 1, 0 }, { 28, 16, 1, 0 }, { 44, 26, 1, 0 }, { 64, 18, 2, 0 }, { 86, 24, 2, 0 },
    { 108, 16, 4, 0 }, { 124, 18, 4, 0 }, { 154, 22, 2, 2 }, { 182, 22, 3, 2 }, { 216, 26, 4, 1 },
};
static const uint8_t QR_ALIGN[QR_MAXV + 1][3] = {
    { 0 }, { 0 }, { 6, 18 }, { 6, 22 }, { 6, 26 }, { 6, 30 }, { 6, 34 }, { 6, 22, 38 }, { 6, 24, 42 }, { 6, 26, 46 }, { 6, 28, 50 },
};

static uint8_t qr_exp[512], qr_log[256];
static void qr_gf_init(void)
{
    if (qr_exp[1]) return;
    int x = 1;
    for (int i = 0; i < 255; i++) { qr_exp[i] = (uint8_t)x; qr_log[x] = (uint8_t)i; x <<= 1; if (x & 0x100) x ^= 0x11D; }
    for (int i = 255; i < 512; i++) qr_exp[i] = qr_exp[i - 255];
}
static uint8_t qr_mul(uint8_t a, uint8_t b) { return a && b ? qr_exp[qr_log[a] + qr_log[b]] : 0; }
/* Reed-Solomon: resto de dividir los datos por el polinomio generador de grado ec */
static void qr_rs(const uint8_t *d, int n, int ec, uint8_t *out)
{
    uint8_t g[32] = { 0 };                      /* coeficientes sin el termino principal */
    g[ec - 1] = 1;
    for (int i = 0, root = 1; i < ec; i++) {
        for (int j = 0; j < ec; j++) { g[j] = qr_mul(g[j], (uint8_t)root); if (j + 1 < ec) g[j] ^= g[j + 1]; }
        root = qr_mul((uint8_t)root, 2);
    }
    memset(out, 0, ec);
    for (int i = 0; i < n; i++) {
        uint8_t f = d[i] ^ out[0];
        memmove(out, out + 1, ec - 1); out[ec - 1] = 0;
        for (int j = 0; j < ec; j++) out[j] ^= qr_mul(g[j], f);
    }
}

static void qr_set(struct qr *q, uint8_t fn[QR_MAXN][QR_MAXN], int x, int y, int dark)
{
    if (x < 0 || y < 0 || x >= q->n || y >= q->n) return;
    q->m[y][x] = (uint8_t)(dark != 0); fn[y][x] = 1;
}
static void qr_format(struct qr *q, uint8_t fn[QR_MAXN][QR_MAXN], int mask)
{
    int data = mask, rem = data, n = q->n;      /* nivel M = 00 */
    for (int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    int bits = ((data << 10) | (rem & 0x3FF)) ^ 0x5412;
#define QB(i) ((bits >> (i)) & 1)
    for (int i = 0; i <= 5; i++) qr_set(q, fn, 8, i, QB(i));
    qr_set(q, fn, 8, 7, QB(6)); qr_set(q, fn, 8, 8, QB(7)); qr_set(q, fn, 7, 8, QB(8));
    for (int i = 9; i < 15; i++) qr_set(q, fn, 14 - i, 8, QB(i));
    for (int i = 0; i < 8; i++) qr_set(q, fn, n - 1 - i, 8, QB(i));
    for (int i = 8; i < 15; i++) qr_set(q, fn, 8, n - 15 + i, QB(i));
#undef QB
    qr_set(q, fn, 8, n - 8, 1);
}
static void qr_function(struct qr *q, uint8_t fn[QR_MAXN][QR_MAXN])
{
    int n = q->n;
    for (int i = 0; i < n; i++) { qr_set(q, fn, 6, i, i % 2 == 0); qr_set(q, fn, i, 6, i % 2 == 0); }
    static const int fx[3] = { 3, -4, 3 }, fy[3] = { 3, 3, -4 };
    for (int k = 0; k < 3; k++) {               /* buscadores con su separador */
        int cx = fx[k] < 0 ? n + fx[k] : fx[k], cy = fy[k] < 0 ? n + fy[k] : fy[k];
        for (int dy = -4; dy <= 4; dy++) for (int dx = -4; dx <= 4; dx++) {
            int dist = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
            qr_set(q, fn, cx + dx, cy + dy, dist != 2 && dist != 4);
        }
    }
    const uint8_t *al = QR_ALIGN[q->ver]; int na = al[2] ? 3 : al[0] ? 2 : 0;
    for (int i = 0; i < na; i++) for (int j = 0; j < na; j++) {
        if ((i == 0 && j == 0) || (i == 0 && j == na - 1) || (i == na - 1 && j == 0)) continue;
        for (int dy = -2; dy <= 2; dy++) for (int dx = -2; dx <= 2; dx++)
            qr_set(q, fn, al[i] + dx, al[j] + dy, (abs(dx) > abs(dy) ? abs(dx) : abs(dy)) != 1);
    }
    qr_format(q, fn, 0);                         /* reserva la zona (se reescribe con la mascara) */
    if (q->ver >= 7) {
        int rem = q->ver;
        for (int i = 0; i < 12; i++) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
        long bits = ((long)q->ver << 12) | (rem & 0xFFF);
        for (int i = 0; i < 18; i++) {
            int b = (int)((bits >> i) & 1), a = n - 11 + i % 3, c = i / 3;
            qr_set(q, fn, a, c, b); qr_set(q, fn, c, a, b);
        }
    }
}
static int qr_maskbit(int k, int x, int y)
{
    switch (k) {
    case 0: return (x + y) % 2 == 0;
    case 1: return y % 2 == 0;
    case 2: return x % 3 == 0;
    case 3: return (x + y) % 3 == 0;
    case 4: return (x / 3 + y / 2) % 2 == 0;
    case 5: return x * y % 2 + x * y % 3 == 0;
    case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
    default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
    }
}
static void qr_apply(struct qr *q, uint8_t fn[QR_MAXN][QR_MAXN], int k)
{
    for (int y = 0; y < q->n; y++) for (int x = 0; x < q->n; x++) if (!fn[y][x] && qr_maskbit(k, x, y)) q->m[y][x] ^= 1;
}
static int qr_penalty(const struct qr *q)
{
    int n = q->n, pen = 0, dark = 0;
    for (int dir = 0; dir < 2; dir++) for (int a = 0; a < n; a++) {
        int run = 0, col = -1; unsigned hist = 0;
        for (int b = 0; b < n; b++) {
            int v = dir ? q->m[b][a] : q->m[a][b];
            if (v == col) { if (++run == 5) pen += 3; else if (run > 5) pen++; } else { col = v; run = 1; }
            hist = ((hist << 1) | (unsigned)v) & 0x7FF;                  /* 1011101 con 4 claros a un lado */
            if (b >= 10 && (hist == 0x5D0 || hist == 0x05D)) pen += 40;
        }
    }
    for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) {
        int v = q->m[y][x]; dark += v;
        if (x + 1 < n && y + 1 < n && v == q->m[y][x + 1] && v == q->m[y + 1][x] && v == q->m[y + 1][x + 1]) pen += 3;
    }
    int total = n * n, d = dark * 20 - total * 10; if (d < 0) d = -d;
    int k = (d + total - 1) / total - 1; if (k < 0) k = 0;
    return pen + k * 10;
}
/* codifica len bytes; mask 0..7 la fuerza, -1 elige la mejor. Devuelve 0 si no cabe */
static int qr_encode(const void *src, int len, struct qr *q, int mask)
{
    qr_gf_init();
    int ver = 0;
    for (int v = 1; v <= QR_MAXV && !ver; v++) if (len <= QR_M[v].data - (v < 10 ? 2 : 3)) ver = v;
    if (!ver || len < 0) return 0;
    memset(q, 0, sizeof *q); q->ver = ver; q->n = 17 + 4 * ver;
    int nd = QR_M[ver].data, ec = QR_M[ver].ec, g1 = QR_M[ver].g1, g2 = QR_M[ver].g2, nb = g1 + g2, d1 = (nd - g2) / nb;
    /* flujo de bits: modo byte, longitud, datos, terminador y relleno */
    uint8_t data[216] = { 0 }; int bit = 0;
#define PUT(v, nbits) for (int i_ = (nbits) - 1; i_ >= 0; i_--, bit++) if (((v) >> i_) & 1) data[bit >> 3] |= (uint8_t)(0x80 >> (bit & 7))
    PUT(4, 4); PUT(len, ver < 10 ? 8 : 16);
    for (int i = 0; i < len; i++) PUT(((const uint8_t *)src)[i], 8);
#undef PUT
    bit += nd * 8 - bit < 4 ? nd * 8 - bit : 4;
    int used = (bit + 7) / 8;
    for (int i = used, p = 0; i < nd; i++, p ^= 1) data[i] = p ? 0x11 : 0xEC;
    /* bloques, correccion e intercalado */
    uint8_t ecw[5][32], all[346]; int off[5], dl[5], tot = 0;
    for (int b = 0, o = 0; b < nb; b++) { dl[b] = d1 + (b >= g1); off[b] = o; qr_rs(data + o, dl[b], ec, ecw[b]); o += dl[b]; }
    for (int i = 0; i < d1 + 1; i++) for (int b = 0; b < nb; b++) if (i < dl[b]) all[tot++] = data[off[b] + i];
    for (int i = 0; i < ec; i++) for (int b = 0; b < nb; b++) all[tot++] = ecw[b][i];
    /* patrones fijos y colocacion en zigzag */
    static uint8_t fn[QR_MAXN][QR_MAXN];
    memset(fn, 0, sizeof fn);
    qr_function(q, fn);
    int n = q->n, i = 0;
    for (int right = n - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int vert = 0; vert < n; vert++) for (int j = 0; j < 2; j++) {
            int x = right - j, up = ((right + 1) & 2) == 0, y = up ? n - 1 - vert : vert;
            if (fn[y][x] || i >= tot * 8) continue;
            q->m[y][x] = (all[i >> 3] >> (7 - (i & 7))) & 1; i++;
        }
    }
    if (mask < 0) {
        int best = 1 << 30;
        for (int k = 0; k < 8; k++) {
            qr_apply(q, fn, k); qr_format(q, fn, k);
            int p = qr_penalty(q); if (p < best) { best = p; mask = k; }
            qr_apply(q, fn, k);
        }
    }
    qr_apply(q, fn, mask & 7); qr_format(q, fn, mask & 7); q->mask = mask & 7;
    return 1;
}
