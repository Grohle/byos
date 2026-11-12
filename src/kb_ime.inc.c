/* kb_ime.inc.c - lo que el teclado escribe, sin pantalla ni tactil (se prueba en el PC: tests/kb_test.c).
 *  - Distribuciones de los 13 idiomas de la interfaz (el id de cada una es su LANG_*): filas, capa de
 *    mayusculas propia (coreano, tailandes), variantes de la pulsacion larga, capas 123 y #+=.
 *  - Motor de edicion (ebuf): insertar, borrar, mover el cursor (sin quedarse entre una consonante
 *    tailandesa y su marca).
 *  - Hangul de 2 conjuntos (Dubeolsik): la silaba se compone dentro del propio texto, como en iOS, y
 *    el borrado deshace pulsacion a pulsacion (닭 + ㅏ -> 달가, borrar -> 닭).
 *  - Japones: romaji -> kana (Hepburn y kunrei) en una barra de candidatos (hiragana, katakana).
 *  - Chino: pinyin sin tonos -> caracteres (simplificado o tradicional), un caracter cada vez; las
 *    tablas las genera tools/gen_ime_tables.py (ime_zh.inc.c, unos 34 KB).
 * La preedicion (letras pinyin, kana sin confirmar) no entra en el texto: las busquedas en vivo solo
 * ven lo confirmado. Nada reserva memoria al escribir. */

/* ------------------------------------------------------------------ caracteres */
static void put_utf8(char *o, int c)
{
    if (c < 0x80) { o[0] = (char)c; o[1] = 0; }
    else if (c < 0x800) { o[0] = (char)(0xC0 | c >> 6); o[1] = (char)(0x80 | (c & 63)); o[2] = 0; }
    else { o[0] = (char)(0xE0 | c >> 12); o[1] = (char)(0x80 | ((c >> 6) & 63)); o[2] = (char)(0x80 | (c & 63)); o[3] = 0; }
}
/* mayuscula: latin-1, latin extendido A (polaco...) y cirilico (ruso y ucraniano); ß no tiene */
static int up_case(int c)
{
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
    if (c == 0xFF) return 0x178;
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) return c & 1 ? c - 1 : c;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return c & 1 ? c : c - 1;
    if (c >= 0x430 && c <= 0x44F) return c - 0x20;
    if (c >= 0x450 && c <= 0x45F) return c - 0x50;
    if (c == 0x491) return 0x490;
    return c;
}

