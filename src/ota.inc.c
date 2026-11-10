/* ota.inc.c - Ajustes > General > Actualizacion de software.
 *  1. En Internet (docs/OTA.md): busca la ultima version de HiByOS en GitHub, la descarga a la
 *     SD (reanudable), verifica firma Ed25519 + sha256 e instala el payload con el instalador de
 *     siempre (hibyos_install.sh). Guarda la version anterior en la SD para volver a ella.
 *     Todo en segundo plano (ota_net.inc.c): la pagina solo pinta ota_st.
 *  2. En la tarjeta SD: elegir que firmware (.upt) instalar, HiByOS o el oficial de HiBy. Usa el
 *     recovery oficial (`hibymod reinstall`), que instala el r3ii_2025.upt de la raiz de la SD.
 *       SD/HiByOS/firmware/<x>.upt     -> HiByOS (compilado por el usuario, ver README)
 *       SD/hibymod/oficial/<x>.upt     -> oficial de HiBy
 *       SD/r3ii_2025.upt              -> se identifica por tamano con la copia oficial
 *     Volver al oficial NO esta probado: se avisa antes. Los datos de /usr/data se conservan. */
#include "lang/ota_tr.inc.c"
#include "ota_net.inc.c"
/* textos de esta pagina: tabla propia (13 idiomas) y, si no esta, tr() */
static const char *otr(const char *k)
{
    for (unsigned i = 0; i < sizeof OTR / sizeof OTR[0]; i++) if (!strcmp(OTR[i][0], k)) {
        int l = lang_ok(ui_lang) ? ui_lang : LANG_EN;
        const char *v = OTR[i][1 + l];
        return v && *v ? v : OTR[i][1 + LANG_EN];
    }
    return tr(k);
}

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

