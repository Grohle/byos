/* radio_src.inc.c - Radio: fuentes de emisoras (parte pura; se incluye al final de radio_core.inc.c,
 * la prueban tests/radio_test.c). docs/RADIO.md, "Sources".
 *   - las fuentes: el directorio Radio Browser (integrado, el de siempre), la lista integrada
 *     "Radio publica europea" (RA_EU, abajo) y las que anade el usuario: una URL de una lista
 *     M3U/M3U8 de emisoras, PLS, JSON (formato propio, abierto) u otro servidor compatible con
 *     Radio Browser
 *   - que es lo que se bajo (ra_src_sniff): una lista HLS no es una lista de emisoras sino UNA
 *     emision (se ofrece como emisora)
 *   - las listas -> emisoras (ra_parse_m3u / ra_parse_pls / ra_parse_jlist)
 *   - el fichero sources.tsv (pequeno: la memoria interna va justa; las listas bajadas van a la SD)
 * Mismo esquema que las fuentes de Vinyl (freemusic_src.inc.c). */
#ifndef RADIO_SRC_INC
#define RADIO_SRC_INC

enum { RSRC_RB, RSRC_EU, RSRC_M3U, RSRC_PLS, RSRC_JSON, RSRC_RBX, RSRC_URL /* aun sin probar */, RSRC_N };
#define RA_MAXSRC 12
struct ra_src {
    unsigned char type, on, named, busy;   /* named: el usuario le puso nombre; busy: cargando (no se guarda) */
    int n, err;                      /* emisoras de la ultima carga; err: 0, RSE_* */
    long long fetched;               /* time() de la ultima carga buena */
    unsigned sid;                    /* hash de la URL ("builtin:rb" / "builtin:eu") */
    char name[56], url[256];
};
struct ra_srcs { struct ra_src s[RA_MAXSRC]; int n; };
enum { RSE_OK = 0, RSE_NET = 1, RSE_EMPTY = 2, RSE_NOTLIST = 3 };

static unsigned ra_hash(const char *s) { unsigned h = 2166136261u; while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; } return h ? h : 1; }

/* ------------------------------------------------------------------ lista integrada
 * Emisiones oficiales y gratuitas de radios publicas europeas, comprobadas una a una desde aqui
 * con curl (que den audio, o una lista HLS cuyos segmentos den audio); la procedencia de cada
 * URL esta en docs/RADIO.md. RNE (Radio Nacional de Espana) ya solo emite en HLS. */
