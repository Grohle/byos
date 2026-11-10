/* i18n.inc.c - textos de la interfaz en los 13 idiomas del firmware de HiBy.
 * tr("texto en espanol") devuelve la traduccion del idioma actual. La clave es siempre el
 * literal en espanol (o un id para los textos largos, p. ej. "ADB_HELP_WIFI").
 *   - TR[][3]: espanol (clave), ingles y frances. Una fila nueva se anade aqui.
 *   - lang/<codigo>.inc.c: una tabla { clave, traduccion } por cada idioma restante.
 * Si falta una traduccion (o esta vacia) se usa la inglesa; si tampoco hay, la clave.
 * Los literales tienen direccion fija: se guarda la ultima busqueda de cada uno (cache).
 * Comprobacion en PC: tests/i18n_test.c (claves, formatos %, glifos y anchos). */

enum { LANG_ES, LANG_EN, LANG_FR, LANG_DE, LANG_IT, LANG_JA, LANG_KO, LANG_PL, LANG_RU, LANG_ZHS, LANG_TH, LANG_ZHT, LANG_UK, LANG_N };
static int ui_lang = LANG_ES;
static int ui_lang_sys = 1;        /* 1: "Igual que el sistema" (sigue al idioma del reproductor) */
static int kb_cfg_height, kb_cfg_numrow, kb_cfg_layout;   /* definidas en keyboard.inc.c */
static const char *const LANG_CODES[LANG_N] = { "es", "en", "fr", "de", "it", "ja", "ko", "pl", "ru", "zh-Hans", "th", "zh-Hant", "uk" };
static const char *const LANG_NAMES[LANG_N] = { "Español", "English", "Français", "Deutsch", "Italiano", "日本語", "한국어", "Polski",
                                                "Русский", "简体中文", "ไทย", "繁體中文", "Українська" };
static int lang_ok(int l) { return l >= 0 && l < LANG_N; }
static const char *lang_code(void) { return LANG_CODES[lang_ok(ui_lang) ? ui_lang : LANG_EN]; }
static int lang_from_code(const char *c)
{
    for (int i = 0; i < LANG_N; i++) if (!strcmp(c, LANG_CODES[i])) return i;
    return -1;
}
/* indice para datos que solo existen en es/en/fr (nombres de apps .hpk): el resto, ingles */
static int lang3(void) { return ui_lang == LANG_ES || ui_lang == LANG_FR ? ui_lang : LANG_EN; }

/* Idioma del reproductor oficial: get_param(5) de hiby_player = int32 en el offset 8 de
 * /usr/data/user.ini (la imagen binaria de sus ajustes). Su orden es el de la tabla de rutas
 * z:\str\... del binario (0 = chino simplificado, que es tambien su valor por defecto). */
static const signed char HIBY_LANG[13] = { LANG_ZHS, LANG_ZHT, LANG_EN, LANG_RU, LANG_KO, LANG_JA, LANG_DE,
                                           LANG_IT, LANG_ES, LANG_PL, LANG_FR, LANG_TH, LANG_UK };
static int lang_system(void)
{
    int fd = open("/usr/data/user.ini", O_RDONLY);
    int32_t v = -1;
    if (fd >= 0) { if (pread(fd, &v, 4, 8) != 4) v = -1; close(fd); }
    return v >= 0 && v < 13 ? HIBY_LANG[v] : LANG_ES;
}

