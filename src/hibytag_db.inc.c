/* hibytag_db.inc.c - la base de datos de la biblioteca construida en el PC o el movil
 * (docs/SYNC_PROTOCOL.md §13, docs/SYNC_LIBRARY_DESIGN.md §4).
 *
 *   hibytag dbsnapshot VIVA COPIA   copia coherente con la API de copia en linea de SQLite, sin
 *                                    parar el reproductor (busy_timeout 5 s). La usa hibysync
 *                                    para DB_SNAPSHOT.
 *   hibytag dbcheck BD              pragma integrity_check + las tablas que necesita el
 *                                    reproductor de serie + VERSION_TABLE = 20181015. Escribe
 *                                    "ok" (salida 0) o el motivo (salida 1). Lo usa DB_STAGE.
 *   hibytag apply-staged            al arrancar el reproductor, antes de que abra la base de
 *                                    datos (la libreria de pantalla lo lanza si hay staged.json):
 *                                    quick_check, copia de seguridad .bak, cambio atomico y el
 *                                    resultado en apply.result (1 aplicada, 2 rechazada por
 *                                    integridad, 3 sin espacio, 4 otro motivo). Nunca deja al
 *                                    reproductor sin base de datos: ante cualquier duda, la viva.
 */
#define HTDB_DIR     "/usr/data/hibymod/sync/db"
#define HTDB_LIVE    "/usr/data/usrlocal_media.db"
#define HTDB_VERSION 20181015

/* que el guardian no lo corte a medias (lo respeta con -700) */
static void htdb_protect(void) { int f = open("/proc/self/oom_score_adj", O_WRONLY); if (f >= 0) { if (write(f, "-700", 4) != 4) {} close(f); } }
static const char *const htdb_need[] = {
    "MEDIA_TABLE", "MEDIA2_TABLE", "ALBUM_TABLE", "ARTIST_TABLE", "ALBUM_ARTIST_TABLE", "GENRE_TABLE",
    "COLLECT_TABLE", "HISTORY_TABLE", "VERSION_TABLE", "SEARCH_TABLE", "M3U_TABLE", "PLAYLIST_TABLE",
    "MANAGER_TABLE", "BOOK_TABLE", 0
};

/* 1 si la base de datos sirve; si no, 0 y el motivo en why */
static int htdb_check(const char *path, int full, char *why, int cap)
{
    sqlite3 *db = 0;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, 0) != SQLITE_OK) { snprintf(why, cap, "no se abre"); sqlite3_close(db); return 0; }
    sqlite3_busy_timeout(db, 5000);
    sqlite3_stmt *st = 0; int ok = 0;
    if (sqlite3_prepare_v2(db, full ? "pragma integrity_check" : "pragma quick_check", -1, &st, 0) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) {
        const char *r = (const char *)sqlite3_column_text(st, 0);
        ok = r && !strcmp(r, "ok");
        if (!ok) snprintf(why, cap, "integridad: %.80s", r ? r : "?");
    } else snprintf(why, cap, "no se pudo comprobar");
    sqlite3_finalize(st);
    for (int i = 0; ok && htdb_need[i]; i++) {
        st = 0; ok = 0;
        if (sqlite3_prepare_v2(db, "select count(*) from sqlite_master where type='table' and name=?", -1, &st, 0) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, htdb_need[i], -1, SQLITE_STATIC);
            ok = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 1;
        }
        sqlite3_finalize(st);
        if (!ok) snprintf(why, cap, "falta la tabla %s", htdb_need[i]);
    }
    if (ok) {
        st = 0; ok = 0;
        if (sqlite3_prepare_v2(db, "select * from VERSION_TABLE limit 1", -1, &st, 0) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
            ok = sqlite3_column_int(st, 0) == HTDB_VERSION;
        sqlite3_finalize(st);
        if (!ok) snprintf(why, cap, "VERSION_TABLE no es %d", HTDB_VERSION);
    }
    sqlite3_close(db);
    return ok;
}

static int htdb_snapshot(const char *live, const char *out)
{
    htdb_protect();
    sqlite3 *src = 0, *dst = 0; int rc = 1;
    unlink(out);
    if (sqlite3_open_v2(live, &src, SQLITE_OPEN_READONLY, 0) == SQLITE_OK &&
        sqlite3_open_v2(out, &dst, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, 0) == SQLITE_OK) {
        sqlite3_busy_timeout(src, 5000);
        sqlite3_backup *b = sqlite3_backup_init(dst, "main", src, "main");
        if (b) {
            int s;
            /* por tramos: si el reproductor escribe a la vez, la copia vuelve a empezar sola */
            for (int tries = 0; tries < 200; tries++) {
                s = sqlite3_backup_step(b, 256);
                if (s == SQLITE_DONE) break;
                if (s == SQLITE_BUSY || s == SQLITE_LOCKED) { usleep(25000); continue; }
                if (s != SQLITE_OK) break;
            }
            rc = sqlite3_backup_finish(b) == SQLITE_OK && s == SQLITE_DONE ? 0 : 1;
        }
    }
    sqlite3_close(src); sqlite3_close(dst);
    if (rc) { unlink(out); fprintf(stderr, "dbsnapshot: no se pudo copiar %s\n", live); }
    return rc;
}

