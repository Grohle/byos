/* ab_test.c - pruebas en PC del nucleo de la tienda de Libros (src/books_abcore.inc.c) con
 * respuestas reales grabadas en tests/ab_samples/ (archive.org, Gutendex).
 *   ab_test SAMPLES_DIR                    parsers, nombres, URL, book.json (ida y vuelta)
 *   ab_test --worker QFILE PAUSED BINDIR   ejecuta el proceso de descarga (lo usa ab_test.sh) */
#define _GNU_SOURCE
#include <stdint.h>
#include "books_abcore.inc.c"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void p(const char *d, const char *n, char *o) { sprintf(o, "%s/%s", d, n); }

int main(int argc, char **argv)
{
    if (argc == 5 && !strcmp(argv[1], "--worker")) return ab_worker_run(argv[2], argv[3], argv[4]);
    if (argc < 2) { fprintf(stderr, "uso: ab_test SAMPLES\n"); return 2; }
    const char *D = argv[1]; char f[600];
    static struct ab_hits r;

    /* --- busqueda: populares en espanol */
    p(D, "search_popular_spa.json", f);
    CHECK(!ab_search_parse(f, &r), "parse %s", f);
    printf("populares spa: %d de %ld\n", r.n, r.found);
    for (int i = 0; i < r.n && i < 5; i++) printf("  %s | %s | %s | %s\n", r.h[i].id, r.h[i].title, r.h[i].author, r.h[i].runtime);
    CHECK(r.n >= 30 && r.found > 400, "n=%d found=%ld", r.n, r.found);
    CHECK(!strcmp(r.h[0].lang, "spa"), "lang %s", r.h[0].lang);
    for (int i = 0; i < r.n; i++) CHECK(r.h[i].id[0] && r.h[i].title[0], "vacio %d", i);
    /* autor con ";" y "Apellido, Nombre" */
    int townsend = 0; for (int i = 0; i < r.n; i++) if (!strcmp(r.h[i].author, "George Fyler Townsend")) townsend = 1;
    CHECK(townsend, "autor normalizado (Townsend)");

    p(D, "search_recent_fra.json", f);
    CHECK(!ab_search_parse(f, &r), "parse fra");
    printf("recientes fra: %d de %ld; 1: %s | %s\n", r.n, r.found, r.h[0].title, r.h[0].author);
    CHECK(r.n > 10, "fra n=%d", r.n);

    p(D, "search_query_eng.json", f);
    CHECK(!ab_search_parse(f, &r), "parse eng");
    printf("busqueda sherlock: %d de %ld; 1: %s | %s\n", r.n, r.found, r.h[0].title, r.h[0].author);
    CHECK(r.n > 3 && strstr(r.h[0].title, "Sherlock"), "sherlock");

    /* --- ficha: Alice (64 kbps, pistas "1/12") */
    struct ab_book b;
    p(D, "meta_alice.json", f);
    CHECK(!ab_meta_parse(f, &b), "meta alice");
    printf("alice: '%s' / '%s' / %s / %d caps / %lld bytes / %d s\n", b.title, b.author, b.lang, b.nsec, b.total, b.totdur);
    CHECK(b.nsec == 12 && !strcmp(b.title, "Alice's Adventures in Wonderland") && !strcmp(b.author, "Lewis Carroll"), "alice cabecera");
    CHECK(!strcmp(b.sec[0].remote, "wonderland_ch_01_64kb.mp3") && b.sec[0].size == 5124992 && b.sec[0].dur == 640, "alice cap 1: %s %lld %d", b.sec[0].remote, b.sec[0].size, b.sec[0].dur);
    CHECK(!strcmp(b.sec[11].remote, "wonderland_ch_12_64kb.mp3"), "orden");
    CHECK(!b.sec[0].title[0] && !strcmp(b.sec[0].file, "01 - 1.mp3"), "titulo = nombre de fichero -> vacio (%s / %s)", b.sec[0].title, b.sec[0].file);
    CHECK(b.totdur > 10000 && b.totdur < 11000, "duracion total %d", b.totdur);
    printf("  desc: %.160s…\n", b.desc);
    CHECK(strstr(b.desc, "LibriVox recording") && !strchr(b.desc, '<'), "descripcion sin html");

    /* book.json: ida y vuelta */
    char bj[] = "/tmp/ab_test_book.json";
    CHECK(!ab_book_save(&b, bj), "guardar");
    struct ab_book c;
    CHECK(!ab_book_load(bj, &c, 1), "cargar");
    CHECK(c.nsec == b.nsec && c.total == b.total && c.totdur == b.totdur && !strcmp(c.title, b.title) && !strcmp(c.desc, b.desc), "ida y vuelta");
    CHECK(!strcmp(c.sec[5].file, b.sec[5].file) && c.sec[5].size == b.sec[5].size, "ida y vuelta cap");
    ab_book_free(&c);
    CHECK(!ab_book_load(bj, &c, 0) && c.nsec == 12 && c.sec == 0 && c.total == b.total, "carga ligera");
    ab_book_free(&c);
    ab_book_free(&b);

    /* Don Quijote: sin pista, titulos "01: Tasa & El Rey", 37 ficheros */
    p(D, "meta_quijote.json", f);
    CHECK(!ab_meta_parse(f, &b), "meta quijote");
    printf("quijote: '%s' / '%s' / %d caps; 1 '%s' -> %s; 6 '%s'\n", b.title, b.author, b.nsec, b.sec[0].title, b.sec[0].file, b.sec[5].title);
    CHECK(b.nsec == 37 && !strcmp(b.sec[0].title, "Tasa & El Rey") && !strcmp(b.sec[5].title, "Capitulo IX-X"), "quijote");
    CHECK(!strcmp(b.sec[0].file, "01 - Tasa & El Rey.mp3"), "fichero %s", b.sec[0].file);
    ab_book_free(&b);

    p(D, "meta_tito.json", f);
    CHECK(!ab_meta_parse(f, &b), "meta tito");
    printf("tito: '%s' / %d caps / %lld bytes / '%s' -> %s\n", b.title, b.nsec, b.total, b.sec[0].title, b.sec[0].file);
    CHECK(b.nsec == 1 && !strcmp(b.sec[0].title, "Tito Chapters 1-3"), "tito: '%s'", b.sec[0].title);
    { char dn[200]; ab_sanitize("Reina Valera - Bible (Reina Valera 1909) NT 17: Tito", dn, 90); CHECK(!strcmp(dn, "Reina Valera - Bible (Reina Valera 1909) NT 17 - Tito"), "carpeta '%s'", dn); }
    ab_book_free(&b);

    /* Esopo: titulos con entidades HTML y numero de pista delante ("002 El &aacute;guila...") */
    p(D, "meta_esopo.json", f);
    CHECK(!ab_meta_parse(f, &b), "meta esopo");
    printf("esopo: %d caps; 2 '%s' -> %s\n", b.nsec, b.sec[1].title, b.sec[1].file);
    CHECK(b.nsec == 30 && !strncmp(b.sec[1].title, "El águila", 10) && !strstr(b.sec[1].title, "&"), "esopo cap 2 '%s'", b.sec[1].title);
    ab_book_free(&b);

    /* La Fontaine: un titulo codificado en UTF-8 muchas veces seguidas (mojibake de archive.org) */
    p(D, "meta_fontaine.json", f);
    CHECK(!ab_meta_parse(f, &b), "meta fontaine");
    printf("fontaine: %d caps; 1 '%s'; 2 '%s'\n", b.nsec, b.sec[0].title, b.sec[1].title);
    CHECK(!strcmp(b.sec[0].title, "\xC3\x80 Monseigneur le Dauphin"), "mojibake sin arreglar");
    ab_book_free(&b);
    { char m1[64] = "Caf\xC3\x83\xC2\xA9 cr\xC3\x83\xC2\xA8me", m2[32] = "F\xC3\xA1" "bulas"; ab_unmojibake(m1); ab_unmojibake(m2);
      CHECK(!strcmp(m1, "Caf\xC3\xA9 cr\xC3\xA8me") && !strcmp(m2, "F\xC3\xA1" "bulas"), "unmojibake '%s' '%s'", m1, m2); }

    /* --- Gutenberg (catalogo OPDS) */
    p(D, "opds_es.xml", f);
    CHECK(!ab_gut_parse(f, &r), "opds");
    printf("opds: %d; 1: %s | %s | %s | %s\n", r.n, r.h[0].id, r.h[0].title, r.h[0].author, r.h[0].cover);
    CHECK(r.n == 25 && !strcmp(r.h[0].id, "2000") && !strcmp(r.h[0].title, "Don Quijote") && !strcmp(r.h[0].author, "Miguel de Cervantes Saavedra") && strstr(r.h[0].cover, "/2000/pg2000.cover.medium.jpg"), "opds 1");
    for (int i = 0; i < r.n; i++) CHECK(r.h[i].title[0] && !strchr(r.h[i].title, '\n') && !strstr(r.h[i].title, "(Spanish)") && !strstr(r.h[i].author, "downloads"), "opds fila %d '%s' '%s'", i, r.h[i].title, r.h[i].author);
    p(D, "opds_q.xml", f);
    CHECK(!ab_gut_parse(f, &r) && r.n >= 5 && !strcmp(r.h[0].id, "84") && !r.h[r.n - 1].author[0], "opds busqueda (%d, autor '%s')", r.n, r.h[r.n - 1].author);
    { char ds[700]; p(D, "opds_2000.xml", f);
      CHECK(!ab_gut_detail_parse(f, ds, sizeof ds) && !strncmp(ds, "\"Don Quijote\" by Miguel", 23) && !strstr(ds, "automatically") && ds[strlen(ds) - 1] == '.', "opds resumen '%s'", ds); }
    { char t[64] = "Faust (Erster Teil) (German)"; ab_gut_title_fix(t); CHECK(!strcmp(t, "Faust (Erster Teil)"), "titulo '%s'", t);
      strcpy(t, "Diccionario (Spanish) (Tagalog)"); ab_gut_title_fix(t); CHECK(!strcmp(t, "Diccionario"), "titulo '%s'", t); }

    /* --- utilidades */
    char o[200];
    ab_sanitize("AC/DC: \"Live\"?*  ", o, 80); CHECK(!strcmp(o, "AC DC - Live"), "sanitize '%s'", o);
    ab_sanitize("Vol. 1...", o, 80); CHECK(!strcmp(o, "Vol. 1"), "sanitize puntos '%s'", o);
    ab_sanitize("ñññññ", o, 5); CHECK(!strcmp(o, "ññ"), "sanitize utf8 '%s'", o);
    CHECK(ab_parse_dur("2:58:51") == 10731 && ab_parse_dur("41:37") == 2497 && ab_parse_dur("640.61") == 641, "duraciones");
    char h[100]; strcpy(h, "Verne, Jules"); ab_author_norm(h); CHECK(!strcmp(h, "Jules Verne"), "autor %s", h);
    strcpy(h, "Sir Arthur Conan Doyle"); ab_author_norm(h); CHECK(!strcmp(h, "Sir Arthur Conan Doyle"), "autor %s", h);
    ab_html_text("<p>Uno&nbsp;&amp; dos</p><p>tres&#233; <b>x</b></p>", o, sizeof o); CHECK(!strcmp(o, "Uno & dos\ntresé x"), "html '%s'", o);
    char u[1200];
    ab_search_url(u, sizeof u, ab_lang_find("fra"), "Jules Verne (1)", 2, 1);
    printf("url: %s\n", u);
    CHECK(strstr(u, "language%3A%28fre%20OR%20fra%29") && strstr(u, "title%3A%28Jules%20Verne%201%29") && !strstr(u, "sort"), "url busqueda");
    ab_search_url(u, sizeof u, ab_lang_find("all"), 0, 0, 2);
    CHECK(!strstr(u, "language%3A") && strstr(u, "downloads+desc") && strstr(u, "page=2"), "url populares todos");
    ab_gut_url(u, sizeof u, ab_lang_find("spa"), "don quijote l.en", 0, 1); printf("gut: %s\n", u);
    CHECK(strstr(u, "search.opds/?sort_order=downloads&query=don%20quijote%20l%20en%20l.es") && !strstr(u, "start_index"), "url opds");
    ab_gut_url(u, sizeof u, ab_lang_find("all"), 0, 1, 3);
    CHECK(strstr(u, "sort_order=release_date&start_index=51") && !strstr(u, "query"), "url opds novedades '%s'", u);
    /* cola */
    struct ab_qe q[2] = { { "id1", 'm', "https://a/b.mp3", "/tmp/x/01 - a.mp3", 10 }, { "id2", 'c', "https://a/c|/tmp/c.cov", "/tmp/x/cover.jpg", 0 } };
    CHECK(!ab_q_save("/tmp/ab_test.q", q, 2), "q save");
    int n; struct ab_qe *v = ab_q_load("/tmp/ab_test.q", &n);
    CHECK(n == 2 && v && !strcmp(v[1].url, "https://a/c|/tmp/c.cov") && v[0].size == 10 && v[1].kind == 'c', "q load");
    free(v);
    CHECK(ab_in_list("a\nbb\nc", "bb") && !ab_in_list("a\nbbb\n", "bb"), "in_list");

    printf(fails ? "\n%d FALLOS\n" : "\nTODO BIEN\n", fails);
    return fails != 0;
}