/* ------------------------------------------------------------------ distribuciones */
enum {
    KL_CASE = 1,        /* mayusculas con up_case (bloqueo con doble toque) */
    KL_AUTO = 2,        /* mayuscula automatica al empezar frase y doble espacio -> ". " */
    KL_LATIN = 4,       /* solo letras latinas (vale para los campos que no admiten otras) */
    KL_HANGUL = 8,      /* jamo que se componen en silabas */
    KL_KANA = 16,       /* romaji -> kana con barra de candidatos */
    KL_PINYIN = 32,     /* pinyin -> caracteres con barra de candidatos */
    KL_HANT = 64,       /* ... en chino tradicional */
};
struct kb_lay {
    const char *code, *name;        /* codigo (el de LANG_CODES) y nombre propio */
    const char *abc, *space;        /* tecla para volver a las letras desde 123; barra espaciadora */
    unsigned char nrows;            /* filas de letras: 3 (o 4, tailandes) */
    const char *rows[4], *srows[4]; /* letras y su capa de mayusculas propia (0: up_case) */
    const char *alts;               /* pulsacion larga: "a:äàá|o:ö..." (como iOS para ese idioma) */
    const char *cur, *cur3;         /* moneda de la capa 123 y las otras tres de la #+= */
    const char *n2, *n3, *s1, *s2, *s3;   /* filas propias de 123 (2a y 3a) y #+= (0: las de siempre) */
    unsigned short flags;
};
#define KB_NLAY LANG_N
#define KL_LAT (KL_CASE | KL_AUTO | KL_LATIN)
#define KB_QWERTY { "qwertyuiop", "asdfghjkl", "zxcvbnm" }
static const struct kb_lay KB_LAYS[KB_NLAY] = {
    [LANG_ES] = { "es", "Español", "ABC", "espacio", 3, { "qwertyuiop", "asdfghjklñ", "zxcvbnm" }, { 0 },
                  "a:áàäâãåæª|e:éèëêęėē|i:íïìîįī|o:óòöôõøœº|u:úüùûū", "€", "$£¥", 0, 0, 0, 0, 0, KL_LAT },
    [LANG_EN] = { "en", "English", "ABC", "space", 3, KB_QWERTY, { 0 },
                  "a:àáâäæãåā|e:èéêëēėę|i:îïíīįì|o:ôöòóœøōõ|u:ûüùúū", "$", "€£¥", 0, 0, 0, 0, 0, KL_LAT },
    [LANG_FR] = { "fr", "Français", "ABC", "espace", 3, { "azertyuiop", "qsdfghjklm", "wxcvbn'" }, { 0 },
                  "a:àâæáäãåā|e:éèêëēėę|i:îïìíįī|o:ôœöòóõøō|u:ùûüúū", "€", "$£¥", 0, 0, 0, 0, 0, KL_LAT },
    [LANG_DE] = { "de", "Deutsch", "ABC", "Leerzeichen", 3, { "qwertzuiopü", "asdfghjklöä", "yxcvbnm" }, { 0 },
                  "a:äàáâæãåā|o:öôòóœøōõ|u:üûùúū|s:ßśš|e:éèêëėē", "€", "$£¥", 0, 0, 0, 0, 0, KL_LAT },
    [LANG_IT] = { "it", "Italiano", "ABC", "spazio", 3, KB_QWERTY, { 0 },
                  "e:èéêëēėę|a:àáâäæãåā|i:ìíîïīį|o:òóôöõœøō|u:ùúûüū", "€", "$£¥", 0, 0, 0, 0, 0, KL_LAT },
    [LANG_JA] = { "ja", "日本語", "あいう", "空白", 3, KB_QWERTY, { 0 }, 0, "¥", "$€£",
                  "ー/：；（）¥＆＠「」", "。、？！・", "［］｛｝＃％＾＊＋＝", "＿＼｜～〈〉＄€£•", "…。、？！", KL_CASE | KL_KANA },
    [LANG_KO] = { "ko", "한국어", "한글", "스페이스", 3, { "ㅂㅈㄷㄱㅅㅛㅕㅑㅐㅔ", "ㅁㄴㅇㄹㅎㅗㅓㅏㅣ", "ㅋㅌㅊㅍㅠㅜㅡ" },
                  { "ㅃㅉㄸㄲㅆㅛㅕㅑㅒㅖ", "ㅁㄴㅇㄹㅎㅗㅓㅏㅣ", "ㅋㅌㅊㅍㅠㅜㅡ" }, 0, "₩", "$€£", 0, 0, 0, 0, 0, KL_HANGUL },
    [LANG_PL] = { "pl", "Polski", "ABC", "spacja", 3, KB_QWERTY, { 0 },
                  "a:ąàáâäæãå|c:ćçč|e:ęèéêëēė|l:ł|n:ńñ|o:óòôöõœøō|s:śšß|z:żźž", "€", "$£¥", 0, 0, 0, 0, 0, KL_LAT },
    [LANG_RU] = { "ru", "Русский", "АБВ", "Пробел", 3, { "йцукенгшщзх", "фывапролджэ", "ячсмитьбю" }, { 0 },
                  "е:ё|ь:ъ|х:ъ", "€", "$£¥", 0, 0, 0, 0, 0, KL_CASE | KL_AUTO },
    [LANG_ZHS] = { "zh-Hans", "简体拼音", "拼音", "空格", 3, KB_QWERTY, { 0 }, 0, "¥", "$€£",
                   "-/：；（）¥@“”", "。，、？！'", "【】｛｝#%^*+=", "_\\|~《》$€£·", "…。，？！", KL_CASE | KL_PINYIN },
    [LANG_TH] = { "th", "ไทย", "กขค", "เว้นวรรค", 4, { "ๅ/-ภถุึคตจขช", "ๆไำพะัีรนยบล", "ฟหกดเ้่าสวง", "ผปแอิืทมใฝ" },
                  { "+๑๒๓๔ู฿๕๖๗๘๙", "๐\"ฎฑธํ๊ณฯญฐ,", "ฤฆฏโฌ็๋ษศซ.", "()ฉฮฺ์?ฒฬฦ" }, "ข:ฃ|ค:ฅ", "฿", "$€£", 0, 0, 0, 0, 0, 0 },
    [LANG_ZHT] = { "zh-Hant", "繁體拼音", "拼音", "空格", 3, KB_QWERTY, { 0 }, 0, "$", "¥€£",
                   "-/：；（）$@「」", "。，、？！'", "【】｛｝#%^*+=", "_\\|~《》¥€£·", "…。，？！", KL_CASE | KL_PINYIN | KL_HANT },
    [LANG_UK] = { "uk", "Українська", "АБВ", "пробіл", 3, { "йцукенгшщзхї", "фівапролджє", "ячсмитьбю" }, { 0 },
                  "г:ґ|ь:'ʼ|е:є|і:ї", "₴", "$€£", 0, 0, 0, 0, 0, KL_CASE | KL_AUTO },
};
/* orden en listas y menus (el de Ajustes > Idioma) */
static const unsigned char KB_ORD[KB_NLAY] = { LANG_EN, LANG_ES, LANG_FR, LANG_DE, LANG_IT, LANG_PL, LANG_RU,
                                               LANG_UK, LANG_JA, LANG_KO, LANG_ZHS, LANG_ZHT, LANG_TH };
static const struct kb_lay *kb_lay(int l) { return &KB_LAYS[l >= 0 && l < KB_NLAY ? l : LANG_EN]; }
/* ancho de tecla y hueco de una fila de n teclas en avail px (con 11-12 teclas, mas juntas, como iOS) */
static void kb_row_geom(int n, int avail, int maxkw, int *kw, int *gap)
{
    *gap = n >= 11 ? 3 : 4;
    *kw = n > 0 ? (avail - (n - 1) * *gap) / n : 0; if (*kw > maxkw) *kw = maxkw;
}
/* ancho de mayusculas y borrar en la ultima fila de letras, segun las letras que lleva */
static int kb_side_w(int n) { return n >= 10 ? 34 : n >= 9 ? 36 : 42; }
/* variantes de la pulsacion larga de c en la distribucion l: puntero al principio (acaban en '|' o 0) */
static const char *kb_lay_alts(int l, int c)
{
    const char *s = kb_lay(l)->alts;
    while (s && *s) {
        const char *p = s; int k = utf8_next(&p);
        if (*p == ':' && k == c) return p + 1;
        s = strchr(s, '|'); if (s) s++;
    }
    /* comunes a todas (signos y latinas sin variante propia en esa distribucion) */
    static const char *const SH[][2] = {
        { "a", "àáâäæãåā" }, { "e", "èéêëēėę" }, { "i", "îïíīįì" }, { "o", "ôöòóœøōõ" }, { "u", "ûüùúū" },
        { "c", "çćč" }, { "n", "ñń" }, { "s", "ßśš" }, { "y", "ÿ" }, { "z", "žźż" }, { "l", "ł" },
        { "?", "¿" }, { "!", "¡" }, { "-", "–—•" }, { "/", "\\" }, { "$", "€£¥¢" }, { "€", "$£¥¢" },
        { "&", "§" }, { ".", "…" }, { "'", "‘’`" }, { "\"", "“”„«»" }, { "0", "°" }, { "%", "‰" },
    };
    for (unsigned i = 0; i < sizeof SH / sizeof SH[0]; i++) { const char *p = SH[i][0]; if (utf8_next(&p) == c) return SH[i][1]; }
    return 0;
}
/* filas de una capa: r[0..n) de arriba abajo; devuelve cuantas. layer 0 letras, 1 123, 2 #+=;
 * shift: capa de mayusculas propia (coreano, tailandes). numrow: fila de numeros encima (solo con 3
 * filas: el tailandes ya tiene 4, y en sus capas 123 y #+= se pone siempre para no cambiar de alto). */
