/* hpk.inc.c - instalador de paquetes de apps (.hpk) de hibymod. Se incluye desde hibyshell.c
 * (o desde el arnes de pruebas tests/hpk_test.c); no tiene main. Interfaz en hpk.h y formato en
 * docs/APP_FORMAT.md.
 *
 * Principios:
 *  - Nada se escribe fuera de <raiz>/apps/.tmp-<id> hasta que el paquete entero se ha validado y
 *    extraido; luego dos rename() dejan la version nueva en su sitio (la vieja pasa a .old-<id> y
 *    se borra). Si se corta la luz a medias, hpk_recover() deja siempre una version completa.
 *  - El tar se lee con pread() en trozos de <= 64 KB: la memoria no depende del paquete.
 *  - Se rechaza todo lo que no sea fichero normal o directorio (enlaces, dispositivos, pax/GNU),
 *    rutas absolutas o con "..", cabeceras con suma erronea y tamanos que no caben en el fichero.
 *  - Los errores son claves ("err.traversal") traducibles con hpk_msg(). */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include "hpk.h"

#ifndef HPK_HAVE_STB_IMAGE
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#endif

#ifndef HPK_HIBYMOD_VERSION
#define HPK_HIBYMOD_VERSION "0.1.0"      /* version del shell que instala (min_hibymod <= esta) */
#endif
#define HPK_BUF (64 * 1024)               /* bufer de copia */
#define HPK_MAX_ENTRIES 4096
#define HPK_MAX_FILE (256LL << 20)        /* ningun fichero de un paquete pasa de 256 MB */
#define HPK_MAX_MANIFEST 16384
#define HPK_MAX_ICON_BYTES (1 << 20)
#define HPK_ICON_MAXPX 512                /* lado maximo de icon.png (512x512x4 = 1 MB al decodificar) */
#define HPK_SPACE_MARGIN (512 * 1024)     /* margen libre que se deja en /usr/data */

char hpk_err_detail[160];
static char hpk_rootbuf[200];
#ifdef HPK_TEST
long long hpk_test_fail_at = -1;          /* pruebas: falla la extraccion al pasar estos bytes */
#endif

void hpk_set_root(const char *r)
{
    if (!r) r = getenv("HPK_ROOT");
    if (!r || !*r) r = "/usr/data/hibymod";
    snprintf(hpk_rootbuf, sizeof hpk_rootbuf, "%s", r);
}
const char *hpk_root(void) { if (!hpk_rootbuf[0]) hpk_set_root(NULL); return hpk_rootbuf; }

static int hpk_fail(int code, const char *detail)
{
    if (detail && !hpk_err_detail[0]) snprintf(hpk_err_detail, sizeof hpk_err_detail, "%s", detail);
    return -code;
}

/* ------------------------------------------------------------------ textos (3 idiomas) */
static const char *const hpk_keys[HPK_E_COUNT] = {
    "ok", "err.open", "err.io", "err.format", "err.checksum", "err.traversal", "err.type",
    "err.toobig", "err.nomanifest", "err.manifest", "err.id", "err.version", "err.arch",
    "err.newer", "err.noexec", "err.elf", "err.space", "err.icon", "err.dup", "err.swap",
    "err.notinst", "err.nomem",
};
const char *hpk_errkey(int code)
{
    if (code < 0) code = -code;
    return code < HPK_E_COUNT ? hpk_keys[code] : "err.io";
}