static const char *const TR[][3] = {
    /* apps */
    { "Música", "Music", "Musique" },
    { "Ajustes", "Settings", "Réglages" },
    { "Inalámbrico", "Wireless", "Sans fil" },
    { "Libros", "Books", "Livres" },
    { "DAC USB", "USB DAC", "DAC USB" },
    { "Reproduciendo", "Now Playing", "À l'écoute" },
    { "Archivos", "Files", "Fichiers" },
    { "Radio", "Radio", "Radio" },
    { "Personalizar", "Customize", "Personnaliser" },
    { "Apps", "Apps", "Apps" },
    /* launcher */
    { "Listo", "Done", "OK" },
    { "Inicio", "Home Screen", "Écran d'accueil" },
    { "Tamaño de iconos", "Icon size", "Taille des icônes" },
    { "Columnas", "Columns", "Colonnes" },
    { "Fondo de pantalla", "Wallpaper", "Fond d'écran" },
    { "Elegir de la SD…", "Choose from SD…", "Choisir sur la SD…" },
    { "Restablecer inicio", "Reset Home Screen", "Réinitialiser l'accueil" },
    { "Pequeños", "Small", "Petites" },
    { "Normales", "Default", "Normales" },
    { "Grandes", "Large", "Grandes" },
    { "Cerrar", "Close", "Fermer" },
    { "Toda la SD", "Whole SD", "Toute la SD" },
    { "Sin imágenes en /Pictures", "No images in /Pictures", "Aucune image dans /Pictures" },
    { "Aplicando fondo…", "Applying wallpaper…", "Application du fond…" },
    { "Imagen demasiado grande", "Image too large", "Image trop grande" },
    { "No se pudo leer la imagen", "Couldn't read the image", "Impossible de lire l'image" },
    { "No se pudo abrir", "Couldn't open", "Ouverture impossible" },
    { "Radio llega en la próxima versión", "Radio is coming in the next version", "La radio arrive dans la prochaine version" },
    /* multitarea */
    { "Donde estabas", "Where you were", "Là où vous étiez" },
    /* ajustes */
    { "General", "General", "Général" },
    { "Idioma", "Language", "Langue" },
    { "Gestos", "Gestures", "Gestes" },
    { "Teclado", "Keyboard", "Clavier" },
    { "Desarrollador", "Developer", "Développeur" },
    { "Acerca de", "About", "À propos" },
    { "Atrás", "Back", "Retour" },
    { "Flecha de música en el inicio", "Music arrow on Home", "Flèche musique sur l'accueil" },
    { "Desde abajo (en apps)", "From bottom (in apps)", "Depuis le bas (apps)" },
    { "Desde abajo (en el inicio)", "From bottom (on Home)", "Depuis le bas (accueil)" },
    { "Desde arriba", "From top", "Depuis le haut" },
    { "Desde la izquierda", "From left edge", "Depuis le bord gauche" },
    { "Desde la derecha", "From right edge", "Depuis le bord droit" },
    { "Inicio y multitarea", "Home & app switcher", "Accueil et multitâche" },
    { "Reproductor", "Music player", "Lecteur" },
    { "Centro de control", "Control Center", "Centre de contrôle" },
    { "Atrás (HiBy)", "Back (HiBy)", "Retour (HiBy)" },
    { "Nada", "Nothing", "Rien" },
    { "BORDES", "EDGES", "BORDS" },
    { "Desliza desde el borde para ver la animación de ejemplo.", "Swipe from the edge to preview.", "Glissez depuis le bord pour l'aperçu." },
    { "Altura", "Height", "Hauteur" },
    { "Compacta", "Compact", "Compacte" },
    { "Normal", "Normal", "Normale" },
    { "Alta", "Tall", "Haute" },
    { "Fila de números", "Number row", "Rangée de chiffres" },
    { "Distribución", "Layout", "Disposition" },
    { "Español (QWERTY)", "Spanish (QWERTY)", "Espagnol (QWERTY)" },
    { "Inglés (QWERTY)", "English (QWERTY)", "Anglais (QWERTY)" },
    { "Francés (AZERTY)", "French (AZERTY)", "Français (AZERTY)" },
    { "ADB", "ADB", "ADB" },
    { "Desactivado", "Off", "Désactivé" },
    { "Wi-Fi", "Wi-Fi", "Wi-Fi" },
    { "USB", "USB", "USB" },
    { "Dirección", "Address", "Adresse" },
    { "Estado", "Status", "État" },
    { "Activo", "Active", "Actif" },
    { "Sin conexión Wi-Fi", "No Wi-Fi connection", "Pas de connexion Wi-Fi" },
    { "Versión", "Version", "Version" },
    { "Memoria libre", "Free memory", "Mémoire libre" },
    { "Apps instaladas", "Installed apps", "Apps installées" },
    { "Aplicando…", "Applying…", "Application…" },
    { "Cambios guardados", "Changes saved", "Modifications enregistrées" },
    { "Restablecer", "Reset", "Réinitialiser" },
    { "¿Restablecer el inicio? Se pierde la colocación de las apps.", "Reset the Home Screen? App positions will be lost.", "Réinitialiser l'accueil ? La disposition des apps sera perdue." },
    { "No se puede abrir la carpeta", "Can't open the folder", "Impossible d'ouvrir le dossier" },
    { "UBICACIONES", "LOCATIONS", "EMPLACEMENTS" },
    { "Carpeta vacía", "Empty folder", "Dossier vide" },
    { "Cancelar", "Cancel", "Annuler" },
    { "Mover aquí", "Move here", "Déplacer ici" },
    { "Cargando…", "Loading…", "Chargement…" },
    { "No se pudo abrir la imagen", "Couldn't open the image", "Impossible d'ouvrir l'image" },
    { "intro", "return", "entrée" },
    { "Hay cambios sin guardar", "You have unsaved changes", "Modifications non enregistrées" },
    { "Descartar cambios", "Discard changes", "Ignorer les modifications" },
    { "Guardado", "Saved", "Enregistré" },
    { "No se pudo guardar", "Couldn't save", "Échec de l'enregistrement" },
    { "Nueva carpeta", "New folder", "Nouveau dossier" },
    { "Nombre", "Name", "Nom" },
    { "No se pudo crear", "Couldn't create", "Création impossible" },
    { "Carpeta creada", "Folder created", "Dossier créé" },
    { "No se puede mover dentro de sí misma", "Can't move into itself", "Impossible de déplacer dans lui-même" },
    { "Ya existe uno con ese nombre", "That name already exists", "Ce nom existe déjà" },
    { "Solo dentro de la misma ubicación", "Only within the same location", "Seulement dans le même emplacement" },
    { "No se pudo mover", "Couldn't move", "Déplacement impossible" },
    { "Movido", "Moved", "Déplacé" },
    { "La SD está conectada al ordenador", "The SD card is in use by the computer", "La carte SD est utilisée par l'ordinateur" },
    { "Información", "Info", "Infos" },
    { "Renombrar", "Rename", "Renommer" },
    { "Mover", "Move", "Déplacer" },
    { "Eliminar", "Delete", "Supprimer" },
    { "No se pudo renombrar", "Couldn't rename", "Impossible de renommer" },
    { "Renombrado", "Renamed", "Renommé" },
    { "Eliminar carpeta", "Delete folder", "Supprimer le dossier" },
    { "Eliminando…", "Deleting…", "Suppression…" },
    { "No se pudo eliminar del todo", "Couldn't delete everything", "Suppression incomplète" },
    { "Eliminado", "Deleted", "Supprimé" },
    { "Para reproducir, usa Música › Carpetas", "To play it, use Music › Files", "Pour la lire, utilisez Musique › Fichiers" },
    { "Aceptar", "OK", "OK" },
    { "Pictures", "Pictures", "Pictures" },
    { "HiBy", "HiBy", "HiBy" },
    { "Tarjeta SD", "SD Card", "Carte SD" },
    { "En el HiBy", "On My HiBy", "Sur mon HiBy" },
    { "En uso por el ordenador (USB)", "In use by the computer (USB)", "Utilisée par l'ordinateur (USB)" },
    { "Solo lectura", "Read only", "Lecture seule" },
    { "Guardar", "Save", "Enregistrer" },
    { "Editar", "Edit", "Modifier" },
    { "Elige la carpeta de destino", "Choose the destination folder", "Choisissez le dossier de destination" },
    { "Mover «%s»", "Move “%s”", "Déplacer « %s »" },
    { "¿Eliminar «%s»? No se puede deshacer.", "Delete “%s”? This can't be undone.", "Supprimer « %s » ? Action irréversible." },
    { "Carpeta · modificada el %s", "Folder · modified %s", "Dossier · modifié le %s" },
    { "%s · modificado el %s", "%s · modified %s", "%s · modifié le %s" },
    { "Sin imágenes en la SD (carpeta Fondos)", "No images on the SD", "Aucune image sur la SD" },
    { "HPK_HELP", "Copy .hpk packages to the Apps folder on the SD card to install them here.", "Copiez les paquets .hpk dans le dossier Apps de la carte SD pour les installer ici." },
    { "Abajo, en apps", "Bottom, in apps", "Bas, dans les apps" },
    { "Abajo, en el inicio", "Bottom, on Home", "Bas, sur l'accueil" },
    { "Arriba", "Top", "Haut" },
    { "Izquierda", "Left edge", "Bord gauche" },
    { "Derecha", "Right edge", "Bord droit" },
    { "Editar canciones", "Edit Songs", "Modifier les morceaux" },
    { "Editar canción", "Edit Song", "Modifier le morceau" },
    { "%d canciones", "%d songs", "%d morceaux" },
    { "Título", "Title", "Titre" },
    { "Artista", "Artist", "Artiste" },
    { "Álbum", "Album", "Album" },
    { "Artista del álbum", "Album artist", "Artiste de l'album" },
    { "Género", "Genre", "Genre" },
    { "Año", "Year", "Année" },
    { "Pista", "Track", "Piste" },
    { "Disco", "Disc", "Disque" },
    { "Varios valores", "Multiple values", "Valeurs multiples" },
    { "Distinto en cada una", "Different on each", "Différent pour chacun" },
    { "%d de %d", "%d of %d", "%d sur %d" },
    { "Guardando etiquetas", "Saving tags", "Enregistrement des tags" },
    { "Leyendo etiquetas", "Reading tags", "Lecture des tags" },
    { "Actualizando la biblioteca", "Updating the library", "Mise à jour de la bibliothèque" },
    { "Algunas no se pudieron guardar", "Some couldn't be saved", "Certains n'ont pas pu être enregistrés" },
    { "%d seleccionados", "%d selected", "%d sélectionnés" },
    { "Mover %d elementos", "Move %d items", "Déplacer %d éléments" },
    { "Algunos no se pudieron mover", "Some couldn't be moved", "Certains n'ont pas pu être déplacés" },
    { "¿Eliminar %d elementos? No se puede deshacer.", "Delete %d items? This can't be undone.", "Supprimer %d éléments ? Action irréversible." },
    { "Esta app necesita la memoria de la música: se pausará.", "This app needs the music player's memory: music will pause.", "Cette app a besoin de la mémoire de la musique : elle sera mise en pause." },
    { "Abrir", "Open", "Ouvrir" },
    { "OBTENER", "GET", "OBTENIR" },
    { "ACTUALIZAR", "UPDATE", "METTRE À JOUR" },
    { "ABRIR", "OPEN", "OUVRIR" },
    { "EN LA TARJETA SD (/Apps)", "ON THE SD CARD (/Apps)", "SUR LA CARTE SD (/Apps)" },
    { "Instalando…", "Installing…", "Installation…" },
    { "Instalar", "Install", "Installer" },
    { "Actualizar", "Update", "Mettre à jour" },
    { "Desinstalar", "Uninstall", "Désinstaller" },
    { "Instalada", "Installed", "Installée" },
    { "Desinstalada", "Uninstalled", "Désinstallée" },
    { "¿Desinstalar la app? Sus datos se conservan.", "Uninstall the app? Its data is kept.", "Désinstaller l'app ? Ses données sont conservées." },
    { "ADB_HELP_WIFI", "On your computer run: adb connect ADDRESS (same Wi-Fi network). Android platform-tools must be installed.", "Sur l'ordinateur : adb connect ADRESSE (même réseau Wi-Fi). Les platform-tools d'Android doivent être installés." },
    { "ADB_HELP_USB", "Connect the USB cable: the computer sees the disk and an \"ADB Interface\" at the same time (Windows needs no drivers). The first time, run \"adb shell\" within 90 s to confirm, or it goes back to Wi-Fi.", "Branchez le câble USB : l'ordinateur voit le disque et une « ADB Interface » en même temps (Windows sans pilote). La première fois, lancez « adb shell » en moins de 90 s pour confirmer, sinon retour au Wi-Fi." },
    { "Audio y sistema", "Audio & system", "Audio et système" },
    { "DAC ES9219", "ES9219 DAC", "DAC ES9219" },
    { "Filtro digital", "Digital filter", "Filtre numérique" },
    { "Rápido, fase lineal", "Fast, linear phase", "Rapide, phase linéaire" },
    { "Lento, fase lineal", "Slow, linear phase", "Lent, phase linéaire" },
    { "Rápido, fase mínima", "Fast, minimum phase", "Rapide, phase minimale" },
    { "Lento, fase mínima", "Slow, minimum phase", "Lent, phase minimale" },
    { "Apodizante", "Apodizing", "Apodisant" },
    { "Híbrido, fase mínima", "Hybrid, minimum phase", "Hybride, phase minimale" },
    { "Brickwall", "Brickwall", "Brickwall" },
    { "VOLUMEN", "VOLUME", "VOLUME" },
    { "Aviso de volumen alto (UE)", "High volume warning (EU)", "Alerte volume élevé (UE)" },
    { "TARJETA SD", "SD CARD", "CARTE SD" },
    { "Biblioteca en la tarjeta SD", "Library on the SD card", "Bibliothèque sur la carte SD" },
    { "Carátulas en caché en la SD", "Cache covers on the SD", "Pochettes en cache sur la SD" },
    { "AJUSTES DE HIBY", "HIBY SETTINGS", "RÉGLAGES HIBY" },
    { "Funciones activadas", "Features enabled", "Fonctions activées" },
    { "Reiniciar para aplicar", "Restart to apply", "Redémarrer pour appliquer" },
    { "¿Reiniciar ahora? La música se detendrá.", "Restart now? Music will stop.", "Redémarrer maintenant ? La musique s'arrêtera." },
    { "Reiniciar", "Restart", "Redémarrer" },
    { "Quitar aviso", "Remove warning", "Retirer l'alerte" },
    { "No se pudo aplicar el filtro", "Couldn't apply the filter", "Impossible d'appliquer le filtre" },
    { "No se pudo guardar", "Couldn't save", "Impossible d'enregistrer" },
    { "FILT_HELP", "The DAC's 8 filter shapes, unlocked (stock firmware only allowed 2). Pure listening preference: none can damage anything. Applied instantly and kept after restarts.", "Les 8 formes de filtre du DAC, débloquées (le firmware d'origine n'en permettait que 2). Pure préférence d'écoute : aucune ne peut rien endommager. Appliqué tout de suite et conservé au redémarrage." },
    { "FILT_HELP_OLD", "Only 2 filters available: install hibymod 0.5.4 or later to unlock the 8 filters of the DAC.", "Seulement 2 filtres : installez hibymod 0.5.4 ou plus récent pour débloquer les 8 filtres du DAC." },
    { "VOLWARN_HELP", "Turning it off doesn't add power: it only removes the hearing-safety prompt above 34 (3.5 mm) / 28 (4.4 mm). Takes effect after restarting.", "Le désactiver n'ajoute pas de puissance : cela retire seulement l'alerte auditive au-delà de 34 (3,5 mm) / 28 (4,4 mm). Effet après redémarrage." },
    { "VOLWARN_CONFIRM", "Remove the hearing-safety warning? Listening at high volume for a long time can damage your hearing.", "Retirer l'alerte de sécurité auditive ? Écouter longtemps à volume élevé peut abîmer l'audition." },
    { "TF_HELP", "Keeps the music database and cover cache on the SD instead of internal memory: faster with big libraries and less internal wear. Needs a restart and a library rescan.", "Garde la base musicale et le cache des pochettes sur la SD plutôt qu'en mémoire interne : plus rapide avec de grandes bibliothèques et moins d'usure. Redémarrage et nouvelle analyse nécessaires." },
    { "HIDDEN_HELP", "Car mode (plays on USB power, turns off when unplugged), standby, double-tap to wake and no charging in DAC mode now appear in HiBy's System settings.", "Mode voiture (lecture sur alimentation USB, extinction au débranchement), veille, double appui pour réveiller et pas de charge en mode DAC apparaissent maintenant dans les Réglages système de HiBy." },
    { "REBOOT_HELP", "Some changes apply after restarting the HiBy.", "Certains changements s'appliquent après le redémarrage du HiBy." },
    { "PROYECTO", "PROJECT", "PROJET" },
    { "Código", "Source", "Code source" },
    { "ABOUT_HELP", "HiByOS is a free community firmware for the HiBy R3 II, directed by Grohle and built mostly by AI agents. Not affiliated with HiBy.", "HiByOS est un firmware communautaire gratuit pour le HiBy R3 II, dirigé par Grohle et réalisé en grande partie par des agents d'IA. Non affilié à HiBy." },
    { "Centro de control", "Control Centre", "Centre de contrôle" },
    { "CONTROLES", "CONTROLS", "COMMANDES" },
    { "Solo USB", "USB only", "USB seul" },
    { "Modo de energía", "Power mode", "Mode d'énergie" },
    { "Panel de HiBy", "HiBy panel", "Panneau HiBy" },
    { "Batería y energía", "Battery & power", "Batterie et énergie" },
    { "Ahorro", "Low power", "Économie" },
    { "Rendimiento", "Performance", "Performance" },
    { "Solo USB (no cargar)", "USB only (don't charge)", "USB seul (sans charge)" },
    { "Sonando", "Playing", "En lecture" },
    { "En pausa", "Paused", "En pause" },
    { "Solo USB: la batería no se carga", "USB only: battery not charging", "USB seul : batterie non chargée" },
    { "Carga de la batería activada", "Battery charging on", "Charge de la batterie activée" },
    { "Modo Ahorro", "Low Power Mode", "Mode Économie" },
    { "Modo Normal", "Normal Mode", "Mode Normal" },
    { "Modo Rendimiento", "Performance Mode", "Mode Performance" },
    { "CC_HELP", "Choose the controls shown in the Control Centre (swipe down from the top).", "Choisissez les commandes du Centre de contrôle (balayez depuis le haut)." },
    { "POWER_HELP", "Low power: 30 fps animations and brightness capped at 40 %. Normal: 44 fps (the panel's maximum). Performance: no limits, for when it's plugged in. There is no overclock: the CPU already runs 10 % above its rating.", "Économie : animations à 30 i/s et luminosité limitée à 40 %. Normal : 44 i/s (le maximum de l'écran). Performance : sans limites, quand il est branché. Pas d'overclock : le processeur tourne déjà 10 % au-dessus de sa fréquence nominale." },
    { "NOCHARGE_HELP", "The HiBy runs from USB power without charging the battery: good for long sessions plugged in (DAC, desk).", "Le HiBy fonctionne sur l'alimentation USB sans charger la batterie : idéal pour de longues sessions branché (DAC, bureau)." },
    { "Arriba, izquierda", "Top, left", "Haut, gauche" },
    { "Arriba, derecha", "Top, right", "Haut, droite" },
    { "Borde izquierdo", "Left edge", "Bord gauche" },
    { "Borde derecho", "Right edge", "Bord droit" },
    { "INSTALADAS", "INSTALLED", "INSTALLÉES" },
    { "Ninguna app instalada", "No apps installed", "Aucune app installée" },
    { "Copia de seguridad", "Back up", "Sauvegarder" },
    { "Desinstalar y borrar datos", "Uninstall and delete data", "Désinstaller et effacer les données" },
    { "¿Desinstalar y borrar sus datos? No se puede deshacer.", "Uninstall and delete its data? This can't be undone.", "Désinstaller et effacer ses données ? Action irréversible." },
    { "Copiando…", "Copying…", "Copie…" },
    { "No se pudo hacer la copia", "The backup failed", "La sauvegarde a échoué" },
    { "Copia guardada en Apps/Copias", "Backup saved in Apps/Copias", "Sauvegarde enregistrée dans Apps/Copias" },
    { "ADB_HELP_OFF", "ADB lets a computer control the HiBy (developer tools). Leave it off if you don't use it.", "ADB permet à un ordinateur de piloter le HiBy (outils de développement). Laissez-le désactivé si vous ne l'utilisez pas." },
    /* idioma y teclado */
    { "espacio", "space", "espace" },
    { "Igual que el sistema", "Same as System", "Comme le système" },
    { "LANG_SYS_HELP", "Follows the language chosen in the HiBy's own settings.", "Suit la langue choisie dans les réglages du HiBy." },
    /* caratulas y letras */
    { "Buscar carátula", "Find artwork", "Chercher la pochette" },
    { "Buscar letra", "Find lyrics", "Chercher les paroles" },
    { "Carátula", "Artwork", "Pochette" },
    { "Letra", "Lyrics", "Paroles" },
    { "Buscar", "Search", "Rechercher" },
    { "Buscando carátulas", "Searching artwork", "Recherche de pochettes" },
    { "Descargando carátulas", "Downloading artwork", "Téléchargement des pochettes" },
    { "Buscando letras", "Searching lyrics", "Recherche de paroles" },
    { "Descargando letra", "Downloading lyrics", "Téléchargement des paroles" },
    { "Elegir imagen de la tarjeta SD", "Choose an image from the SD card", "Choisir une image sur la carte SD" },
    { "Elegir imagen", "Choose image", "Choisir une image" },
    { "Usar esta carátula", "Use this artwork", "Utiliser cette pochette" },
    { "Carátula guardada", "Artwork saved", "Pochette enregistrée" },
    { "Letra guardada", "Lyrics saved", "Paroles enregistrées" },
    { "Usa una imagen JPG", "Use a JPG image", "Utilisez une image JPG" },
    { "No se pudo descargar", "Download failed", "Échec du téléchargement" },
    { "Falta el componente de red (hibynet)", "The network component is missing (hibynet)", "Le composant réseau est absent (hibynet)" },
    { "No se encontraron carátulas. Cambia la búsqueda o elige una imagen de la tarjeta SD.", "No artwork found. Change the search or choose an image from the SD card.", "Aucune pochette trouvée. Modifiez la recherche ou choisissez une image sur la carte SD." },
    { "No se encontraron letras. Prueba a cambiar la búsqueda.", "No lyrics found. Try changing the search.", "Aucunes paroles trouvées. Essayez une autre recherche." },
    { "Incrustando carátula", "Embedding artwork", "Intégration de la pochette" },
};
/* textos largos en espanol (la clave no es el texto) */
static const char *tr_es_long(const char *k)
{
    if (!strcmp(k, "ADB_HELP_WIFI")) return "En el ordenador ejecuta: adb connect DIRECCIÓN (misma red Wi-Fi). Hacen falta las platform-tools de Android.";
    if (!strcmp(k, "ADB_HELP_USB")) return "Conecta el cable USB: el ordenador ve el disco y una «ADB Interface» a la vez (Windows no necesita drivers). La primera vez, ejecuta «adb shell» en menos de 90 s para confirmarlo; si no, vuelve a Wi-Fi.";
    if (!strcmp(k, "HPK_HELP")) return "Copia paquetes .hpk en la carpeta Apps de la SD para instalarlos desde aquí.";
    if (!strcmp(k, "FILT_HELP")) return "Las 8 formas de filtro del DAC, desbloqueadas (el firmware de serie solo dejaba 2). Es pura preferencia de escucha: ninguna puede dañar nada. Se aplica al instante y se conserva al reiniciar.";
    if (!strcmp(k, "FILT_HELP_OLD")) return "Solo hay 2 filtros: instala hibymod 0.5.4 o posterior para desbloquear los 8 filtros del DAC.";
    if (!strcmp(k, "VOLWARN_HELP")) return "Desactivarlo no añade potencia: solo quita el aviso de seguridad auditiva por encima de 34 (3,5 mm) / 28 (4,4 mm). Se aplica al reiniciar.";
    if (!strcmp(k, "VOLWARN_CONFIRM")) return "¿Quitar el aviso de seguridad auditiva? Escuchar mucho tiempo a volumen alto puede dañar el oído.";
    if (!strcmp(k, "TF_HELP")) return "Guarda la base de datos de música y la caché de carátulas en la SD en lugar de la memoria interna: más rápido con bibliotecas grandes y menos desgaste. Requiere reiniciar y volver a escanear la biblioteca.";
    if (!strcmp(k, "HIDDEN_HELP")) return "El modo coche (reproduce al recibir corriente USB y se apaga al quitarla), el standby, el doble toque para despertar y no cargar en modo DAC aparecen ya en los Ajustes del sistema de HiBy.";
    if (!strcmp(k, "REBOOT_HELP")) return "Algunos cambios se aplican al reiniciar el HiBy.";
    if (!strcmp(k, "ABOUT_HELP")) return "HiByOS es un firmware comunitario y gratuito para el HiBy R3 II, dirigido por Grohle y construido en su mayor parte por agentes de IA. Sin relación con HiBy.";
    if (!strcmp(k, "CC_HELP")) return "Elige los controles del Centro de control (desliza desde arriba).";
    if (!strcmp(k, "POWER_HELP")) return "Ahorro: animaciones a 30 fps y brillo como mucho al 40 %. Normal: 44 fps (el máximo del panel). Rendimiento: sin límites, para cuando está enchufado. No hay overclock: la CPU ya va un 10 % por encima de su frecuencia nominal.";
    if (!strcmp(k, "NOCHARGE_HELP")) return "El HiBy funciona con la corriente del USB sin cargar la batería: ideal para sesiones largas enchufado (DAC, escritorio).";
    if (!strcmp(k, "ADB_HELP_OFF")) return "ADB permite que un ordenador controle el HiBy (herramientas de desarrollo). Déjalo desactivado si no lo usas.";
    if (!strcmp(k, "LANG_SYS_HELP")) return "Sigue el idioma elegido en los ajustes del propio HiBy.";
    return 0;
}

