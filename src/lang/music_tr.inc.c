/* lang/music_tr.inc.c - textos nuevos de Musica y Reproduciendo (music.inc.c, nowui.inc.c) en los
 * 13 idiomas. Una fila: { clave, es, en, fr, de, it, ja, ko, pl, ru, zh-Hans, th, zh-Hant, uk }
 * (orden de LANG_*). La consulta mtr_lookup() la hace tr_lang() (i18n.inc.c). */
static const char *const MTR[][LANG_N + 1] = {
    { "Falta el componente de mezclas (hibymix)", "Falta el componente de mezclas (hibymix)", "The mix component is missing (hibymix)",
      "Le composant des mix est absent (hibymix)", "Die Mix-Komponente fehlt (hibymix)", "Manca il componente dei mix (hibymix)",
      "ミックスのコンポーネントがありません（hibymix）", "믹스 구성 요소가 없습니다(hibymix)", "Brak komponentu miksów (hibymix)",
      "Нет компонента миксов (hibymix)", "缺少混音组件（hibymix）", "ไม่มีส่วนประกอบมิกซ์ (hibymix)", "缺少混音元件（hibymix）",
      "Немає компонента міксів (hibymix)" },
    { "No se pudo leer la biblioteca de HiBy", "No se pudo leer la biblioteca de HiBy", "Couldn't read the HiBy library",
      "Impossible de lire la bibliothèque HiBy", "HiBy-Mediathek konnte nicht gelesen werden", "Impossibile leggere la libreria HiBy",
      "HiByライブラリを読み込めませんでした", "HiBy 보관함을 읽을 수 없음", "Nie można odczytać biblioteki HiBy",
      "Не удалось прочитать медиатеку HiBy", "无法读取 HiBy 资料库", "ไม่สามารถอ่านคลัง HiBy ได้", "無法讀取 HiBy 資料庫",
      "Не вдалося прочитати медіатеку HiBy" },
    { "Modo avión", "Modo avión", "Airplane Mode", "Mode Avion", "Flugmodus", "Uso in aereo",
      "機内モード", "에어플레인 모드", "Tryb samolotowy", "Авиарежим", "飞行模式", "โหมดเครื่องบิน", "飛航模式", "Режим польоту" },
    { "Modo avión activado", "Modo avión activado", "Airplane Mode On", "Mode Avion activé", "Flugmodus ein", "Uso in aereo attivo",
      "機内モード オン", "에어플레인 모드 켬", "Tryb samolotowy włączony", "Авиарежим включён", "飞行模式已打开", "เปิดโหมดเครื่องบินแล้ว", "飛航模式已開啟", "Режим польоту увімкнено" },
    { "Modo avión desactivado", "Modo avión desactivado", "Airplane Mode Off", "Mode Avion désactivé", "Flugmodus aus", "Uso in aereo disattivato",
      "機内モード オフ", "에어플레인 모드 끔", "Tryb samolotowy wyłączony", "Авиарежим выключен", "飞行模式已关闭", "ปิดโหมดเครื่องบินแล้ว", "飛航模式已關閉", "Режим польоту вимкнено" },
    { "Bluetooth activado", "Bluetooth activado", "Bluetooth On", "Bluetooth activé", "Bluetooth ein", "Bluetooth attivo",
      "Bluetooth オン", "Bluetooth 켬", "Bluetooth włączony", "Bluetooth включён", "蓝牙已打开", "เปิดบลูทูธแล้ว", "藍牙已開啟", "Bluetooth увімкнено" },
    { "Bluetooth desactivado", "Bluetooth desactivado", "Bluetooth Off", "Bluetooth désactivé", "Bluetooth aus", "Bluetooth disattivato",
      "Bluetooth オフ", "Bluetooth 끔", "Bluetooth wyłączony", "Bluetooth выключен", "蓝牙已关闭", "ปิดบลูทูธแล้ว", "藍牙已關閉", "Bluetooth вимкнено" },
    { "Editar álbum", "Editar álbum", "Edit Album", "Modifier l'album", "Album bearbeiten", "Modifica album",
      "アルバムを編集", "앨범 편집", "Edytuj album", "Изменить альбом", "编辑专辑", "แก้ไขอัลบั้ม", "編輯專輯", "Редагувати альбом" },
    /* letras: busqueda y ajuste de sincronizacion (lyrics.inc.c, nowui.inc.c) */
    { "Texto", "Texto", "Text", "Texte", "Text", "Testo", "テキスト", "텍스트", "Tekst", "Текст", "文本", "ข้อความ", "文字", "Текст" },
    { "La letra sale antes", "La letra sale antes", "Lyrics appear earlier", "Paroles affichées plus tôt", "Text erscheint früher", "Testo anticipato", "歌詞を早く表示", "가사가 더 일찍 표시됨", "Tekst pojawia się wcześniej", "Текст раньше", "歌词提前显示", "เนื้อเพลงแสดงเร็วขึ้น", "歌詞提前顯示", "Текст раніше" },
    { "La letra sale después", "La letra sale después", "Lyrics appear later", "Paroles affichées plus tard", "Text erscheint später", "Testo ritardato", "歌詞を遅く表示", "가사가 더 늦게 표시됨", "Tekst pojawia się później", "Текст позже", "歌词延后显示", "เนื้อเพลงแสดงช้าลง", "歌詞延後顯示", "Текст пізніше" },
    { "Arrastra para afinar", "Arrastra para afinar", "Drag to fine-tune", "Glissez pour affiner", "Zum Feinabstimmen ziehen", "Trascina per regolare", "ドラッグして微調整", "드래그하여 미세 조정", "Przeciągnij, aby dostroić", "Перетащите для подстройки", "拖动以微调", "ลากเพื่อปรับละเอียด", "拖曳以微調", "Перетягніть для підлаштування" },
    { "No se pudo conectar. Comprueba la Wi-Fi.", "No se pudo conectar. Comprueba la Wi-Fi.", "Couldn't connect. Check your Wi-Fi.", "Connexion impossible. Vérifiez le Wi-Fi.", "Keine Verbindung. Prüfe das WLAN.", "Impossibile connettersi. Controlla il Wi-Fi.", "接続できませんでした。Wi-Fiを確認してください。", "연결할 수 없습니다. Wi-Fi를 확인하세요.", "Nie można połączyć. Sprawdź Wi-Fi.", "Не удалось подключиться. Проверьте Wi-Fi.", "无法连接。请检查 Wi-Fi。", "เชื่อมต่อไม่ได้ ตรวจสอบ Wi-Fi", "無法連線。請檢查 Wi-Fi。", "Не вдалося під'єднатися. Перевірте Wi-Fi." },
    { "Reintentar", "Reintentar", "Try Again", "Réessayer", "Erneut versuchen", "Riprova", "再試行", "다시 시도", "Spróbuj ponownie", "Повторить", "重试", "ลองอีกครั้ง", "重試", "Повторити" },
    { "Editar búsqueda", "Editar búsqueda", "Edit Search", "Modifier la recherche", "Suche bearbeiten", "Modifica ricerca", "検索を編集", "검색 편집", "Edytuj wyszukiwanie", "Изменить поиск", "编辑搜索", "แก้ไขการค้นหา", "編輯搜尋", "Змінити пошук" },
    { "Sonando ahora", "Sonando ahora", "Now Playing", "En cours de lecture", "Wird gespielt", "In riproduzione", "再生中", "지금 재생 중", "Teraz odtwarzane", "Сейчас играет", "正在播放", "กำลังเล่น", "正在播放", "Зараз грає" },
    { "Letra sin sincronizar", "Letra sin sincronizar", "Unsynced lyrics", "Paroles non synchronisées", "Text ohne Synchronisierung", "Testo non sincronizzato", "同期なしの歌詞", "동기화되지 않은 가사", "Tekst bez synchronizacji", "Текст без синхронизации", "未同步的歌词", "เนื้อเพลงไม่ซิงค์", "未同步的歌詞", "Текст без синхронізації" },
    { "Usar esta letra", "Usar esta letra", "Use These Lyrics", "Utiliser ces paroles", "Diesen Text verwenden", "Usa questo testo", "この歌詞を使用", "이 가사 사용", "Użyj tego tekstu", "Использовать этот текст", "使用此歌词", "ใช้เนื้อเพลงนี้", "使用此歌詞", "Використати цей текст" },
    { "Resultados", "Resultados", "Results", "Résultats", "Ergebnisse", "Risultati", "結果", "결과", "Wyniki", "Результаты", "结果", "ผลลัพธ์", "結果", "Результати" },
    { "Vista previa", "Vista previa", "Preview", "Aperçu", "Vorschau", "Anteprima", "プレビュー", "미리보기", "Podgląd", "Просмотр", "预览", "ตัวอย่าง", "預覽", "Перегляд" },
    { "Sin letra", "Sin letra", "No Lyrics", "Pas de paroles", "Kein Songtext", "Nessun testo", "歌詞なし", "가사 없음", "Brak tekstu", "Нет текста", "无歌词", "ไม่มีเนื้อเพลง", "無歌詞", "Немає тексту" },
    { "Busca la letra de esta canción", "Busca la letra de esta canción", "Find the lyrics for this song", "Trouvez les paroles de ce morceau", "Songtext für diesen Titel suchen", "Cerca il testo di questo brano", "この曲の歌詞を検索", "이 노래의 가사 찾기", "Znajdź tekst tego utworu", "Найдите текст этой песни", "查找这首歌的歌词", "ค้นหาเนื้อเพลงของเพลงนี้", "尋找這首歌的歌詞", "Знайдіть текст цієї пісні" },
    { "Letras", "Letras", "Lyrics", "Paroles", "Songtexte", "Testi", "歌詞", "가사", "Teksty", "Тексты", "歌词", "เนื้อเพลง", "歌詞", "Тексти" },
    { "Sincronización", "Sincronización", "Timing", "Synchronisation", "Timing", "Sincronia", "タイミング", "싱크", "Synchronizacja", "Синхронизация", "同步", "จังหวะ", "同步", "Синхронізація" },
    { "Sincronización guardada", "Sincronización guardada", "Timing saved", "Synchronisation enregistrée", "Timing gespeichert", "Sincronia salvata", "タイミングを保存しました", "싱크 저장됨", "Zapisano synchronizację", "Синхронизация сохранена", "已保存同步", "บันทึกจังหวะแล้ว", "已儲存同步", "Синхронізацію збережено" },
    { "Cambiar letra", "Cambiar letra", "Change Lyrics", "Changer les paroles", "Songtext ändern", "Cambia testo", "歌詞を変更", "가사 변경", "Zmień tekst", "Изменить текст", "更换歌词", "เปลี่ยนเนื้อเพลง", "更換歌詞", "Змінити текст" },
    { "Ajustar sincronización", "Ajustar sincronización", "Adjust Timing", "Ajuster la synchro", "Timing anpassen", "Regola sincronia", "タイミングを調整", "싱크 조정", "Ustaw synchronizację", "Сдвиг текста", "调整同步", "ปรับจังหวะ", "調整同步", "Зсув тексту" },
};
static const char *mtr_lookup(const char *s, int l)
{
    for (unsigned i = 0; i < sizeof MTR / sizeof MTR[0]; i++)
        if (!strcmp(MTR[i][0], s)) return l >= 0 && l < LANG_N ? MTR[i][l + 1] : 0;
    return 0;
}