static int kb_lay_rows(int l, int layer, int shift, int numrow, const char **r)
{
    const struct kb_lay *k = kb_lay(l);
    static char n2[48], s2[48];
    int n = 0, four = k->nrows == 4;
    if (layer == 0) {
        if (numrow && !four) r[n++] = "1234567890";
        for (int i = 0; i < k->nrows; i++) r[n++] = shift && k->srows[i] ? k->srows[i] : k->rows[i];
        return n;
    }
    snprintf(n2, sizeof n2, "-/:;()%s&@\"", k->cur);
    snprintf(s2, sizeof s2, "_\\|~<>%s•", k->cur3);
    const char *num = "1234567890", *sym = k->s1 ? k->s1 : "[]{}#%^*+=";
    if (layer == 1) {
        if (numrow || four) r[n++] = sym;
        r[n++] = num; r[n++] = k->n2 ? k->n2 : n2; r[n++] = k->n3 ? k->n3 : ".,?!'";
    } else {
        if (numrow || four) r[n++] = num;
        r[n++] = sym; r[n++] = k->s2 ? k->s2 : s2; r[n++] = k->s3 ? k->s3 : ".,?!'";
    }
    return n;
}

/* ------------------------------------------------------------------ motor de edicion */
struct ebuf { char *b; int len, cap, cur; };
static void eb_insert(struct ebuf *e, const char *s)
{
    int n = (int)strlen(s);
    if (e->len + n >= e->cap) return;
    memmove(e->b + e->cur + n, e->b + e->cur, e->len - e->cur + 1);
    memcpy(e->b + e->cur, s, n); e->len += n; e->cur += n;
}
static void eb_back(struct ebuf *e)
{
    if (e->cur <= 0) return;
    int p = e->cur - 1;
    while (p > 0 && ((unsigned char)e->b[p] & 0xC0) == 0x80) p--;
    memmove(e->b + p, e->b + e->cur, e->len - e->cur + 1);
    e->len -= e->cur - p; e->cur = p;
}
static void eb_back_word(struct ebuf *e)
{
    while (e->cur > 0 && (e->b[e->cur - 1] == ' ' || e->b[e->cur - 1] == '\n')) eb_back(e);
    while (e->cur > 0 && e->b[e->cur - 1] != ' ' && e->b[e->cur - 1] != '\n') eb_back(e);
}
static int eb_cp_at(const struct ebuf *e, int p) { const char *s = e->b + p; return p < e->len ? utf8_next(&s) : 0; }
/* n caracteres a derecha (+) o izquierda (-); nunca entre una letra y su marca (tailandes, acentos) */
static void eb_move(struct ebuf *e, int n)
{
    for (; n > 0 && e->cur < e->len; n--) {
        do { e->cur++; while (e->cur < e->len && ((unsigned char)e->b[e->cur] & 0xC0) == 0x80) e->cur++; }
        while (e->cur < e->len && text_is_mark(eb_cp_at(e, e->cur)));
    }
    for (; n < 0 && e->cur > 0; n++) {
        do { e->cur--; while (e->cur > 0 && ((unsigned char)e->b[e->cur] & 0xC0) == 0x80) e->cur--; }
        while (e->cur > 0 && text_is_mark(eb_cp_at(e, e->cur)));
    }
}
/* cambia los old bytes desde start por s y deja el cursor detras; 0 si no cabe */
static int eb_replace(struct ebuf *e, int start, int old, const char *s)
{
    int n = (int)strlen(s);
    if (start < 0 || start + old > e->len || e->len - old + n >= e->cap) return 0;
    memmove(e->b + start + n, e->b + start + old, e->len - start - old + 1);
    memcpy(e->b + start, s, n); e->len += n - old; e->cur = start + n;
    return 1;
}

/* ------------------------------------------------------------------ hangul (2 conjuntos) */
/* jamo de compatibilidad (U+3131..U+3163, los de las teclas) -> indice de inicial y de final */
static const signed char HG_CHO[30] = { 0, 1, -1, 2, -1, -1, 3, 4, 5, -1, -1, -1, -1, -1, -1, -1, 6, 7, 8, -1, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18 };
static const signed char HG_JONG[30] = { 1, 2, 3, 4, 5, 6, 7, 0, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 0, 18, 19, 20, 21, 22, 0, 23, 24, 25, 26, 27 };
/* finales dobles (ㄳ ㄵ ㄶ ㄺ ㄻ ㄼ ㄽ ㄾ ㄿ ㅀ ㅄ) y vocales dobles (ㅘ ㅙ ㅚ ㅝ ㅞ ㅟ ㅢ): a, b -> ab */
static const unsigned short HG_TT[11][3] = {
    { 0x3131, 0x3145, 0x3133 }, { 0x3134, 0x3148, 0x3135 }, { 0x3134, 0x314E, 0x3136 }, { 0x3139, 0x3131, 0x313A },
    { 0x3139, 0x3141, 0x313B }, { 0x3139, 0x3142, 0x313C }, { 0x3139, 0x3145, 0x313D }, { 0x3139, 0x314C, 0x313E },
    { 0x3139, 0x314D, 0x313F }, { 0x3139, 0x314E, 0x3140 }, { 0x3142, 0x3145, 0x3144 } };