/* tablas del resto de idiomas: { clave (espanol o id), traduccion }, terminadas en { 0, 0 } */
struct trp { const char *k, *v; };
#include "lang/de.inc.c"
#include "lang/it.inc.c"
#include "lang/ja.inc.c"
#include "lang/ko.inc.c"
#include "lang/pl.inc.c"
#include "lang/ru.inc.c"
#include "lang/zh_hans.inc.c"
#include "lang/th.inc.c"
#include "lang/zh_hant.inc.c"
#include "lang/uk.inc.c"
static const struct trp *const TRX[LANG_N] = { 0, 0, 0, TR_DE, TR_IT, TR_JA, TR_KO, TR_PL, TR_RU, TR_ZHS, TR_TH, TR_ZHT, TR_UK };

/* traduccion de s al idioma l sin reservas (0 si no hay); en espanol, el texto largo o s */
static const char *tr_lang(const char *s, int l)
{
    if (l == LANG_ES) { const char *x = tr_es_long(s); return x ? x : s; }
    if (l == LANG_EN || l == LANG_FR) {
        for (unsigned i = 0; i < sizeof TR / sizeof TR[0]; i++) if (!strcmp(TR[i][0], s)) return TR[i][l];
        return 0;
    }
    if (!lang_ok(l) || !TRX[l]) return 0;
    for (const struct trp *p = TRX[l]; p->k; p++) if (!strcmp(p->k, s)) return p->v;
    return 0;
}