struct ra_eu { const char *name, *url, *cc, *codec; short br; const char *tags; };
static const struct ra_eu RA_EU[] = {
    /* España: RTVE (solo HLS, AAC en MPEG-TS), CCMA, EITB, RTVA, CRTVG, IB3 */
    { "RNE Radio 1", "https://rtvelivestream.rtve.es/rtvesec/rne/rne_r1_main.m3u8", "ES", "AAC", 192, "news, talk" },
    { "RNE Radio Clásica", "https://rtvelivestream.rtve.es/rtvesec/rne/rne_r2_main.m3u8", "ES", "AAC", 192, "classical" },
    { "RNE Radio 3", "https://rtvelivestream.rtve.es/rtvesec/rne/rne_r3_main.m3u8", "ES", "AAC", 192, "indie, alternative, pop" },
    { "RNE Ràdio 4", "https://rtvelivestream.rtve.es/rtvesec/rne/rne_r4_main.m3u8", "ES", "AAC", 192, "catalan, news, talk" },
    { "RNE Radio 5", "https://rtvelivestream.rtve.es/rtvesec/rne/rne_r5_madrid_main.m3u8", "ES", "AAC", 192, "news" },
    { "RNE Radio Exterior", "https://rtvelivestream.rtve.es/rtvesec/rne/rne_re_main.m3u8", "ES", "AAC", 192, "news, talk" },
    { "Catalunya Ràdio", "https://shoutcast.ccma.cat/ccma/catalunyaradioHD.mp3", "ES", "AAC", 128, "catalan, news, talk" },
    { "Catalunya Música", "https://shoutcast.ccma.cat/ccma/catalunyamusicaHD.mp3", "ES", "AAC", 128, "classical, catalan" },
    { "Catalunya Informació", "https://shoutcast.ccma.cat/ccma/catalunyainformacioHD.mp3", "ES", "AAC", 128, "news, catalan" },
    { "iCat", "https://shoutcast.ccma.cat/ccma/icatHD.mp3", "ES", "AAC", 128, "indie, catalan" },
    { "Radio Euskadi", "https://multimedia.eitb.eus/live-content/radioeuskadi-hls/master.m3u8", "ES", "AAC", 128, "news, talk" },
    { "Euskadi Irratia", "http://mp3-eitb.stream.flumotion.com/eitb/euskadiirratia.mp3", "ES", "MP3", 128, "basque, news, talk" },
    { "Gaztea", "https://multimedia.eitb.eus/live-content/gaztea-hls/master.m3u8", "ES", "AAC", 128, "pop, hits, basque" },
    { "Canal Sur Radio", "https://rtva-live-radio.flumotion.com/rtva/csr.mp3", "ES", "MP3", 320, "news, talk, andalusia" },
    { "Canal Fiesta Radio", "https://rtva-live-radio.flumotion.com/rtva/cfr.mp3", "ES", "MP3", 320, "pop, hits, andalusia" },
    { "Radio Galega", "https://crtvg-radiogalega-hls.flumotion.cloud/playlist.m3u8", "ES", "AAC", 192, "galician, news, talk" },
    { "IB3 Ràdio", "https://icecast.ib3radio.com/ib3radio.mp3", "ES", "MP3", 128, "catalan, news, talk" },
    /* Portugal: RTP (HLS, AAC empaquetado) */
    { "RTP Antena 1", "https://streaming-live.rtp.pt/liveradio/antena180a/playlist.m3u8", "PT", "AAC", 160, "news, talk" },
    { "RTP Antena 2", "https://streaming-live.rtp.pt/liveradio/antena280a/playlist.m3u8", "PT", "AAC", 256, "classical" },
    { "RTP Antena 3", "https://streaming-live.rtp.pt/liveradio/antena380a/playlist.m3u8", "PT", "AAC", 160, "pop, rock" },
    /* Francia: Radio France */
    { "France Inter", "https://icecast.radiofrance.fr/franceinter-midfi.mp3", "FR", "MP3", 128, "news, talk" },
    { "France Culture", "https://icecast.radiofrance.fr/franceculture-midfi.mp3", "FR", "MP3", 128, "culture, talk" },
    { "France Musique", "https://icecast.radiofrance.fr/francemusique-midfi.mp3", "FR", "MP3", 128, "classical, jazz" },
    { "FIP", "https://icecast.radiofrance.fr/fip-midfi.mp3", "FR", "MP3", 128, "eclectic, jazz, world" },
    { "FIP Jazz", "https://icecast.radiofrance.fr/fipjazz-midfi.mp3", "FR", "MP3", 128, "jazz" },
    { "franceinfo", "https://icecast.radiofrance.fr/franceinfo-midfi.mp3", "FR", "MP3", 128, "news" },
    { "Mouv'", "https://icecast.radiofrance.fr/mouv-midfi.mp3", "FR", "MP3", 128, "hip hop, rap" },
    /* Alemania: ARD (Deutschlandradio, BR, WDR, NDR, SWR, MDR, rbb, hr) */
    { "Deutschlandfunk", "https://st01.sslstream.dlf.de/dlf/01/128/mp3/stream.mp3", "DE", "MP3", 128, "news, talk" },
    { "Deutschlandfunk Kultur", "https://st02.sslstream.dlf.de/dlf/02/128/mp3/stream.mp3", "DE", "MP3", 128, "culture, talk" },
    { "Deutschlandfunk Nova", "https://st03.sslstream.dlf.de/dlf/03/128/mp3/stream.mp3", "DE", "MP3", 128, "talk, indie" },
    { "Bayern 2", "https://dispatcher.rndfnk.com/br/br2/live/mp3/mid", "DE", "MP3", 128, "culture, talk" },
    { "BR-Klassik", "https://dispatcher.rndfnk.com/br/brklassik/live/mp3/high", "DE", "MP3", 256, "classical" },
    { "BR24", "https://dispatcher.rndfnk.com/br/br24/live/mp3/mid", "DE", "MP3", 128, "news" },
    { "WDR 3", "https://wdr-wdr3-live.icecastssl.wdr.de/wdr/wdr3/live/mp3/256/stream.mp3", "DE", "MP3", 256, "classical, culture" },
    { "WDR 5", "https://wdr-wdr5-live.icecastssl.wdr.de/wdr/wdr5/live/mp3/128/stream.mp3", "DE", "MP3", 128, "talk, news" },
    { "1LIVE", "https://wdr-1live-live.icecastssl.wdr.de/wdr/1live/live/mp3/128/stream.mp3", "DE", "MP3", 128, "pop, hits" },
    { "NDR Kultur", "https://icecast.ndr.de/ndr/ndrkultur/live/mp3/128/stream.mp3", "DE", "MP3", 128, "classical, culture" },
    { "NDR Info", "https://icecast.ndr.de/ndr/ndrinfo/hamburg/mp3/128/stream.mp3", "DE", "MP3", 128, "news" },
    { "SWR Kultur", "https://liveradio.swr.de/sw282p3/swr2/play.mp3", "DE", "MP3", 256, "culture, classical" },
    { "MDR Kultur", "https://mdr-284310-0.sslcast.mdr.de/mdr/284310/0/mp3/high/stream.mp3", "DE", "MP3", 128, "culture, classical" },
    { "radioeins", "https://dispatcher.rndfnk.com/rbb/radioeins/live/mp3/mid", "DE", "MP3", 128, "rock, indie" },
    { "hr2-kultur", "https://dispatcher.rndfnk.com/hr/hr2/live/mp3/high", "DE", "MP3", 256, "culture, classical" },
    /* Italia: RAI */
    { "Rai Radio 1", "https://icestreaming.rai.it/1.mp3", "IT", "MP3", 256, "news, talk, sports" },
    { "Rai Radio 2", "https://icestreaming.rai.it/2.mp3", "IT", "MP3", 256, "pop, talk" },
    { "Rai Radio 3", "https://icestreaming.rai.it/3.mp3", "IT", "MP3", 320, "classical, culture" },
    { "Rai Radio Classica", "https://icestreaming.rai.it/5.mp3", "IT", "MP3", 256, "classical" },
    /* Reino Unido: BBC (las de "ww" se oyen fuera del Reino Unido) */
    { "BBC Radio 3", "http://as-hls-ww-live.akamaized.net/pool_23461179/live/ww/bbc_radio_three/bbc_radio_three.isml/bbc_radio_three-audio%3d128000.norewind.m3u8", "GB", "AAC", 128, "classical, jazz" },
    { "BBC Radio 4", "http://as-hls-ww-live.akamaized.net/pool_55057080/live/ww/bbc_radio_fourfm/bbc_radio_fourfm.isml/bbc_radio_fourfm-audio%3d128000.norewind.m3u8", "GB", "AAC", 128, "news, talk, drama" },
    { "BBC World Service", "http://stream.live.vc.bbcmedia.co.uk/bbc_world_service", "GB", "MP3", 56, "news" },
    /* Irlanda: RTÉ */
    { "RTÉ Radio 1", "https://icecast.rte.ie/radio1", "IE", "MP3", 160, "news, talk" },
    { "RTÉ lyric fm", "https://icecast.rte.ie/lyric", "IE", "MP3", 160, "classical" },
    { "RTÉ 2fm", "https://icecast.rte.ie/2fm", "IE", "MP3", 160, "pop, hits" },
    { "RTÉ Raidió na Gaeltachta", "https://icecast.rte.ie/rnag", "IE", "MP3", 160, "irish, talk" },
    /* Paises Bajos: NPO */
    { "NPO Radio 1", "https://icecast.omroep.nl/radio1-bb-mp3", "NL", "MP3", 192, "news, talk" },
    { "NPO Radio 2", "https://icecast.omroep.nl/radio2-bb-mp3", "NL", "MP3", 192, "pop, hits" },
    { "NPO 3FM", "https://icecast.omroep.nl/3fm-bb-mp3", "NL", "MP3", 192, "pop, rock" },
    { "NPO Klassiek", "https://icecast.omroep.nl/radio4-bb-mp3", "NL", "MP3", 192, "classical" },
    /* Belgica: VRT y RTBF */
    { "VRT Radio 1", "http://icecast.vrtcdn.be/radio1-high.mp3", "BE", "MP3", 128, "news, talk" },
    { "Klara", "http://icecast.vrtcdn.be/klara-high.mp3", "BE", "MP3", 128, "classical" },
    { "Studio Brussel", "http://icecast.vrtcdn.be/stubru-high.mp3", "BE", "MP3", 128, "rock, indie" },
    { "MNM", "http://icecast.vrtcdn.be/mnm-high.mp3", "BE", "MP3", 128, "pop, hits" },
    { "La Première", "https://radios.rtbf.be/laprem1ere-128.mp3", "BE", "MP3", 160, "news, talk" },
    { "Musiq3", "https://radios.rtbf.be/musiq3-128.mp3", "BE", "MP3", 320, "classical" },
    { "Classic 21", "https://radios.rtbf.be/classic21-128.mp3", "BE", "MP3", 160, "rock, classic rock" },
    /* Suiza: SRG SSR */
    { "SRF 1", "http://stream.srg-ssr.ch/m/drs1/mp3_128", "CH", "MP3", 128, "news, talk" },
    { "SRF 2 Kultur", "http://stream.srg-ssr.ch/m/drs2/mp3_128", "CH", "MP3", 128, "classical, culture" },
    { "RTS La 1ère", "http://stream.srg-ssr.ch/m/la-1ere/mp3_128", "CH", "MP3", 128, "news, talk" },
    { "RTS Espace 2", "http://stream.srg-ssr.ch/m/espace-2/mp3_128", "CH", "MP3", 128, "classical, culture" },
    { "RSI Rete Due", "http://stream.srg-ssr.ch/m/retedue/mp3_128", "CH", "MP3", 128, "classical, culture" },
    { "Radio Swiss Classic", "http://stream.srg-ssr.ch/m/rsc_de/mp3_128", "CH", "MP3", 128, "classical" },
    { "Radio Swiss Jazz", "http://stream.srg-ssr.ch/m/rsj/mp3_128", "CH", "MP3", 128, "jazz" },
    /* Austria: ORF */
    { "Ö1", "https://orf-live.ors-shoutcast.at/oe1-q2a", "AT", "MP3", 192, "classical, culture, news" },
    { "FM4", "https://orf-live.ors-shoutcast.at/fm4-q2a", "AT", "MP3", 192, "indie, alternative" },
    { "Hitradio Ö3", "https://orf-live.ors-shoutcast.at/oe3-q2a", "AT", "MP3", 192, "pop, hits" },
    /* Nordicos: DR, NRK, SR, Yle, RÚV */
    { "DR P1", "https://live-icy.dr.dk/A/A03H.mp3", "DK", "MP3", 128, "news, talk" },
    { "DR P2 Klassisk", "https://live-icy.dr.dk/A/A04H.mp3", "DK", "MP3", 128, "classical" },
    { "DR P3", "https://live-icy.dr.dk/A/A05H.mp3", "DK", "MP3", 128, "pop, hits" },
    { "DR P8 Jazz", "https://live-icy.dr.dk/A/A22H.mp3", "DK", "MP3", 128, "jazz" },
    { "NRK P1", "https://lyd.nrk.no/nrk_radio_p1_ostlandssendingen_mp3_h", "NO", "MP3", 192, "pop, talk" },
    { "NRK P2", "https://lyd.nrk.no/nrk_radio_p2_mp3_h", "NO", "MP3", 192, "culture, talk" },
    { "NRK Klassisk", "https://lyd.nrk.no/nrk_radio_klassisk_mp3_h", "NO", "MP3", 192, "classical" },
    { "NRK Jazz", "https://lyd.nrk.no/nrk_radio_jazz_mp3_h", "NO", "MP3", 192, "jazz" },
    { "Sveriges Radio P1", "https://sverigesradio.se/topsy/direkt/srapi/132.mp3", "SE", "MP3", 128, "news, talk" },
    { "Sveriges Radio P2", "https://sverigesradio.se/topsy/direkt/srapi/163.mp3", "SE", "MP3", 224, "classical, jazz" },
    { "Sveriges Radio P3", "https://sverigesradio.se/topsy/direkt/srapi/164.mp3", "SE", "MP3", 128, "pop, hits" },
    { "Yle Radio 1", "https://icecast.live.yle.fi/radio/YleRadio1/icecast.audio", "FI", "AAC", 128, "classical, culture" },
    { "Yle Klassinen", "https://icecast.live.yle.fi/radio/YleKlassinen/icecast.audio", "FI", "AAC", 128, "classical" },
    { "YleX", "https://icecast.live.yle.fi/radio/YleX/icecast.audio", "FI", "AAC", 128, "pop, hits" },
    { "RÚV Rás 1", "https://ruv-radio-live.akamaized.net/streymi/ras1/ras1/ras1.m3u8", "IS", "AAC", 320, "culture, news" },
    { "RÚV Rás 2", "https://ruv-radio-live.akamaized.net/streymi/ras2/ras2/ras2.m3u8", "IS", "AAC", 320, "pop, rock" },
    /* Centro y este: Polskie Radio, Český rozhlas, RTVS, MTVA, RTV SLO, HRT, ERR, ERT */
    { "Polskie Radio Jedynka", "https://stream11.polskieradio.pl/pr1/pr1.sdp/playlist.m3u8", "PL", "AAC", 192, "news, talk" },
    { "Polskie Radio Trójka", "https://stream13.polskieradio.pl/pr3/pr3.sdp/playlist.m3u8", "PL", "AAC", 192, "rock, pop, talk" },
    { "ČRo Radiožurnál", "https://rozhlas.stream/radiozurnal_mp3_128.mp3", "CZ", "MP3", 128, "news" },
    { "ČRo Vltava", "https://rozhlas.stream/vltava_mp3_128.mp3", "CZ", "MP3", 128, "classical, culture" },
    { "ČRo Radio Wave", "https://rozhlas.stream/wave_mp3_128.mp3", "CZ", "MP3", 128, "indie, electronic" },
    { "Rádio Devín", "https://icecast.stv.livebox.sk/devin_128.mp3", "SK", "MP3", 128, "classical, culture" },
    { "Bartók Rádió", "https://mr-stream.connectmedia.hu/4741/mr3.mp3", "HU", "MP3", 320, "classical" },
    { "Kossuth Rádió", "https://icast.connectmedia.hu/4736/mr1.mp3", "HU", "MP3", 192, "news, talk" },
    { "Radio Slovenija Ars", "https://mp3.rtvslo.si/ars", "SI", "AAC", 192, "classical, culture" },
    { "Radio Slovenija Prvi", "https://mp3.rtvslo.si/ra1", "SI", "AAC", 192, "news, talk" },
    { "HRT HR3", "https://playerservices.streamtheworld.com/api/livestream-redirect/PROGRAM3.mp3", "HR", "MP3", 128, "classical, culture" },
    { "Klassikaraadio", "https://icecast.err.ee/klassikaraadio.mp3", "EE", "MP3", 128, "classical" },
    { "Vikerraadio", "https://icecast.err.ee/vikerraadio.mp3", "EE", "MP3", 128, "news, talk" },
    { "ERT Proto", "https://radiostreaming.ert.gr/ert-proto", "GR", "MP3", 256, "news, talk" },
    { "ERT Trito", "https://radiostreaming.ert.gr/ert-trito", "GR", "MP3", 256, "classical, culture" },
};
#define RA_NEU ((int)(sizeof RA_EU / sizeof RA_EU[0]))