/* columnas: es, en, fr, de, it, ja, ko, pl, ru, zh-Hans, th, zh-Hant, uk (orden de LANG_*) */
static const struct { const char *key, *t[HPK_NLANG]; } hpk_tr[] = {
    { "ok",             { "Listo", "Done", "Terminé", "Fertig", "Fine", "完了", "완료", "Gotowe", "Готово", "完成", "เสร็จสิ้น", "完成", "Готово" } },
    { "err.open",       { "No se puede abrir el paquete", "Cannot open the package", "Impossible d'ouvrir le paquet", "Paket kann nicht geöffnet werden", "Impossibile aprire il pacchetto", "パッケージを開けません", "패키지를 열 수 없습니다", "Nie można otworzyć pakietu", "Не удается открыть пакет", "无法打开软件包", "เปิดแพ็กเกจไม่ได้", "無法打開套件", "Не вдається відкрити пакет" } },
    { "err.io",         { "Error de lectura o escritura", "Read or write error", "Erreur de lecture ou d'écriture", "Lese- oder Schreibfehler", "Errore di lettura o scrittura", "読み書きエラー", "읽기 또는 쓰기 오류", "Błąd odczytu lub zapisu", "Ошибка чтения или записи", "读写错误", "ข้อผิดพลาดในการอ่านหรือเขียน", "讀寫錯誤", "Помилка читання або запису" } },
    { "err.format",     { "El paquete está dañado o incompleto", "The package is damaged or incomplete", "Le paquet est endommagé ou incomplet", "Paket beschädigt oder unvollständig", "Pacchetto danneggiato o incompleto", "パッケージが破損しているか不完全です", "패키지가 손상되었거나 불완전합니다", "Pakiet jest uszkodzony lub niekompletny", "Пакет поврежден или неполон", "软件包已损坏或不完整", "แพ็กเกจเสียหายหรือไม่สมบูรณ์", "套件已損毀或不完整", "Пакет пошкоджений або неповний" } },
    { "err.checksum",   { "El paquete está dañado (suma de control)", "The package is damaged (checksum)", "Le paquet est endommagé (somme de contrôle)", "Paket beschädigt (Prüfsumme)", "Pacchetto danneggiato (checksum)", "パッケージが破損しています（チェックサム）", "패키지가 손상되었습니다(체크섬)", "Pakiet jest uszkodzony (suma kontrolna)", "Пакет поврежден (контрольная сумма)", "软件包已损坏（校验和）", "แพ็กเกจเสียหาย (checksum)", "套件已損毀（校驗和）", "Пакет пошкоджений (контрольна сума)" } },
    { "err.traversal",  { "El paquete contiene rutas no permitidas", "The package contains forbidden paths", "Le paquet contient des chemins interdits", "Paket enthält unzulässige Pfade", "Il pacchetto contiene percorsi non consentiti", "パッケージに不正なパスがあります", "패키지에 허용되지 않는 경로가 있습니다", "Pakiet zawiera niedozwolone ścieżki", "Пакет содержит запрещенные пути", "软件包含有不允许的路径", "แพ็กเกจมีเส้นทางที่ไม่อนุญาต", "套件含有不允許的路徑", "Пакет містить заборонені шляхи" } },
    { "err.type",       { "El paquete contiene enlaces o ficheros especiales", "The package contains links or special files", "Le paquet contient des liens ou des fichiers spéciaux", "Paket enthält Links oder Spezialdateien", "Il pacchetto contiene link o file speciali", "パッケージにリンクや特殊ファイルがあります", "패키지에 링크나 특수 파일이 있습니다", "Pakiet zawiera dowiązania lub pliki specjalne", "Пакет содержит ссылки или особые файлы", "软件包含有链接或特殊文件", "แพ็กเกจมีลิงก์หรือไฟล์พิเศษ", "套件含有連結或特殊檔案", "Пакет містить посилання або спеціальні файли" } },
    { "err.toobig",     { "El paquete es demasiado grande", "The package is too large", "Le paquet est trop volumineux", "Paket ist zu groß", "Il pacchetto è troppo grande", "パッケージが大きすぎます", "패키지가 너무 큽니다", "Pakiet jest za duży", "Пакет слишком большой", "软件包太大", "แพ็กเกจใหญ่เกินไป", "套件太大", "Пакет завеликий" } },
    { "err.nomanifest", { "Falta manifest.ini", "manifest.ini is missing", "manifest.ini est absent", "manifest.ini fehlt", "Manca manifest.ini", "manifest.ini がありません", "manifest.ini가 없습니다", "Brak pliku manifest.ini", "Отсутствует manifest.ini", "缺少 manifest.ini", "ไม่มี manifest.ini", "缺少 manifest.ini", "Немає manifest.ini" } },
    { "err.manifest",   { "manifest.ini no es válido", "manifest.ini is not valid", "manifest.ini n'est pas valide", "manifest.ini ist ungültig", "manifest.ini non è valido", "manifest.ini が無効です", "manifest.ini가 올바르지 않습니다", "Plik manifest.ini jest nieprawidłowy", "Недопустимый manifest.ini", "manifest.ini 无效", "manifest.ini ไม่ถูกต้อง", "manifest.ini 無效", "Недійсний manifest.ini" } },
    { "err.id",         { "Identificador de app no válido", "Invalid app identifier", "Identifiant d'app invalide", "Ungültige App-Kennung", "Identificatore app non valido", "無効なアプリ ID", "잘못된 앱 식별자", "Nieprawidłowy identyfikator aplikacji", "Недопустимый идентификатор приложения", "无效的 App 标识符", "ตัวระบุแอปไม่ถูกต้อง", "無效的 App 識別碼", "Недійсний ідентифікатор програми" } },
    { "err.version",    { "Número de versión no válido", "Invalid version number", "Numéro de version invalide", "Ungültige Versionsnummer", "Numero di versione non valido", "無効なバージョン番号", "잘못된 버전 번호", "Nieprawidłowy numer wersji", "Недопустимый номер версии", "无效的版本号", "หมายเลขเวอร์ชันไม่ถูกต้อง", "無效的版本號碼", "Недійсний номер версії" } },
    { "err.arch",       { "La app no es para este reproductor", "This app is not for this player", "Cette app n'est pas pour ce lecteur", "Diese App ist nicht für diesen Player", "Questa app non è per questo lettore", "このプレーヤー用のアプリではありません", "이 플레이어용 앱이 아닙니다", "Ta aplikacja nie jest dla tego odtwarzacza", "Приложение не для этого плеера", "此 App 不适用于本播放器", "แอปนี้ไม่ใช่สำหรับเครื่องเล่นนี้", "此 App 不適用於本播放器", "Програма не для цього плеєра" } },
    { "err.newer",      { "Necesita una versión más nueva de hibymod", "Requires a newer hibymod", "Nécessite une version plus récente de hibymod", "Erfordert ein neueres hibymod", "Richiede un hibymod più recente", "新しい hibymod が必要です", "최신 hibymod가 필요합니다", "Wymaga nowszego hibymod", "Нужна более новая версия hibymod", "需要更新版本的 hibymod", "ต้องใช้ hibymod เวอร์ชันใหม่กว่า", "需要較新版本的 hibymod", "Потрібна новіша версія hibymod" } },
    { "err.noexec",     { "Falta el ejecutable de la app", "The app executable is missing", "L'exécutable de l'app est absent", "Programmdatei der App fehlt", "Manca l'eseguibile dell'app", "アプリの実行ファイルがありません", "앱 실행 파일이 없습니다", "Brak pliku wykonywalnego aplikacji", "Отсутствует исполняемый файл", "缺少 App 可执行文件", "ไม่มีไฟล์ปฏิบัติการของแอป", "缺少 App 執行檔", "Немає виконуваного файлу програми" } },
    { "err.elf",        { "El ejecutable no es compatible", "The executable is not compatible", "L'exécutable n'est pas compatible", "Programmdatei ist nicht kompatibel", "L'eseguibile non è compatibile", "実行ファイルに互換性がありません", "실행 파일이 호환되지 않습니다", "Plik wykonywalny jest niezgodny", "Исполняемый файл несовместим", "可执行文件不兼容", "ไฟล์ปฏิบัติการไม่รองรับ", "執行檔不相容", "Виконуваний файл несумісний" } },
    { "err.space",      { "No hay espacio suficiente", "Not enough free space", "Espace libre insuffisant", "Nicht genug freier Speicher", "Spazio libero insufficiente", "空き容量が足りません", "여유 공간이 부족합니다", "Za mało wolnego miejsca", "Недостаточно места", "可用空间不足", "พื้นที่ว่างไม่พอ", "可用空間不足", "Недостатньо місця" } },
    { "err.icon",       { "El icono no es válido", "The icon is not valid", "L'icône n'est pas valide", "Symbol ist ungültig", "L'icona non è valida", "アイコンが無効です", "아이콘이 올바르지 않습니다", "Ikona jest nieprawidłowa", "Недопустимый значок", "图标无效", "ไอคอนไม่ถูกต้อง", "圖示無效", "Недійсна іконка" } },
    { "err.dup",        { "El paquete tiene ficheros repetidos", "The package has duplicate files", "Le paquet contient des fichiers en double", "Paket enthält doppelte Dateien", "Il pacchetto ha file duplicati", "パッケージに重複ファイルがあります", "패키지에 중복 파일이 있습니다", "Pakiet zawiera zduplikowane pliki", "В пакете повторяются файлы", "软件包含有重复文件", "แพ็กเกจมีไฟล์ซ้ำ", "套件含有重複檔案", "У пакеті є повторювані файли" } },
    { "err.swap",       { "No se pudo activar la nueva versión", "Could not activate the new version", "Impossible d'activer la nouvelle version", "Neue Version nicht aktivierbar", "Impossibile attivare la nuova versione", "新しいバージョンを有効にできません", "새 버전을 활성화할 수 없습니다", "Nie można włączyć nowej wersji", "Не удалось включить новую версию", "无法启用新版本", "เปิดใช้เวอร์ชันใหม่ไม่ได้", "無法啟用新版本", "Не вдалося ввімкнути нову версію" } },
    { "err.notinst",    { "La app no está instalada", "The app is not installed", "L'app n'est pas installée", "App ist nicht installiert", "L'app non è installata", "アプリはインストールされていません", "앱이 설치되어 있지 않습니다", "Aplikacja nie jest zainstalowana", "Приложение не установлено", "App 未安装", "ไม่ได้ติดตั้งแอปนี้", "App 未安裝", "Програму не встановлено" } },
    { "err.nomem",      { "No hay memoria suficiente", "Not enough memory", "Mémoire insuffisante", "Nicht genug Arbeitsspeicher", "Memoria insufficiente", "メモリが足りません", "메모리가 부족합니다", "Za mało pamięci", "Недостаточно памяти", "内存不足", "หน่วยความจำไม่พอ", "記憶體不足", "Недостатньо пам'яті" } },
    { "st.verifying",   { "Comprobando…", "Verifying…", "Vérification…", "Wird geprüft …", "Verifica…", "確認中…", "확인 중…", "Sprawdzanie…", "Проверка…", "正在验证…", "กำลังตรวจสอบ…", "正在驗證⋯", "Перевірка…" } },
    { "st.installing",  { "Instalando…", "Installing…", "Installation…", "Wird installiert …", "Installazione…", "インストール中…", "설치 중…", "Instalowanie…", "Установка…", "正在安装…", "กำลังติดตั้ง…", "正在安裝⋯", "Встановлення…" } },
    { "st.icons",       { "Preparando icono…", "Preparing icon…", "Préparation de l'icône…", "Symbol wird erstellt …", "Preparazione icona…", "アイコンを準備中…", "아이콘 준비 중…", "Przygotowywanie ikony…", "Подготовка значка…", "正在准备图标…", "กำลังเตรียมไอคอน…", "正在準備圖示⋯", "Підготовка іконки…" } },
    { "st.installed",   { "Instalada", "Installed", "Installée", "Installiert", "Installata", "インストール済み", "설치됨", "Zainstalowana", "Установлено", "已安装", "ติดตั้งแล้ว", "已安裝", "Встановлено" } },
    { "st.notinst",     { "No instalada", "Not installed", "Non installée", "Nicht installiert", "Non installata", "未インストール", "설치되지 않음", "Niezainstalowana", "Не установлено", "未安装", "ยังไม่ได้ติดตั้ง", "未安裝", "Не встановлено" } },
    { "st.update",      { "Actualizar", "Update", "Mettre à jour", "Aktualisieren", "Aggiorna", "アップデート", "업데이트", "Uaktualnij", "Обновить", "更新", "อัปเดต", "更新", "Оновити" } },
    { "st.install",     { "Instalar", "Install", "Installer", "Installieren", "Installa", "インストール", "설치", "Zainstaluj", "Установить", "安装", "ติดตั้ง", "安裝", "Встановити" } },
    { "st.current",     { "Al día", "Up to date", "À jour", "Aktuell", "Aggiornata", "最新", "최신", "Aktualna", "Актуально", "已是最新", "เป็นปัจจุบัน", "已是最新", "Актуально" } },
    { "st.older",       { "La instalada es más nueva", "Installed version is newer", "La version installée est plus récente", "Installierte Version ist neuer", "La versione installata è più recente", "インストール済みの方が新しいです", "설치된 버전이 더 최신입니다", "Zainstalowana wersja jest nowsza", "Установленная версия новее", "已安装的版本更新", "เวอร์ชันที่ติดตั้งใหม่กว่า", "已安裝的版本較新", "Встановлена версія новіша" } },
    { "st.uninstall",   { "Desinstalar", "Uninstall", "Désinstaller", "Deinstallieren", "Disinstalla", "アンインストール", "제거", "Odinstaluj", "Удалить", "卸载", "ถอนการติดตั้ง", "解除安裝", "Видалити" } },
    { "st.uninstalling",{ "Desinstalando…", "Uninstalling…", "Désinstallation…", "Wird deinstalliert …", "Disinstallazione…", "アンインストール中…", "제거 중…", "Odinstalowywanie…", "Удаление…", "正在卸载…", "กำลังถอนการติดตั้ง…", "正在解除安裝⋯", "Видалення…" } },
    { "st.uninstalled", { "Desinstalada", "Uninstalled", "Désinstallée", "Deinstalliert", "Disinstallata", "アンインストール済み", "제거됨", "Odinstalowana", "Удалено", "已卸载", "ถอนการติดตั้งแล้ว", "已解除安裝", "Видалено" } },
    { "st.keepdata",    { "Conservar sus datos", "Keep its data", "Conserver ses données", "Daten behalten", "Conserva i dati", "データを残す", "데이터 유지", "Zachowaj dane", "Сохранить данные", "保留其数据", "เก็บข้อมูลไว้", "保留其資料", "Зберегти дані" } },
    { "st.damaged",     { "Paquete dañado", "Damaged package", "Paquet endommagé", "Beschädigtes Paket", "Pacchetto danneggiato", "破損したパッケージ", "손상된 패키지", "Uszkodzony pakiet", "Поврежденный пакет", "软件包已损坏", "แพ็กเกจเสียหาย", "套件已損毀", "Пошкоджений пакет" } },
    { "st.stopplayer",  { "Esta app detiene la música mientras está abierta", "This app stops the music while open", "Cette app arrête la musique pendant son utilisation", "Diese App stoppt die Musik, solange sie offen ist", "Questa app ferma la musica finché è aperta", "このアプリを開いている間は音楽が止まります", "이 앱이 열려 있는 동안 음악이 정지됩니다", "Ta aplikacja zatrzymuje muzykę, gdy jest otwarta", "Пока приложение открыто, музыка остановлена", "此 App 打开时音乐会停止", "แอปนี้จะหยุดเพลงขณะเปิดอยู่", "此 App 開啟時音樂會停止", "Поки програма відкрита, музика зупиняється" } },
    { "st.noapps",      { "No hay paquetes en la carpeta Apps de la tarjeta", "No packages in the card's Apps folder", "Aucun paquet dans le dossier Apps de la carte", "Keine Pakete im Ordner Apps der Karte", "Nessun pacchetto nella cartella Apps della scheda", "カードの Apps フォルダにパッケージがありません", "카드의 Apps 폴더에 패키지가 없습니다", "Brak pakietów w folderze Apps na karcie", "В папке Apps на карте нет пакетов", "存储卡的 Apps 文件夹中没有软件包", "ไม่มีแพ็กเกจในโฟลเดอร์ Apps ของการ์ด", "記憶卡的 Apps 資料夾中沒有套件", "У папці Apps на картці немає пакетів" } },
};
const char *hpk_msg(const char *key, int lang)
{
    if (lang < 0 || lang >= HPK_NLANG) lang = HPK_LANG_EN;
    for (unsigned i = 0; i < sizeof hpk_tr / sizeof hpk_tr[0]; i++)
        if (!strcmp(hpk_tr[i].key, key)) return hpk_tr[i].t[lang] && hpk_tr[i].t[lang][0] ? hpk_tr[i].t[lang] : hpk_tr[i].t[HPK_LANG_EN];
    return key;
}

