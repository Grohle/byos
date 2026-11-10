/* buds_test.c - PC tests of the headphone protocols (docs/HEADPHONES.md):
 *   CRCs, frame builders and parsers of every brand module (Apple AAP, Samsung legacy/new,
 *   Nothing/CMF, Sony MDR V1/V2, Fast Pair Message Stream), stream reassembly with random
 *   splits, garbage resync, fuzzing (ASan/UBSan), model identification, capability tables,
 *   state file and command round trips, the BlueZ dump parser, SDP and sdptool parsers.
 * Vectors: ours, built from the documented layouts, or the frames published in
 * docs/research_headphones_nothing_sony_jbl.md. All SYNTHETIC: replace / extend them with
 * captures of the owner's hardware (device test plan in docs/HEADPHONES.md).
 * Run: bash tests/buds_test.sh */
#define main budsd_main
#include "../src/hibybudsd.c"
#undef main

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* capture of what a module sends */
static uint8_t cap[16][600]; static int capn[16], ncap;
static int cap_tx(struct bud *b, const uint8_t *p, int n) { (void)b; if (ncap < 16) { memcpy(cap[ncap], p, (size_t)n); capn[ncap++] = n; } return n; }
static void quiet(struct bud *b, const char *m) { (void)b; if (getenv("BUDS_TEST_LOG")) printf("    log: %s\n", m); }
static void mkbud(struct bud *b, const struct buds_model *m, const struct buds_brand *br)
{
    memset(b, 0, sizeof *b); buds_st_init(&b->s); bud_set_model(b, m); b->br = br; if (br->fill) br->fill(b);
    b->tx = cap_tx; b->log = quiet; ncap = 0; b->now = 1000;
}
static int hexb(const char *h, uint8_t *o)
{
    int n = 0; unsigned v;
    while (*h) { while (*h == ' ') h++; if (!*h) break; if (sscanf(h, "%2x", &v) != 1) break; o[n++] = (uint8_t)v; h += 2; }
    return n;
}
static int same(const uint8_t *a, int an, const char *hex) { uint8_t b[600]; int bn = hexb(hex, b); return an == bn && !memcmp(a, b, (size_t)an); }
static void feed_split(struct bud *b, const uint8_t *p, int n, unsigned seed)
{
    for (int i = 0; i < n; ) { int k = 1 + (int)((seed = seed * 1103515245u + 12345u) >> 16) % 7; if (i + k > n) k = n - i; b->br->rx(b, p + i, k); i += k; }
}