/* ------------------------------------------------------------------ texto -> emisoras */
static void ra_st_set(struct ra_station *s, const char *name, const char *url, const char *cc, const char *codec, int br, const char *tags)
{
    memset(s, 0, sizeof *s);
    fm_cpy(s->name, sizeof s->name, name); ra_clean(s->name);
    fm_cpy(s->url, sizeof s->url, url);
    if (!s->name[0]) ra_url_host(s->url, s->name, sizeof s->name);
    if (cc && strlen(cc) == 2 && isalpha((unsigned char)cc[0]) && isalpha((unsigned char)cc[1])) { s->cc[0] = (char)toupper((unsigned char)cc[0]); s->cc[1] = (char)toupper((unsigned char)cc[1]); }
    if (codec) fm_cpy(s->codec, sizeof s->codec, codec);
    if (tags) { char t[400]; fm_cpy(t, sizeof t, tags); ra_clean(t); ra_tags(t, s->tags, sizeof s->tags, 3); }
    s->br = (short)(br > 0 && br < 4000 ? br : 0);
    int hls = rs_url_kind(s->url) == RS_HLS;
    s->kind = (unsigned char)ra_codec_kind(s->codec, hls);
    if (!hls && s->kind == RS_UNKNOWN) { int k = rs_url_kind(s->url); if (k == RS_MP3 || k == RS_AAC) s->kind = (unsigned char)k; }
    for (char *c = s->codec; *c; c++) *c = (char)toupper((unsigned char)*c);
}
static void ra_eu_station(int i, struct ra_station *s)
{
    const struct ra_eu *e = &RA_EU[i];
    ra_st_set(s, e->name, e->url, e->cc, e->codec, e->br, e->tags);
}
/* anade s a o[] si cabe, suena y su URL no esta ya; 1 = anadida */
static int ra_st_push(struct ra_station *o, int *n, int max, const struct ra_station *s)
{
    if (*n >= max || !s->name[0] || !rs_url_ok(s->url) || strchr(s->url, ' ') || !ra_playable(s->kind)) return 0;
    for (int k = 0; k < *n; k++) if (!strcmp(o[k].url, s->url)) return 0;
    o[(*n)++] = *s;
    return 1;
}
/* siguiente linea de t (sin \r\n ni espacios en los extremos) en l; 0 = no quedan */
static int ra_nextline(const char **t, char *l, int cap)
{
    const char *p = *t;
    if (!p || !*p) return 0;
    int k = 0;
    while (*p && *p != '\n' && *p != '\r') { if (k < cap - 1) l[k++] = *p; p++; }
    while (*p == '\n' || *p == '\r') p++;
    l[k] = 0; rs_trim(l);
    *t = p;
    return 1;
}
/* M3U extendida (#EXTINF:-1 atributos,Nombre + la URL en la linea siguiente) o una lista de URLs
 * sin mas. -2 = es una lista HLS (#EXT-X-...): una emision, no una lista de emisoras. */