static const unsigned short HG_VV[7][3] = {
    { 0x3157, 0x314F, 0x3158 }, { 0x3157, 0x3150, 0x3159 }, { 0x3157, 0x3163, 0x315A }, { 0x315C, 0x3153, 0x315D },
    { 0x315C, 0x3154, 0x315E }, { 0x315C, 0x3163, 0x315F }, { 0x3161, 0x3163, 0x3162 } };
static int hg_pair(const unsigned short (*t)[3], int n, int a, int b) { for (int i = 0; i < n; i++) if (t[i][0] == a && t[i][1] == b) return t[i][2]; return 0; }
static int hg_is_jamo(int c) { return c >= 0x3131 && c <= 0x3163; }
static int hg_is_vowel(int c) { return c >= 0x314F && c <= 0x3163; }
struct hg_syl { unsigned short l, v, t0, t1; int k0; };   /* inicial, vocal, final (1 o 2 jamo), 1a pulsacion */
static int hg_cp(const struct hg_syl *s)
{
    if (s->l && s->v) {
        int t = s->t0 ? (s->t1 ? hg_pair(HG_TT, 11, s->t0, s->t1) : s->t0) : 0;
        return 0xAC00 + (HG_CHO[s->l - 0x3131] * 21 + (s->v - 0x314F)) * 28 + (t ? HG_JONG[t - 0x3131] : 0);
    }
    return s->l ? s->l : s->v;
}
/* automata: pulsaciones -> silabas (todas menos la ultima ya no cambian al seguir escribiendo) */
static int hg_run(const unsigned short *ks, int n, struct hg_syl *sy, int maxs)
{
    int ns = 0, have = 0; struct hg_syl c = { 0 };
#define HG_EMIT() do { if (ns < maxs) sy[ns++] = c; } while (0)
    for (int i = 0; i < n; i++) {
        int k = ks[i];
        if (hg_is_vowel(k)) {
            int vv;
            if (!have) { c = (struct hg_syl){ 0, (unsigned short)k, 0, 0, i }; have = 1; }
            else if (c.t0) {                        /* la final pasa a la silaba nueva: 닭 + ㅏ -> 달가 */
                int nl;
                if (c.t1) { nl = c.t1; c.t1 = 0; } else { nl = c.t0; c.t0 = 0; }
                HG_EMIT(); c = (struct hg_syl){ (unsigned short)nl, (unsigned short)k, 0, 0, i - 1 };
            }
            else if (!c.v) c.v = (unsigned short)k;
            else if ((vv = hg_pair(HG_VV, 7, c.v, k))) c.v = (unsigned short)vv;
            else { HG_EMIT(); c = (struct hg_syl){ 0, (unsigned short)k, 0, 0, i }; }
        } else {
            if (!have) { c = (struct hg_syl){ (unsigned short)k, 0, 0, 0, i }; have = 1; }
            else if (!c.v || !c.l) { HG_EMIT(); c = (struct hg_syl){ (unsigned short)k, 0, 0, 0, i }; }
            else if (!c.t0 && HG_JONG[k - 0x3131]) c.t0 = (unsigned short)k;
            else if (c.t0 && !c.t1 && hg_pair(HG_TT, 11, c.t0, k)) c.t1 = (unsigned short)k;
            else { HG_EMIT(); c = (struct hg_syl){ (unsigned short)k, 0, 0, 0, i }; }
        }
    }
    if (have) HG_EMIT();
#undef HG_EMIT
    return ns;
}
#define HG_MAXK 16
static unsigned short hg_ks[HG_MAXK]; static int hg_n;   /* pulsaciones de la composicion en curso */
static int hg_start, hg_blen, hg_elen;                   /* su tramo en el texto y la longitud esperada */
static int hg_valid(const struct ebuf *e) { return hg_n > 0 && e->cur == hg_start + hg_blen && e->len == hg_elen; }
/* reescribe el tramo con las pulsaciones; deja como mucho 2 silabas abiertas (la anterior, para
 * poder deshacer 달가 -> 닭) y confirma las de antes. 0 si no cabe en el texto. */
static int hg_render(struct ebuf *e)
{
    struct hg_syl sy[HG_MAXK]; int ns = hg_run(hg_ks, hg_n, sy, HG_MAXK);
    char out[HG_MAXK * 3 + 1]; int o = 0;
    for (int i = 0; i < ns; i++) { put_utf8(out + o, hg_cp(&sy[i])); o += (int)strlen(out + o); }
    out[o] = 0;
    if (!eb_replace(e, hg_start, hg_blen, out)) return 0;
    hg_blen = o; hg_elen = e->len;
    if (ns >= 3) {                                /* las primeras ya no pueden cambiar */
        int k0 = sy[ns - 2].k0, drop = 0;
        for (int i = 0; i < ns - 2; i++) { char t[4]; put_utf8(t, hg_cp(&sy[i])); drop += (int)strlen(t); }
        memmove(hg_ks, hg_ks + k0, (hg_n - k0) * sizeof hg_ks[0]); hg_n -= k0;
        hg_start += drop; hg_blen -= drop;
    }
    if (!hg_n) hg_blen = 0;
    return 1;
}
static void hg_commit(void) { hg_n = 0; hg_blen = 0; }

