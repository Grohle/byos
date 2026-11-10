/* hpk_test.c - pruebas en PC (x86, WSL) del instalador hpk.inc.c contra una raiz temporal.
 * Compilar/ejecutar: ~/hiby/tools/hpk_test.sh  (genera los paquetes con mkpkgs.py)
 *   hpk_test DIR_PAQUETES RAIZ_TEMPORAL HOLA.hpk */
#define HPK_TEST 1
#include "hpk.inc.c"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

static int fails, passes;
static const char *PK, *ROOT;
#define CHECK(c, ...) do { if (c) passes++; else { fails++; printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int exists(const char *fmt, const char *a)
{
    char p[600]; snprintf(p, sizeof p, fmt, ROOT, a);
    struct stat s; return !lstat(p, &s);
}
static long fsize(const char *p) { struct stat s; return stat(p, &s) ? -1 : (long)s.st_size; }
static int readfile(const char *p, char *b, int n)
{
    FILE *f = fopen(p, "rb"); if (!f) return -1;
    int r = (int)fread(b, 1, n - 1, f); fclose(f); b[r < 0 ? 0 : r] = 0; return r;
}
/* nada a medias en apps/: ni .tmp-, ni .old-, ni .del- */
static int apps_clean(void)
{
    char p[300]; snprintf(p, sizeof p, "%s/apps", ROOT);
    DIR *d = opendir(p); if (!d) return 1;
    struct dirent *de; int ok = 1;
    while ((de = readdir(d))) if (de->d_name[0] == '.' && strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) { ok = 0; printf("  resto: %s\n", de->d_name); }
    closedir(d); return ok;
}

static long last_done, last_total; static int ncalls;
static void prog(long done, long total, void *c) { (void)c; last_done = done; last_total = total; ncalls++; }

static int inst(const char *name)
{
    char p[400], err[64]; snprintf(p, sizeof p, "%s/%s", PK, name);
    hpk_err_detail[0] = 0;
    int r = hpk_install(p, prog, 0, err, sizeof err);
    printf("  %-26s -> %-15s %s\n", name, err, hpk_err_detail);
    return r;
}

static void test_bad(void)
{
    static const struct { const char *f; int code; } T[] = {
        { "bad_traversal.hpk", HPK_E_TRAVERSAL }, { "bad_traversal2.hpk", HPK_E_TRAVERSAL },
        { "bad_absolute.hpk", HPK_E_TRAVERSAL }, { "bad_prefix_trav.hpk", HPK_E_TRAVERSAL },
        { "bad_backslash.hpk", HPK_E_TRAVERSAL },
        { "bad_symlink.hpk", HPK_E_TYPE }, { "bad_symlink_then_file.hpk", HPK_E_TYPE },
        { "bad_hardlink.hpk", HPK_E_TYPE }, { "bad_chardev.hpk", HPK_E_TYPE }, { "bad_fifo.hpk", HPK_E_TYPE },
        { "bad_pax.hpk", HPK_E_TYPE }, { "bad_checksum.hpk", HPK_E_CHECKSUM },
        { "bad_nomanifest.hpk", HPK_E_NOMANIFEST }, { "bad_empty.hpk", HPK_E_FORMAT },
        { "bad_notar.hpk", HPK_E_FORMAT }, { "bad_huge.hpk", HPK_E_TOOBIG },
        { "bad_toolarge_decl.hpk", HPK_E_TOOBIG }, { "bad_base256.hpk", HPK_E_TOOBIG },
        { "bad_truncated.hpk", HPK_E_FORMAT }, { "bad_noexec.hpk", HPK_E_NOEXEC },
        { "bad_exec_dir.hpk", HPK_E_NOEXEC }, { "bad_dynamic.hpk", HPK_E_ELF },
        { "bad_softfloat.hpk", HPK_E_ELF }, { "bad_x86.hpk", HPK_E_ELF }, { "bad_elf64.hpk", HPK_E_ELF },
        { "bad_mips64isa.hpk", HPK_E_ELF }, { "bad_script.hpk", HPK_E_ELF },
        { "bad_id.hpk", HPK_E_ID }, { "bad_id_upper.hpk", HPK_E_ID }, { "bad_id_long.hpk", HPK_E_ID },
        { "bad_version.hpk", HPK_E_VERSION }, { "bad_arch.hpk", HPK_E_ARCH }, { "bad_newer.hpk", HPK_E_NEWER },
        { "bad_exec_trav.hpk", HPK_E_TRAVERSAL }, { "bad_input.hpk", HPK_E_MANIFEST },
        { "bad_noname.hpk", HPK_E_MANIFEST }, { "bad_dup.hpk", HPK_E_DUP }, { "bad_dupmanifest.hpk", HPK_E_DUP },
        { "bad_file_dir.hpk", HPK_E_DUP }, { "bad_icon.hpk", HPK_E_ICON }, { "bad_icon_huge.hpk", HPK_E_ICON },
        { "bad_manifest_big.hpk", HPK_E_TOOBIG },
        { "no_existe.hpk", HPK_E_OPEN },
    };
    printf("[paquetes maliciosos o rotos]\n");
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
        int r = inst(T[i].f);
        CHECK(r == -T[i].code, "%s: esperado %s, obtenido %s", T[i].f, hpk_errkey(T[i].code), hpk_errkey(r));
        CHECK(!exists("%s/apps/%s", "t.app"), "%s dejo apps/t.app", T[i].f);
        CHECK(apps_clean(), "%s dejo restos en apps/", T[i].f);
        CHECK(!exists("%s/%s", "evil.txt") && !exists("%s/../%s", "evil.txt") && !exists("%s/apps/%s", "evil.txt"), "%s escribio evil.txt", T[i].f);
    }
    struct stat s; CHECK(stat("/tmp/evil.txt", &s), "/tmp/evil.txt existe");
    /* sin espacio */
    setenv("HPK_FAKE_FREE", "1000", 1);
    CHECK(inst("good_v1.hpk") == -HPK_E_SPACE, "sin espacio no fallo");
    unsetenv("HPK_FAKE_FREE");
    CHECK(apps_clean() && !exists("%s/apps/%s", "t.app"), "sin espacio dejo restos");
}