static int ra_parse_m3u(const char *t, struct ra_station *o, int max)
{
    int n = 0; char l[RA_URLN + 400], name[96] = "", cc[8] = "", grp[80] = "";
    if (t && (unsigned char)t[0] == 0xEF && (unsigned char)t[1] == 0xBB && (unsigned char)t[2] == 0xBF) t += 3;
    while (ra_nextline(&t, l, sizeof l)) {
        if (!l[0]) continue;
        if (!strncasecmp(l, "#EXT-X-", 7)) return -2;
        if (!strncasecmp(l, "#EXTINF:", 8)) {
            const char *p = l + 8; int q = 0;                       /* el nombre va tras la primera coma fuera de comillas */
            for (; *p && (q || *p != ','); p++) if (*p == '"') q = !q;
            fm_cpy(name, sizeof name, *p ? p + 1 : ""); rs_trim(name);
            char v[80];
            if (rs_hls_attr(l, "tvg-country", v, sizeof v) || rs_hls_attr(l, "country", v, sizeof v)) fm_cpy(cc, sizeof cc, v); else cc[0] = 0;
            if (rs_hls_attr(l, "group-title", v, sizeof v)) fm_cpy(grp, sizeof grp, v); else grp[0] = 0;
            /* rs_hls_attr busca tras ':' o ','; los atributos de #EXTINF van tras un espacio */
            if (!cc[0] || !grp[0]) {
                const char *a;
                if (!cc[0] && (a = strcasestr(l, "tvg-country=\""))) { a += 13; int k = (int)strcspn(a, "\""); if (k < (int)sizeof cc) { memcpy(cc, a, k); cc[k] = 0; } }
                if (!grp[0] && (a = strcasestr(l, "group-title=\""))) { a += 13; int k = (int)strcspn(a, "\""); if (k < (int)sizeof grp) { memcpy(grp, a, k); grp[k] = 0; } }
            }
            continue;
        }
        if (l[0] == '#') continue;
        if (rs_url_ok(l) && strlen(l) < RA_URLN) {
            struct ra_station s; ra_st_set(&s, name, l, cc, "", 0, grp);
            ra_st_push(o, &n, max, &s);
        }
        name[0] = cc[0] = grp[0] = 0;
    }
    return n;
}
/* PLS: FileN=URL, TitleN=Nombre (en cualquier orden) */
static int ra_parse_pls(const char *t, struct ra_station *o, int max)
{
    enum { NPLS = 64 };
    struct { char url[RA_URLN], title[96]; } *e = calloc(NPLS, sizeof *e);
    if (!e) return 0;
    char l[RA_URLN + 64]; int top = 0;
    while (ra_nextline(&t, l, sizeof l)) {
        int file = !strncasecmp(l, "File", 4), title = !strncasecmp(l, "Title", 5);
        if (!file && !title) continue;
        const char *p = l + (file ? 4 : 5); int i = atoi(p);
        const char *v = strchr(l, '='); if (!v || i < 1 || i > NPLS) continue;
        v++; while (*v == ' ') v++;
        if (file && strlen(v) < RA_URLN) snprintf(e[i - 1].url, sizeof e[i - 1].url, "%s", v);
        if (title) fm_cpy(e[i - 1].title, sizeof e[i - 1].title, v);
        if (i > top) top = i;
    }
    int n = 0;
    for (int i = 0; i < top; i++) {
        if (!rs_url_ok(e[i].url)) continue;
        struct ra_station s; ra_st_set(&s, e[i].title, e[i].url, "", "", 0, "");
        ra_st_push(o, &n, max, &s);
    }
    free(e);
    return n;
}
/* JSON (docs/RADIO.md):
 *   { "hibyos_radio": 1, "name": "Mi lista", "stations": [
 *       { "name": "...", "url": "https://...", "country": "ES", "codec": "AAC", "bitrate": 128, "tags": "jazz, soul" } ] }
 * o directamente el array de emisoras. Tambien vale una respuesta de Radio Browser (url_resolved,
 * countrycode, tags...). title: el "name" de la lista ("" si no trae). -1 = no es una lista. */