static void t_crc(void)
{
    CHECK(buds_crc16((const uint8_t *)"123456789", 9) == 0x31C3, "XMODEM check value");
    CHECK(nt_crc16((const uint8_t *)"123456789", 9) == 0x4B37, "MODBUS check value");
    uint8_t f[16]; int n = hexb("55 60 01 0F F0 03 00 CB 01 07 00", f);   /* published Ear (2) frame */
    CHECK(nt_crc16(f, n) == 0xAFC5, "Nothing published transparency frame CRC %04x", nt_crc16(f, n));
}
static void t_apple(void)
{
    struct bud b; mkbud(&b, aap_model_by_id(0x2014), &BRAND_APPLE);
    aap_start(&b);
    CHECK(ncap == 3 && same(cap[0], capn[0], "00 00 04 00 01 00 02 00 00 00 00 00 00 00 00 00"), "hello");
    CHECK(same(cap[2], capn[2], "04 00 04 00 0F 00 FF FF FF FF"), "notification request");
    uint8_t p[64]; int n = hexb("04 00 04 00 04 00 03 02 01 64 02 01 04 01 5A 01 01 08 01 1E 04 01", p);
    aap_rx(&b, p, n);
    CHECK(b.s.batt[1] == 100 && b.s.batt[0] == 90 && b.s.chg[0] == 1 && b.s.batt[2] == -1, "battery R=%d L=%d chgL=%d case=%d", b.s.batt[1], b.s.batt[0], b.s.chg[0], b.s.batt[2]);
    n = hexb("04 00 04 00 06 00 00 01", p); aap_rx(&b, p, n);
    CHECK(b.s.wear[0] == BW_IN && b.s.wear[1] == BW_OUT, "ear");
    n = hexb("04 00 04 00 09 00 0D 03 00 00 00", p); aap_rx(&b, p, n);
    CHECK(b.s.noise == BN_AMBIENT, "listening mode transparency");
    n = hexb("04 00 04 00 09 00 1A 0A 00 00 00", p); aap_rx(&b, p, n);
    CHECK(b.s.cycle == (BUDS_MASK(BN_ANC) | BUDS_MASK(BN_ADAPTIVE)), "cycle %d", b.s.cycle);
    struct bcmd c = { 1, "", BCMD_NOISE, BN_ADAPTIVE, 0, 0, "" }; ncap = 0;
    CHECK(aap_cmd(&b, &c) == BE_OK && same(cap[0], capn[0], "04 00 04 00 09 00 0D 04 00 00 00"), "set adaptive");
    c.op = BCMD_CYCLE; c.a = BUDS_MASK(BN_ANC);
    CHECK(aap_cmd(&b, &c) == BE_RANGE, "cycle needs two modes");
    c.op = BCMD_CONV; c.a = 0; ncap = 0;
    CHECK(aap_cmd(&b, &c) == BE_OK && same(cap[0], capn[0], "04 00 04 00 09 00 28 02 00 00 00"), "CA off");
    c.op = BCMD_EQ; CHECK(aap_cmd(&b, &c) == BE_NOTSUP, "no EQ on AirPods");
    /* metadata, name-only identification refined by the model number */
    struct bd_dev d; memset(&d, 0, sizeof d); strcpy(d.name, "Alberto's AirPods Pro");
    const struct buds_model *m = aap_identify(&d);
    CHECK(m && m->family == BF_PODS_PRO && !m->id, "name fallback");
    mkbud(&b, m, &BRAND_APPLE);
    static const char meta[] = "\x04\x00\x04\x00\x1d\x00\x02\xed\x00" "AirPods Pro\0A2084\0Apple Inc.\0GX8ABCDEF12\0" "6F21\0" "6F21";
    aap_rx(&b, (const uint8_t *)meta, (int)sizeof meta);
    CHECK(!strcmp(b.s.modelno, "A2084") && !strcmp(b.s.fw, "6F21") && !strcmp(b.s.serial, "GX8ABCDEF12") && b.s.model_id == 0x200E, "metadata %s %s %s %x", b.s.modelno, b.s.fw, b.s.serial, b.s.model_id);
    strcpy(d.modalias, "bluetooth:v004Cp2024d0B10"); d.name[0] = 0;
    CHECK(aap_identify(&d) == aap_model_by_id(0x2024), "modalias");
}
static void t_samsung(void)
{
    struct bud b; uint8_t f[300], pl[40];
    CHECK(sam_frame(0, 0, 0x78, (const uint8_t *)"\x01", 1, 0, f, sizeof f) == 8, "frame length");
    uint16_t crc = buds_crc16((const uint8_t *)"\x78\x01", 2);
    CHECK(f[0] == 0xFD && f[1] == 4 && f[2] == 0 && f[3] == 0x78 && f[5] == (crc & 0xFF) && f[7] == 0xDD, "new framing");
    CHECK(sam_frame(1, 1, 0x61, (const uint8_t *)"\x01", 1, 1, f, sizeof f) == 8 && f[0] == 0xFE && f[1] == 1 && f[6] == (crc = buds_crc16((const uint8_t *)"\x61\x01", 2), crc & 0xFF) && f[5] == crc >> 8 && f[7] == 0xEE, "legacy framing, BE crc");
    /* identification */
    struct bd_dev d; memset(&d, 0, sizeof d); strcpy(d.name, "Galaxy Buds2 Pro (A1B2)"); strcpy(d.uuid[0], SAM_UUID_NEW); d.nuuid = 1;
    CHECK(sam_identify(&d) && sam_identify(&d)->id == 7, "Buds2 Pro");
    strcpy(d.name, "Galaxy Buds3 Pro (77)"); CHECK(sam_identify(&d)->id == 11, "Buds3 Pro");
    strcpy(d.name, "Galaxy Buds (1234)"); strcpy(d.uuid[0], SAM_UUID_LEGACY); CHECK(sam_identify(&d)->layout == SL_LEGACY, "Buds 2019");
    strcpy(d.name, "My buds"); strcpy(d.uuid[0], SAM_UUID_NEW); CHECK(sam_identify(&d) == &SAMSUNG_GENERIC, "renamed: generic");
    strcpy(d.name, "Buds2"); d.nuuid = 0; CHECK(!sam_identify(&d), "another brand's Buds2");
    /* extended status, BE crc learnt, then a command uses BE */
    mkbud(&b, sam_model_by_id(7), &BRAND_SAMSUNG);
    memset(pl, 0, sizeof pl); pl[0] = 0x0B; pl[2] = 66; pl[3] = 70; pl[6] = 0x13; pl[7] = 40; pl[9] = 2; pl[10] = 1; pl[11] = 0x13; pl[12] = 1;
    int n = sam_frame(0, 1, 0x61, pl, 20, 1, f, sizeof f);
    feed_split(&b, f, n, 7);
    CHECK(b.s.batt[0] == 66 && b.s.batt[1] == 70 && b.s.batt[2] == 40 && b.s.wear[0] == BW_IN && b.s.wear[1] == BW_CASE, "ext battery/wear");
    CHECK(b.s.eq == 2 && b.s.lock == 1 && b.s.noise == BN_ANC && b.s.gest[0][BG_HOLD] == BA_VOICE && b.s.gest[1][BG_HOLD] == BA_VOLUME, "ext eq/lock/noise/touch");
    CHECK(b.crc_order == 2 && ncap == 1 && cap[0][3] == SAM_MANAGER_INFO, "manager info answer, crc order %d", b.crc_order);
    struct bcmd c = { 1, "", BCMD_NOISE, BN_AMBIENT, 0, 0, "" }; ncap = 0;
    CHECK(sam_cmd(&b, &c) == BE_OK && cap[0][3] == 0x78 && cap[0][4] == 2, "noise 0x78");
    crc = buds_crc16(cap[0] + 3, 2); CHECK(cap[0][5] == crc >> 8 && cap[0][6] == (crc & 0xFF), "BE crc used");
    c.op = BCMD_NOISE; c.a = BN_ADAPTIVE; CHECK(sam_cmd(&b, &c) == BE_RANGE, "no adaptive on Buds2 Pro");
    c.op = BCMD_GEST; c.a = 1; c.b = BG_HOLD; c.c = BA_SPOTIFY; ncap = 0;
    CHECK(sam_cmd(&b, &c) == BE_OK && cap[0][3] == 0x92 && cap[0][4] == 1 && cap[0][5] == 5, "touch options");
    c.op = BCMD_FIND; c.a = 0; c.b = 1; ncap = 0;
    CHECK(sam_cmd(&b, &c) == BE_OK && ncap == 2 && cap[0][3] == 0xA0 && cap[1][3] == 0xA2 && cap[1][4] == 0 && cap[1][5] == 1, "find left = start + mute right");
    sam_tick(&b, 2000); sam_tick(&b, 200000); CHECK(!b.s.find[0], "find auto-stop");
    /* noise update, fragments, garbage */
    n = sam_frame(0, 1, 0x77, (const uint8_t *)"\x00", 1, 1, f, sizeof f);
    uint8_t g[400]; int gn = 0; static const uint8_t junk[] = { 0x00, 0xFD, 0x01, 0xDD, 0x55, 0xFE };
    memcpy(g, junk, sizeof junk); gn = sizeof junk; memcpy(g + gn, f, (size_t)n); gn += n;
    b.br->rx(&b, g, gn); CHECK(b.s.noise == BN_OFF, "resync after garbage");
    /* fragmented status (bit 13) */
    uint8_t f1[40], f2[40]; uint8_t a[4] = { 0x0B, 55 }, a2[6] = { 60, 1, 0, 0x11, 33, 0 };
    int n1 = sam_frame(0, 1, 0x60, a, 2, 1, f1, sizeof f1); f1[2] |= 0x20;
    { uint16_t cr = buds_crc16(f1 + 3, 3); f1[6] = (uint8_t)(cr >> 8); f1[7] = (uint8_t)cr; }
    int n2 = sam_frame(0, 1, 0x60, a2, 6, 1, f2, sizeof f2);
    b.br->rx(&b, f1, n1); b.br->rx(&b, f2, n2);
    CHECK(b.s.batt[0] == 55 && b.s.batt[1] == 60 && b.s.batt[2] == 33, "fragment reassembly %d %d %d", b.s.batt[0], b.s.batt[1], b.s.batt[2]);
    /* older models: other noise commands */
    mkbud(&b, sam_model_by_id(3), &BRAND_SAMSUNG); c.op = BCMD_NOISE; c.a = BN_AMBIENT; ncap = 0;
    CHECK(sam_cmd(&b, &c) == BE_OK && cap[0][3] == 0x80 && cap[0][4] == 1, "Buds+ ambient 0x80");
    mkbud(&b, sam_model_by_id(4), &BRAND_SAMSUNG); c.a = BN_ANC; ncap = 0;
    CHECK(sam_cmd(&b, &c) == BE_OK && cap[0][3] == 0x98 && cap[0][4] == 1, "Live ANC 0x98");
    mkbud(&b, sam_model_by_id(2), &BRAND_SAMSUNG); c.a = BN_AMBIENT; ncap = 0;
    CHECK(sam_cmd(&b, &c) == BE_OK && cap[0][0] == 0xFE && cap[0][3] == 0x80, "legacy framing for Buds 2019");
}
static void t_nothing(void)
{
    uint8_t f[80];
    static const struct { uint16_t ctl, cmd; uint8_t seq; const char *pl, *exp; } V[] = {
        { 0x0120, 0xC007, 1, "", "55 20 01 07 C0 00 00 01 ED 1B" },
        { 0x0120, 0xF00F, 2, "01 01 00", "55 20 01 0F F0 03 00 02 01 01 00 FB 47" },
        { 0x0120, 0xF002, 3, "02 01", "55 20 01 02 F0 02 00 03 02 01 45 0B" },
        { 0x0120, 0xC042, 4, "", "55 20 01 42 C0 00 00 04 E0 D7" },
        { 0x0160, 0xF00F, 0xCB, "01 07 00", "55 60 01 0F F0 03 00 CB 01 07 00 C5 AF" },
    };
    for (unsigned i = 0; i < sizeof V / sizeof *V; i++) {
        uint8_t pl[8]; int pn = hexb(V[i].pl, pl), n = nt_frame(V[i].ctl, V[i].cmd, V[i].seq, pl, pn, f, sizeof f);
        CHECK(same(f, n, V[i].exp), "doc vector %u", i);
    }
    const struct buds_model *m = 0;
    struct bd_dev d; memset(&d, 0, sizeof d); strcpy(d.name, "Nothing Ear (2)"); CHECK((m = nt_identify(&d)) && m->id == 0xB155, "Ear (2)");
    strcpy(d.name, "CMF Buds Pro 2"); CHECK(nt_identify(&d)->id == 0xB172, "CMF Buds Pro 2");
    strcpy(d.name, "Earbuds"); strcpy(d.uuid[0], NT_UUID); d.nuuid = 1; CHECK(nt_identify(&d) == &NOTHING_GENERIC.m, "uuid only");
    struct bud b; mkbud(&b, nt_identify(&(struct bd_dev){ .name = "Nothing Ear (2)" }), &BRAND_NOTHING);
    CHECK(b.s.anc_max == 2 && (b.s.noise_mask & BUDS_MASK(BN_ADAPTIVE)) && b.s.eq_set == ES_NOTHING, "fill");
    nt_start(&b);
    CHECK(ncap == 1 && capn[0] == 10 && cap[0][3] == 0x42 && cap[0][4] == 0xC0, "first get = firmware");
    /* replies split at random */
    uint8_t s[200]; int sn = 0, k;
    k = nt_frame(0x0160, 0x4042, 1, (const uint8_t *)"1.0.1.95", 8, s + sn, 80); sn += k;
    k = nt_frame(0x0160, 0x4007, 2, (const uint8_t *)"\x03\x02\xd5\x03\x50\x04\x28", 7, s + sn, 80); sn += k;
    k = nt_frame(0x0100, 0x401E, 3, (const uint8_t *)"\x01\x02\x00", 3, s + sn, 80); sn += k;
    feed_split(&b, s, sn, 3);
    CHECK(!strcmp(b.s.fw, "1.0.1.95"), "firmware");
    CHECK(b.s.batt[0] == 85 && b.s.chg[0] == 1 && b.s.batt[1] == 80 && b.s.batt[2] == 40, "battery %d %d %d", b.s.batt[0], b.s.batt[1], b.s.batt[2]);
    CHECK(b.s.noise == BN_ANC && b.s.anc_lvl == 1, "ANC mid");
    uint8_t gl[] = { 2, 0x02, 0x01, 0x02, 0x09, 0x03, 0x01, 0x07, 0x0A };
    k = nt_frame(0x0160, 0x4018, 4, gl, sizeof gl, s, 80); b.br->rx(&b, s, k);
    CHECK(b.s.gest[0][BG_DOUBLE] == BA_NEXT && b.s.gest[1][BG_HOLD] == BA_NOISE, "gestures");
    k = nt_frame(0x0160, 0xE00D, 5, (const uint8_t *)"\x00\x01", 2, s, 80); b.br->rx(&b, s, k);
    CHECK(b.s.fit[0] == 0 && b.s.fit[1] == 1, "fit test");
    s[k - 1] ^= 1; b.s.fit[0] = 7; b.br->rx(&b, s, k); CHECK(b.s.fit[0] == 7, "bad CRC ignored");
    struct bcmd c = { 1, "", BCMD_NOISE, BN_AMBIENT, 0, 0, "" }; ncap = 0;
    CHECK(nt_cmd(&b, &c) == BE_OK && cap[0][3] == 0x0F && cap[0][4] == 0xF0 && cap[0][8] == 1 && cap[0][9] == 0x07, "transparency");
    c.op = BCMD_ANCLVL; c.a = 0; ncap = 0; CHECK(nt_cmd(&b, &c) == BE_OK && cap[0][9] == 0x03, "ANC low");
    c.op = BCMD_BANDS; strcpy(c.s, "3,0,-2"); ncap = 0;
    CHECK(nt_cmd(&b, &c) == BE_OK && cap[0][3] == 0x41 && cap[0][4] == 0xF0 && (cap[0][5] | cap[0][6] << 8) == 1 + 4 + 39 + 9, "3-band EQ");
    CHECK(nt_f32(cap[0] + 9) == -3.0f && nt_f32(cap[0] + 14) == 3.0f, "EQ floats");
    strcpy(c.s, "9,0,0"); CHECK(nt_cmd(&b, &c) == BE_RANGE, "EQ range");
    c.op = BCMD_FIND; c.a = 1; c.b = 1; ncap = 0; CHECK(nt_cmd(&b, &c) == BE_OK && ncap == 1 && cap[0][8] == 3 && cap[0][9] == 1, "ring right");
    c.op = BCMD_GEST; c.a = 0; c.b = BG_TRIPLE; c.c = BA_VOICE; ncap = 0; CHECK(nt_cmd(&b, &c) == BE_OK && cap[0][11] == 0x03 && cap[0][12] == 0x0B, "gesture set");
    /* unanswered get: retry then skip */
    mkbud(&b, &NOTHING_MODELS[0].m, &BRAND_NOTHING); nt_start(&b); ncap = 0;
    nt_tick(&b, 3500); CHECK(ncap == 1, "retry"); nt_tick(&b, 6000); CHECK(ntp(&b)->ctl == 0x0120, "switch control word when silent");
}
static void t_sony(void)
{
    uint8_t f[64]; int n;
    n = sy_frame(SY_CMD, 0, (const uint8_t *)"\x00\x00", 2, f, sizeof f); CHECK(same(f, n, "3E 0C 00 00 00 00 02 00 00 0E 3C"), "init");
    n = sy_frame(SY_ACK, 1, 0, 0, f, sizeof f); CHECK(same(f, n, "3E 01 01 00 00 00 00 02 3C"), "ack");
    n = sy_frame(SY_CMD, 1, (const uint8_t *)"\x10\x01", 2, f, sizeof f); CHECK(same(f, n, "3E 0C 01 00 00 00 02 10 01 20 3C"), "battery get");
    uint8_t pl[12]; int pn = sy_asm_set(1, 1, BN_ANC, 0, 0, pl);
    n = sy_frame(SY_CMD, 0, pl, pn, f, sizeof f); CHECK(same(f, n, "3E 0C 00 00 00 00 08 68 02 11 02 02 01 00 00 94 3C"), "NC on V1 wind");
    n = sy_frame(SY_CMD, 0, (const uint8_t *)"\x3E\x3C", 2, f, sizeof f); CHECK(f[7] == 0x3D && f[8] == 0x2E && f[9] == 0x3D && f[10] == 0x2C, "escaping");
    struct bud b; struct bd_dev d; memset(&d, 0, sizeof d); strcpy(d.name, "WF-1000XM4"); strcpy(d.uuid[0], SONY_UUID_V1); d.nuuid = 1;
    mkbud(&b, sony_identify(&d), &BRAND_SONY);
    CHECK(b.s.caps & BC_RO, "read-only by default");
    sony_start(&b); CHECK(ncap == 1, "init sent");
    uint8_t s[300]; int sn = 0;
    sn += sy_frame(SY_ACK, 1, 0, 0, s + sn, 60);
    sn += sy_frame(SY_CMD, 0, (const uint8_t *)"\x01\x00\x01\x00", 4, s + sn, 60);
    ncap = 0; feed_split(&b, s, sn, 11);
    CHECK(syp(&b)->dialect == 1 && ncap >= 2 && cap[0][1] == SY_ACK && cap[0][2] == 1, "V1 + ack seq 1-s");
    CHECK(cap[1][7] == 0x04, "first read = firmware");
    sn = sy_frame(SY_CMD, 1, (const uint8_t *)"\x11\x01\x46\x00\x50\x01", 6, s, 60); b.br->rx(&b, s, sn);
    CHECK(b.s.batt[0] == 70 && b.s.batt[1] == 80 && b.s.chg[1] == 1, "dual battery");
    sn = sy_frame(SY_CMD, 0, (const uint8_t *)"\x69\x02\x11\x02\x00\x01\x01\x0C", 8, s, 60); b.br->rx(&b, s, sn);
    CHECK(b.s.noise == BN_AMBIENT && b.s.amb_lvl == 12 && b.s.conv == 1, "ASM notify");
    sn = sy_frame(SY_CMD, 1, (const uint8_t *)"\x57\x01\x16\x06\x0A\x0C\x0A\x0A\x08\x0A", 10, s, 60); b.br->rx(&b, s, sn);
    CHECK(b.s.eq == 7 && b.s.bands[0] == 2 && b.s.bands[3] == -2, "EQ reply");
    s[sn - 2] ^= 1; b.s.eq = 3; b.br->rx(&b, s, sn); CHECK(b.s.eq == 3, "bad checksum ignored");
    struct bcmd c = { 1, "", BCMD_NOISE, BN_ANC, 0, 0, "" }; CHECK(sony_cmd(&b, &c) == BE_NOTSUP, "writes disabled");
}
static void t_fastpair(void)
{
    struct bud b; struct bd_dev d; memset(&d, 0, sizeof d); strcpy(d.name, "JBL Tune Beam"); strcpy(d.uuid[0], FP_UUID); d.nuuid = 1;
    mkbud(&b, fp_identify(&d), &BRAND_FASTPAIR);
    uint8_t s[] = { 0x03, 0x01, 0x00, 0x03, 0xAB, 0xCD, 0xEF, 0x03, 0x03, 0x00, 0x03, 0xD5, 0x50, 0x7F, 0x03, 0x09, 0x00, 0x05, '1', '.', '2', '.', '3' };
    feed_split(&b, s, sizeof s, 5);
    CHECK(!strcmp(b.s.modelno, "ABCDEF") && b.s.batt[0] == 85 && b.s.chg[0] == 1 && b.s.batt[1] == 80 && b.s.batt[2] == -1 && !strcmp(b.s.fw, "1.2.3"), "fast pair");
    d.nuuid = 0; CHECK(!fp_identify(&d), "needs the UUID");
}
/* SYNTHETIC frames for the brands added from public notes (docs/HEADPHONES.md section 10):
 * built from the documented layouts; the expected bytes are recomputed by hand (checksums) */