/* ------------------------------------------------------------------ en Internet */
static int ota_last_op = -1;           /* ultima operacion pedida desde aqui (para el texto de error) */
static double ota_inst_t0;             /* inicio de la instalacion (barra estimada) */
static char ota_v[4][64], ota_hdr[48], ota_foot[3][2200];
static int ota_pm;
static struct ota_info ota_ui;      /* copy of ota_st.info for this frame (page_ota) */
static void ota_mb(long long b, char *o, int cap) { snprintf(o, cap, b >= 1 << 20 ? "%.1f MB" : "%.0f KB", b >= 1 << 20 ? b / 1048576.0 : b / 1024.0); }
static const char *v_ota_inst(void) { return ota_st.installed[0] ? ota_st.installed : v_version(); }
static const char *v_ota_fw(void) { return ota_st.firmware[0] ? ota_st.firmware : "?"; }
static const char *v_ota_size(void) { ota_mb(ota_ui.size, ota_v[0], sizeof ota_v[0]); return ota_v[0]; }
static const char *v_ota_prog(void)
{
    char a[24], b[24]; long long t = ota_st.total > 0 ? ota_st.total : ota_ui.size;
    ota_mb(ota_st.got, a, sizeof a); ota_mb(t, b, sizeof b);
    snprintf(ota_v[1], sizeof ota_v[1], otr("%s de %s"), a, b); return ota_v[1];
}
static const char *v_ota_pct(void) { snprintf(ota_v[2], sizeof ota_v[2], "%d %%", ota_pm / 10); return ota_v[2]; }
static const char *v_ota_done(void) { return ota_st.installed; }
static int ota_needs_fw(void)
{
    return ota_ui.needs_firmware || (ota_ui.min_firmware[0] && ota_st.firmware[0] && ota_vercmp(ota_st.firmware, ota_ui.min_firmware) < 0);
}
static int ota_too_old(void) { return ota_ui.min_version[0] && ota_st.installed[0] && ota_vercmp(ota_st.installed, ota_ui.min_version) < 0; }
static const char *ota_note(int kind) { return otr(kind == OTA_RS_SHELL ? "OTA_NOTE_SHELL" : "OTA_NOTE_REBOOT"); }
static const char *ota_err_text(int e)
{
    static char b[400];
    switch (e) {
    case OE_NOKEY: return otr("OTA_E_NOKEY");
    case OE_NOSD: return otr("OTA_E_NOSD");
    case OE_NET: return otr("OTA_E_NET");
    case OE_NONET: return otr("OTA_E_NONET");
    case OE_SIG: return otr("OTA_E_SIG");
    case OE_SHA: case OE_TAR: return otr("OTA_E_SHA");
    case OE_SPACE_SD: { char m[24]; ota_mb(ota_st.need, m, sizeof m); snprintf(b, sizeof b, otr("OTA_E_SPACE_SD"), m); return b; }
    case OE_SPACE_DATA: return otr("OTA_E_SPACE_DATA");
    case OE_BATT: snprintf(b, sizeof b, otr("OTA_E_BATT"), OTA_MIN_BATT); return b;
    case OE_INSTALL: return otr("OTA_E_INSTALL");
    case OE_IO: return otr("OTA_E_IO");
    case OE_MINVER: snprintf(b, sizeof b, otr("OTA_MINVER"), ota_ui.min_version); return b;
    case OE_FIRMWARE: return otr("OTA_NEEDS_FW");
    case OE_NOPREV: return otr("OTA_E_NOPREV");
    case OE_DOWNGRADE: case OE_FORMAT: default: return otr("OTA_E_FORMAT");
    }
}
static void ota_checked_line(char *o, int cap)
{
    o[0] = 0;
    if (ota_st.last_check <= 0) return;
    char d[48]; lang_date((time_t)ota_st.last_check, d, sizeof d); snprintf(o, cap, otr("Comprobado: %s"), d);
}
static void ota_changelog_ui(char *o, int cap)
{
    pthread_mutex_lock(&ota_st.mu);
    ota_changelog(ota_st.json, lang_code(), o, cap);
    pthread_mutex_unlock(&ota_st.mu);
}
/* the page starts a silent load (re-verifies the saved JSON) when opened: a tap waits for it */
static int ota_start_ui(int op)
{
    for (double t0 = now_ms(); ota_st.busy && ota_op == OTA_OP_LOAD && now_ms() - t0 < 3000; ) { msleep(20); st->shell_beat++; }
    return ota_start(op);
}
static void act_ota_check(void) { ota_last_op = OTA_OP_CHECK; ota_st.quiet = 0; ota_start_ui(OTA_OP_CHECK); }
static int ota_pre_ok(void)   /* lo que se puede decir antes de empezar */
{
    if (!ota_sd_ok()) { ss_alert(otr("OTA_E_NOSD"), 0, tr("Cerrar"), 0); return 0; }
    if (!ota_power_ok()) { ss_alert(ota_err_text(OE_BATT), 0, tr("Cerrar"), 0); return 0; }
    return 1;
}
static void act_ota_install(void)
{
    if (!ota_pre_ok()) return;
    char t[96]; snprintf(t, sizeof t, otr("¿Instalar HiByOS %s?"), ota_ui.version);
    if (!ss_alert(t, ota_note(ota_ui.restart), tr("Instalar"), 0)) return;
    ota_st.allow_down = 0; ota_last_op = OTA_OP_INSTALL; ota_inst_t0 = 0; ota_start_ui(OTA_OP_INSTALL);
}
static void act_ota_resume(void) { if (!ota_pre_ok()) return; ota_last_op = OTA_OP_INSTALL; ota_start_ui(OTA_OP_INSTALL); }
static void act_ota_downgrade(void)
{
    if (!ota_pre_ok()) return;
    char t[96]; snprintf(t, sizeof t, otr("¿Instalar HiByOS %s?"), ota_ui.version);
    if (!ss_alert(t, otr("OTA_DOWNGRADE"), otr("Instalar de todos modos"), 1)) return;
    ota_st.allow_down = 1; ota_last_op = OTA_OP_INSTALL; ota_start_ui(OTA_OP_INSTALL);
}
static void act_ota_pause(void) { ota_cancel(); }
static void act_ota_rollback(void)
{
    if (!ota_pre_ok()) return;
    char t[96]; snprintf(t, sizeof t, otr("¿Volver a HiByOS %s?"), ota_st.prev);
    if (!ss_alert(t, otr("OTA_NOTE_REBOOT"), otr("Restaurar"), 1)) return;
    ota_last_op = OTA_OP_ROLLBACK; ota_inst_t0 = 0; ota_start_ui(OTA_OP_ROLLBACK);
}
static void act_ota_restart(void)
{
    progress_card(0, 0, -1); progress_card(otr("Reiniciando…"), 0, 1);
    sync();
    if (ota_st.restart == OTA_RS_SHELL) { lg("ota: restarting hibyshell"); exit(0); }   /* hiby_player.sh lo relanza */
    char *argv[] = { "/sbin/reboot", 0 }; pid_t pid; posix_spawn(&pid, argv[0], 0, 0, argv, environ);
}
#ifdef OTA_AUTO_TICK_WIRED
static int ota_auto_ui;
static void apply_ota_auto(void) { ota_st.auto_on = ota_auto_ui; ota_conf_save(); }
#endif

