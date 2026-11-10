/* lang/widgets_tr.inc.c - texts of the Home Screen widgets (widgets.inc.c) in the 13 languages.
 * One row: { key, es, en, fr, de, it, ja, ko, pl, ru, zh-Hans, th, zh-Hant, uk } (LANG_* order).
 * The key is the Spanish literal, or an id (WG_...) for the long texts.
 * widgets.inc.c reads it through wgt() (this table first, then tr()). It can also be hooked into
 * tr_lang() (i18n.inc.c) with wgtr_lookup(): the include guard lets both files include it. */
#ifndef WIDGETS_TR_INC
#define WIDGETS_TR_INC
static const char *const WGTR[][LANG_N + 1] = {
    { "Grande", "Grande", "Large", "Grand", "Groß", "Grande", "大", "크게", "Duży", "Большой", "大", "ใหญ่", "大", "Великий" },
    { "Reloj", "Reloj", "Clock", "Horloge", "Uhr", "Orologio", "時計", "시계", "Zegar", "Часы", "时钟", "นาฬิกา", "時鐘", "Годинник" },
    { "Esta semana", "Esta semana", "This Week", "Cette semaine", "Diese Woche", "Questa settimana", "今週", "이번 주", "W tym tygodniu", "На этой неделе", "本周", "สัปดาห์นี้", "本週", "Цього тижня" },
    { "WG_TODAY", "Hoy", "Today", "Aujourd'hui", "Heute", "Oggi", "今日", "오늘", "Dzisiaj", "Сегодня", "今天", "วันนี้", "今天", "Сьогодні" },
    { "min hoy", "min hoy", "min today", "min aujourd'hui", "Min. heute", "min oggi", "分（今日）", "분 (오늘)", "min dziś", "мин сегодня", "分钟（今天）", "นาที วันนี้", "分鐘（今天）", "хв сьогодні" },
    { "%d min", "%d min", "%d min", "%d min", "%d Min.", "%d min", "%d分", "%d분", "%d min", "%d мин", "%d分钟", "%d นาที", "%d分鐘", "%d хв" },
    { "%d h %d min", "%d h %d min", "%d h %d min", "%d h %d min", "%d Std. %d Min.", "%d h %d min", "%d時間%d分", "%d시간 %d분", "%d godz. %d min", "%d ч %d мин", "%d小时%d分钟",
      "%d ชม. %d นาที", "%d小時%d分鐘", "%d год %d хв" },
    { "MÚSICA", "MÚSICA", "MUSIC", "MUSIQUE", "MUSIK", "MUSICA", "ミュージック", "음악", "MUZYKA", "МУЗЫКА", "音乐", "เพลง", "音樂", "МУЗИКА" },
    { "SISTEMA", "SISTEMA", "SYSTEM", "SYSTÈME", "SYSTEM", "SISTEMA", "システム", "시스템", "SYSTEM", "СИСТЕМА", "系统", "ระบบ", "系統", "СИСТЕМА" },
    { "Sin álbumes", "Sin álbumes", "No Albums", "Aucun album", "Keine Alben", "Nessun album", "アルバムなし", "앨범 없음", "Brak albumów", "Нет альбомов", "没有专辑", "ไม่มีอัลบั้ม", "沒有專輯", "Немає альбомів" },
    { "WG_DESC_BATT", "La batería del HiBy y de tus auriculares (izquierdo, derecho y estuche).", "The battery of your HiBy and your headphones (left, right and case).",
      "La batterie du HiBy et de vos écouteurs (gauche, droit et boîtier).", "Akku des HiBy und deiner Kopfhörer (links, rechts und Etui).",
      "La batteria dell'HiBy e delle cuffie (sinistra, destra e custodia).", "HiBy とヘッドフォン（左・右・ケース）のバッテリー。", "HiBy와 헤드폰(왼쪽, 오른쪽, 케이스)의 배터리.",
      "Bateria HiBy i słuchawek (lewa, prawa i etui).", "Заряд HiBy и наушников (левый, правый и кейс).", "HiBy 和耳机（左耳、右耳、充电盒）的电量。",
      "แบตเตอรี่ของ HiBy และหูฟัง (ซ้าย ขวา และเคส)", "HiBy 和耳機（左耳、右耳、充電盒）的電量。", "Заряд HiBy і навушників (лівий, правий і кейс)." },
    { "WG_DESC_NOW", "Lo que suena ahora, con su progreso y los controles.", "What's playing now, with its progress and the controls.",
      "Ce qui joue en ce moment, avec la progression et les commandes.", "Was gerade läuft, mit Fortschritt und Steuerung.",
      "Cosa stai ascoltando, con avanzamento e controlli.", "再生中の曲と進行状況、コントロール。", "지금 재생 중인 음악과 진행 상황, 컨트롤.",
      "To, co teraz gra, z postępem i sterowaniem.", "Что играет сейчас: прогресс и управление.", "正在播放的内容、进度和控制。",
      "เพลงที่กำลังเล่น พร้อมความคืบหน้าและปุ่มควบคุม", "正在播放的內容、進度和控制。", "Що грає зараз: прогрес і керування." },
    { "WG_DESC_RECENT", "Los últimos álbumes que has añadido, con sus carátulas.", "The albums you added most recently, with their covers.",
      "Les derniers albums ajoutés, avec leur pochette.", "Deine zuletzt hinzugefügten Alben mit Cover.", "Gli ultimi album aggiunti, con le copertine.",
      "最近追加したアルバムをアートワーク付きで。", "최근에 추가한 앨범과 커버.", "Ostatnio dodane albumy z okładkami.", "Недавно добавленные альбомы с обложками.",
      "最近添加的专辑及其封面。", "อัลบั้มที่เพิ่มล่าสุด พร้อมปก", "最近加入的專輯及其封面。", "Нещодавно додані альбоми з обкладинками." },
    { "WG_DESC_LISTEN", "Los minutos que escuchas hoy y esta semana, y tu artista favorito.", "Minutes listened today and this week, and your top artist.",
      "Vos minutes d'écoute aujourd'hui et cette semaine, et votre artiste préféré.", "Heute und diese Woche gehörte Minuten und dein Top-Künstler.",
      "Minuti di ascolto oggi e questa settimana, e il tuo artista preferito.", "今日と今週の再生時間と、よく聴くアーティスト。",
      "오늘과 이번 주의 감상 시간, 그리고 자주 듣는 아티스트.", "Minuty słuchania dziś i w tym tygodniu oraz ulubiony wykonawca.",
      "Сколько минут вы слушали сегодня и за неделю, и любимый исполнитель.", "今天和本周的收听分钟数，以及最常听的艺人。",
      "นาทีที่ฟังวันนี้และสัปดาห์นี้ และศิลปินที่ฟังบ่อยที่สุด", "今天和本週的收聽分鐘數，以及最常聽的藝人。",
      "Скільки хвилин ви слухали сьогодні й цього тижня та улюблений виконавець." },
    { "WG_DESC_CLOCK", "La hora en un reloj analógico, con la fecha.", "The time on an analog clock, with the date.", "L'heure sur une horloge analogique, avec la date.",
      "Die Uhrzeit auf einer analogen Uhr, mit Datum.", "L'ora su un orologio analogico, con la data.", "アナログ時計で時刻と日付を表示します。",
      "아날로그 시계로 시간과 날짜를 표시합니다.", "Godzina na zegarze analogowym, z datą.", "Время на аналоговых часах и дата.", "以指针时钟显示时间和日期。",
      "เวลาบนนาฬิกาแบบเข็ม พร้อมวันที่", "以指針時鐘顯示時間和日期。", "Час на аналоговому годиннику та дата." },
    { "WG_DESC_MIX", "Tus mezclas de Música, a un toque.", "Your Music mixes, one tap away.", "Vos mix de Musique, en un geste.", "Deine Musik-Mixe, nur einen Tipp entfernt.",
      "I tuoi mix di Musica, a portata di tocco.", "ミュージックのミックスをワンタップで。", "음악 믹스를 한 번에.", "Twoje miksy z Muzyki na wyciągnięcie palca.",
      "Ваши миксы из Музыки — в одно касание.", "一键打开“音乐”里的混音。", "มิกซ์ของคุณในแอปเพลง แตะเดียว", "一鍵打開「音樂」裡的混音。", "Ваші мікси з Музики — в один дотик." },
};
static const char *wgtr_lookup(const char *s, int l)
{
    if (!lang_ok(l)) return 0;
    for (unsigned i = 0; i < sizeof WGTR / sizeof WGTR[0]; i++) if (!strcmp(WGTR[i][0], s)) return WGTR[i][1 + l];
    return 0;
}
#endif