/* ------------------------------------------------------------------ utilidades */
static void hpk_cpy(char *d, int n, const char *s)   /* copia truncando en frontera UTF-8 */
{
    int l = (int)strlen(s);
    if (l > n - 1) { l = n - 1; while (l > 0 && ((unsigned char)s[l] & 0xC0) == 0x80) l--; }
    memcpy(d, s, l); d[l] = 0;
}

/* recorre p en profundidad sin seguir enlaces; borra (del=1) o suma tamanos. Devuelve bytes */
static long long hpk_tree(const char *p, int del, int depth)
{
    struct stat s;
    if (lstat(p, &s)) return 0;
    if (!S_ISDIR(s.st_mode)) { if (del) unlink(p); return s.st_size; }
    long long n = 0;
    for (int pass = 0; pass < 3; pass++) {               /* borrar mientras se lee: se repasa */
        DIR *d = depth < 32 ? opendir(p) : 0;
        if (d) {
            struct dirent *de; char c[512];
            while ((de = readdir(d))) {
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
                if (snprintf(c, sizeof c, "%s/%s", p, de->d_name) >= (int)sizeof c) continue;
                n += hpk_tree(c, del, depth + 1);
            }
            closedir(d);
        }
        if (!del || !rmdir(p) || errno != ENOTEMPTY) break;
    }
    return n;
}
static void hpk_rmrf(const char *p) { hpk_tree(p, 1, 0); }
static long long hpk_du(const char *p) { return hpk_tree(p, 0, 0); }

static int hpk_isdir(const char *p) { struct stat s; return !lstat(p, &s) && S_ISDIR(s.st_mode); }

static int hpk_mkpath(const char *p)                 /* mkdir -p de una ruta absoluta */
{
    char b[512]; snprintf(b, sizeof b, "%s", p);
    for (char *q = b + 1; ; q++) {
        if (*q == '/' || !*q) {
            char c = *q; *q = 0;
            if (mkdir(b, 0755) && errno != EEXIST) return -1;
            if (!hpk_isdir(b)) return -1;
            if (!c) break;
            *q = c;
        }
    }
    return 0;
}

/* crea los directorios de rel dentro de base (base existe); last=1 incluye el ultimo componente */
static int hpk_mkdirs(const char *base, const char *rel, int last)
{
    char b[512]; int n = snprintf(b, sizeof b, "%s/%s", base, rel);
    if (n >= (int)sizeof b) return -HPK_E_TOOBIG;
    char *q = b + strlen(base) + 1;
    for (;; q++) {
        if (*q == '/' || (!*q && last)) {
            char c = *q; *q = 0;
            if (mkdir(b, 0755) && errno != EEXIST) return -HPK_E_IO;
            if (!hpk_isdir(b)) return -HPK_E_DUP;   /* ya existe un fichero con ese nombre */
            if (!c) break;
            *q = c;
        }
        if (!*q) break;
    }
    return 0;
}

static void hpk_fsync_dir(const char *p)
{
    int d = open(p, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (d >= 0) { fsync(d); close(d); }
}

static int hpk_rd_all(int fd, void *b, long n, long long off)
{
    char *p = b;
    while (n > 0) {
        ssize_t r = pread(fd, p, n > HPK_BUF ? HPK_BUF : n, off);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; return -1; }
        p += r; n -= r; off += r;
    }
    return 0;
}
static int hpk_wr_all(int fd, const void *b, long n)
{
    const char *p = b;
    while (n > 0) {
        ssize_t r = write(fd, p, n);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; return -1; }
        p += r; n -= r;
    }
    return 0;
}