/* ------------------------------------------------------------------ preedicion (japones y chino) */
#define IME_MAXC 96
static char ime_pre[100];                     /* zh: letras pinyin (y '); ja: kana confirmados */
static char ime_rom[8];                       /* ja: romaji aun sin convertir */
static char ime_pool[1600]; static short ime_coff[IME_MAXC + 1]; static int ime_nc, ime_cons, ime_cl = -1;
static int ime_active(void) { return ime_pre[0] || ime_rom[0]; }
static void ime_reset(void) { ime_pre[0] = ime_rom[0] = 0; ime_nc = 0; ime_cl = -1; }
static const char *ime_cand(int i) { return i >= 0 && i < ime_nc ? ime_pool + ime_coff[i] : ""; }
static int ime_glyph_ok(int c) { int gi; font_glyph(c, &gi); return gi != 0; }
static void ime_cand_add(const char *s)
{
    if (ime_nc >= IME_MAXC || !*s) return;
    for (int i = 0; i < ime_nc; i++) if (!strcmp(ime_pool + ime_coff[i], s)) return;
    int o = ime_nc ? ime_coff[ime_nc] : 0, n = (int)strlen(s);
    if (o + n + 1 > (int)sizeof ime_pool) return;
    memcpy(ime_pool + o, s, n + 1); ime_coff[ime_nc++] = (short)o; ime_coff[ime_nc] = (short)(o + n + 1);
}
static void ime_cand_cp(int c) { char t[4]; if (!ime_glyph_ok(c)) return; put_utf8(t, c); ime_cand_add(t); }