static int ra_parse_jlist(const char *js, struct ra_station *o, int max, char *title, int tcap)
{
    if (title && tcap) title[0] = 0;
    const char *p = fj_w(js), *arr = 0;
    if (!p) return -1;
    if (*p == '{') {
        if (title) { fj_gs(js, "name", title, tcap); ra_clean(title); }
        arr = fj_get(js, "stations");
        if (!arr || *fj_w(arr) != '[') return -1;
    } else if (*p == '[') arr = p;
    else return -1;
    int n = 0, tot = 0;
    for (const char *e = fj_first(arr); e; e = fj_next(e)) {
        tot++;
        if (*fj_w(e) != '{') continue;
        char name[200], url[1100], cc[16], codec[16], tags[400], num[16];
        fj_gs(e, "url_resolved", url, sizeof url); ra_clean(url);
        if (!rs_url_ok(url)) { fj_gs(e, "url", url, sizeof url); ra_clean(url); }
        if (!rs_url_ok(url)) { fj_gs(e, "stream", url, sizeof url); ra_clean(url); }
        if (!rs_url_ok(url) || strlen(url) >= RA_URLN) continue;
        fj_gs(e, "name", name, sizeof name);
        fj_gs(e, "country", cc, sizeof cc); if (strlen(cc) != 2) fj_gs(e, "countrycode", cc, sizeof cc);
        fj_gs(e, "codec", codec, sizeof codec);
        fj_gs(e, "tags", tags, sizeof tags);
        long long br = fj_gl(e, "bitrate");
        struct ra_station s; ra_st_set(&s, name, url, cc, codec, (int)br, tags);
        fj_gs(e, "hls", num, sizeof num);
        if (atoi(num) && s.kind != RS_HLS) s.kind = (unsigned char)ra_codec_kind(s.codec, 1);
        ra_st_push(o, &n, max, &s);
    }
    return tot || n ? n : 0;
}
/* lo que se bajo de una fuente: RSRC_M3U / RSRC_PLS / RSRC_JSON / RSRC_RBX (respuesta de
 * /json/stats de un servidor Radio Browser), -2 una emision HLS, -1 otra cosa */
