/* ota.inc.c - Personalizar > Actualizacion de software: elegir que firmware instalar desde
 * la tarjeta SD, HiByOS o el oficial de HiBy. Usa el recovery oficial (`hibymod reinstall`),
 * que instala el r3ii_2025.upt de la raiz de la SD y reinicia.
 *   SD/HiByOS/firmware/<x>.upt     -> HiByOS (compilado por el usuario, ver README)
 *   SD/hibymod/oficial/<x>.upt     -> oficial de HiBy
 *   SD/r3ii_2025.upt              -> se identifica por tamano con la copia oficial
 * Volver al oficial NO esta probado: se avisa antes. Los datos de /usr/data se conservan. */
#define SD_ROOT "/usr/data/mnt/sd_0"
struct fw_c { char path[300], label[80]; int official; long long size; };
static struct fw_c fws[8]; static int nfw;
static long long file_size(const char *p) { struct stat sb; return stat(p, &sb) ? -1 : (long long)sb.st_size; }
static void fw_scan(void)
{
    nfw = 0;
    long long offsz = -1;
    const char *dirs[2] = { SD_ROOT "/HiByOS/firmware", SD_ROOT "/hibymod/oficial" };
    for (int d = 0; d < 2; d++) {
        DIR *dd = opendir(dirs[d]); struct dirent *e;
        while (dd && (e = readdir(dd)) && nfw < 7) {
            const char *x = strrchr(e->d_name, '.'); if (!x || strcasecmp(x, ".upt")) continue;
            struct fw_c *f = &fws[nfw++]; snprintf(f->path, sizeof f->path, "%s/%s", dirs[d], e->d_name);
            f->official = d == 1; f->size = file_size(f->path); if (d == 1) offsz = f->size;
            snprintf(f->label, sizeof f->label, "%s", d == 1 ? tr("Firmware oficial de HiBy") : e->d_name);
        }
        if (dd) closedir(dd);
    }
    long long rs = file_size(SD_ROOT "/r3ii_2025.upt");
    if (rs > 0 && nfw < 8) {
        struct fw_c *f = &fws[nfw++]; snprintf(f->path, sizeof f->path, SD_ROOT "/r3ii_2025.upt");
        f->size = rs; f->official = offsz > 0 && rs == offsz;
        snprintf(f->label, sizeof f->label, "%s", f->official ? tr("Oficial (raíz de la SD)") : tr("HiByOS (raíz de la SD)"));
    }
}
static void fw_install(int i)
{
    struct fw_c *f = &fws[i];
    if (f->official) {
        if (!confirm(tr("OTA_OFFICIAL_WARN"), tr("Instalar el oficial"))) return;
    } else if (!confirm(tr("¿Instalar este firmware? El HiBy se reiniciará y tardará un minuto."), tr("Instalar"))) return;
    /* el recovery instala el r3ii_2025.upt de la raiz: copiarlo alli si hace falta */
    if (strcmp(f->path, SD_ROOT "/r3ii_2025.upt")) {
        progress_card(0, 0, -1); progress_card(tr("Preparando la actualización"), 0, 0.1);
        int in = open(f->path, O_RDONLY), out = open(SD_ROOT "/r3ii_2025.upt.part", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        int ok = in >= 0 && out >= 0; long long done = 0; char *b = malloc(65536); ssize_t n;
        while (ok && b && (n = read(in, b, 65536)) > 0) {
            if (write(out, b, n) != n) ok = 0;
            done += n; st->shell_beat++;
            if ((done & ((1 << 21) - 1)) < 65536) progress_card(tr("Preparando la actualización"), 0, 0.1 + 0.85 * done / (double)(f->size > 0 ? f->size : 1));
        }
        free(b); if (in >= 0) close(in);
        if (out >= 0) { if (fsync(out)) ok = 0; close(out); }
        if (!ok || rename(SD_ROOT "/r3ii_2025.upt.part", SD_ROOT "/r3ii_2025.upt")) { unlink(SD_ROOT "/r3ii_2025.upt.part"); toast(tr("No se pudo copiar el firmware")); return; }
    }
    sync();
    progress_card(tr("Reiniciando para actualizar…"), 0, 1);
    char *argv[] = { "/usr/bin/hibymod", "reinstall", "--yes", 0 };
    pid_t pid; posix_spawn(&pid, argv[0], 0, 0, argv, environ);
}
static void act_fw0(void) { fw_install(0); } static void act_fw1(void) { fw_install(1); } static void act_fw2(void) { fw_install(2); }
static void act_fw3(void) { fw_install(3); } static void act_fw4(void) { fw_install(4); }
static void (*const fw_act[5])(void) = { act_fw0, act_fw1, act_fw2, act_fw3, act_fw4 };
static void page_ota(struct spage *p)
{
    *p = (struct spage){ "Actualización de software" };
    p->secs[0] = (struct ssec){ 0, 0, { ROW("Versión instalada", R_INFO, 0, 0, 0, 0, v_version, 0) }, 1 };
    p->secs[1] = (struct ssec){ "EN LA TARJETA SD", nfw ? "OTA_HELP" : "OTA_NONE", { {0} }, 0 };
    for (int i = 0; i < nfw && i < 5; i++) p->secs[1].rows[p->secs[1].n++] = ROW(fws[i].label, R_ACTION, 0, 0, 0, fw_act[i], 0, 0);
    p->nsec = 2;
}
static void nav_ota(void) { fw_scan(); open_page(page_ota, "General"); }