/* --- romaji -> kana */
static const char *const ROMAJI[][2] = {
    { "a", "あ" }, { "i", "い" }, { "u", "う" }, { "e", "え" }, { "o", "お" },
    { "ka", "か" }, { "ki", "き" }, { "ku", "く" }, { "ke", "け" }, { "ko", "こ" },
    { "kya", "きゃ" }, { "kyi", "きぃ" }, { "kyu", "きゅ" }, { "kye", "きぇ" }, { "kyo", "きょ" },
    { "ga", "が" }, { "gi", "ぎ" }, { "gu", "ぐ" }, { "ge", "げ" }, { "go", "ご" },
    { "gya", "ぎゃ" }, { "gyi", "ぎぃ" }, { "gyu", "ぎゅ" }, { "gye", "ぎぇ" }, { "gyo", "ぎょ" },
    { "sa", "さ" }, { "si", "し" }, { "shi", "し" }, { "su", "す" }, { "se", "せ" }, { "so", "そ" },
    { "sha", "しゃ" }, { "shu", "しゅ" }, { "she", "しぇ" }, { "sho", "しょ" },
    { "sya", "しゃ" }, { "syi", "しぃ" }, { "syu", "しゅ" }, { "sye", "しぇ" }, { "syo", "しょ" },
    { "za", "ざ" }, { "zi", "じ" }, { "zu", "ず" }, { "ze", "ぜ" }, { "zo", "ぞ" },
    { "zya", "じゃ" }, { "zyi", "じぃ" }, { "zyu", "じゅ" }, { "zye", "じぇ" }, { "zyo", "じょ" },
    { "ja", "じゃ" }, { "ji", "じ" }, { "ju", "じゅ" }, { "je", "じぇ" }, { "jo", "じょ" },
    { "jya", "じゃ" }, { "jyi", "じぃ" }, { "jyu", "じゅ" }, { "jye", "じぇ" }, { "jyo", "じょ" },
    { "ta", "た" }, { "ti", "ち" }, { "chi", "ち" }, { "tu", "つ" }, { "tsu", "つ" }, { "te", "て" }, { "to", "と" },
    { "tya", "ちゃ" }, { "tyi", "ちぃ" }, { "tyu", "ちゅ" }, { "tye", "ちぇ" }, { "tyo", "ちょ" },
    { "cha", "ちゃ" }, { "chu", "ちゅ" }, { "che", "ちぇ" }, { "cho", "ちょ" },
    { "cya", "ちゃ" }, { "cyi", "ちぃ" }, { "cyu", "ちゅ" }, { "cye", "ちぇ" }, { "cyo", "ちょ" },
    { "tha", "てゃ" }, { "thi", "てぃ" }, { "thu", "てゅ" }, { "the", "てぇ" }, { "tho", "てょ" },
    { "tsa", "つぁ" }, { "tsi", "つぃ" }, { "tse", "つぇ" }, { "tso", "つぉ" }, { "twu", "とぅ" },
    { "ca", "か" }, { "ci", "し" }, { "cu", "く" }, { "ce", "せ" }, { "co", "こ" },
    { "da", "だ" }, { "di", "ぢ" }, { "du", "づ" }, { "de", "で" }, { "do", "ど" },
    { "dya", "ぢゃ" }, { "dyi", "ぢぃ" }, { "dyu", "ぢゅ" }, { "dye", "ぢぇ" }, { "dyo", "ぢょ" },
    { "dha", "でゃ" }, { "dhi", "でぃ" }, { "dhu", "でゅ" }, { "dhe", "でぇ" }, { "dho", "でょ" }, { "dwu", "どぅ" },
    { "na", "な" }, { "ni", "に" }, { "nu", "ぬ" }, { "ne", "ね" }, { "no", "の" },
    { "nya", "にゃ" }, { "nyi", "にぃ" }, { "nyu", "にゅ" }, { "nye", "にぇ" }, { "nyo", "にょ" },
    { "nn", "ん" }, { "n'", "ん" }, { "xn", "ん" },
    { "ha", "は" }, { "hi", "ひ" }, { "hu", "ふ" }, { "he", "へ" }, { "ho", "ほ" },
    { "hya", "ひゃ" }, { "hyi", "ひぃ" }, { "hyu", "ひゅ" }, { "hye", "ひぇ" }, { "hyo", "ひょ" },
    { "fa", "ふぁ" }, { "fi", "ふぃ" }, { "fu", "ふ" }, { "fe", "ふぇ" }, { "fo", "ふぉ" },
    { "fya", "ふゃ" }, { "fyu", "ふゅ" }, { "fyo", "ふょ" },
    { "ba", "ば" }, { "bi", "び" }, { "bu", "ぶ" }, { "be", "べ" }, { "bo", "ぼ" },
    { "bya", "びゃ" }, { "byi", "びぃ" }, { "byu", "びゅ" }, { "bye", "びぇ" }, { "byo", "びょ" },
    { "pa", "ぱ" }, { "pi", "ぴ" }, { "pu", "ぷ" }, { "pe", "ぺ" }, { "po", "ぽ" },
    { "pya", "ぴゃ" }, { "pyi", "ぴぃ" }, { "pyu", "ぴゅ" }, { "pye", "ぴぇ" }, { "pyo", "ぴょ" },
    { "ma", "ま" }, { "mi", "み" }, { "mu", "む" }, { "me", "め" }, { "mo", "も" },
    { "mya", "みゃ" }, { "myi", "みぃ" }, { "myu", "みゅ" }, { "mye", "みぇ" }, { "myo", "みょ" },
    { "ya", "や" }, { "yi", "い" }, { "yu", "ゆ" }, { "ye", "いぇ" }, { "yo", "よ" },
    { "ra", "ら" }, { "ri", "り" }, { "ru", "る" }, { "re", "れ" }, { "ro", "ろ" },
    { "rya", "りゃ" }, { "ryi", "りぃ" }, { "ryu", "りゅ" }, { "rye", "りぇ" }, { "ryo", "りょ" },
    { "la", "ぁ" }, { "li", "ぃ" }, { "lu", "ぅ" }, { "le", "ぇ" }, { "lo", "ぉ" },
    { "lya", "ゃ" }, { "lyu", "ゅ" }, { "lyo", "ょ" }, { "ltu", "っ" }, { "ltsu", "っ" }, { "lwa", "ゎ" },
    { "xa", "ぁ" }, { "xi", "ぃ" }, { "xu", "ぅ" }, { "xe", "ぇ" }, { "xo", "ぉ" },
    { "xya", "ゃ" }, { "xyu", "ゅ" }, { "xyo", "ょ" }, { "xtu", "っ" }, { "xtsu", "っ" }, { "xwa", "ゎ" },
    { "xka", "ゕ" }, { "xke", "ゖ" },
    { "wa", "わ" }, { "wi", "うぃ" }, { "wu", "う" }, { "we", "うぇ" }, { "wo", "を" },
    { "wha", "うぁ" }, { "whi", "うぃ" }, { "whe", "うぇ" }, { "who", "うぉ" },
    { "va", "ゔぁ" }, { "vi", "ゔぃ" }, { "vu", "ゔ" }, { "ve", "ゔぇ" }, { "vo", "ゔぉ" },
    { "qa", "くぁ" }, { "qi", "くぃ" }, { "qu", "く" }, { "qe", "くぇ" }, { "qo", "くぉ" },
    { "-", "ー" },
};
#define NROMAJI ((int)(sizeof ROMAJI / sizeof ROMAJI[0]))
/* 2: igual a una entrada (y no empieza otra mas larga); 1: empieza alguna; 0: ninguna */
static int rj_match(const char *p, const char **kana)
{
    int n = (int)strlen(p), pre = 0; *kana = 0;
    for (int i = 0; i < NROMAJI; i++) {
        if (strncmp(ROMAJI[i][0], p, n)) continue;
        if (!ROMAJI[i][0][n]) *kana = ROMAJI[i][1]; else pre = 1;
    }
    return pre ? 1 : *kana ? 2 : 0;
}
static void rj_emit(const char *s) { size_t l = strlen(ime_pre); if (l + strlen(s) < sizeof ime_pre) strcat(ime_pre, s); }
static int rj_is_cons(int c) { return c >= 'a' && c <= 'z' && !strchr("aiueon", c); }
/* convierte todo lo que ya se puede de ime_rom (MS-IME/Google: kitte -> きって, n+consonante -> ん) */
static void rj_convert(void)
{
    for (int guard = 0; ime_rom[0] && guard < 16; guard++) {
        const char *k; int m = rj_match(ime_rom, &k);
        if (m == 2) { rj_emit(k); ime_rom[0] = 0; return; }
        if (m == 1) return;
        char *p = ime_rom;
        if (p[1] && ((p[0] == p[1] && rj_is_cons(p[0])) || (p[0] == 't' && p[1] == 'c'))) rj_emit("っ");   /* kitte, matcha */
        else if (p[0] == 'n' && p[1]) rj_emit("ん");
        else { char t[2] = { p[0], 0 }; rj_emit(t); }
        memmove(p, p + 1, strlen(p));
    }
}
/* lo que hay sin convertir al confirmar: "n" -> ん, el resto tal cual */
static void rj_finish(char *o, int n)
{
    snprintf(o, n, "%s%s", ime_pre, !strcmp(ime_rom, "n") ? "ん" : ime_rom);
}
static void kana_to_kata(char *s)
{
    for (unsigned char *p = (unsigned char *)s; *p; ) {
        const char *q = (const char *)p; int c = utf8_next(&q); int n = (int)((const unsigned char *)q - p);
        if (c >= 0x3041 && c <= 0x3096 && n == 3) { char t[4]; put_utf8(t, c + 0x60); memcpy(p, t, 3); }
        p += n;
    }
}