static void t_soundcore(void)
{
    uint8_t f[64]; int n;
    n = sc_packet(0, 0x01, 0x01, 0, 0, f, sizeof f);          /* request state: 08 EE 00 00 00 01 01 0A 00 + sum */
    CHECK(same(f, n, "08 EE 00 00 00 01 01 0A 00 02"), "request state (sum 0x102 -> 02)");
    n = sc_packet(0, 0x01, 0x03, 0, 0, f, sizeof f); CHECK(same(f, n, "08 EE 00 00 00 01 03 0A 00 04"), "request battery");
    struct bd_dev d; memset(&d, 0, sizeof d); strcpy(d.name, "Soundcore Life Q30"); strcpy(d.uuid[0], "0CF12D31-FAC3-4553-BD80-D6832E7B3028"); d.nuuid = 1;
    const struct buds_model *m = sc_identify(&d);
    CHECK(m && m->id == 0x3028 && sc_pick_uuid(&d) == d.uuid[0], "Q30 by name, uuid family");
    strcpy(d.name, "My headphones"); CHECK(sc_identify(&d) == &SOUNDCORE_GENERIC, "uuid only");
    d.nuuid = 0; CHECK(!sc_identify(&d), "nothing");
    struct bud b; mkbud(&b, m, &BRAND_SOUNDCORE); sc_start(&b);
    CHECK(ncap == 4 && cap[1][5] == 0x01 && cap[1][6] == 0x03, "start: state, battery, charging, firmware");
    uint8_t s[200]; int sn = 0, k;
    k = sc_packet(1, 0x01, 0x03, (const uint8_t *)"\x04", 1, s + sn, 60); sn += k;
    k = sc_packet(1, 0x01, 0x04, (const uint8_t *)"\x01", 1, s + sn, 60); sn += k;
    k = sc_packet(1, 0x06, 0x01, (const uint8_t *)"\x00\x01\x01\x00", 4, s + sn, 60); sn += k;
    feed_split(&b, s, sn, 9);
    CHECK(b.s.batt[0] == 80 && b.s.chg[0] == 1, "battery 4/5 = 80 %%, charging");
    CHECK(b.s.noise == BN_ANC && b.s.anc_lvl == 1, "ANC outdoor = middle level");
    s[sn - 1] ^= 1; b.s.noise = BN_OFF; b.br->rx(&b, s + sn - k, k); CHECK(b.s.noise == BN_OFF, "bad checksum ignored");
    struct bcmd c = { 1, "", BCMD_NOISE, BN_AMBIENT, 0, 0, "" }; ncap = 0;
    CHECK(sc_cmd(&b, &c) == BE_OK && same(cap[0], capn[0], "08 EE 00 00 00 06 81 0E 00 01 01 01 00 8E"), "transparency (sum 0x18E)");
    c.op = BCMD_ANCLVL; c.a = 2; ncap = 0;
    CHECK(sc_cmd(&b, &c) == BE_OK && cap[0][9] == 0x00 && cap[0][10] == 0x00, "ANC transport (strongest)");
    mkbud(&b, &SOUNDCORE_MODELS[0], &BRAND_SOUNDCORE);       /* Liberty 4 Pro: not verified */
    c.op = BCMD_NOISE; c.a = BN_ANC; CHECK((b.s.caps & BC_RO) && sc_cmd(&b, &c) == BE_NOTSUP, "unverified model read-only");
    mkbud(&b, &SOUNDCORE_MODELS[5], &BRAND_SOUNDCORE);       /* Liberty Air 2 Pro: dual battery */
    k = sc_packet(1, 0x01, 0x03, (const uint8_t *)"\x05\x02", 2, s, 60); b.br->rx(&b, s, k);
    CHECK(b.s.batt[0] == 100 && b.s.batt[1] == 40, "dual battery");
}
static void t_huawei(void)
{
    uint8_t f[64]; int n; struct hw_tlv tl[3] = { { 1, 0, 0 }, { 2, 0, 0 }, { 3, 0, 0 } };
    n = hw_frame(0x01, 0x08, tl, 3, f, sizeof f);
    uint16_t c = buds_crc16(f, n - 2);
    CHECK(n == 14 && f[0] == 0x5A && f[1] == 0 && f[2] == 9 && f[3] == 0 && f[4] == 1 && f[5] == 8 && f[6] == 1 && f[7] == 0 && f[n - 2] == c >> 8 && f[n - 1] == (c & 0xFF), "battery read frame");
    struct bd_dev d; memset(&d, 0, sizeof d); strcpy(d.name, "HUAWEI FreeBuds Pro 2");
    const struct buds_model *m = hw_identify(&d); CHECK(m && m->id == 13, "FreeBuds Pro 2");
    strcpy(d.name, "HUAWEI FreeBuds 5i"); CHECK(hw_identify(&d)->id == 9, "5i");
    struct bud b; mkbud(&b, hw_identify(&d), &BRAND_HUAWEI);
    CHECK(hw_channel(&b) == 16, "5i on channel 16"); mkbud(&b, m, &BRAND_HUAWEI); CHECK(hw_channel(&b) == 1, "Pro 2 on channel 1");
    hw_start(&b); CHECK(ncap == 3, "info, battery, ANC reads");
    uint8_t s[200]; int sn = 0, k;
    struct hw_tlv bt[3] = { { 1, 1, (const uint8_t *)"\x50" }, { 2, 3, (const uint8_t *)"\x5a\x46\x32" }, { 3, 3, (const uint8_t *)"\x00\x00\x01" } };
    k = hw_frame(0x01, 0x08, bt, 3, s + sn, 80); sn += k;
    struct hw_tlv an[1] = { { 1, 2, (const uint8_t *)"\x02\x01" } };         /* level ultra (2), mode cancellation (1) */
    k = hw_frame(0x2B, 0x2A, an, 1, s + sn, 80); sn += k;
    struct hw_tlv info[2] = { { 7, 9, (const uint8_t *)"1.0.0.210" }, { 15, 5, (const uint8_t *)"T0006" } };
    k = hw_frame(0x01, 0x07, info, 2, s + sn, 80); sn += k;
    feed_split(&b, s, sn, 13);
    CHECK(b.s.batt[0] == 90 && b.s.batt[1] == 70 && b.s.batt[2] == 50 && b.s.chg[2] == 1, "battery L/R/case %d %d %d", b.s.batt[0], b.s.batt[1], b.s.batt[2]);
    CHECK(b.s.noise == BN_ANC && b.s.anc_lvl == 2, "ANC ultra");
    CHECK(!strcmp(b.s.fw, "1.0.0.210") && !strcmp(b.s.modelno, "T0006"), "device info");
    struct hw_tlv ev[1] = { { 8, 1, (const uint8_t *)"\x00" } };
    k = hw_frame(0x2B, 0x03, ev, 1, s, 80); b.br->rx(&b, s, k); CHECK(b.s.wear[0] == BW_OUT, "in-ear event");
    struct bcmd cm = { 1, "", BCMD_NOISE, BN_AMBIENT, 0, 0, "" }; ncap = 0;
    CHECK(hw_cmd(&b, &cm) == BE_OK && cap[0][4] == 0x2B && cap[0][5] == 0x04 && cap[0][6] == 1 && cap[0][7] == 2 && cap[0][8] == 2 && cap[0][9] == 0xFF && ncap == 2, "awareness + read back");
    cm.op = BCMD_ANCLVL; cm.a = 0; ncap = 0; CHECK(hw_cmd(&b, &cm) == BE_OK && cap[0][8] == 1 && cap[0][9] == 1, "comfort level");
    cm.op = BCMD_NOISE; cm.a = BN_ADAPTIVE; CHECK(hw_cmd(&b, &cm) == BE_RANGE, "no dynamic on Pro 2");
    k = hw_frame(0x2B, 0x2A, an, 1, s, 80); s[k - 1] ^= 1; b.s.noise = BN_OFF; b.br->rx(&b, s, k); CHECK(b.s.noise == BN_OFF, "bad CRC ignored");
}
static void t_oppo(void)
{
    uint8_t f[32]; int n = op_frame(0x0106, 0, 0, 0, f, sizeof f);
    CHECK(same(f, n, "AA 07 00 00 06 01 00 00 00"), "battery request");
    n = op_frame(0x0400, 5, (const uint8_t *)"\x01", 1, f, sizeof f); CHECK(same(f, n, "AA 08 00 00 00 04 05 01 00 01"), "find on");
    struct bd_dev d; memset(&d, 0, sizeof d); strcpy(d.name, "OPPO Enco Air2");
    struct bud b; mkbud(&b, op_identify(&d), &BRAND_OPPO); CHECK(b.m->id == 2, "Enco Air2");
    strcpy(d.name, "OnePlus Buds"); CHECK(!op_identify(&d), "not without the UUID"); strcpy(d.uuid[0], OP_UUID); d.nuuid = 1; CHECK(op_identify(&d) == &OPPO_GENERIC, "uuid: generic");
    op_start(&b); CHECK(ncap == 3, "battery, firmware, touch requests");
    uint8_t s[160]; int sn = 0;
    sn += op_frame(0x8106, 0, (const uint8_t *)"\x00\x03\x01\x55\x02\xd0\x03\x28", 8, s + sn, 40);
    static const char fw[] = "\x00\x00" "1,2,1.0.25,2,2,3,3,1,9";
    sn += op_frame(0x8105, 1, (const uint8_t *)fw, (int)sizeof fw - 1, s + sn, 60);
    sn += op_frame(0x8108, 2, (const uint8_t *)"\x00\x02\x01\x01\x02\x06\x02\x01\x04\x03", 10, s + sn, 40);
    feed_split(&b, s, sn, 21);
    CHECK(b.s.batt[0] == 85 && b.s.batt[1] == 80 && b.s.chg[1] == 1 && b.s.batt[2] == 40, "battery");
    CHECK(!strcmp(b.s.fw, "1.0.25"), "firmware '%s'", b.s.fw);
    CHECK(b.s.gest[0][BG_DOUBLE] == BA_NEXT && b.s.gest[1][BG_HOLD] == BA_VOICE, "touch config");
    struct bcmd c = { 1, "", BCMD_GEST, 1, BG_TRIPLE, BA_PREV, "" }; ncap = 0;
    CHECK(op_cmd(&b, &c) == BE_OK && same(cap[0], capn[0], "AA 0C 00 00 01 04 03 05 00 01 02 01 03 05"), "touch set");
    c.op = BCMD_NOISE; CHECK(op_cmd(&b, &c) == BE_NOTSUP, "no noise control");
}
static void t_google(void)
{
    struct bd_dev d; memset(&d, 0, sizeof d); strcpy(d.name, "Pixel Buds Pro"); strcpy(d.uuid[0], FP_UUID); d.nuuid = 1;
    const struct buds_model *m = 0;
    for (int k = 0; k < NBRANDS && !m; k++) m = BRANDS[k]->identify(&d);
    CHECK(m && m->brand == BB_GOOGLE && m->id == 3, "Pixel Buds Pro through Fast Pair");
}
static void t_fuzz(void)
{
    struct bud b; unsigned seed = 1; uint8_t r[300];
    const struct buds_model *ms[8] = { aap_model_by_id(0x2014), sam_model_by_id(7), &NOTHING_MODELS[0].m, &SONY_MODELS[0], &FP_BUDS,
                                       &SOUNDCORE_MODELS[6], &HUAWEI_MODELS[0], &OPPO_MODELS[0] };
    const struct buds_brand *bs[8] = { &BRAND_APPLE, &BRAND_SAMSUNG, &BRAND_NOTHING, &BRAND_SONY, &BRAND_FASTPAIR, &BRAND_SOUNDCORE, &BRAND_HUAWEI, &BRAND_OPPO };
    static const uint8_t sync[8] = { 0x04, 0xFD, 0x55, 0x3E, 0x03, 0x09, 0x5A, 0xAA };
    for (int k = 0; k < 8; k++) {
        mkbud(&b, ms[k], bs[k]); b.br->start(&b);
        for (int it = 0; it < 3000; it++) {
            int n = 1 + (int)((seed = seed * 1103515245u + 12345u) >> 16) % 299;
            for (int i = 0; i < n; i++) r[i] = (uint8_t)((seed = seed * 1103515245u + 12345u) >> 16);
            if (it % 3 == 0) r[0] = sync[k];
            if (k == 0 && n > 4) { r[1] = 0; r[2] = 4; r[3] = 0; }
            b.br->rx(&b, r, n); ncap = 0;
            if (b.br->tick) b.br->tick(&b, it * 700.0);
        }
        CHECK(b.rn >= 0 && b.rn <= (int)sizeof b.rbuf, "bounded buffer %d", k);
    }
}
static void t_tables(void)
{
    static struct buds_cat c[BUDS_CAT_MAX]; int n = buds_catalog(c, BUDS_CAT_MAX);
    CHECK(n > 50, "catalog %d", n);
    for (int i = 0; i < n; i++) {
        const struct buds_st *s = &c[i].s;
        CHECK(!(s->caps & BC_NOISE) == !s->noise_mask, "%s: noise cap vs mask", c[i].m->name);
        CHECK(!(s->caps & BC_EQ) || s->eq_n > 0, "%s: EQ presets", c[i].m->name);
        CHECK(!(s->caps & BC_AMB_LVL) || s->amb_max > 0, "%s: ambient max", c[i].m->name);
        CHECK(!(s->caps & BC_GEST) || s->gest_opts[0] | s->gest_opts[1] | s->gest_opts[2] | s->gest_opts[3], "%s: gesture options", c[i].m->name);
        CHECK(!(s->caps & BC_BATT_ONE) || !(s->caps & BC_BATT_CASE) || s->brand == BB_OTHER || s->brand == BB_APPLE, "%s: battery kinds", c[i].m->name);
        for (int j = 0; j < i; j++) CHECK(c[j].m->brand != c[i].m->brand || c[j].m->id != c[i].m->id || !c[i].m->id, "%s: duplicate id", c[i].m->name);
    }
}
static void t_state(void)
{
    struct buds_st a, out[3]; struct buds_hdr h = { 1, 42, 1, 7, 5, 4, -1 }, h2;
    buds_st_init(&a); strcpy(a.addr, "AA:BB:CC:DD:EE:FF"); strcpy(a.name, "Name\nwith newline"); strcpy(a.model, "Galaxy Buds2 Pro");
    a.batt[0] = 10; a.batt[2] = 99; a.noise = 2; a.noise_mask = 7; a.gest[1][3] = 4; a.nbands = 3; a.bands[0] = -3; a.bands[2] = 6; a.eq_set = ES_CMF; a.fit[1] = 1; a.busy = 1;
    char buf[4096]; FILE *f = fmemopen(buf, sizeof buf, "w"); buds_hdr_write(f, &h); buds_st_write(f, &a); buds_st_write(f, &a); fclose(f);
    int n = buds_state_parse(buf, &h2, out, 3);
    CHECK(n == 2 && h2.pid == 42 && h2.ack == 5 && h2.err == 4 && h2.err_code == -1, "header");
    CHECK(!strcmp(out[0].name, "Name with newline") && out[0].batt[0] == 10 && out[0].batt[1] == -1 && out[0].batt[2] == 99, "fields");
    CHECK(out[0].noise == 2 && out[0].gest[1][3] == 4 && out[0].nbands == 3 && out[0].bands[2] == 6 && out[0].eq_set == ES_CMF && out[0].fit[1] == 1 && out[0].busy == 1, "fields 2");
    struct bcmd c = { 9, "AA:BB:CC:DD:EE:FF", BCMD_GEST, 1, 3, 5, "" }, c2; char l[128];
    bcmd_format(l, sizeof l, &c); CHECK(!bcmd_parse(l, &c2) && c2.seq == 9 && c2.op == BCMD_GEST && c2.c == 5 && !strcmp(c2.addr, c.addr), "command");
    c.op = BCMD_RENAME; strcpy(c.s, "Mis cascos ñ"); bcmd_format(l, sizeof l, &c); CHECK(!bcmd_parse(l, &c2) && !strcmp(c2.s, "Mis cascos ñ"), "rename");
    c.op = BCMD_BANDS; strcpy(c.s, "1,-2,3"); bcmd_format(l, sizeof l, &c); CHECK(!bcmd_parse(l, &c2) && !strcmp(c2.s, "1,-2,3"), "bands");
    CHECK(bcmd_parse("x y z", &c2) < 0 && bcmd_parse("1 - nope", &c2) < 0, "bad commands");
}
static const char BLUEZ[] =
"method return time=1.2 sender=:1.3 -> destination=:1.9 serial=5 reply_serial=2\n"
"   array [\n      dict entry(\n         object path \"/org/bluez/hci0\"\n         array [\n            dict entry(\n               string \"org.bluez.Adapter1\"\n               array [\n"
"                  dict entry(\n                     string \"Powered\"\n                     variant                         boolean true\n                  )\n               ]\n            )\n         ]\n      )\n"
"      dict entry(\n         object path \"/org/bluez/hci0/dev_F0_5C_77_A1_02_14\"\n         array [\n            dict entry(\n               string \"org.bluez.Device1\"\n               array [\n"
"                  dict entry(\n                     string \"Name\"\n                     variant                         string \"AirPods Pro\"\n                  )\n"
"                  dict entry(\n                     string \"Alias\"\n                     variant                         string \"Mis AirPods\"\n                  )\n"
"                  dict entry(\n                     string \"Paired\"\n                     variant                         boolean true\n                  )\n"
"                  dict entry(\n                     string \"Connected\"\n                     variant                         boolean true\n                  )\n"
"                  dict entry(\n                     string \"UUIDs\"\n                     variant                         array [\n"
"                           string \"0000110b-0000-1000-8000-00805f9b34fb\"\n                           string \"74ec2172-0bad-4d01-8f77-997b2be0722a\"\n                        ]\n                  )\n"
"                  dict entry(\n                     string \"Modalias\"\n                     variant                         string \"bluetooth:v004Cp2014d0B10\"\n                  )\n"
"               ]\n            )\n         ]\n      )\n"
"      dict entry(\n         object path \"/org/bluez/hci0/dev_11_22_33_44_55_66\"\n         array [\n            dict entry(\n               string \"org.bluez.Device1\"\n               array [\n"
"                  dict entry(\n                     string \"Name\"\n                     variant                         string \"Galaxy Buds+ (1A2B)\"\n                  )\n"
"                  dict entry(\n                     string \"UUIDs\"\n                     variant                         array [\n"
"                           string \"2e73a4ad-332d-41fc-90e2-16bef06523f2\"\n                        ]\n                  )\n"
"                  dict entry(\n                     string \"Paired\"\n                     variant                         boolean true\n                  )\n"
"               ]\n            )\n         ]\n      )\n   ]\n";
static void t_bluez(void)
{
    struct bd_dev d[4]; int n = bd_bluez_parse(BLUEZ, d, 4);
    CHECK(n == 2, "devices %d", n);
    CHECK(!strcmp(d[0].addr, "F0:5C:77:A1:02:14") && !strcmp(d[0].alias, "Mis AirPods") && d[0].connected && d[0].nuuid == 2 && bd_audio(&d[0]), "AirPods props");
    unsigned v, p; CHECK(bd_modalias(d[0].modalias, &v, &p) && v == 0x4C && p == 0x2014, "modalias");
    const struct buds_model *m = 0;
    for (int k = 0; k < NBRANDS && !m; k++) m = BRANDS[k]->identify(&d[1]);
    CHECK(m && m->id == 3, "Buds+ by name + uuid");
}
static void t_sdp(void)
{
    uint8_t u[16], q[64]; CHECK(!uuid_bytes(SAM_UUID_NEW, u) && u[0] == 0x2e && u[15] == 0xf2, "uuid bytes");
    int n = sdp_req(q, 1, u, 0, 0);
    CHECK(q[0] == 0x06 && n == 5 + 19 + 2 + 5 + 1 && (q[3] << 8 | q[4]) == n - 5, "request");
    /* response: AttributeLists = DES{ DES{ 0x0004, DES{ DES{UUID16 0x0100}, DES{UUID16 0x0003, UINT8 9} } } } */
    static const uint8_t attr[] = { 0x35, 0x13, 0x35, 0x11, 0x09, 0x00, 0x04, 0x35, 0x0C, 0x35, 0x03, 0x19, 0x01, 0x00, 0x35, 0x05, 0x19, 0x00, 0x03, 0x08, 0x09 };
    uint8_t r[64] = { 0x07, 0x00, 0x01, 0x00, 0x00, 0x00, (uint8_t)sizeof attr };
    memcpy(r + 7, attr, sizeof attr); r[7 + sizeof attr] = 0;
    uint8_t acc[64], cont[17]; int an = 0;
    CHECK(sdp_rsp(r, 8 + (int)sizeof attr, acc, &an, sizeof acc, cont) == 0, "response");
    CHECK(sdp_find_rfcomm(acc, an) == 9, "channel %d", sdp_find_rfcomm(acc, an));
    static const char txt[] =
        "Browsing F0:5C:77:5A:07:02 ...\nService Name: Audio Sink\nService RecHandle: 0x10001\nService Class ID List:\n  \"Audio Sink\" (0x110b)\n\n"
        "Service Name: GEARMANAGER\nService RecHandle: 0x10006\nService Class ID List:\n  \"\" (0x2e73a4ad-332d-41fc-90e2-16bef06523f2)\nProtocol Descriptor List:\n  \"L2CAP\" (0x0100)\n  \"RFCOMM\" (0x0003)\n    Channel: 27\n";
    CHECK(sdptool_channel_parse(txt, SAM_UUID_NEW) == 27, "sdptool");
    CHECK(sdptool_channel_parse(txt, NT_UUID) == -1, "sdptool missing");
}
int main(void)
{
    t_crc(); t_apple(); t_samsung(); t_nothing(); t_sony(); t_fastpair(); t_soundcore(); t_huawei(); t_oppo(); t_google(); t_fuzz(); t_tables(); t_state(); t_bluez(); t_sdp();
    printf("%d checks, %d failures\n", checks, fails);
    return fails != 0;
}