/* ------------------------------------------------------------------ versiones (semver) */
static int hpk_semver(const char *v, long n[3])
{
    const char *p = v;
    for (int i = 0; i < 3; i++) {
        if (*p < '0' || *p > '9') return -1;
        if (*p == '0' && p[1] >= '0' && p[1] <= '9') return -1;   /* sin ceros a la izquierda */
        long x = 0;
        while (*p >= '0' && *p <= '9') { x = x * 10 + (*p++ - '0'); if (x > 999999) return -1; }
        n[i] = x;
        if (i < 2 && *p++ != '.') return -1;
    }
    if (*p == '-' || *p == '+') {
        if (!p[1]) return -1;
        for (const char *q = p + 1; *q; q++)
            if (!((*q >= '0' && *q <= '9') || (*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || *q == '.' || *q == '-' || *q == '+')) return -1;
    } else if (*p) return -1;
    return 0;
}
int hpk_vercmp(const char *a, const char *b)
{
    long x[3], y[3];
    int ea = hpk_semver(a, x), eb = hpk_semver(b, y);
    if (ea || eb) return ea - eb;            /* las invalidas cuentan como menores */
    for (int i = 0; i < 3; i++) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    const char *pa = strchr(a, '-'), *pb = strchr(b, '-');
    const char *ba = strchr(a, '+'), *bb = strchr(b, '+');
    if (pa && ba && ba < pa) pa = 0;
    if (pb && bb && bb < pb) pb = 0;
    if (!pa && !pb) return 0;
    if (!pa) return 1;                       /* 1.0.0 > 1.0.0-beta */
    if (!pb) return -1;
    int la = ba && ba > pa ? (int)(ba - pa) : (int)strlen(pa), lb = bb && bb > pb ? (int)(bb - pb) : (int)strlen(pb);
    int c = strncmp(pa, pb, la < lb ? la : lb);
    return c ? c : la - lb;
}

/* ------------------------------------------------------------------ tar ustar */
struct hpk_ent { char name[260]; char type; long long size, off; unsigned mode; int root; };

static int hpk_oct(const unsigned char *p, int n, long long *v)
{
    if (p[0] & 0x80) return -1;              /* codificacion base-256 de GNU: no se admite */
    int i = 0, any = 0; long long r = 0;
    while (i < n && p[i] == ' ') i++;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) {
        if (r > (LLONG_MAX >> 3)) return -1;
        r = r * 8 + (p[i] - '0'); any = 1;
    }
    if (i < n && p[i] != ' ' && p[i] != 0) return -1;
    *v = r;
    return any ? 0 : -1;
}

/* ruta del tar -> relativa limpia ("a/b"). 0 bien, 1 = la raiz ("./"), <0 error */
static int hpk_norm(char *s)
{
    char out[260]; int o = 0;
    if (s[0] == '/') return -HPK_E_TRAVERSAL;
    const char *p = s;
    while (*p) {
        const char *q = p; while (*q && *q != '/') q++;
        int n = (int)(q - p);
        if (n == 0 || (n == 1 && p[0] == '.')) { /* "//" o "./": nada */ }
        else if (n == 2 && p[0] == '.' && p[1] == '.') return -HPK_E_TRAVERSAL;
        else {
            for (int i = 0; i < n; i++) {
                unsigned char c = p[i];
                if (c < 0x20 || c == 0x7f || c == '\\') return -HPK_E_TRAVERSAL;
            }
            if (o + n + 2 > (int)sizeof out) return -HPK_E_TOOBIG;
            if (o) out[o++] = '/';
            memcpy(out + o, p, n); o += n;
        }
        p = *q ? q + 1 : q;
    }
    if (!o) return 1;
    out[o] = 0; memcpy(s, out, o + 1);
    return 0;
}

/* lee la cabecera de off. 1 = entrada, 0 = fin del archivo, <0 error */
static int hpk_hdr(int fd, long long fsize, long long off, struct hpk_ent *e)
{
    unsigned char b[512];
    if (off == fsize) return 0;                          /* sin bloques de cierre: se tolera */
    if (off + 512 > fsize) return -HPK_E_FORMAT;
    if (hpk_rd_all(fd, b, 512, off)) return -HPK_E_IO;
    int zero = 1;
    for (int i = 0; i < 512; i++) if (b[i]) { zero = 0; break; }
    if (zero) return 0;
    if (memcmp(b + 257, "ustar", 5)) return -HPK_E_FORMAT;
    long long ck;
    if (hpk_oct(b + 148, 8, &ck)) return -HPK_E_CHECKSUM;
    unsigned su = 0; int ss = 0;
    for (int i = 0; i < 512; i++) { unsigned c = (i >= 148 && i < 156) ? ' ' : b[i]; su += c; ss += (i >= 148 && i < 156) ? ' ' : (signed char)b[i]; }
    if (ck != su && ck != ss) return -HPK_E_CHECKSUM;
    memset(e, 0, sizeof *e);
    char nm[101], px[156];
    memcpy(nm, b, 100); nm[100] = 0;
    memcpy(px, b + 345, 155); px[155] = 0;
    if (px[0]) snprintf(e->name, sizeof e->name, "%s/%s", px, nm); else snprintf(e->name, sizeof e->name, "%s", nm);
    e->type = b[156];
    long long v;
    if (hpk_oct(b + 124, 12, &v)) return hpk_fail(HPK_E_TOOBIG, e->name);
    e->size = v;
    e->mode = hpk_oct(b + 100, 8, &v) ? 0644 : (unsigned)(v & 07777);
    e->off = off + 512;
    if (e->type != '0' && e->type != 0 && e->type != '5') return hpk_fail(HPK_E_TYPE, e->name);
    if (e->size > HPK_MAX_FILE) return hpk_fail(HPK_E_TOOBIG, e->name);
    if (e->off + e->size > fsize) return hpk_fail(HPK_E_FORMAT, e->name);
    int r = hpk_norm(e->name);
    if (r < 0) return hpk_fail(-r, e->name);
    e->root = r == 1;
    if (e->root && e->type != '5') return hpk_fail(HPK_E_TRAVERSAL, "/");
    return 1;
}

/* recorre el tar; cb devuelve 0 para seguir, >0 para parar (exito), <0 error */
typedef int (*hpk_walk_cb)(int fd, const struct hpk_ent *e, void *ctx);
static int hpk_walk(int fd, long long fsize, hpk_walk_cb cb, void *ctx)
{
    long long off = 0; int n = 0;
    for (;;) {
        struct hpk_ent e;
        int r = hpk_hdr(fd, fsize, off, &e);
        if (r < 0) return r;
        if (r == 0) return n ? 0 : -HPK_E_FORMAT;
        if (++n > HPK_MAX_ENTRIES) return -HPK_E_TOOBIG;
        off = e.off + ((e.size + 511) & ~511LL);
        if (e.root) continue;
        if (cb && (r = cb(fd, &e, ctx))) return r;
    }
}

/* ------------------------------------------------------------------ manifest.ini */
static int hpk_valid_id(const char *s)
{
    int n = (int)strlen(s);
    if (n < 1 || n > HPK_ID_MAX) return 0;
    if (!((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= '0' && s[0] <= '9'))) return 0;
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return 0;
        if (c == '.' && s[i + 1] == '.') return 0;
    }
    return 1;
}

static char *hpk_trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
    return s;
}