static int ra_src_sniff(const char *b)
{
    if (!b) return -1;
    const char *p = b;
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (!strncasecmp(p, "#EXTM3U", 7)) return strstr(p, "#EXT-X-") ? -2 : RSRC_M3U;
    if (!strncasecmp(p, "[playlist]", 10)) return RSRC_PLS;
    if (*p == '{') {
        if (fj_get(p, "supported_version") && fj_get(p, "software_version")) return RSRC_RBX;
        return fj_get(p, "stations") ? RSRC_JSON : -1;
    }
    if (*p == '[') return RSRC_JSON;
    if (!strncasecmp(p, "http://", 7) || !strncasecmp(p, "https://", 8)) return RSRC_M3U;
    return -1;
}
/* texto bajado -> emisoras segun su tipo; -2 HLS, -1 no es una lista */
static int ra_src_parse(int type, const char *b, struct ra_station *o, int max, char *title, int tcap)
{
    if (title && tcap) title[0] = 0;
    int k = ra_src_sniff(b);
    if (k == -2) return -2;
    if (k < 0) return -1;
    if (type == RSRC_URL || type != k) type = k;
    if (type == RSRC_M3U) return ra_parse_m3u(b, o, max);
    if (type == RSRC_PLS) return ra_parse_pls(b, o, max);
    if (type == RSRC_JSON) return ra_parse_jlist(b, o, max, title, tcap);
    return -1;
}