/* escribe el fichero entero y lo deja en disco antes de nombrarlo (rename atomico) */
static int htdb_write_atomic(const char *path, const char *s)
{
    char tmp[512]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600); if (fd < 0) return -1;
    size_t n = strlen(s); int ok = write(fd, s, n) == (ssize_t)n && !fsync(fd);
    close(fd);
    if (!ok || rename(tmp, path)) { unlink(tmp); return -1; }
    return 0;
}
static int htdb_copy(const char *from, const char *to)
{
    char tmp[512]; snprintf(tmp, sizeof tmp, "%s.tmp", to);
    int in = open(from, O_RDONLY); if (in < 0) return -1;
    int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (out < 0) { close(in); return -1; }
    char buf[65536]; ssize_t r; int ok = 1;
    while ((r = read(in, buf, sizeof buf)) > 0) if (write(out, buf, (size_t)r) != r) { ok = 0; break; }
    if (r < 0) ok = 0;
    if (fsync(out)) ok = 0;
    close(in); close(out);
    if (!ok || rename(tmp, to)) { unlink(tmp); return -1; }
    return 0;
}
static void htdb_result(int code, const char *why)
{
    char s[256]; snprintf(s, sizeof s, "%d %lld %s\n", code, (long long)time(0), why ? why : "");
    htdb_write_atomic(HTDB_DIR "/apply.result", s);
}
/* modo "base de datos en la tarjeta" (tf_music_db_enable): aun no se aplica aqui (D1) */
static int htdb_tf_mode(void)
{
    static const char *const cf[] = { "/usr/data/hibymod/override/config.json", "/usr/resource/config.json", 0 };
    for (int i = 0; cf[i]; i++) {
        FILE *f = fopen(cf[i], "r"); if (!f) continue;
        char b[8192]; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0;
        char *q = strstr(b, "\"tf_music_db_enable\""); if (!q) return 0;
        q = strchr(q + 20, ':'); if (!q) return 0;
        while (*++q == ' ') {}
        return atoi(q) == 1;
    }
    return 0;
}
static int htdb_apply_staged(void)
{
    htdb_protect();
    const char *staged = HTDB_DIR "/staged.db", *json = HTDB_DIR "/staged.json";
    struct stat st, sl;
    if (stat(json, &st) || stat(staged, &st)) return 0;                       /* nada pendiente */
    char why[160] = "";
    if (htdb_tf_mode()) { htdb_result(4, "la base de datos va en la tarjeta: aun no soportado"); return 0; }
    if (!htdb_check(staged, 0, why, sizeof why)) {                            /* integridad: fuera (se guarda aparte) */
        rename(staged, HTDB_DIR "/staged.rejected"); unlink(json);
        htdb_result(2, why); fprintf(stderr, "apply-staged: rechazada: %s\n", why); return 1;
    }
    struct statvfs v; long long free_b = !statvfs("/usr/data", &v) ? (long long)v.f_bavail * v.f_frsize : 0;
    long long live_sz = !stat(HTDB_LIVE, &sl) ? (long long)sl.st_size : 0;
    if (free_b < live_sz + (1 << 20)) { htdb_result(3, "sin espacio para la copia de seguridad"); return 1; }   /* se reintentara */
    if (live_sz && htdb_copy(HTDB_LIVE, HTDB_LIVE ".bak")) { htdb_result(4, "no se pudo guardar la copia de seguridad"); return 1; }
    /* los ficheros de diario de la base de datos vieja no son de la nueva: aplicados a ella la romperian */
    unlink(HTDB_LIVE "-journal"); unlink(HTDB_LIVE "-wal"); unlink(HTDB_LIVE "-shm");
    if (rename(staged, HTDB_LIVE)) { htdb_result(4, "no se pudo sustituir"); return 1; }  /* mismo sistema de ficheros: atomico */
    { int d = open("/usr/data", O_RDONLY); if (d >= 0) { fsync(d); close(d); } }
    unlink(json);
    htdb_result(1, "aplicada");
    return 0;
}