/* rellena in con el texto del manifest (se modifica). Devuelve 0 o -HPK_E_* (detalle = campo) */
static int hpk_mf_parse(char *txt, struct hpk_info *in)
{
    char arch[24] = "", sp[8] = "", ram[16] = "";
    int has_name[3] = { 0 }, has_desc[3] = { 0 };
    memset(in, 0, sizeof *in);
    in->icon_off = in->icon_len = -1;
    if (!memcmp(txt, "\xEF\xBB\xBF", 3)) txt += 3;
    for (char *line = txt; line && *line; ) {
        char *nl = strchr(line, '\n'); if (nl) *nl++ = 0;
        char *l = hpk_trim(line); line = nl;
        if (!*l || *l == '#' || *l == ';' || *l == '[') continue;
        char *eq = strchr(l, '='); if (!eq) continue;
        *eq = 0;
        char *k = hpk_trim(l), *v = hpk_trim(eq + 1);
        int vl = (int)strlen(v);
        if (vl >= 2 && v[0] == '"' && v[vl - 1] == '"') { v[vl - 1] = 0; v++; }
        for (char *q = v; *q; q++) if ((unsigned char)*q < 0x20) *q = ' ';
        if (!strcmp(k, "id")) hpk_cpy(in->id, sizeof in->id, strlen(v) > HPK_ID_MAX ? "!" : v);
        else if (!strcmp(k, "version")) hpk_cpy(in->version, sizeof in->version, v);
        else if (!strcmp(k, "min_hibymod")) hpk_cpy(in->min_hibymod, sizeof in->min_hibymod, v);
        else if (!strcmp(k, "name") || !strcmp(k, "name.es")) { hpk_cpy(in->name[0], HPK_NAME_MAX, v); has_name[0] = 1; }
        else if (!strcmp(k, "name.en")) { hpk_cpy(in->name[1], HPK_NAME_MAX, v); has_name[1] = 1; }
        else if (!strcmp(k, "name.fr")) { hpk_cpy(in->name[2], HPK_NAME_MAX, v); has_name[2] = 1; }
        else if (!strcmp(k, "description") || !strcmp(k, "description.es")) { hpk_cpy(in->desc[0], HPK_DESC_MAX, v); has_desc[0] = 1; }
        else if (!strcmp(k, "description.en")) { hpk_cpy(in->desc[1], HPK_DESC_MAX, v); has_desc[1] = 1; }
        else if (!strcmp(k, "description.fr")) { hpk_cpy(in->desc[2], HPK_DESC_MAX, v); has_desc[2] = 1; }
        else if (!strcmp(k, "exec")) hpk_cpy(in->exec, sizeof in->exec, strlen(v) >= sizeof in->exec ? "/" : v);
        else if (!strcmp(k, "arch")) hpk_cpy(arch, sizeof arch, v);
        else if (!strcmp(k, "input")) hpk_cpy(in->input, sizeof in->input, v);
        else if (!strcmp(k, "stop_player")) hpk_cpy(sp, sizeof sp, v);
        else if (!strcmp(k, "ram_kb")) hpk_cpy(ram, sizeof ram, v);
        else if (!strcmp(k, "category")) hpk_cpy(in->category, sizeof in->category, v);
        else if (!strcmp(k, "author")) hpk_cpy(in->author, sizeof in->author, v);
        else if (!strcmp(k, "homepage")) hpk_cpy(in->homepage, sizeof in->homepage, v);
        else if (!strcmp(k, "refresh")) in->refresh_full = !strcmp(v, "full");
        else if (!strcmp(k, "back")) in->back_app = !strcmp(v, "app");
        /* claves desconocidas: se ignoran (compatibilidad hacia delante) */
    }
    for (int i = 1; i < 3; i++) {
        if (!has_name[i] || !in->name[i][0]) memcpy(in->name[i], in->name[0], HPK_NAME_MAX);
        if (!has_desc[i] || !in->desc[i][0]) memcpy(in->desc[i], in->desc[0], HPK_DESC_MAX);
    }
    if (!in->id[0]) return hpk_fail(HPK_E_MANIFEST, "id");
    if (!hpk_valid_id(in->id)) return hpk_fail(HPK_E_ID, in->id);
    long n[3];
    if (!in->version[0] || hpk_semver(in->version, n)) return hpk_fail(HPK_E_VERSION, "version");
    if (!in->min_hibymod[0] || hpk_semver(in->min_hibymod, n)) return hpk_fail(HPK_E_MANIFEST, "min_hibymod");
    if (!has_name[0] || !in->name[0][0]) return hpk_fail(HPK_E_MANIFEST, "name");
    if (!in->exec[0]) return hpk_fail(HPK_E_MANIFEST, "exec");
    int r = hpk_norm(in->exec);
    if (r) return hpk_fail(r < 0 ? -r : HPK_E_MANIFEST, "exec");
    if (strcmp(arch, "mipsel-hf")) return hpk_fail(arch[0] ? HPK_E_ARCH : HPK_E_MANIFEST, arch[0] ? arch : "arch");
    if (strcmp(in->input, "touch") && strcmp(in->input, "keys") && strcmp(in->input, "both")) return hpk_fail(HPK_E_MANIFEST, "input");
    if (!sp[0] || !strcmp(sp, "0")) in->stop_player = 0;
    else if (!strcmp(sp, "1")) in->stop_player = 1;
    else return hpk_fail(HPK_E_MANIFEST, "stop_player");
    if (ram[0]) {
        char *e; long k = strtol(ram, &e, 10);
        if (*e || k < 0 || k > (1L << 20)) return hpk_fail(HPK_E_MANIFEST, "ram_kb");
        in->ram_kb = (int)k;
    }
    if (hpk_vercmp(in->min_hibymod, HPK_HIBYMOD_VERSION) > 0) return hpk_fail(HPK_E_NEWER, in->min_hibymod);
    return 0;
}

static int hpk_read_installed(const char *id, struct hpk_info *in)
{
    char p[400]; snprintf(p, sizeof p, "%s/apps/%s/manifest.ini", hpk_root(), id);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -HPK_E_NOTINST;
    char *b = malloc(HPK_MAX_MANIFEST + 1);
    if (!b) { close(fd); return -HPK_E_NOMEM; }
    ssize_t n = read(fd, b, HPK_MAX_MANIFEST);
    close(fd);
    if (n < 0) { free(b); return -HPK_E_IO; }
    b[n] = 0;
    int r = hpk_mf_parse(b, in);
    free(b);
    return r;
}

/* ------------------------------------------------------------------ ELF */
static unsigned hpk_r16(const unsigned char *p) { return p[0] | p[1] << 8; }
static unsigned hpk_r32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }

/* ELF32 LE MIPS (ISA <= mips32r2), sin interprete (estatico), coma flotante hard si lo indica */
static int hpk_elf_check(int fd, long long off, long long size, const char *name)
{
    unsigned char eh[52];
    if (size < 52 || hpk_rd_all(fd, eh, 52, off)) return hpk_fail(HPK_E_ELF, name);
    if (memcmp(eh, "\177ELF", 4) || eh[4] != 1 || eh[5] != 1) return hpk_fail(HPK_E_ELF, name);
    unsigned type = hpk_r16(eh + 16), mach = hpk_r16(eh + 18), flags = hpk_r32(eh + 36);
    if ((type != 2 && type != 3) || mach != 8) return hpk_fail(HPK_E_ELF, name);
    unsigned isa = flags & 0xf0000000u;
    if (isa != 0 && isa != 0x10000000u && isa != 0x50000000u && isa != 0x70000000u) return hpk_fail(HPK_E_ELF, name);
    unsigned phoff = hpk_r32(eh + 28), phes = hpk_r16(eh + 42), phn = hpk_r16(eh + 44);
    if (phes != 32 || phn == 0 || phn > 64 || (long long)phoff + phn * 32LL > size) return hpk_fail(HPK_E_ELF, name);
    unsigned char ph[64 * 32];
    if (hpk_rd_all(fd, ph, phn * 32, off + phoff)) return hpk_fail(HPK_E_ELF, name);
    for (unsigned i = 0; i < phn; i++) {
        const unsigned char *p = ph + i * 32;
        unsigned pt = hpk_r32(p), po = hpk_r32(p + 4), pf = hpk_r32(p + 16);
        if (pt == 3) return hpk_fail(HPK_E_ELF, "PT_INTERP");          /* dinamico */
        if (pt == 0x70000003u && pf >= 24 && (long long)po + 24 <= size) { /* PT_MIPS_ABIFLAGS */
            unsigned char af[24];
            if (hpk_rd_all(fd, af, 24, off + po)) return hpk_fail(HPK_E_ELF, name);
            if (af[7] == 3) return hpk_fail(HPK_E_ELF, "soft-float");  /* Val_GNU_MIPS_ABI_FP_SOFT */
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ iconos */
/* cobertura (0..1) del pixel de centro (px,py) en un rectangulo redondeado (SDF + antialias) */
static float hpk_rrcov(float px, float py, float x0, float y0, float x1, float y1, float r)
{
    float cx = (x0 + x1) * .5f, cy = (y0 + y1) * .5f;
    float qx = fabsf(px - cx) - ((x1 - x0) * .5f - r), qy = fabsf(py - cy) - ((y1 - y0) * .5f - r);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float d = sqrtf(ox * ox + oy * oy) + (qx > qy ? (qx < 0 ? qx : 0) : (qy < 0 ? qy : 0)) - r;
    float c = .5f - d;
    return c < 0 ? 0 : c > 1 ? 1 : c;
}

/* src RGBA sw x sh (se recorta al cuadrado central) -> icono s x s con la mascara de los iconos
 * del launcher (theme/launcher_icons.js): radio 22,37 %, brillo blanco 30 %->0 en la mitad
 * superior y borde interior de 1 px blanco al 28 %. Salida RGB565 + alfa (sin premultiplicar). */
static void hpk_icon_render(const unsigned char *src, int sw, int sh, int s, uint16_t *rgb, uint8_t *a)
{
    int m = sw < sh ? sw : sh, ox = (sw - m) / 2, oy = (sh - m) / 2;
    float r = s * 0.2237f;
    for (int y = 0; y < s; y++) for (int x = 0; x < s; x++) {
        float cr = 0, cg = 0, cb = 0, ca = 0;
        if (m >= s) {                                 /* reduccion: promedio por area */
            int x0 = x * m / s, x1 = (x + 1) * m / s, y0 = y * m / s, y1 = (y + 1) * m / s;
            if (x1 <= x0) x1 = x0 + 1;
            if (y1 <= y0) y1 = y0 + 1;
            for (int j = y0; j < y1; j++) for (int i = x0; i < x1; i++) {
                const unsigned char *p = src + ((size_t)(oy + j) * sw + ox + i) * 4;
                float al = p[3];
                cr += p[0] * al; cg += p[1] * al; cb += p[2] * al; ca += al;
            }
            int cnt = (x1 - x0) * (y1 - y0);
            if (ca > 0) { cr /= ca; cg /= ca; cb /= ca; }
            ca /= cnt;
        } else {                                      /* ampliacion: bilineal */
            float fx = (x + .5f) * m / s - .5f, fy = (y + .5f) * m / s - .5f;
            if (fx < 0) fx = 0;
            if (fy < 0) fy = 0;
            int ix = (int)fx, iy = (int)fy; float tx = fx - ix, ty = fy - iy;
            int jx = ix + 1 < m ? ix + 1 : ix, jy = iy + 1 < m ? iy + 1 : iy;
            const int px[4] = { ix, jx, ix, jx }, py[4] = { iy, iy, jy, jy };
            const float w[4] = { (1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty };
            for (int k = 0; k < 4; k++) {
                const unsigned char *p = src + ((size_t)(oy + py[k]) * sw + ox + px[k]) * 4;
                float al = p[3] * w[k];
                cr += p[0] * al; cg += p[1] * al; cb += p[2] * al; ca += al;
            }
            if (ca > 0) { cr /= ca; cg /= ca; cb /= ca; }
        }
        /* el icono debe ser opaco; si no, se compone sobre negro (como iOS) */
        float k = ca / 255.f;
        cr *= k; cg *= k; cb *= k;
        float pxc = x + .5f, pyc = y + .5f;
        (void)pyc;                                    /* sin brillo superior: como los iconos del sistema */
        float outer = hpk_rrcov(pxc, pyc, 0, 0, s, s, r);
        float ring = outer - hpk_rrcov(pxc, pyc, 1, 1, s - 1, s - 1, r - 1);
        float st = (ring > 0 ? ring : 0) * (pyc < s * .5f ? .10f : 0);          /* filo de luz solo arriba */
        cr += (255 - cr) * st; cg += (255 - cg) * st; cb += (255 - cb) * st;
        /* tramado ordenado 4x4 (como tools/shell_assets.py): sin bandas en el panel de 16 bits */
        static const float BAY[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
        float dth = (BAY[(y & 3) * 4 + (x & 3)] + .5f) / 16.f - .5f;
        int R = (int)(cr * 31 / 255 + .5f + dth), G = (int)(cg * 63 / 255 + .5f + dth), B = (int)(cb * 31 / 255 + .5f + dth);
        if (R < 0) R = 0; if (R > 31) R = 31; if (G < 0) G = 0; if (G > 63) G = 63; if (B < 0) B = 0; if (B > 31) B = 31;
        rgb[y * s + x] = (uint16_t)(R << 11 | G << 5 | B);
        a[y * s + x] = (uint8_t)(outer * 255 + .5f);
    }
}

/* icono de reserva si el paquete no trae icon.png: degradado con el tono derivado del id */
static unsigned char *hpk_icon_placeholder(const char *id, int *w, int *h)
{
    unsigned hs = 2166136261u;
    for (const char *p = id; *p; p++) hs = (hs ^ (unsigned char)*p) * 16777619u;
    float hue = (hs % 360) / 60.f;
    float rgb[3]; int i = (int)hue; float f = hue - i;
    float t[6][3] = { { 1, f, 0 }, { 1 - f, 1, 0 }, { 0, 1, f }, { 0, 1 - f, 1 }, { f, 0, 1 }, { 1, 0, 1 - f } };
    memcpy(rgb, t[i % 6], sizeof rgb);
    int n = 64; unsigned char *b = malloc(n * n * 4);
    if (!b) return 0;
    for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) {
        float l = 1.f - .45f * (y + x * .35f) / (n * 1.35f);
        unsigned char *p = b + (y * n + x) * 4;
        for (int c = 0; c < 3; c++) p[c] = (unsigned char)((60 + 170 * rgb[c]) * l);
        p[3] = 255;
    }
    *w = *h = n;
    return b;
}

static unsigned char *hpk_png_decode(const unsigned char *png, int len, int *w, int *h)
{
    int c;
    if (!stbi_info_from_memory(png, len, w, h, &c)) return 0;
    if (*w < 16 || *h < 16 || *w > HPK_ICON_MAXPX || *h > HPK_ICON_MAXPX) return 0;
    return stbi_load_from_memory(png, len, w, h, &c, 4);
}

/* decodifica el icono (de un .hpk abierto) o genera el de reserva */
static unsigned char *hpk_icon_src(int fd, long long off, long long len, const char *id, int *w, int *h)
{
    if (len <= 0) return hpk_icon_placeholder(id, w, h);
    if (len > HPK_MAX_ICON_BYTES) return 0;
    unsigned char *png = malloc(len);
    if (!png) return 0;
    unsigned char *img = 0;
    if (!hpk_rd_all(fd, png, (long)len, off)) img = hpk_png_decode(png, (int)len, w, h);
    free(png);
    return img;
}

static int hpk_write_ov(const char *path, int s, const uint16_t *rgb, const uint8_t *a)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    unsigned char hd[8] = { 'H', 'M', 'O', 'V', (unsigned char)s, (unsigned char)(s >> 8), (unsigned char)s, (unsigned char)(s >> 8) };
    unsigned char row[64 * 2];
    int ok = !hpk_wr_all(fd, hd, 8);
    for (int y = 0; ok && y < s; y++) {               /* RGB565 en little-endian, sea cual sea el anfitrion */
        for (int x = 0; x < s; x++) { row[x * 2] = rgb[y * s + x] & 255; row[x * 2 + 1] = rgb[y * s + x] >> 8; }
        ok = !hpk_wr_all(fd, row, s * 2);
    }
    ok = ok && !hpk_wr_all(fd, a, s * s) && !fsync(fd);
    return close(fd) || !ok ? -1 : 0;
}

int hpk_icon(const struct hpk_info *pkg, int s, unsigned short *rgb, unsigned char *a)
{
    if (s < 8 || s > 64) return -HPK_E_ICON;
    int fd = -1, w, h;
    if (pkg->icon_len > 0 && (fd = open(pkg->path, O_RDONLY | O_CLOEXEC)) < 0) return -HPK_E_OPEN;
    unsigned char *img = hpk_icon_src(fd, pkg->icon_off, fd < 0 ? 0 : pkg->icon_len, pkg->id, &w, &h);
    if (fd >= 0) close(fd);
    if (!img) return -HPK_E_ICON;
    hpk_icon_render(img, w, h, s, rgb, a);
    free(img);
    return 0;
}

/* ------------------------------------------------------------------ escaneo de la SD */
struct hpk_scan_ctx { char *mf; long mflen; int mf_seen; long long icon_off, icon_len; long long total; int dup; };
static int hpk_scan_cb(int fd, const struct hpk_ent *e, void *c)
{
    struct hpk_scan_ctx *x = c;
    if (e->type == '5') return 0;
    x->total += e->size;
    if (!strcmp(e->name, "manifest.ini")) {
        if (x->mf_seen) return hpk_fail(HPK_E_DUP, e->name);
        if (e->size > HPK_MAX_MANIFEST) return hpk_fail(HPK_E_TOOBIG, e->name);
        if (hpk_rd_all(fd, x->mf, (long)e->size, e->off)) return -HPK_E_IO;
        x->mf[e->size] = 0; x->mflen = (long)e->size; x->mf_seen = 1;
    } else if (!strcmp(e->name, "icon.png")) {
        if (x->icon_len >= 0) return hpk_fail(HPK_E_DUP, e->name);
        x->icon_off = e->off; x->icon_len = e->size;
    }
    return 0;
}
static int hpk_scan_stop_cb(int fd, const struct hpk_ent *e, void *c)   /* para al tener ambos */
{
    struct hpk_scan_ctx *x = c;
    int r = hpk_scan_cb(fd, e, c);
    return r ? r : (x->mf_seen && x->icon_len >= 0) ? 1 : 0;
}

static int hpk_cmp_info(const void *a, const void *b)
{
    const struct hpk_info *x = a, *y = b;
    return strcasecmp(x->name[0][0] ? x->name[0] : x->path, y->name[0][0] ? y->name[0] : y->path);
}

int hpk_scan(const char *dir, struct hpk_info *out, int max)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    char *mf = malloc(HPK_MAX_MANIFEST + 1);
    if (!mf) { closedir(d); return -HPK_E_NOMEM; }
    int n = 0; struct dirent *de;
    while (n < max && (de = readdir(d))) {
        int l = (int)strlen(de->d_name);
        if (l < 5 || de->d_name[0] == '.' || strcasecmp(de->d_name + l - 4, ".hpk")) continue;
        struct hpk_info *in = &out[n];
        char p[512];
        if (snprintf(p, sizeof p, "%s/%s", dir, de->d_name) >= (int)sizeof out->path) continue;
        struct stat s;
        if (stat(p, &s) || !S_ISREG(s.st_mode)) continue;
        int fd = open(p, O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        hpk_err_detail[0] = 0;
        struct hpk_scan_ctx x = { mf, 0, 0, -1, -1, 0, 0 };
        int r = hpk_walk(fd, s.st_size, hpk_scan_stop_cb, &x);
        close(fd);
        if (r >= 0 && !x.mf_seen) r = -HPK_E_NOMANIFEST;
        if (r >= 0) r = hpk_mf_parse(mf, in); else memset(in, 0, sizeof *in);
        if (r < 0 && !in->name[0][0]) {               /* danado: se muestra con el nombre del fichero */
            hpk_cpy(in->name[0], HPK_NAME_MAX, de->d_name);
            memcpy(in->name[1], in->name[0], HPK_NAME_MAX); memcpy(in->name[2], in->name[0], HPK_NAME_MAX);
        }
        in->status = r < 0 ? r : 0;
        in->size = s.st_size;
        in->icon_off = x.icon_off; in->icon_len = x.icon_len;
        hpk_cpy(in->path, sizeof in->path, p);
        in->installed[0] = 0;
        struct hpk_info old;
        if (in->id[0] && hpk_valid_id(in->id) && !hpk_read_installed(in->id, &old)) memcpy(in->installed, old.version, sizeof in->installed);
        n++;
    }
    closedir(d);
    free(mf);
    qsort(out, n, sizeof *out, hpk_cmp_info);
    return n;
}

/* ------------------------------------------------------------------ instalacion */
struct hpk_find_ctx { const char *name; long long off, size; int found; };
static int hpk_find_cb(int fd, const struct hpk_ent *e, void *c)
{
    (void)fd;
    struct hpk_find_ctx *x = c;
    if (strcmp(e->name, x->name)) return 0;
    if (e->type == '5') return hpk_fail(HPK_E_NOEXEC, e->name);
    x->off = e->off; x->size = e->size; x->found = 1;
    return 1;
}

struct hpk_x_ctx {
    char dir[320]; const char *exec; unsigned char *buf;
    long done, total; void (*progress)(long, long, void *); void *pctx;
};
static int hpk_x_cb(int fd, const struct hpk_ent *e, void *c)
{
    struct hpk_x_ctx *x = c;
    if (e->type == '5') { int r = hpk_mkdirs(x->dir, e->name, 1); return r ? hpk_fail(-r, e->name) : 0; }
    const char *sl = strrchr(e->name, '/');
    if (sl) {
        char par[260]; memcpy(par, e->name, sl - e->name); par[sl - e->name] = 0;
        int r = hpk_mkdirs(x->dir, par, 1);
        if (r) return hpk_fail(-r, e->name);
    }
    char p[600];
    if (snprintf(p, sizeof p, "%s/%s", x->dir, e->name) >= (int)sizeof p) return hpk_fail(HPK_E_TOOBIG, e->name);
    mode_t mode = ((e->mode & 0111) || !strcmp(e->name, x->exec)) ? 0755 : 0644;   /* sin suid/sgid */
    int o = open(p, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode);
    if (o < 0) return hpk_fail(errno == EEXIST ? HPK_E_DUP : errno == ENOSPC ? HPK_E_SPACE : HPK_E_IO, e->name);
    long long left = e->size, off = e->off;
    int err = 0;
    while (left > 0 && !err) {
        long n = left > HPK_BUF ? HPK_BUF : (long)left;
        if (hpk_rd_all(fd, x->buf, n, off)) { err = HPK_E_IO; break; }
        if (hpk_wr_all(o, x->buf, n)) { err = errno == ENOSPC ? HPK_E_SPACE : HPK_E_IO; break; }
        left -= n; off += n; x->done += n;
#ifdef HPK_TEST
        if (hpk_test_fail_at >= 0 && x->done >= hpk_test_fail_at) { err = HPK_E_IO; break; }
#endif
        if (x->progress) x->progress(x->done, x->total, x->pctx);
    }
    if (!err && (fchmod(o, mode) || fsync(o))) err = HPK_E_IO;
    if (close(o) && !err) err = HPK_E_IO;
    return err ? hpk_fail(err, e->name) : 0;
}

static int hpk_free_ok(long long need)
{
    long long avail;
#ifdef HPK_TEST
    const char *ff = getenv("HPK_FAKE_FREE");
    if (ff) avail = atoll(ff); else
#endif
    {
        struct statvfs v;
        if (statvfs(hpk_root(), &v)) return 1;        /* no se sabe: lo dira write() con ENOSPC */
        avail = (long long)v.f_bavail * v.f_frsize;
    }
    return avail >= need + HPK_SPACE_MARGIN;
}

void hpk_recover(void)
{
    char apps[256]; snprintf(apps, sizeof apps, "%s/apps", hpk_root());
    DIR *d = opendir(apps);
    if (!d) return;
    struct dirent *de;
    char names[32][HPK_ID_MAX + 8]; int n = 0;
    while ((de = readdir(d)) && n < 32)
        if (!strncmp(de->d_name, ".tmp-", 5) || !strncmp(de->d_name, ".old-", 5) || !strncmp(de->d_name, ".del-", 5))
            hpk_cpy(names[n++], sizeof names[0], de->d_name);
    closedir(d);
    for (int i = 0; i < n; i++) {
        char p[400], cur[400];
        snprintf(p, sizeof p, "%s/%s", apps, names[i]);
        snprintf(cur, sizeof cur, "%s/%s", apps, names[i] + 5);
        /* .old-X sin X: se corto entre los dos rename -> vuelve la version anterior */
        if (!strncmp(names[i], ".old-", 5) && hpk_valid_id(names[i] + 5) && !hpk_isdir(cur) && !rename(p, cur)) continue;
        hpk_rmrf(p);
    }
    hpk_fsync_dir(apps);
}

int hpk_install(const char *path, void (*progress)(long done, long total, void *ctx), void *ctx, char *err, int errlen)
{
    int r, fd = -1, tmp_made = 0;
    char apps[256], tmp[320], cur[320], old[320], data[320];
    struct hpk_scan_ctx v = { 0, 0, 0, -1, -1, 0, 0 };
    struct hpk_x_ctx x;
    struct hpk_info *in = malloc(sizeof *in);
    unsigned char *buf = 0, *img = 0;
    hpk_err_detail[0] = 0;
    memset(&x, 0, sizeof x);
    if (!in || !(v.mf = malloc(HPK_MAX_MANIFEST + 1))) { r = -HPK_E_NOMEM; goto out; }
    hpk_recover();

    /* 1. validar todo el archivo antes de escribir nada */
    struct stat s;
    if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0 || fstat(fd, &s) || !S_ISREG(s.st_mode)) { r = hpk_fail(HPK_E_OPEN, path); goto out; }
    if ((r = hpk_walk(fd, s.st_size, hpk_scan_cb, &v)) < 0) goto out;
    if (!v.mf_seen) { r = -HPK_E_NOMANIFEST; goto out; }
    if ((r = hpk_mf_parse(v.mf, in)) < 0) goto out;
    struct hpk_find_ctx fx = { in->exec, 0, 0, 0 };
    if ((r = hpk_walk(fd, s.st_size, hpk_find_cb, &fx)) < 0) goto out;
    if (!fx.found) { r = hpk_fail(HPK_E_NOEXEC, in->exec); goto out; }
    if ((r = hpk_elf_check(fd, fx.off, fx.size, in->exec)) < 0) goto out;
    if (v.icon_len > HPK_MAX_ICON_BYTES) { r = hpk_fail(HPK_E_ICON, "icon.png"); goto out; }
    if (!hpk_free_ok(v.total + 3 * 8192 + 64 * 4096)) { r = -HPK_E_SPACE; goto out; }

    /* 2. extraer en apps/.tmp-<id> */
    snprintf(apps, sizeof apps, "%s/apps", hpk_root());
    snprintf(tmp, sizeof tmp, "%s/.tmp-%s", apps, in->id);
    snprintf(cur, sizeof cur, "%s/%s", apps, in->id);
    snprintf(old, sizeof old, "%s/.old-%s", apps, in->id);
    snprintf(data, sizeof data, "%s/appdata/%s", hpk_root(), in->id);
    if (hpk_mkpath(apps)) { r = hpk_fail(HPK_E_IO, apps); goto out; }
    hpk_rmrf(tmp);
    if (mkdir(tmp, 0755)) { r = hpk_fail(HPK_E_IO, tmp); goto out; }
    tmp_made = 1;
    if (!(buf = malloc(HPK_BUF))) { r = -HPK_E_NOMEM; goto out; }
    snprintf(x.dir, sizeof x.dir, "%s", tmp);
    x.exec = in->exec; x.buf = buf; x.total = (long)v.total + 1; x.progress = progress; x.pctx = ctx;
    if (progress) progress(0, x.total, ctx);
    if ((r = hpk_walk(fd, s.st_size, hpk_x_cb, &x)) < 0) goto out;
    free(buf); buf = 0;

    /* 3. iconos 48/56/64 con la mascara de la casa */
    int w, h;
    if (!(img = hpk_icon_src(fd, v.icon_off, v.icon_len, in->id, &w, &h))) { r = hpk_fail(HPK_E_ICON, "icon.png"); goto out; }
    {
        uint16_t rgb[64 * 64]; uint8_t al[64 * 64];
        static const int sz[3] = { 48, 56, 64 };
        for (int i = 0; i < 3; i++) {
            char p[400]; snprintf(p, sizeof p, "%s/icon_%d.ov", tmp, sz[i]);
            hpk_icon_render(img, w, h, sz[i], rgb, al);
            if (hpk_write_ov(p, sz[i], rgb, al)) { r = hpk_fail(errno == ENOSPC ? HPK_E_SPACE : HPK_E_IO, p); goto out; }
        }
    }
    hpk_fsync_dir(tmp);
    if (hpk_mkpath(data)) { r = hpk_fail(HPK_E_IO, data); goto out; }

    /* 4. cambio atomico: <id> -> .old-<id>, .tmp-<id> -> <id>, borrar .old-<id> */
    hpk_rmrf(old);
    int had = hpk_isdir(cur);
    if (had && rename(cur, old)) { r = hpk_fail(HPK_E_SWAP, cur); goto out; }
    if (rename(tmp, cur)) {
        if (had) rename(old, cur);
        r = hpk_fail(HPK_E_SWAP, cur); goto out;
    }
    tmp_made = 0;
    hpk_fsync_dir(apps);
    hpk_rmrf(old);
    if (progress) progress(x.total, x.total, ctx);
    r = 0;
out:
    if (tmp_made) hpk_rmrf(tmp);
    if (fd >= 0) close(fd);
    free(img); free(buf);
    if (in) free(in);
    free(v.mf);
    if (err && errlen > 0) snprintf(err, errlen, "%s", hpk_errkey(r));
    return r;
}

int hpk_uninstall_ex(const char *id, int flags)
{
    hpk_err_detail[0] = 0;
    if (!hpk_valid_id(id)) return hpk_fail(HPK_E_ID, id);
    char cur[320], del[320];
    snprintf(cur, sizeof cur, "%s/apps/%s", hpk_root(), id);
    snprintf(del, sizeof del, "%s/apps/.del-%s", hpk_root(), id);
    if (!hpk_isdir(cur)) return hpk_fail(HPK_E_NOTINST, id);
    hpk_rmrf(del);
    if (rename(cur, del)) return hpk_fail(HPK_E_IO, cur);   /* desaparece de golpe del launcher */
    char apps[256]; snprintf(apps, sizeof apps, "%s/apps", hpk_root());
    hpk_fsync_dir(apps);
    hpk_rmrf(del);
    if (flags & HPK_PURGE_DATA) {
        char data[320]; snprintf(data, sizeof data, "%s/appdata/%s", hpk_root(), id);
        hpk_rmrf(data);
    }
    return 0;
}
int hpk_uninstall(const char *id) { return hpk_uninstall_ex(id, 0); }

int hpk_list_installed(struct hpk_info *out, int max)
{
    hpk_recover();
    char apps[256]; snprintf(apps, sizeof apps, "%s/apps", hpk_root());
    DIR *d = opendir(apps);
    if (!d) return 0;
    int n = 0; struct dirent *de;
    while (n < max && (de = readdir(d))) {
        if (de->d_name[0] == '.' || !hpk_valid_id(de->d_name)) continue;
        struct hpk_info *in = &out[n];
        int r = hpk_read_installed(de->d_name, in);
        if (r == -HPK_E_NOTINST) continue;
        if (r < 0) {
            memset(in, 0, sizeof *in);
            hpk_cpy(in->id, sizeof in->id, de->d_name);
            for (int i = 0; i < 3; i++) hpk_cpy(in->name[i], HPK_NAME_MAX, de->d_name);
        } else if (strcmp(in->id, de->d_name)) r = -HPK_E_ID;   /* directorio renombrado a mano */
        in->status = r < 0 ? r : 0;
        char ip[512]; snprintf(ip, sizeof ip, "%s/%s", apps, de->d_name);
        hpk_cpy(in->path, sizeof in->path, ip);
        memcpy(in->installed, in->version, sizeof in->installed);
        in->size = hpk_du(in->path);
        in->icon_off = in->icon_len = -1;
        n++;
    }
    closedir(d);
    qsort(out, n, sizeof *out, hpk_cmp_info);
    return n;
}