/* ------------------------------------------------------------------ las fuentes */
/* lo que el usuario escribio -> URL normalizada y tipo supuesto por la extension (se confirma
 * al bajarla). La raiz de un servidor ("https://radio.example.org") se toma por un servidor
 * compatible con Radio Browser. -1 = no es una URL */
static int ra_src_detect(const char *in, char *url, int cap)
{
    if (ra_url_norm(in, url, cap)) { if (cap) url[0] = 0; return -1; }
    if (!strncmp(url, "http://", 7) && !strncasecmp(in, "Https://", 8)) { memmove(url + 8, url + 7, strlen(url + 7) + 1); memcpy(url, "https://", 8); }
    char p[RA_URLN]; const char *q = strchr(strstr(url, "://") + 3, '/');
    snprintf(p, sizeof p, "%s", q ? q : "/"); p[strcspn(p, "?#")] = 0;
    for (char *c = p; *c; c++) *c = (char)tolower((unsigned char)*c);
    int pl = (int)strlen(p);
#define RA_ENDS(x) (pl >= (int)sizeof(x) - 1 && !strcmp(p + pl - (int)sizeof(x) + 1, x))
    if (RA_ENDS(".m3u") || RA_ENDS(".m3u8")) return RSRC_M3U;
    if (RA_ENDS(".pls")) return RSRC_PLS;
    if (RA_ENDS(".json")) return RSRC_JSON;
#undef RA_ENDS
    if (!strcmp(p, "/") || !strcmp(p, "/json") || !strcmp(p, "/json/")) {
        char *e = strstr(url, "://") + 3; e += strcspn(e, "/?#"); *e = 0;      /* solo el servidor */
        return RSRC_RBX;
    }
    return RSRC_URL;
}
static struct ra_src *ra_src_add(struct ra_srcs *S, int type, const char *url, const char *name)
{
    if (S->n >= RA_MAXSRC) return 0;
    struct ra_src *s = &S->s[S->n++]; memset(s, 0, sizeof *s);
    s->type = (unsigned char)type; s->on = 1;
    fm_cpy(s->url, sizeof s->url, url);
    s->sid = type == RSRC_RB ? ra_hash("builtin:rb") : type == RSRC_EU ? ra_hash("builtin:eu") : ra_hash(url);
    if (name && name[0]) { fm_cpy(s->name, sizeof s->name, name); ra_clean(s->name); s->named = 1; }
    else if (type == RSRC_RB) snprintf(s->name, sizeof s->name, "Radio Browser");
    else if (type != RSRC_EU) ra_url_host(url, s->name, sizeof s->name);
    if (type == RSRC_EU) s->n = RA_NEU;
    return s;
}
static int ra_src_ix(const struct ra_srcs *S, unsigned sid) { for (int i = 0; i < S->n; i++) if (S->s[i].sid == sid) return i; return -1; }
static int ra_src_find_url(const struct ra_srcs *S, const char *url) { for (int i = 0; i < S->n; i++) if (S->s[i].type > RSRC_EU && !strcasecmp(S->s[i].url, url)) return i; return -1; }
static void ra_src_del(struct ra_srcs *S, int i)
{
    if (i < 0 || i >= S->n || S->s[i].type <= RSRC_EU) return;
    memmove(&S->s[i], &S->s[i + 1], (size_t)(S->n - i - 1) * sizeof S->s[0]); S->n--;
}
/* sources.tsv: tipo activa nombrada emisoras cargada err nombre url (una por linea) */
static void ra_srcs_write(const struct ra_srcs *S, FILE *f)
{
    fputs("# HiByOS Radio sources 1: type on named stations fetched err name url\n", f);
    for (int i = 0; i < S->n; i++) {
        const struct ra_src *s = &S->s[i];
        fprintf(f, "%d\t%d\t%d\t%d\t%lld\t%d\t", s->type, s->on, s->named, s->n, s->fetched, s->err);
        for (const char *c = s->named ? s->name : ""; *c; c++) fputc(*c == '\t' || *c == '\n' || *c == '\r' ? ' ' : *c, f);
        fputc('\t', f);
        for (const char *c = s->url; *c; c++) fputc(*c == '\t' || *c == '\n' || *c == '\r' ? ' ' : *c, f);
        fputc('\n', f);
    }
}
/* las integradas siempre estan: Radio Browser la primera y la europea la segunda si faltan */
static void ra_srcs_parse(struct ra_srcs *S, char *text)
{
    memset(S, 0, sizeof *S);
    for (char *l = text, *nx; l && *l; l = nx) {
        nx = strchr(l, '\n'); if (nx) *nx++ = 0;
        l[strcspn(l, "\r")] = 0;
        if (l[0] == '#' || !l[0]) continue;
        char *f[8]; int nf = 0; f[nf++] = l;
        for (char *p = l; *p && nf < 8; p++) if (*p == '\t') { *p = 0; f[nf++] = p + 1; }
        if (nf < 8) continue;
        int type = atoi(f[0]); if (type < 0 || type >= RSRC_N) continue;
        if (type > RSRC_EU && (!rs_url_ok(f[7]) || strlen(f[7]) >= sizeof S->s[0].url || ra_src_find_url(S, f[7]) >= 0)) continue;
        int dup = 0; for (int i = 0; i < S->n; i++) if (type <= RSRC_EU && S->s[i].type == type) dup = 1;
        if (dup) continue;
        struct ra_src *s = ra_src_add(S, type, type > RSRC_EU ? f[7] : "", atoi(f[2]) ? f[6] : "");
        if (!s) break;
        s->on = atoi(f[1]) != 0; s->fetched = atoll(f[4]); s->err = atoi(f[5]);
        if (type != RSRC_EU) s->n = atoi(f[3]);
    }
    int rb = -1, eu = -1;
    for (int i = 0; i < S->n; i++) { if (S->s[i].type == RSRC_RB) rb = i; if (S->s[i].type == RSRC_EU) eu = i; }
    if (rb < 0 && S->n < RA_MAXSRC) { struct ra_src *s = ra_src_add(S, RSRC_RB, "", 0); struct ra_src t = *s; memmove(&S->s[1], &S->s[0], (size_t)(S->n - 1) * sizeof t); S->s[0] = t; if (eu >= 0) eu++; }
    if (eu < 0 && S->n < RA_MAXSRC) { struct ra_src *s = ra_src_add(S, RSRC_EU, "", 0); struct ra_src t = *s; memmove(&S->s[2], &S->s[1], (size_t)(S->n - 2) * sizeof t); S->s[1] = t; }
}
#endif