static void test_good(const char *hola)
{
    char p[600], b[4096];
    printf("[instalacion correcta]\n");
    ncalls = 0;
    CHECK(inst("good_v1.hpk") == 0, "good_v1 no se instalo");
    CHECK(ncalls >= 2 && last_done == last_total, "progreso: %d llamadas, %ld/%ld", ncalls, last_done, last_total);
    snprintf(p, sizeof p, "%s/apps/t.app/data/info.txt", ROOT); readfile(p, b, sizeof b);
    CHECK(!strcmp(b, "v1"), "contenido v1: %s", b);
    snprintf(p, sizeof p, "%s/apps/t.app/bin/app", ROOT);
    struct stat s; CHECK(!stat(p, &s) && (s.st_mode & 0777) == 0755, "exec no es 0755 (%o)", s.st_mode & 0777);
    snprintf(p, sizeof p, "%s/apps/t.app/data/info.txt", ROOT);
    CHECK(!stat(p, &s) && (s.st_mode & 0777) == 0644, "datos no son 0644");
    static const int sz[3] = { 48, 56, 64 };
    for (int i = 0; i < 3; i++) {
        snprintf(p, sizeof p, "%s/apps/t.app/icon_%d.ov", ROOT, sz[i]);
        CHECK(fsize(p) == 8 + sz[i] * sz[i] * 3, "icon_%d.ov tamano %ld", sz[i], fsize(p));
    }
    CHECK(exists("%s/appdata/%s", "t.app"), "no se creo appdata");
    CHECK(apps_clean(), "restos tras instalar");

    /* el .ov: esquina transparente, centro opaco */
    snprintf(p, sizeof p, "%s/apps/t.app/icon_64.ov", ROOT);
    FILE *f = fopen(p, "rb"); unsigned char ov[8 + 64 * 64 * 3]; size_t n = fread(ov, 1, sizeof ov, f); fclose(f);
    CHECK(n == sizeof ov && !memcmp(ov, "HMOV", 4) && ov[4] == 64 && ov[6] == 64, "cabecera HMOV");
    const unsigned char *al = ov + 8 + 64 * 64 * 2;
    CHECK(al[0] == 0 && al[32 * 64 + 32] == 255 && al[32 * 64] > 200, "mascara: esquina %d centro %d borde %d", al[0], al[32 * 64 + 32], al[32 * 64]);

    /* datos de la app que deben sobrevivir */
    snprintf(p, sizeof p, "%s/appdata/t.app/save.dat", ROOT);
    f = fopen(p, "w"); fputs("partida", f); fclose(f);

    printf("[actualizacion que falla a mitad: la v1 debe quedar intacta]\n");
    hpk_test_fail_at = 200 * 1024;
    CHECK(inst("good_v2.hpk") == -HPK_E_IO, "el fallo simulado no fallo");
    hpk_test_fail_at = -1;
    snprintf(p, sizeof p, "%s/apps/t.app/data/info.txt", ROOT); readfile(p, b, sizeof b);
    CHECK(!strcmp(b, "v1"), "tras fallo el contenido es %s", b);
    snprintf(p, sizeof p, "%s/apps/t.app/data/big.bin", ROOT);
    CHECK(fsize(p) < 0, "big.bin de la v2 a medias visible");
    CHECK(apps_clean(), "restos tras fallo");
    struct hpk_info in;
    CHECK(!hpk_read_installed("t.app", &in) && !strcmp(in.version, "1.0.0"), "version tras fallo %s", in.version);

    printf("[actualizacion correcta]\n");
    CHECK(inst("good_v2.hpk") == 0, "v2 no se instalo");
    snprintf(p, sizeof p, "%s/apps/t.app/data/info.txt", ROOT); readfile(p, b, sizeof b);
    CHECK(!strcmp(b, "v2"), "contenido v2: %s", b);
    snprintf(p, sizeof p, "%s/apps/t.app/data/big.bin", ROOT);
    CHECK(fsize(p) == 600 * 1024, "big.bin %ld", fsize(p));
    snprintf(p, sizeof p, "%s/appdata/t.app/save.dat", ROOT);
    CHECK(fsize(p) == 7, "appdata perdida en la actualizacion");
    CHECK(apps_clean(), "restos tras actualizar");

    CHECK(inst("good_noicon.hpk") == 0, "sin icono");
    CHECK(inst("good_dotslash.hpk") == 0, "./ inicial");
    CHECK(inst("good_prefix.hpk") == 0, "campo prefix");
    snprintf(p, sizeof p, "%s/apps/t.prefix/%s/%s/deep.txt", ROOT, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    CHECK(fsize(p) == 1, "prefix mal extraido");
    CHECK(inst(hola) == 0, "hola.hpk (ELF MIPS real) no se instalo");

    /* vista previa de los iconos generados (para revisarlos a ojo) */
    unsigned char rgba[64 * 64 * 4 * 3] = { 0 };
    const char *ids[3] = { "t.app", "t.noicon", "org.hibymod.hola" };
    for (int k = 0; k < 3; k++) {
        snprintf(p, sizeof p, "%s/apps/%s/icon_64.ov", ROOT, ids[k]);
        f = fopen(p, "rb"); if (!f) continue; n = fread(ov, 1, sizeof ov, f); fclose(f);
        for (int i = 0; i < 64 * 64; i++) {
            unsigned c = ov[8 + i * 2] | ov[9 + i * 2] << 8; int y = i / 64, x = i % 64;
            unsigned char *o = rgba + ((y * 192) + k * 64 + x) * 4;
            o[0] = (c >> 11) * 255 / 31; o[1] = ((c >> 5) & 63) * 255 / 63; o[2] = (c & 31) * 255 / 31; o[3] = ov[8 + 64 * 64 * 2 + i];
        }
    }
    snprintf(p, sizeof p, "%s/iconos.png", ROOT);
    stbi_write_png(p, 192, 64, 4, rgba, 192 * 4);
}

static void test_scan_list(void)
{
    printf("[escaneo y lista]\n");
    struct hpk_info *a = calloc(80, sizeof *a);
    int n = hpk_scan(PK, a, 80), good = 0, bad = 0, inst = 0;
    for (int i = 0; i < n; i++) { if (a[i].status) bad++; else good++; if (a[i].installed[0]) inst++; }
    printf("  %d paquetes: %d validos, %d danados, %d con version instalada\n", n, good, bad, inst);
    CHECK(n >= 45, "scan encontro %d", n);
    int seen = 0;
    for (int i = 0; i < n; i++) if (!strcmp(a[i].id, "t.app") && !a[i].status && !strcmp(a[i].version, "1.1.0")) {
        seen = 1;
        CHECK(!strcmp(a[i].installed, "1.1.0"), "installed=%s", a[i].installed);
        CHECK(!strcmp(a[i].name[1], "Test") && !strcmp(a[i].name[2], "Essai"), "nombres");
        CHECK(!strcmp(a[i].desc[1], a[i].desc[0]), "desc.en debe caer a la de por defecto");
        CHECK(a[i].stop_player == 1 && a[i].ram_kb == 1234 && a[i].icon_len > 0, "campos");
        unsigned short rgb[56 * 56]; unsigned char al[56 * 56];
        CHECK(!hpk_icon(&a[i], 56, rgb, al) && al[0] == 0 && al[28 * 56 + 28] == 255, "hpk_icon");
    }
    CHECK(seen, "good_v2 no aparece en el escaneo");
    n = hpk_list_installed(a, 80);
    printf("  instaladas: %d\n", n);
    for (int i = 0; i < n; i++) printf("    %-18s %-6s %7lld B  %s / %s / %s\n", a[i].id, a[i].installed, a[i].size, a[i].name[0], a[i].name[1], a[i].name[2]);
    CHECK(n == 5, "instaladas %d", n);
    free(a);
}

static void test_recover_uninstall(void)
{
    char p[600], q[600];
    printf("[recuperacion tras corte de luz]\n");
    /* corte entre los dos rename: solo queda .old-<id> */
    snprintf(p, sizeof p, "%s/apps/t.noicon", ROOT); snprintf(q, sizeof q, "%s/apps/.old-t.noicon", ROOT);
    CHECK(!rename(p, q), "preparar .old");
    snprintf(p, sizeof p, "%s/apps/.tmp-t.noicon", ROOT); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/apps/.del-zzz", ROOT); mkdir(p, 0755);
    hpk_recover();
    CHECK(exists("%s/apps/%s", "t.noicon") && apps_clean(), "recuperacion de .old");
    /* corte tras el segundo rename: <id> y .old-<id> -> se borra el viejo */
    snprintf(p, sizeof p, "%s/apps/.old-t.dot", ROOT); mkdir(p, 0755);
    hpk_recover();
    CHECK(exists("%s/apps/%s", "t.dot") && apps_clean(), "limpieza de .old sobrante");

    printf("[desinstalacion]\n");
    CHECK(hpk_uninstall("t.app") == 0, "desinstalar");
    CHECK(!exists("%s/apps/%s", "t.app") && exists("%s/appdata/%s", "t.app/save.dat"), "appdata debe conservarse");
    CHECK(hpk_uninstall("t.app") == -HPK_E_NOTINST, "doble desinstalacion");
    CHECK(hpk_uninstall("../apps") == -HPK_E_ID, "id malicioso en desinstalar");
    CHECK(hpk_uninstall_ex("t.dot", HPK_PURGE_DATA) == 0 && !exists("%s/appdata/%s", "t.dot"), "purga");
    CHECK(apps_clean(), "restos tras desinstalar");
}

static void test_misc(void)
{
    printf("[versiones y textos]\n");
    CHECK(hpk_vercmp("1.0.0", "1.0.1") < 0 && hpk_vercmp("1.10.0", "1.9.9") > 0 && hpk_vercmp("2.0.0", "2.0.0") == 0, "vercmp");
    CHECK(hpk_vercmp("1.0.0-beta", "1.0.0") < 0 && hpk_vercmp("1.0.0-beta.2", "1.0.0-beta.1") > 0 && hpk_vercmp("1.0.0+b1", "1.0.0") == 0, "vercmp pre");
    CHECK(hpk_vercmp("1.0", "0.0.1") < 0, "invalida menor");
    for (int c = 0; c < HPK_E_COUNT; c++) for (int l = 0; l < 3; l++)
        CHECK(hpk_msg(hpk_errkey(c), l) != hpk_errkey(c), "falta traduccion %s/%d", hpk_errkey(c), l);
    CHECK(!strcmp(hpk_msg("err.space", HPK_LANG_FR), "Espace libre insuffisant"), "fr");
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "uso: hpk_test PAQUETES RAIZ HOLA.hpk\n"); return 2; }
    PK = argv[1]; ROOT = argv[2];
    hpk_set_root(ROOT);
    test_misc();
    test_bad();
    test_good(argv[3]);
    test_scan_list();
    test_recover_uninstall();
    printf("\n%d comprobaciones correctas, %d fallos\n", passes, fails);
    return fails != 0;
}