static const char *tr(const char *s)
{
    if (!s) return s;
    static const char *ck[64]; static const char *cv[64]; static int cl[64];
    unsigned h = ((unsigned)(uintptr_t)s >> 2) & 63;
    if (ck[h] == s && cl[h] == ui_lang) return cv[h];
    const char *r = tr_lang(s, ui_lang);
    if (!r || !*r) r = tr_lang(s, LANG_EN);        /* sin traduccion: ingles */
    if (!r || !*r) r = s;                          /* ni eso: la clave */
    ck[h] = s; cv[h] = r; cl[h] = ui_lang;
    return r;
}

/* fecha corta ("3 ene 2026", "Jan 3, 2026", "2026年1月3日"...) en el idioma actual */
static void lang_date(time_t t, char *o, int n)
{
    static const char *const MON[LANG_N][12] = {
        { "ene", "feb", "mar", "abr", "may", "jun", "jul", "ago", "sept", "oct", "nov", "dic" },
        { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" },
        { "janv.", "févr.", "mars", "avr.", "mai", "juin", "juil.", "août", "sept.", "oct.", "nov.", "déc." },
        { "Jan.", "Feb.", "März", "Apr.", "Mai", "Juni", "Juli", "Aug.", "Sept.", "Okt.", "Nov.", "Dez." },
        { "gen", "feb", "mar", "apr", "mag", "giu", "lug", "ago", "set", "ott", "nov", "dic" },
        { 0 }, { 0 },
        { "sty", "lut", "mar", "kwi", "maj", "cze", "lip", "sie", "wrz", "paź", "lis", "gru" },
        { "янв.", "февр.", "мар.", "апр.", "мая", "июн.", "июл.", "авг.", "сент.", "окт.", "нояб.", "дек." },
        { 0 },
        { "ม.ค.", "ก.พ.", "มี.ค.", "เม.ย.", "พ.ค.", "มิ.ย.", "ก.ค.", "ส.ค.", "ก.ย.", "ต.ค.", "พ.ย.", "ธ.ค." },
        { 0 },
        { "січ.", "лют.", "бер.", "квіт.", "трав.", "черв.", "лип.", "серп.", "вер.", "жовт.", "лист.", "груд." },
    };
    struct tm tm; localtime_r(&t, &tm);
    int l = lang_ok(ui_lang) ? ui_lang : LANG_EN, d = tm.tm_mday, m = tm.tm_mon, y = tm.tm_year + 1900;
    if (m < 0 || m > 11) m = 0;
    switch (l) {
    case LANG_EN: snprintf(o, n, "%s %d, %d", MON[l][m], d, y); break;
    case LANG_DE: snprintf(o, n, "%d. %s %d", d, MON[l][m], y); break;
    case LANG_JA: case LANG_ZHS: case LANG_ZHT: snprintf(o, n, "%d年%d月%d日", y, m + 1, d); break;
    case LANG_KO: snprintf(o, n, "%d년 %d월 %d일", y, m + 1, d); break;
    case LANG_TH: snprintf(o, n, "%d %s %d", d, MON[l][m], y + 543); break;   /* era budista, como iOS */
    default: snprintf(o, n, "%d %s %d", d, MON[l][m], y); break;
    }
}
