/* lang/books_tr.inc.c - textos de la app Libros (books.inc.c) en los 13 idiomas.
 * Una fila: { clave, es, en, fr, de, it, ja, ko, pl, ru, zh-Hans, th, zh-Hant, uk } (orden de
 * LANG_*). La clave es el literal en espanol o un id (BOOKS_...) para los textos largos.
 * La consulta btr_lookup() la hace tr_lang() (i18n.inc.c). */
static const char *const BTR[][LANG_N + 1] = {
    { "No hay libros", "No hay libros", "No Books", "Aucun livre", "Keine Bücher", "Nessun libro", "ブックがありません", "책 없음", "Brak książek", "Нет книг", "没有图书", "ไม่มีหนังสือ", "沒有書籍", "Немає книжок" },
    { "BOOKS_HELP", "Copia libros EPUB o TXT en la carpeta Books (o book) de la tarjeta SD.", "Copy EPUB or TXT books to the Books (or book) folder on the SD card.",
      "Copiez des livres EPUB ou TXT dans le dossier Books (ou book) de la carte SD.", "Kopiere EPUB- oder TXT-Bücher in den Ordner „Books“ (oder „book“) der SD-Karte.",
      "Copia libri EPUB o TXT nella cartella Books (o book) della scheda SD.", "EPUBまたはTXTのブックをSDカードのBooks（またはbook）フォルダにコピーしてください。",
      "EPUB 또는 TXT 책을 SD 카드의 Books(또는 book) 폴더에 복사하십시오.", "Skopiuj książki EPUB lub TXT do folderu Books (lub book) na karcie SD.",
      "Скопируйте книги EPUB или TXT в папку Books (или book) на SD-карте.", "将 EPUB 或 TXT 图书拷贝到 SD 卡的 Books（或 book）文件夹。",
      "คัดลอกหนังสือ EPUB หรือ TXT ไปยังโฟลเดอร์ Books (หรือ book) ในการ์ด SD", "將 EPUB 或 TXT 書籍拷貝到 SD 卡的 Books（或 book）檔案夾。",
      "Скопіюйте книжки EPUB або TXT до папки Books (або book) на SD-картці." },
    { "NUEVO", "NUEVO", "NEW", "NOUVEAU", "NEU", "NUOVO", "新規", "신규", "NOWOŚĆ", "НОВОЕ", "新", "ใหม่", "新", "НОВЕ" },
    { "Terminado", "Terminado", "Finished", "Terminé", "Beendet", "Finito", "読了", "다 읽음", "Ukończono", "Прочитано", "已读完", "อ่านจบแล้ว", "已讀完", "Прочитано" },
    { "Contenido", "Contenido", "Contents", "Table des matières", "Inhalt", "Indice", "目次", "목차", "Spis treści", "Содержание", "目录", "สารบัญ", "目錄", "Зміст" },
    { "Temas y ajustes", "Temas y ajustes", "Themes & Settings", "Thèmes et réglages", "Themen & Einstellungen", "Temi e impostazioni", "テーマと設定", "테마 및 설정", "Motywy i ustawienia", "Темы и настройки", "主题和设置", "ธีมและการตั้งค่า", "主題和設定", "Теми й параметри" },
    { "Original", "Original", "Original", "Original", "Original", "Originale", "オリジナル", "오리지널", "Oryginalny", "Оригинал", "原始", "ต้นฉบับ", "原始", "Оригінал" },
    { "Sepia", "Sepia", "Sepia", "Sépia", "Sepia", "Seppia", "セピア", "세피아", "Sepia", "Сепия", "棕褐色", "ซีเปีย", "棕褐色", "Сепія" },
    { "Noche", "Noche", "Night", "Nuit", "Nacht", "Notte", "夜間", "야간", "Noc", "Ночь", "夜间", "กลางคืน", "夜間", "Ніч" },
    { "Sección %d", "Sección %d", "Section %d", "Section %d", "Abschnitt %d", "Sezione %d", "セクション%d", "섹션 %d", "Sekcja %d", "Раздел %d", "第 %d 节", "ส่วนที่ %d", "第 %d 節", "Розділ %d" },
    { "Calculando…", "Calculando…", "Calculating…", "Calcul…", "Wird berechnet …", "Calcolo…", "計算中…", "계산 중…", "Obliczanie…", "Вычисление…", "正在计算…", "กำลังคำนวณ…", "正在計算…", "Обчислення…" },
    { "Sin capítulos", "Sin capítulos", "No Chapters", "Aucun chapitre", "Keine Kapitel", "Nessun capitolo", "章がありません", "챕터 없음", "Brak rozdziałów", "Нет глав", "没有章节", "ไม่มีบท", "沒有章節", "Немає розділів" },
    { "Marcar como terminado", "Marcar como terminado", "Mark as Finished", "Marquer comme terminé", "Als beendet markieren", "Segna come finito", "読了にする", "다 읽음으로 표시", "Oznacz jako ukończoną", "Отметить как прочитанную", "标记为已读完", "ทำเครื่องหมายว่าอ่านจบแล้ว", "標示為已讀完", "Позначити як прочитану" },
    { "Marcar como no leído", "Marcar como no leído", "Mark as Unread", "Marquer comme non lu", "Als ungelesen markieren", "Segna come non letto", "未読にする", "읽지 않음으로 표시", "Oznacz jako nieprzeczytaną", "Отметить как непрочитанную", "标记为未读", "ทำเครื่องหมายว่ายังไม่อ่าน", "標示為未讀", "Позначити як непрочитану" },
    { "Empezar desde el principio", "Empezar desde el principio", "Start from the Beginning", "Reprendre au début", "Von vorne beginnen", "Ricomincia dall'inizio", "最初から読む", "처음부터 시작", "Zacznij od początku", "Начать сначала", "从头开始", "เริ่มจากต้น", "從頭開始", "Почати спочатку" },
    { "No se pudo abrir el libro", "No se pudo abrir el libro", "Couldn't open the book", "Impossible d'ouvrir le livre", "Buch konnte nicht geöffnet werden", "Impossibile aprire il libro", "ブックを開けませんでした", "책을 열 수 없음", "Nie można otworzyć książki", "Не удалось открыть книгу", "无法打开图书", "ไม่สามารถเปิดหนังสือได้", "無法打開書籍", "Не вдалося відкрити книжку" },
};
static const char *btr_lookup(const char *s, int l)
{
    if (!lang_ok(l)) return 0;
    for (unsigned i = 0; i < sizeof BTR / sizeof BTR[0]; i++) if (!strcmp(BTR[i][0], s)) return BTR[i][1 + l];
    return 0;
}
