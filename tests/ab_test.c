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

/* the strings of entry i of a catalog page */
struct opds_ev { const char *title, *author, *href, *cover, *image, *sum; };
static struct opds_ev opds_ev(const struct opds_feed *f, int i)
{
    const struct opds_e *e = &f->e[i];
    struct opds_ev v = { opds_s(f, e->title), opds_s(f, e->author), opds_s(f, e->href), opds_s(f, e->cover), opds_s(f, e->image), opds_s(f, e->sum) };
    return v;
}

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

    /* --- OPDS catalogs added by the user: real pages captured on 2026-11-10 (trimmed to a few
     * entries each) from textos.info, Wolne Lektury, ebooksgratuits.com, MEK, unglue.it and COPS */
    {
        static struct opds_feed F; char t[OPDS_URL];
#define FS(x) opds_ev(&F, x)
        /* textos.info: navigation root, OpenSearch description, search results with covers */
        p(D, "opds_textos_root.xml", f);
        CHECK(opds_parse(f, "https://www.textos.info/catalogo.atom", &F, 0) == 5 && F.nnav == 5 && !F.nbook && !F.cut, "textos root n=%d", F.n);
        CHECK(!strcmp(F.title, "textos.info") && !F.next[0] && !F.search_tpl && !strcmp(F.search, "https://www.textos.info/opensearch.xml"), "textos root '%s' '%s'", F.title, F.search);
        CHECK(F.e[0].kind == OE_NAV && !strcmp(FS(0).href, "https://www.textos.info/populares.atom") && !strcmp(FS(0).title, "Textos m\xC3\xA1s populares") && FS(0).sum[0], "textos nav '%s' '%s'", FS(0).title, FS(0).href);
        printf("opds textos: %d folders, search %s\n", F.nnav, F.search);
        p(D, "opds_textos_osd.xml", f);
        CHECK(!opds_osd_parse(f, "https://www.textos.info/opensearch.xml", t, sizeof t) && !strcmp(t, "https://www.textos.info/busqueda.atom&query={searchTerms}"), "textos osd '%s'", t);
        CHECK(opds_parse(f, "https://www.textos.info/opensearch.xml", &F, 0) == -1 && !F.n, "an OpenSearch description is not a feed");
        p(D, "opds_textos_search.xml", f);
        CHECK(opds_parse(f, "https://www.textos.info/busqueda.atom&query=quijote", &F, 0) == 4 && F.nbook == 4 && !F.nnav, "textos search n=%d", F.n);
        CHECK(F.e[0].kind == OE_BOOK && !strcmp(FS(0).author, "Juan Montalvo") && !strcmp(FS(0).href, "https://www.textos.info/juan-montalvo/capitulos-que-se-le-olvidaron-a-cervantes/epub") &&
              !strcmp(FS(0).cover, "https://www.textos.info/img/9/9821.jpg") && strlen(FS(0).sum) > 40 && !strchr(FS(0).sum, '<'), "textos book '%s' '%s' '%s'", FS(0).author, FS(0).href, FS(0).cover);
        { char id[24], id2[24]; opds_book_id(F.e[0].h, id, sizeof id); opds_book_id(F.e[1].h, id2, sizeof id2);
          CHECK(opds_is_book_id(id) && strcmp(id, id2) && !opds_is_book_id("2000") && !opds_is_book_id("") && F.e[0].h == ab_hash(FS(0).href), "book ids '%s' '%s'", id, id2); }

        /* Wolne Lektury: rel="subsection" without an OPDS profile; a book entry has both a folder link and
         * the EPUB (the book wins); http:// links; the "covers" are the 1 KB icons of the site */
        p(D, "opds_wl_root.xml", f);
        CHECK(opds_parse(f, "https://wolnelektury.pl/opds/", &F, 0) == 5 && F.nnav == 5 && !strcmp(F.title, "Wolne Lektury") && !strcmp(FS(1).href, "https://wolnelektury.pl/opds/author/"), "wl root %d '%s'", F.n, FS(1).href);
        CHECK(!strcmp(F.search, "http://wolnelektury.pl/static/opensearch.xml") && !F.search_tpl, "wl search '%s'", F.search);
        p(D, "opds_wl_osd.xml", f);
        CHECK(!opds_osd_parse(f, F.search, t, sizeof t) && !strncmp(t, "https://wolnelektury.pl/opds/search/?q={searchTerms}&author={atom:author}", 73), "wl osd '%s'", t);
        { char u[600]; opds_search_url(t, " pan tadeusz ", u, sizeof u);
          CHECK(!strcmp(u, "https://wolnelektury.pl/opds/search/?q=pan%20tadeusz&author=&translator=&title="), "wl search url '%s'", u); }
        p(D, "opds_wl_books.xml", f);
        CHECK(opds_parse(f, "https://wolnelektury.pl/opds/epoch/starozytnosc/", &F, 0) == 5 && F.nbook == 5 && !F.nnav && !F.cut, "wl books %d/%d", F.nbook, F.nnav);
        CHECK(F.e[0].kind == OE_BOOK && !strcmp(FS(0).href, "http://wolnelektury.pl/media/book/epub/bereszit-ksiega-rodzaju.epub") && !FS(0).cover[0] && !FS(0).author[0] && strstr(FS(0).title, "Bereszit"), "wl book '%s' cover '%s'", FS(0).href, FS(0).cover);
        printf("opds wl: %d books; 1: %s | %s\n", F.nbook, FS(0).title, FS(0).href);
        /* the same page cut in the middle of the fourth entry: three books and the "cut" mark */
        p(D, "opds_truncated.xml", f);
        CHECK(opds_parse(f, "https://wolnelektury.pl/opds/epoch/starozytnosc/", &F, 0) == 3 && F.cut && !strcmp(FS(2).href, "http://wolnelektury.pl/media/book/epub/genesis-ksiega-rodzaju-bereszit.epub"), "truncated n=%d cut=%d", F.n, F.cut);

        /* ebooksgratuits.com: links with no rel, data: images, relative search link, summaries in CDATA
         * with <br />, "Surname, Name" authors, a next page */
        p(D, "opds_ebg_root.xml", f);
        CHECK(opds_parse(f, "https://www.ebooksgratuits.com/opds/", &F, 0) == 5 && F.nnav == 5 && !strcmp(FS(1).href, "https://www.ebooksgratuits.com/opds/feed.php?mode=maj") && !FS(1).cover[0], "ebg root %d '%s' '%s'", F.n, FS(1).href, FS(1).cover);
        CHECK(!strcmp(F.search, "https://www.ebooksgratuits.com/opds/opensearch.xml"), "ebg search '%s'", F.search);
        p(D, "opds_ebg_osd.xml", f);
        CHECK(!opds_osd_parse(f, F.search, t, sizeof t) && !strcmp(t, "https://www.ebooksgratuits.com/opds/feed.php?mode=search&query={searchTerms}"), "ebg osd '%s'", t);
        p(D, "opds_ebg_books.xml", f);
        CHECK(opds_parse(f, "https://www.ebooksgratuits.com/opds/feed.php", &F, 0) == 5 && F.nbook == 5 && !strcmp(F.next, "https://www.ebooksgratuits.com/opds/feed.php?page=1"), "ebg books %d next '%s'", F.n, F.next);
        CHECK(!strcmp(FS(0).title, "Henry Esmond") && !strcmp(FS(0).author, "William Makepeace Thackeray") && !strcmp(FS(0).href, "https://www.ebooksgratuits.com/newsendbook.php?id=3529&format=epub") &&
              !strncmp(FS(0).sum, "Henry Esmond nous conte", 23) && !strchr(FS(0).sum, '<') && strlen(FS(0).sum) <= 690, "ebg book '%s' '%s' '%.40s'", FS(0).author, FS(0).href, FS(0).sum);
        /* the next page is appended to the list (here the same file again) */
        CHECK(opds_parse(f, F.next, &F, 1) == 5 && F.n == 10 && F.nbook == 10 && !strcmp(FS(5).title, "Henry Esmond") && !strcmp(FS(0).title, "Henry Esmond") && !strcmp(F.title, "Liste des ebooks"), "append n=%d", F.n);
        printf("opds ebg: %d books after two pages; next %s\n", F.n, F.next);

        /* MEK: served as text/html with blank lines before the XML declaration; rel="next" after the entries;
         * HTML-escaped <content>; jpeg thumbnails */
        p(D, "opds_mek_root.xml", f);
        CHECK(opds_parse(f, "https://mek.oszk.hu/opds/", &F, 0) == 5 && F.nnav == 5 && !strcmp(F.title, "Magyar Elektronikus K\xC3\xB6nyvt\xC3\xA1r") && !strcmp(FS(2).href, "https://bookserver.mek.oszk.hu/ujak/0"), "mek root %d '%s'", F.n, F.title);
        p(D, "opds_mek_books.xml", f);
        CHECK(opds_parse(f, "https://bookserver.mek.oszk.hu/ujak/0", &F, 0) == 5 && F.nbook == 5 && !strcmp(F.next, "https://bookserver.mek.oszk.hu/ujak/1"), "mek books %d next '%s'", F.n, F.next);
        CHECK(!strcmp(FS(0).href, "https://mek.oszk.hu/29000/29089/29089.epub") && !strcmp(FS(0).cover, "https://mek.oszk.hu/29000/29089/borito.jpg") && !strcmp(FS(4).author, "Mary Wollstonecraft") && !strchr(FS(0).sum, '<') && !strstr(FS(0).sum, "&lt;"), "mek book '%s' '%s' '%s'", FS(0).href, FS(0).cover, FS(4).author);

        /* COPS (Calibre): hrefs relative to feed.php, an OpenSearch description with a relative template,
         * thumbnail and full image, download names with %20 and %27 */
        p(D, "opds_cops_root.xml", f);
        CHECK(opds_parse(f, "https://cops-demo.slucas.fr/feed.php", &F, 0) == 8 && F.nnav == 8 && !strcmp(FS(0).href, "https://cops-demo.slucas.fr/feed.php?page=1") && !strcmp(F.search, "https://cops-demo.slucas.fr/feed.php?page=8"), "cops root %d '%s' '%s'", F.n, FS(0).href, F.search);
        p(D, "opds_cops_osd.xml", f);
        CHECK(!opds_osd_parse(f, F.search, t, sizeof t) && !strcmp(t, "https://cops-demo.slucas.fr/feed.php?query={searchTerms}"), "cops osd '%s'", t);
        { char u[600]; opds_search_url(t, "d'Artagnan & co", u, sizeof u); CHECK(!strcmp(u, "https://cops-demo.slucas.fr/feed.php?query=d%27Artagnan%20%26%20co"), "cops search url '%s'", u); }
        p(D, "opds_cops_books.xml", f);
        CHECK(opds_parse(f, "https://cops-demo.slucas.fr/feed.php?page=10", &F, 0) == 5 && F.nbook == 5, "cops books %d", F.n);
        CHECK(!strcmp(FS(0).title, "Alice's Adventures in Wonderland") && !strcmp(FS(0).author, "Lewis Carroll") && !strncmp(FS(0).href, "https://cops-demo.slucas.fr/download/20/Alice%27s%20Adventures", 62) &&
              !strcmp(FS(0).cover, "https://cops-demo.slucas.fr/fetch.php?height=164&id=17") && !strcmp(FS(0).image, "https://cops-demo.slucas.fr/fetch.php?id=17"), "cops book '%s' '%s' '%s'", FS(0).href, FS(0).cover, FS(0).image);
        printf("opds cops: %s | %s | %s\n", FS(0).title, FS(0).author, FS(0).cover);

        /* unglue.it: relative and absolute-path hrefs, no search, a quote in a URL, CJK titles */
        p(D, "opds_unglue_root.xml", f);
        CHECK(opds_parse(f, "https://unglue.it/api/opds/", &F, 0) == 18 && F.nnav == 18 && !F.search[0], "unglue root %d", F.n);
        CHECK(!strcmp(FS(0).href, "https://unglue.it/api/opds/all/?order_by=popular") && !strcmp(FS(5).href, "https://unglue.it/api/opds/epub/"), "unglue hrefs '%s' '%s'", FS(0).href, FS(5).href);
        { int q = 0; for (int i = 0; i < F.n; i++) { if (!strcmp(FS(i).href, "https://unglue.it/api/opds/kw.Children%27s%20literature,%20English/")) q = 1; CHECK(fm_url_ok(FS(i).href), "url %d '%s'", i, FS(i).href); }
          CHECK(q, "a quote inside a URL is percent-encoded"); }
        p(D, "opds_unglue_books.xml", f);
        CHECK(opds_parse(f, "https://unglue.it/api/opds/epub/", &F, 0) == 3 && F.nbook == 3 && !strcmp(F.next, "https://unglue.it/api/opds/epub/?order_by=newest&page=1"), "unglue books %d '%s'", F.n, F.next);
        CHECK(!strcmp(FS(0).href, "https://unglue.it/download_ebook/779416/?feed=opds") && strcmp(FS(0).cover, FS(0).image) && !strncmp(FS(0).cover, "https://files.unglue.it/cache/", 30), "unglue book '%s' '%s'", FS(0).href, FS(0).cover);

        /* Project Gutenberg seen as a plain catalog: the list is folders (one per book) and the entry
         * document of a book offers the EPUB */
        p(D, "opds_es.xml", f);
        CHECK(opds_parse(f, "https://www.gutenberg.org/ebooks/search.opds/", &F, 0) == 25 && F.nnav == 25 && !strcmp(FS(0).href, "https://www.gutenberg.org/ebooks/2000.opds") && !FS(0).cover[0] && strstr(F.next, "start_index=26"), "gutenberg list %d '%s'", F.n, F.next);
        p(D, "opds_gut_book.xml", f);
        CHECK(opds_parse(f, "https://www.gutenberg.org/ebooks/1342.opds", &F, 0) == 2 && F.nbook == 2 && !strcmp(FS(1).href, "https://www.gutenberg.org/ebooks/1342.epub3.images") && !strcmp(FS(0).author, "Jane Austen"), "gutenberg book %d '%s'", F.n, FS(1).href);

        /* garbage, other formats, missing files: no entries and no crash (ASan/UBSan watch) */
        p(D, "opds_garbage.xml", f);
        CHECK(opds_parse(f, "https://example.org/opds", &F, 0) <= 0 && !F.n && !F.next[0], "garbage n=%d", F.n);
        CHECK(opds_osd_parse(f, "https://example.org/osd", t, sizeof t) == -1 && !t[0], "garbage osd");
        p(D, "search_popular_spa.json", f);
        CHECK(opds_parse(f, "https://archive.org/x", &F, 0) == -1 && !F.n, "json is not a feed");
        CHECK(opds_parse("/nonexistent/feed.xml", "https://example.org/", &F, 0) == -1 && !F.n && !F.title[0], "missing file");
        { /* every prefix and every suffix of a real page, and the garbage in slices: never a crash, never a bad URL */
          static const char *const big[] = { "opds_cops_books.xml", "opds_ebg_root.xml", "opds_garbage.xml", "opds_unglue_books.xml" };
          for (unsigned bi = 0; bi < sizeof big / sizeof big[0]; bi++) {
              p(D, big[bi], f);
              char *all = ab_read_small(f, OPDS_READ); CHECK(all != 0, "read %s", f);
              if (!all) continue;
              int L = (int)strlen(all), step = L > 12000 ? 97 : 23;
              for (int cut = 0; cut <= L; cut += step) {
                  char *part = malloc(cut + 1); memcpy(part, all, cut); part[cut] = 0;
                  opds_parse_buf(part, "https://example.org/a/b?c=d", &F, 0);
                  for (int i = 0; i < F.n; i++) CHECK(FS(i).title[0] && (F.e[i].kind != OE_BOOK || fm_url_ok(FS(i).href)) && (!FS(i).cover[0] || fm_url_ok(FS(i).cover)), "prefix %d of %s, entry %d", cut, big[bi], i);
                  opds_osd_parse_buf(part, "https://example.org/osd", t, sizeof t); opds_discover_buf(part, "https://example.org/", t, sizeof t);
                  free(part);
                  opds_parse_buf(all + cut, "https://example.org/a/b?c=d", &F, 1);
                  if (F.n > OPDS_MAXE - 40) opds_feed_reset(&F);
              }
              free(all);
          }
        }
        { /* more entries than fit: the list stops at the cap and says so */
          size_t cap = 200 + (OPDS_MAXE + 50) * 260; char *bigf = malloc(cap); size_t k = 0;
          k += sprintf(bigf + k, "<feed xmlns=\"http://www.w3.org/2005/Atom\"><title>Big</title>");
          for (int i = 0; i < OPDS_MAXE + 50; i++) k += sprintf(bigf + k, "<entry><title>Book %d</title><id>urn:b:%d</id><link rel=\"http://opds-spec.org/acquisition/open-access\" type=\"application/epub+zip\" href=\"b/%d.epub\"/></entry>", i, i, i);
          k += sprintf(bigf + k, "<link rel=\"next\" href=\"?p=2\"/></feed>");
          CHECK(opds_parse_buf(bigf, "http://nas.local:8083/opds/all", &F, 0) == OPDS_MAXE && F.cut && F.n == OPDS_MAXE && !strcmp(FS(OPDS_MAXE - 1).href, "http://nas.local:8083/opds/b/639.epub"), "cap n=%d cut=%d '%s'", F.n, F.cut, FS(OPDS_MAXE - 1).href);
          free(bigf); }
        { /* shapes written by hand: kinds of entries, Latin-1, a page that only advertises its catalog */
          const char *x = "<?xml version='1.0' encoding='ISO-8859-1'?><feed><title>Caf\xE9</title>"
              "<link rel='search' type='application/atom+xml' href='/s?q={searchTerms}&amp;p={startPage?}'/>"
              "<entry><title>Paid</title><link rel='http://opds-spec.org/acquisition/buy' type='application/epub+zip' href='/buy/1.epub'/></entry>"
              "<entry><title>Only PDF</title><link rel='http://opds-spec.org/acquisition' type='application/pdf' href='/1.pdf'/></entry>"
              "<entry><title>No type</title><author><name>A</name></author><author><name>B, C</name></author><link rel='http://opds-spec.org/acquisition' href='x/My Book.EPUB?dl=1'/></entry>"
              "<entry><title>Nothing</title><link rel='alternate' type='text/html' href='/web'/></entry>"
              "<entry><title></title><link rel='subsection' type='application/atom+xml' href='/empty'/></entry>"
              "<entry><title>Sub &amp; <![CDATA[<b>folder</b>]]></title><link type='application/atom+xml;profile=opds-catalog;kind=navigation' href='?folder=2'/><link rel='self' type='application/atom+xml' href='/self'/></entry>"
              "<entry><title>Odd links</title><link rel='subsection' type='application/atom+xml' href='javascript:alert(1)'/><link rel='related' type='application/atom+xml' href='ftp://x/y'/></entry>"
              "</feed>";
          char *lat = opds_to_utf8(strdup(x));
          CHECK(opds_parse_buf(lat, "https://lib.example.org/opds/root.xml?a=1", &F, 0) == 4, "kinds n=%d", F.n);
          CHECK(!strcmp(F.title, "Caf\xC3\xA9") && F.search_tpl && !strcmp(F.search, "https://lib.example.org/s?q={searchTerms}&p={startPage?}"), "latin1 '%s' search '%s'", F.title, F.search);
          CHECK(F.n == 4 && F.e[0].kind == OE_NOEPUB && F.e[1].kind == OE_NOEPUB && !FS(0).href[0] && F.e[2].kind == OE_BOOK && !strcmp(FS(2).href, "https://lib.example.org/opds/x/My%20Book.EPUB?dl=1") && !strcmp(FS(2).author, "A, C B") &&
                F.e[3].kind == OE_NAV && !strcmp(FS(3).title, "Sub & folder") && !strcmp(FS(3).href, "https://lib.example.org/opds/root.xml?folder=2") && F.nbook == 3 && F.nnav == 1, "kinds %d %d %d '%s' '%s' '%s' '%s'", F.e[0].kind, F.e[1].kind, F.e[2].kind, FS(2).href, FS(2).author, FS(3).href, FS(3).title);
          { char u[600]; opds_search_url(F.search, "caf\xC3\xA9", u, sizeof u); CHECK(!strcmp(u, "https://lib.example.org/s?q=caf%C3%A9&p="), "search url '%s'", u);
            char small[20]; opds_search_url(F.search, "something long", small, sizeof small); CHECK(!small[0], "search url that does not fit '%s'", small); }
          free(lat);
          CHECK(!opds_discover_buf("<html><head><link rel=\"alternate\" type=\"application/rss+xml\" href=\"/rss\"><link rel='related' type='application/atom+xml;profile=opds-catalog' href='/opds'></head><body>hi</body></html>", "http://192.168.1.10:8083/", t, sizeof t) &&
                !strcmp(t, "http://192.168.1.10:8083/opds"), "discover '%s'", t);
          CHECK(opds_discover_buf("<html><body>no catalog here</body></html>", "http://x.example/", t, sizeof t) == -1 && !t[0], "discover nothing"); }
#undef FS
        /* what the user types -> URL */
        { static const char *const in[][2] = {
              { "wolnelektury.pl/opds", "https://wolnelektury.pl/opds" }, { "  HTTPS://Example.org/Opds/ ", "https://Example.org/Opds/" },
              { "192.168.1.20:8083/opds", "http://192.168.1.20:8083/opds" }, { "nas.local/opds", "http://nas.local/opds" }, { "calibre:8080/opds", "http://calibre:8080/opds" },
              { "opds://example.org/c.xml", "http://example.org/c.xml" }, { "http://user:secret@example.org/opds", "http://example.org/opds" },
              { "feed.php?u=http://x.example/y", "https://feed.php?u=http://x.example/y" },
              { "", 0 }, { "not a url", 0 }, { "ftp://example.org/x", 0 }, { "https://", 0 }, { "https://\"x\"", 0 }, { "://x", 0 } };
          for (unsigned i = 0; i < sizeof in / sizeof in[0]; i++) {
              char u[OPDS_URL]; int g, r2 = opds_url_norm(in[i][0], u, sizeof u, &g);
              CHECK(in[i][1] ? (!r2 && !strcmp(u, in[i][1])) : (r2 == -1 && !u[0]), "url_norm '%s' -> %d '%s'", in[i][0], r2, u);
          }
          char u[OPDS_URL]; int g; opds_url_norm("example.org/opds", u, sizeof u, &g); CHECK(g == 1, "guessed");
          opds_url_norm("http://example.org/opds", u, sizeof u, &g); CHECK(g == 0, "not guessed");
          char longu[900]; memset(longu, 'a', sizeof longu); longu[sizeof longu - 1] = 0; CHECK(opds_url_norm(longu, u, sizeof u, &g) == -1, "too long"); }
        /* the catalogs file: round trip, junk lines, duplicates, a "cur" that is not in the list */
        { static struct opds_cat c[OPDS_MAXCAT], d[OPDS_MAXCAT]; char cur[OPDS_URL], txt[OPDS_MAXCAT * 1200];
          memset(c, 0, sizeof c);
          strcpy(c[0].url, "https://www.textos.info/catalogo.atom"); strcpy(c[0].name, "textos.info"); strcpy(c[0].tpl, "https://www.textos.info/busqueda.atom&query={searchTerms}"); c[0].st = 1;
          strcpy(c[1].url, "http://192.168.1.20:8083/opds"); strcpy(c[1].name, "Calibre-Web"); c[1].st = 2;
          int l = opds_cats_format(c, 2, c[1].url, txt, sizeof txt);
          CHECK(l > 60 && !strncmp(txt, "cur\thttp://192.168.1.20:8083/opds\ncat\thttps://www.textos.info/catalogo.atom\t1\ttextos.info\t", 88), "cats format '%s'", txt);
          CHECK(opds_cats_parse(txt, d, OPDS_MAXCAT, cur, sizeof cur) == 2 && !strcmp(cur, c[1].url) && !strcmp(d[0].tpl, c[0].tpl) && !strcmp(d[1].name, "Calibre-Web") && d[1].st == 2 && d[0].st == 1 && !d[1].tpl[0], "cats round trip");
          char junk[] = "cur\thttps://gone.example/opds\r\n\n# comment\ncat\ncat\tnot-a-url\t1\tx\ncat\thttps://a.example/opds\t7\t\tjavascript:{searchTerms}\r\ncat\thttps://a.example/opds\t1\tdup\nxyz\t1\t2\t3\t4\t5\t6\ncat\thttps://b.example/o\t1\tB\thttps://b.example/s?q={searchTerms}\textra\tfields";
          int n2 = opds_cats_parse(junk, d, OPDS_MAXCAT, cur, sizeof cur);
          CHECK(n2 == 2 && !cur[0] && !strcmp(d[0].name, "a.example") && d[0].st == 0 && !d[0].tpl[0] && !strcmp(d[1].name, "B") && !strcmp(d[1].tpl, "https://b.example/s?q={searchTerms}"), "cats junk n=%d cur='%s' '%s' '%s'", n2, cur, d[0].name, d[1].tpl);
          char many[OPDS_MAXCAT * 80 + 400]; int k = 0; for (int i = 0; i < OPDS_MAXCAT + 5; i++) k += sprintf(many + k, "cat\thttps://h%d.example/opds\t1\tN%d\t\n", i, i);
          CHECK(opds_cats_parse(many, d, OPDS_MAXCAT, cur, sizeof cur) == OPDS_MAXCAT, "cats cap"); }
        /* the suggestions are well-formed catalog URLs with a name */
        for (int i = 0; i < OPDS_NSUGGEST; i++) { char u[OPDS_URL]; int g; CHECK(!opds_url_norm(OPDS_SUGGEST[i].url, u, sizeof u, &g) && !strcmp(u, OPDS_SUGGEST[i].url) && !g && OPDS_SUGGEST[i].name[0] && !strncmp(u, "https://", 8), "suggestion %d", i); }
        /* relative references (netxml.inc.c) */
        { static const char *const j[][3] = {
              { "https://h.example/opds/feed.php?page=1", "feed.php?page=2", "https://h.example/opds/feed.php?page=2" },
              { "https://h.example/opds/feed.php?page=1", "?page=3", "https://h.example/opds/feed.php?page=3" },
              { "https://h.example/opds/feed.php?page=1#x", "#top", "https://h.example/opds/feed.php?page=1#top" },
              { "https://h.example", "?q=1", "https://h.example/?q=1" },
              { "https://h.example/a/b/", "/c", "https://h.example/c" }, { "http://h.example/a/b", "//cdn.example/i.jpg", "http://cdn.example/i.jpg" },
              { "https://h.example/a/b", "feed.php?u=http://x.example/y", "https://h.example/a/feed.php?u=http://x.example/y" },
              { "https://h.example/a/b", "HTTP://other.example/z", "HTTP://other.example/z" }, { "https://h.example/a/b?x=/y/z", "c", "https://h.example/a/c" } };
          for (unsigned i = 0; i < sizeof j / sizeof j[0]; i++) { char u[300]; fm_url_join(j[i][0], j[i][1], u, sizeof u); CHECK(!strcmp(u, j[i][2]), "join '%s' + '%s' = '%s'", j[i][0], j[i][1], u); } }
    }

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