/* --- pinyin -> caracteres */
#include "ime_zh.inc.c"
static int py_cmp(int i, const char *p, int n) { int r = strncmp(IME_PY_SYL[i], p, n); return r ? r : IME_PY_SYL[i][n] ? 1 : 0; }
/* silaba exacta p[0..n) o -1 */
static int py_find(const char *p, int n)
{
    if (n <= 0 || n > 6) return -1;
    int lo = 0, hi = IME_PY_N - 1;
    while (lo <= hi) { int m = (lo + hi) / 2, r = py_cmp(m, p, n); if (!r) return m; if (r < 0) lo = m + 1; else hi = m - 1; }
    return -1;
}
/* silabas que empiezan por p[0..n): [*a, *b) */
static int py_range(const char *p, int n, int *a, int *b)
{
    int lo = 0, hi = IME_PY_N;
    while (lo < hi) { int m = (lo + hi) / 2; if (strncmp(IME_PY_SYL[m], p, n) < 0) lo = m + 1; else hi = m; }
    *a = lo;
    while (hi < IME_PY_N && !strncmp(IME_PY_SYL[hi], p, n)) hi++;
    if (hi < lo) hi = lo;
    *b = hi; return hi - lo;
}
static int ime_s2t(int c)
{
    int lo = 0, hi = IME_S2T_N - 1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (IME_S2T[m][0] == c) return IME_S2T[m][1]; if (IME_S2T[m][0] < c) lo = m + 1; else hi = m - 1; }
    return c;
}
static void ime_zh_add(int c, int hant)
{
    if (!hant) { ime_cand_cp(c); return; }
    for (int i = 0; i < IME_S2T_ALT_N; i++) if (IME_S2T_ALT[i].s == c) {
        const char *s = IME_S2T_ALT[i].t; int t;
        while ((t = utf8_next(&s))) ime_cand_cp(t);
        return;
    }
    ime_cand_cp(ime_s2t(c));
}
/* candidatos de la primera silaba de ime_pre; ime_cons = letras que consume elegir uno */
static void ime_zh_cands(int hant)
{
    const char *s = ime_pre; while (*s == '\'') s++;
    int m = 0; while (s[m] && s[m] != '\'') m++;
    int best = 0, a, b;
    for (int k = m < 6 ? m : 6; k >= 1; k--) if (py_find(s, k) >= 0) { best = k; break; }
    if (best == 1 && m == 1 && py_range(s, 1, &a, &b) > 1) best = 0;   /* "n": mejor todas las n... */
    ime_cons = (int)(s - ime_pre);
    if (best) {
        int i = py_find(s, best);
        for (int j = IME_PY_OFF[i]; j < IME_PY_OFF[i + 1] && ime_nc < IME_MAXC; j++) ime_zh_add(IME_PY_CH[j], hant);
        ime_cons += best;
        return;
    }
    int k = m < 6 ? m : 6;
    while (k > 0 && !py_range(s, k, &a, &b)) k--;
    if (!k) { char t[2] = { s[0], 0 }; ime_cand_add(t); ime_cons += 1; return; }
    /* prefijo ("zh", "x"): las listas de todas sus silabas mezcladas por frecuencia */
    int cur[64], n = b - a; if (n > 64) n = 64;
    for (int i = 0; i < n; i++) cur[i] = IME_PY_OFF[a + i];
    while (ime_nc < IME_MAXC) {
        int bi = -1, br = 999;
        for (int i = 0; i < n; i++) if (cur[i] < IME_PY_OFF[a + i + 1] && IME_PY_RK[cur[i]] < br) { br = IME_PY_RK[cur[i]]; bi = i; }
        if (bi < 0) break;
        ime_zh_add(IME_PY_CH[cur[bi]++], hant);
    }
    ime_cons += k;
}
/* recalcula los candidatos de la distribucion l (si han cambiado) */
static void ime_build(int l)
{
    if (ime_cl == l) return;
    ime_nc = 0; ime_cons = 0; ime_cl = l;
    if (!ime_active()) return;
    const struct kb_lay *k = kb_lay(l);
    if (k->flags & KL_PINYIN) ime_zh_cands(!!(k->flags & KL_HANT));
    else if (k->flags & KL_KANA) {
        char h[110]; rj_finish(h, sizeof h);
        ime_cand_add(h); kana_to_kata(h); ime_cand_add(h);
    }
}
static void ime_dirty(void) { ime_cl = -1; }
/* lo que se confirmaria tal cual (intro): zh las letras sin ', ja el hiragana */
static void ime_raw(int l, char *o, int n)
{
    if (kb_lay(l)->flags & KL_KANA) { rj_finish(o, n); return; }
    int j = 0; for (const char *s = ime_pre; *s && j < n - 1; s++) if (*s != '\'') o[j++] = *s;
    o[j] = 0;
}
/* elige el candidato i: su texto en o; zh sigue con el resto de las letras (como iOS) */
static void ime_pick(int l, int i, char *o, int n)
{
    ime_build(l);
    snprintf(o, n, "%s", ime_cand(i));
    if (kb_lay(l)->flags & KL_PINYIN) {
        const char *r = ime_pre + (ime_cons < (int)strlen(ime_pre) ? ime_cons : (int)strlen(ime_pre));
        while (*r == '\'') r++;
        memmove(ime_pre, r, strlen(r) + 1);
    } else ime_pre[0] = ime_rom[0] = 0;
    ime_dirty();
}
/* conversion completa (al escribir un signo con preedicion): el primer candidato de cada silaba */
static void ime_auto(int l, char *o, int n)
{
    o[0] = 0;
    if (!(kb_lay(l)->flags & KL_PINYIN)) { ime_raw(l, o, n); ime_reset(); return; }
    for (int guard = 0; ime_active() && guard < 32; guard++) {
        char t[16]; ime_pick(l, 0, t, sizeof t);
        if ((int)(strlen(o) + strlen(t)) < n) strcat(o, t);
    }
    ime_reset();
}
/* una letra (minuscula) hacia la preedicion; 0 si no cabe */
static int ime_letter(int l, int c)
{
    if (kb_lay(l)->flags & KL_PINYIN) {
        size_t n = strlen(ime_pre);
        if (n >= 24 || (c == '\'' && (!n || ime_pre[n - 1] == '\''))) return 0;
        ime_pre[n] = (char)c; ime_pre[n + 1] = 0;
    } else {
        size_t n = strlen(ime_rom);
        if (strlen(ime_pre) > sizeof ime_pre - 12) return 0;
        if (n >= sizeof ime_rom - 1) return 0;
        ime_rom[n] = (char)c; ime_rom[n + 1] = 0;
        rj_convert();
    }
    ime_dirty();
    return 1;
}
static void ime_backspace(void)
{
    size_t n = strlen(ime_rom);
    if (n) ime_rom[n - 1] = 0;
    else if ((n = strlen(ime_pre))) { n--; while (n > 0 && ((unsigned char)ime_pre[n] & 0xC0) == 0x80) n--; ime_pre[n] = 0; }
    ime_dirty();
}
/* preedicion para mostrar (zh: letras con '; ja: kana y romaji pendiente) */
static void ime_preedit(char *o, int n) { snprintf(o, n, "%s%s", ime_pre, ime_rom); }