static void page_ota(struct spage *p)
{
    *p = (struct spage){ otr("OTA_TITLE") };   /* short form: the bar title has 150 px */
    int n = 0, s = ota_st.state, busy = ota_st.busy && ota_op != OTA_OP_LOAD;   /* the silent load does not block taps */
    pthread_mutex_lock(&ota_st.mu); ota_ui = ota_st.info; pthread_mutex_unlock(&ota_st.mu);   /* the worker may be replacing it */
    p->secs[n++] = (struct ssec){ 0, 0, { ROW("Versión instalada", R_INFO, 0, 0, 0, 0, v_ota_inst, 0),
                                          ROW(otr("Firmware"), R_INFO, 0, 0, 0, 0, v_ota_fw, 0) }, 2 };
    struct ssec *o = &p->secs[n++]; *o = (struct ssec){ 0 };
    int with_info = ota_st.have_info && (s == OS_AVAILABLE || s == OS_PAUSED || s == OS_DOWNLOADING || s == OS_VERIFYING ||
                    s == OS_EXTRACTING || s == OS_INSTALLING || (s == OS_ERROR && ota_last_op == OTA_OP_INSTALL));
    char cl[1600]; cl[0] = 0;
    if (with_info) {
        snprintf(ota_hdr, sizeof ota_hdr, "HiByOS %s", ota_ui.version); o->header = ota_hdr;
        ota_changelog_ui(cl, sizeof cl);
        o->rows[o->n++] = ROW(otr("Tamaño"), R_INFO, 0, 0, 0, 0, v_ota_size, 0);
        long long t = ota_st.total > 0 ? ota_st.total : ota_ui.size;
        if (s == OS_DOWNLOADING || s == OS_PAUSED) ota_pm = t > 0 ? (int)(1000 * ota_st.got / t) : 0;
        else if (s == OS_INSTALLING) { if (!ota_inst_t0) ota_inst_t0 = now_ms(); ota_pm = (int)((now_ms() - ota_inst_t0) / 25); if (ota_pm > 950) ota_pm = 950; }
        else ota_pm = (int)(1000 * ota_st.frac);
        if (s == OS_AVAILABLE && ota_needs_fw()) {
            struct srow r = ROW(otr("Necesita el HiByOS Patcher"), R_INFO, 0, 0, 0, 0, 0, 0); r.dis = 1; o->rows[o->n++] = r;
            snprintf(ota_foot[0], sizeof ota_foot[0], "%s", otr("OTA_NEEDS_FW"));
        } else if (s == OS_AVAILABLE && ota_too_old()) {
            snprintf(ota_foot[0], sizeof ota_foot[0], otr("OTA_MINVER"), ota_ui.min_version);
        } else if (s == OS_AVAILABLE) {
            o->rows[o->n++] = ROW(otr("Descargar e instalar"), R_ACTION, 0, 0, 0, act_ota_install, 0, 0);
            snprintf(ota_foot[0], sizeof ota_foot[0], "%s", ota_note(ota_ui.restart));
        } else if (s == OS_DOWNLOADING) {
            o->rows[o->n++] = (struct srow){ otr("Descargando…"), R_BAR, &ota_pm, 0, 0, 0, v_ota_prog, 0 };
            o->rows[o->n++] = ROW(otr("Pausar descarga"), R_ACTION, 0, 0, 0, act_ota_pause, 0, 0);
            snprintf(ota_foot[0], sizeof ota_foot[0], "%s", ota_note(ota_ui.restart));
        } else if (s == OS_PAUSED) {
            o->rows[o->n++] = (struct srow){ otr("Descarga en pausa"), R_BAR, &ota_pm, 0, 0, 0, v_ota_prog, 0 };
            if (!busy) o->rows[o->n++] = ROW(otr("Reanudar descarga"), R_ACTION, 0, 0, 0, act_ota_resume, 0, 0);
            snprintf(ota_foot[0], sizeof ota_foot[0], "%s", ota_st.err ? ota_err_text(ota_st.err) : otr("OTA_PAUSED"));
        } else if (s == OS_ERROR) {
            if (!busy && ota_st.err != OE_FIRMWARE && ota_st.err != OE_MINVER) o->rows[o->n++] = ROW(otr("Reintentar"), R_ACTION, 0, 0, 0, act_ota_resume, 0, 0);
            snprintf(ota_foot[0], sizeof ota_foot[0], "%s", ota_err_text(ota_st.err));
        } else {
            const char *t2 = s == OS_VERIFYING ? "Verificando…" : s == OS_EXTRACTING ? "Preparando…" : "Instalando…";
            o->rows[o->n++] = (struct srow){ otr(t2), R_BAR, &ota_pm, 0, 0, 0, v_ota_pct, 0 };
            snprintf(ota_foot[0], sizeof ota_foot[0], "%s", otr("OTA_DONT_OFF"));
        }
        o->footer = ota_foot[0];
        if (cl[0]) {   /* what's new: a section of text only (no rows), like the iOS release notes */
            snprintf(ota_foot[1], sizeof ota_foot[1], "%s", cl);
            p->secs[n++] = (struct ssec){ otr("NOVEDADES"), ota_foot[1], { { 0 } }, 0 };
        }
    } else if (s == OS_RESTORING) {
        if (!ota_inst_t0) ota_inst_t0 = now_ms();
        ota_pm = (int)((now_ms() - ota_inst_t0) / 25); if (ota_pm > 950) ota_pm = 950;
        o->rows[o->n++] = (struct srow){ otr("Restaurando…"), R_BAR, &ota_pm, 0, 0, 0, v_ota_pct, 0 };
        o->footer = otr("OTA_DONT_OFF");
    } else if (s == OS_DONE) {
        o->rows[o->n++] = ROW(otr("Actualización instalada"), R_INFO, 0, 0, 0, 0, v_ota_done, 0);
        if (ota_st.restart != OTA_RS_NONE)
            o->rows[o->n++] = ROW(otr(ota_st.restart == OTA_RS_SHELL ? "Reiniciar la interfaz" : "Reiniciar ahora"), R_ACTION, 0, 0, 0, act_ota_restart, 0, 0);
        o->footer = ota_st.restart != OTA_RS_NONE ? ota_note(ota_st.restart) : 0;
    } else {   /* buscar: inactivo, buscando, al dia, version instalada mas nueva o error al buscar */
        if (s == OS_CHECKING) o->rows[o->n++] = ROW(otr("Buscando…"), R_INFO, 0, 0, 0, 0, 0, 0);
        else o->rows[o->n++] = ROW(otr("Buscar actualizaciones"), R_ACTION, 0, 0, 0, busy ? 0 : act_ota_check, 0, 0);
        char ck[96]; ota_checked_line(ck, sizeof ck);
        if (s == OS_UPTODATE) snprintf(ota_foot[0], sizeof ota_foot[0], ck[0] ? "%s · %s" : "%s", otr("HiByOS está actualizado"), ck);
        else if (s == OS_NEWER) {
            snprintf(ota_foot[0], sizeof ota_foot[0], otr("OTA_NEWER"), ota_ui.version);
            if (!busy && !ota_needs_fw()) o->rows[o->n++] = ROW(otr("Instalar de todos modos"), R_ACTION, 0, 0, 0, act_ota_downgrade, 0, 0);
        } else if (s == OS_ERROR && !ota_st.quiet) snprintf(ota_foot[0], sizeof ota_foot[0], "%s", ota_err_text(ota_st.err));
        else snprintf(ota_foot[0], sizeof ota_foot[0], "%s", ck);
        o->footer = ota_foot[0][0] ? ota_foot[0] : 0;
    }
#ifdef OTA_AUTO_TICK_WIRED
    ota_auto_ui = ota_st.auto_on;
    p->secs[n++] = (struct ssec){ 0, otr("OTA_AUTO_HELP"), { ROW(otr("Buscar automáticamente"), R_TOGGLE, &ota_auto_ui, 0, 0, apply_ota_auto, 0, 0) }, 1 };
#endif
    if (ota_st.prev[0] && !busy && s != OS_DONE && s != OS_DOWNLOADING && s != OS_PAUSED)
        p->secs[n++] = (struct ssec){ 0, 0, { ROW(otr("Restaurar versión anterior"), R_ACTION, 0, 0, 0, act_ota_rollback, 0, 0) }, 1 };
    /* firmware completo desde la SD */
    p->secs[n] = (struct ssec){ "EN LA TARJETA SD", nfw ? "OTA_HELP" : "OTA_NONE", { {0} }, 0 };
    for (int i = 0; i < nfw && i < 5; i++) p->secs[n].rows[p->secs[n].n++] = ROW(fws[i].label, R_ACTION, 0, 0, 0, fw_act[i], 0, 0);
    p->nsec = n + 1;
}
static void nav_ota(void) { fw_scan(); ota_init(); open_page(page_ota, "General"); }