/* ------------------------------------------------------------------ entrada sobre el texto */
enum { KBI_TEXT = 1, KBI_PRE = 2 };          /* ha cambiado el texto / solo la preedicion */
/* confirma lo que se este componiendo: hangul se queda como esta; la preedicion entra tal cual */
static int kbi_commit(struct ebuf *e, int l)
{
    hg_commit();
    if (!ime_active()) return 0;
    char t[110]; ime_raw(l, t, sizeof t); ime_reset();
    if (e) eb_insert(e, t);
    return KBI_TEXT | KBI_PRE;
}
static void kbi_drop(void) { hg_commit(); ime_reset(); }
/* el llamante movio el cursor o cambio el texto: la silaba hangul ya no se toca */
static void kbi_sync(const struct ebuf *e) { if (hg_n && !hg_valid(e)) hg_commit(); }
/* un caracter de una tecla (ya en mayuscula si toca) */
static int kbi_char(struct ebuf *e, int l, int c)
{
    const struct kb_lay *k = kb_lay(l);
    kbi_sync(e);
    if ((k->flags & KL_HANGUL) && hg_is_jamo(c)) {
        if (!hg_n) { hg_start = e->cur; hg_blen = 0; }
        if (hg_n >= HG_MAXK) return 0;
        hg_ks[hg_n++] = (unsigned short)c;
        if (!hg_render(e)) { hg_n--; if (!hg_n) hg_blen = 0; return 0; }
        return KBI_TEXT;
    }
    hg_commit();
    if (k->flags & (KL_KANA | KL_PINYIN)) {
        int pin = !!(k->flags & KL_PINYIN);
        if ((c >= 'a' && c <= 'z') || (pin && c == '\'' && ime_active()) || (!pin && (c == '-' || c == 0x30FC) && ime_active()))
            return ime_letter(l, c == 0x30FC ? '-' : c) ? KBI_PRE : 0;
        if (ime_active()) {                       /* un signo: primero lo convertido */
            char t[110]; ime_auto(l, t, sizeof t); eb_insert(e, t);
        }
    }
    char o[8]; put_utf8(o, c); eb_insert(e, o);
    return KBI_TEXT | KBI_PRE;
}
/* borrar: la silaba en composicion pulsacion a pulsacion, la preedicion letra a letra */
static int kbi_back(struct ebuf *e, int l)
{
    (void)l;
    kbi_sync(e);
    if (ime_active()) { ime_backspace(); return KBI_PRE; }
    if (hg_n) {
        hg_n--;
        if (!hg_render(e)) { hg_commit(); eb_back(e); }
        return KBI_TEXT;
    }
    eb_back(e);
    return KBI_TEXT;
}
/* espacio con preedicion: el primer candidato (0 si no la hay: el espacio de siempre) */
static int kbi_space(struct ebuf *e, int l)
{
    hg_commit();
    if (!ime_active()) return 0;
    char t[110]; ime_pick(l, 0, t, sizeof t); eb_insert(e, t);
    return KBI_TEXT | KBI_PRE;
}
/* intro con preedicion: se confirma tal cual (0 si no la hay) */
static int kbi_enter(struct ebuf *e, int l) { hg_commit(); return ime_active() ? kbi_commit(e, l) : 0; }
/* toque en el candidato i */
static int kbi_pick(struct ebuf *e, int l, int i)
{
    if (!ime_active()) return 0;
    char t[110]; ime_pick(l, i, t, sizeof t); eb_insert(e, t);
    return KBI_TEXT | KBI_PRE;
}
