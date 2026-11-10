# Tienda de apps (App Store) de HiByOS

La app **Apps** (`src/store.inc.c`) es una tienda ligera al estilo de la App Store de iOS que sirve
para tres cosas:

- instalar apps de **repositorios** en Internet;
- instalar los `.hpk` copiados en la tarjeta SD (`/Apps`);
- gestionar las apps instaladas: abrirlas, moverlas entre la SD y el HiBy, hacer copia de
  seguridad y desinstalarlas.

El formato de los paquetes se describe en `docs/FORMATO_APPS.md`.

## 1. Pantallas

Abajo hay una barra de pestañas como la de iOS:

| Pestaña | Contenido |
|---|---|
| **Hoy** | Tarjetas grandes con las apps destacadas (`featured`). El fondo sale del color medio del icono. Si no hay ninguna destacada, salen todas. |
| **Apps** | Todas las apps de los repositorios, con icono, nombre, descripción y botón. Debajo, los paquetes de la tarjeta SD (`/Apps/*.hpk`). |
| **Buscar** | Campo de búsqueda con el teclado de la casa. Filtra por nombre y descripción en el idioma actual, sin distinguir mayúsculas ni acentos. |
| **Instaladas** | Apps instaladas con su versión y su ubicación (HiBy o tarjeta SD). Si hay una versión nueva, aparece ACTUALIZAR, y la pestaña lleva una insignia roja con el número de actualizaciones. Debajo están los repositorios: estado, número de apps, «Actualizar ahora», «Eliminar» y «Añadir repositorio…». |

Ficha de la app: se abre al tocar una fila o una tarjeta y entra deslizándose desde la derecha.
Muestra:

- icono de 96 px, nombre y autor;
- el botón OBTENER, ACTUALIZAR o ABRIR;
- una franja con TAMAÑO, VERSIÓN y MEMORIA (`ram_kb`);
- la descripción y un aviso si la app pausa la música;
- la sección INFORMACIÓN, con el repositorio, la categoría y el identificador.

Para volver se toca «‹ Hoy» o se arrastra desde el borde izquierdo; la ficha sigue al dedo.

Gestos:

- **Desplazamiento**: inercia de iOS (deceleración 0,998/ms) y rebote en los extremos.
- **Tirar hacia abajo**: actualiza los repositorios (salvo en Buscar).
- **Tocar la pestaña activa**: vuelve arriba.
- **Barra de inicio**: sale. La tienda se encoge hasta su icono.

Botón de una app:

1. **OBTENER**. Si hay tarjeta SD, primero pregunta dónde instalar (ver §4). Después el botón se
   convierte en el **anillo de progreso** de iOS; tocar el anillo cancela la descarga.
2. Al terminar la descarga se comprueban **tamaño y sha256**. El anillo pasa a «instalando»
   (instalador atómico con progreso) y la app aparece al final de la última página del inicio.
3. **ABRIR** cierra la tienda y abre la app desde el centro de la pantalla.

Si se sale de la tienda durante una descarga, la descarga se cancela.

## 2. Repositorios

Un repositorio es **la URL de un `index.json`** por HTTPS (también sirve `http://`).

- El repositorio por defecto, que no se puede quitar, es
  `https://raw.githubusercontent.com/Grohle/hibyos/main/repo/index.json`.
- Los demás se guardan en `/usr/data/hibymod/store_repos.conf`, uno por línea, con un máximo de
  8 repositorios.
- Si dos repositorios publican el mismo `id`, manda el que esté antes en la lista (el de HiByOS
  primero).

### Formato de `index.json` (versión 1)

```json
{
 "format": 1,
 "name": {"es": "HiByOS", "en": "HiByOS", "fr": "HiByOS"},
 "homepage": "https://github.com/Grohle/hibyos",
 "apps": [
  {
   "id": "org.hibymod.navegador",
   "version": "1.0.0",
   "min_hibymod": "0.1.0",
   "name": {"es": "Navegador", "en": "Browser", "fr": "Navigateur"},
   "desc": {"es": "…", "en": "…", "fr": "…"},
   "author": "NetSurf / hibymod",
   "category": "internet",
   "ram_kb": 7000,
   "stop_player": true,
   "featured": true,
   "size": 4300800,
   "sha256": "2d5593b9…",
   "icon": "icons/org.hibymod.navegador.png",
   "hpk": "https://github.com/Grohle/hibyos/releases/download/apps-1/org.hibymod.navegador-1.0.0.hpk"
  }
 ]
}
```

| Campo | Obligatorio | Notas |
|---|---|---|
| `id`, `version` | sí | Mismas reglas que el `manifest.ini`: id DNS inverso y versión semver. |
| `name` | sí | Texto o un objeto por idioma (`es`, `en`, `fr` y cualquier otro código de `lang_code()`). Se busca el idioma actual, después `en`, después `es` y, por último, el primero que haya. |
| `desc` | recomendado | Igual que `name`. |
| `hpk` | sí | URL del paquete, absoluta o relativa al `index.json`. Se siguen hasta 8 redirecciones, así que valen las de las releases de GitHub. |
| `sha256` | sí | 64 dígitos hexadecimales del `.hpk`. Si no coincide, el paquete se borra y no se instala. |
| `size` | recomendado | Bytes del `.hpk`. Sirve para el progreso, el espacio libre y una comprobación extra. |
| `icon` | recomendado | PNG cuadrado y pequeño (128×128 basta, 512 px como máximo, 1 MB como máximo), absoluto o relativo. Se le aplica la máscara de los iconos de la casa. |
| `min_hibymod` | recomendado | Si es mayor que la versión del instalador, el botón aparece gris con el aviso «Necesita una versión más nueva de HiByOS». |
| `ram_kb`, `stop_player`, `author`, `category`, `featured` | no | Datos para la ficha y para la pestaña Hoy. |

Se ignoran las apps con algún campo obligatorio ausente o inválido. Los campos desconocidos
también se ignoran, por compatibilidad hacia delante. El índice puede ocupar como máximo 256 KB y
lista como máximo 64 apps en total.

### Dónde se publica el repositorio de HiByOS

- **Índice e iconos** en la rama `main`, carpeta `repo/`. Se sirven por `raw.githubusercontent.com`:
  respuesta directa sin redirección y caché de unos 5 minutos.
- **Paquetes** como *assets* de una release de GitHub que no es del firmware: `apps-1`, con nombres
  `<id>-<versión>.hpk`. Así los binarios no engordan el historial de git. La URL redirige (302) a
  `release-assets.githubusercontent.com`, y `hibynet` la sigue.

Para generarlo y publicarlo:

```sh
bash ~/hiby/tools/zbuild_app.sh hola                       # out/apps/hola.hpk
bash apps/netsurf-hiby/build/build_hf.sh                   # NetSurf hard-float (WSL)
bash apps/navegador/build.sh                               # out/apps/navegador.hpk
python tools/mkrepo.py out/apps/hola.hpk out/apps/navegador.hpk --tag apps-1
gh release upload apps-1 out/repo_assets/*.hpk --clobber -R Grohle/hibyos
git add repo && git commit -m "repo: …" && git push
```

Una versión nueva de una app es un `.hpk` nuevo, con otro nombre porque lleva la versión, más el
`index.json` regenerado. Los aparatos ven ACTUALIZAR la siguiente vez que refrescan.

## 3. Red, caché y seguridad

- Todo lo que va por la red lo hace **`hibynet get URL SALIDA`**, en procesos aparte, de modo que
  la interfaz no se bloquea nunca. Usa TLS 1.2 con verificación de certificados (mbedTLS) y escribe
  primero en `SALIDA.part` para hacer después un `rename` atómico.
- Va **un trabajo en segundo plano cada vez**: primero los índices y luego los iconos, en serie.
  Una descarga de paquete puede ir en paralelo con ese trabajo.
- Caché en `/usr/data/hibymod/cache/store/`:
  - `<fnv32(url)>.json`: los índices;
  - `icons/<fnv32(url)>.png`: los iconos;
  - `dl/<id>.hpk`: el paquete mientras se descarga, si se instala en el HiBy.

  Si se instala en la SD, el paquete se descarga a `SD/HiByOS/dl/`. En los dos casos se borra
  después de instalar.
- Al abrir la tienda se pinta enseguida lo que haya en caché. Los índices se refrescan si en esta
  sesión hace más de 10 minutos que no se refrescan, o al tirar hacia abajo.
- **Verificación**: el sha256 se calcula en el propio shell (C, FIPS 180-4) sobre el fichero
  descargado, y el tamaño también se comprueba. El instalador vuelve a validar todo (tar, rutas,
  ELF hard-float, espacio) antes de escribir nada (`FORMATO_APPS.md` §5).
- Un repositorio de terceros puede publicar cualquier binario. El sha256 protege de corrupciones y
  de que se cambie el paquete en el servidor de descarga, pero no del autor del índice. Por eso
  hay que añadir solo repositorios en los que se confíe.

## 4. Instalar en la tarjeta SD o en el HiBy

`/usr/data` tiene unos 24 MB libres; la SD tiene cientos de GB. Al pulsar OBTENER con tarjeta,
una hoja pregunta dónde instalar:

- «En la tarjeta SD (406 GB libres)», que va primero;
- «En el HiBy (23 MB libres)».

Antes de descargar se comprueba que haya en el destino al menos el doble del tamaño del paquete más
512 KB. Las actualizaciones se quedan donde está la app.

**Montaje medido**: `/dev/mmcblk0` en `/usr/data/mnt/sd_0`, exfat
`rw,fmask=0022,dmask=0022`, **sin `noexec`**. Todos los ficheros aparecen como 0755 y un binario
copiado allí se ejecuta bien (probado con `hibynet`). Por eso la app se ejecuta directamente desde
la SD, sin copiarla ni usar un cargador.

Estructura:

| Ruta | Contenido |
|---|---|
| `SD/HiByOS/apps/<id>/` | La app completa, instalada por el mismo instalador atómico con la raíz en `SD/HiByOS` (`hpk_set_root`). |
| `/usr/data/hibymod/apps/<id>/` | Un **talón**: `manifest.ini`, `icon_48/56/64.ov` y `sd_location` (la ruta real). Ocupa unos 30 KB. |
| `/usr/data/hibymod/appdata/<id>/` | Los datos de la app, siempre en el HiBy. |

El launcher y la lista de instaladas solo ven el talón. `run_ext_app()` lanza `exec` desde la ruta
real, y `HM_APP_DIR` apunta a la SD.

**Tarjeta ausente** (se detecta comparando `st_dev` de `sd_0` con el de `/usr/data/mnt`):

- en Instaladas la app sale como «Falta la tarjeta SD», en naranja;
- ABRIR y el icono del inicio muestran «Esta app está en la tarjeta SD: insértala para abrirla».

Pendiente fuera de mis ficheros: pintar el icono del inicio atenuado. Hace falta un gancho en
`launcher.inc.c`: si `apps[a].ext` es una app de la SD y `app_sd_missing()`, dibujarla con
alfa 0,4.

**Mover** desde Instaladas, con «Mover a la tarjeta SD» o «Mover al HiBy»:

- copia el árbol al destino, en `.tmp-<id>` y con `fsync`;
- hace el cambio atómico con `.old-<id>`;
- después sustituye o quita el talón y borra el origen.

**A prueba de cortes**:

- En cada momento existe una copia completa y el talón apunta a una ruta válida, o la versión
  anterior sigue en su sitio.
- Los restos (`.tmp-`, `.old-`, `.del-`) los limpian o restauran `hpk_recover()` en `/usr/data` y
  `sd_recover()`, que es `hpk_recover` con la raíz en la SD y se ejecuta al abrir la tienda.

Al desinstalar se borra la copia de la SD, si la tarjeta está, y después el talón. Si se desinstala
sin tarjeta, queda en la SD una carpeta huérfana `HiByOS/apps/<id>`, que se puede borrar a mano.

## 5. Memoria y rendimiento

- Datos de la tienda: unos 90 KB para 64 apps, más un sprite de 48 px por icono (4,6 KB) y uno de
  96 px para la ficha, que se libera al salir.
- Usa los búferes compartidos `img_under` e `img_sheet` para las transiciones; al salir pone
  `have_under = 0`.
- Cada `hibynet` ocupa 1–2 MB de RSS mientras dura.
- Se pinta solo cuando hay cambios o animaciones: progreso, anillo de carga, inercia o aviso.

## 6. Navegador (NetSurf): estado «EN PRUEBAS» (bloqueado)

**Estado actual: no se puede instalar ni abrir.**

- En `repo/index.json` aparece con `"available": false`, sin `hpk` ni `sha256`. El `.hpk` se retiró
  de la release.
- La tienda lo muestra como EN PRUEBAS.
- `run_ext_app()` se niega a abrir los ids de `app_blocked_ids[]` (`org.hibymod.navegador`), aunque
  estén instalados.

**Motivo.** La primera prueba en el aparato, con el reproductor vivo (unos 7,7 MB de MemAvailable),
**congeló el HiBy** (adb incluido) a los 2 s de abrirlo. No hay swap y el OOM killer no llega a
actuar: el kernel va expulsando el código de todos los procesos (*thrashing*).

### Qué se ha cambiado

- **NetSurf 3.11 recompilado en coma flotante hard** (`apps/netsurf-hiby/build/build_hf.sh`). Las
  dependencias (zlib, mbedTLS, curl, libpng, libjpeg y expat) también son hard-float. La pila de los
  hilos se limita a 256 KB.
- **Backend propio** (`nsfb_hiby.c`):
  - lienzo en RAM, copiado entero al búfer oculto y después `FBIOPAN`;
  - táctil de `HM_TOUCH`;
  - desplazamiento 1:1 con inercia de iOS, con el parche de `gui.c` que admite desplazamiento en
    píxeles;
  - `SIGTERM` cierra la app.
- **Lanzador y vigilante** (`apps/navegador/navegador.c`):
  - `oom_score_adj=1000` para `nsfb`;
  - tope `RLIMIT_AS` (prlimit) = VmSize inicial + RAM disponible − margen;
  - vigilante de `MemAvailable` cada 50 ms: por debajo de 2,8 MB hace `SIGKILL` y sale con el código
    99, y el shell muestra «La app se cerró por falta de memoria»;
  - el vigilante corre con `mlockall` y `SCHED_FIFO`.
- **Choices**:
  - caché en memoria de 384 KB, sin caché de disco;
  - sin JavaScript, sin imágenes de fondo, sin animaciones y sin la hoja de bloqueo de anuncios;
  - 2 conexiones;
  - bundle de CA reducido a 41 raíces (`ca.pem`): −0,8 MB de RSS anónima.
- **Manifest**: `stop_player=1` (pausa la música) y `refresh=full` (el shell mantiene `hz_hold`).

### Protocolo de pruebas fuera del aparato (obligatorio antes de volver a probarlo en el HiBy)

El arnés es `apps/harness_vm` (`~/hiby/vm`): qemu malta MIPS32r2 LE, MemTotal de 56 928 kB como el
HiBy y sin swap.

- **Memoria**: el perfil `coexist` (memhog de unos 36 MB bloqueados, como el reproductor) más
  `memhog 2`, lo que deja **unos 7,2–7,5 MB de MemAvailable**, como el aparato con música.
- **Script**: `share/nsm.sh SEG URL OUT` lanza el lanzador real con el mismo entorno que el shell.
  Cada 0,5 s muestrea VmRSS y VmHWM de `nsfb`, MemAvailable y el **hueco entre muestras**: un hueco
  de más de 1,5 s significa que el sistema se atascó.
- **Batería** (`scripts/navt2.cmds`):
  - DuckDuckGo Lite (inicio y búsqueda), text.npr.org y lite.cnn.com;
  - Wikipedia mediano (Ingenic) con 30 arrastres;
  - Wikipedia con imágenes (Cat);
  - Wikipedia enorme (Telegram ×2, Linux y una lista de países).

Criterios para aceptarlo:

1. memhog (el «reproductor») nunca muere;
2. el vigilante mata a `nsfb` en todas las páginas que no caben;
3. **ningún hueco supera 1,5 s**;
4. las páginas ligeras cargan y se pueden desplazar.

### Resultados (2026-11-10, VM, unos 7,3 MB disponibles al empezar)

| Página | VmHWM nsfb | Mínimo de MemAvailable | Resultado | Hueco máximo |
|---|---|---|---|---|
| DDG Lite inicio | 5,2 MB (anón. 1,9) | 4,7 MB | carga | 0,55 s |
| DDG Lite búsqueda | 4,4–6,3 MB | 2,8–4,3 MB | carga o lo mata el vigilante, según la RAM libre al empezar | hasta 4,8 s |
| text.npr.org | 4,8–5,1 MB | 3,7–5,5 MB | carga o lo mata el vigilante | 0,55 s |
| Wikipedia mediano / Cat | 6–7 MB | 1,9–3,2 MB | lo mata el vigilante | 0,8–2,1 s |
| Wikipedia enorme ×4 | 5,3–7,8 MB | 1,2–2,8 MB | lo mata el vigilante (4/4) | **7–10 s** |
| lite.cnn.com | 4,4–6,4 MB | 3,1–3,2 MB | carga o lo mata el vigilante | **hasta 15 s** |

memhog sobrevivió en todas las pruebas.

Con la versión anterior, sin tope ni `mlockall` y con el umbral en 2,3 MB, los huecos llegaban a
10 s y MemAvailable bajaba hasta 1,2 MB. Además, la MemAvailable al empezar va bajando de una prueba a
la siguiente (de 7,5 a 3,7 MB), y por eso la misma página a veces carga y a veces no.

**Veredicto**: con el reproductor vivo **no es utilizable de forma fiable**:

- solo caben páginas mínimas;
- en las demás el vigilante cierra la app, como estaba previsto;
- aun así quedan **atascos de varios segundos** antes de que pueda actuar, porque el *thrashing*
  empieza por encima de 2,8 MB.

Se mantiene bloqueado. Para avanzar hay tres opciones:

- **(a)** abrirlo solo con el reproductor *parado*, liberando su memoria. Hace falta un gancho en el
  shell para detener `hiby_player` y relanzarlo al salir; está por diseñar y medir.
- **(b)** sustituirlo por un visor de lectura con un *proxy* en el PC, de menos de 2 MB (ver
  `docs/investigacion_apps_vm.md`).
- **(c)** recortar NetSurf: libcss/hubbub más pequeños y quitar GIF, BMP y PNG entrelazado, que no
  basta por sí solo.

**No se volverá a abrir en el aparato** hasta que la batería anterior pase los cuatro criterios.

## Summary (English)

**Apps** is a light iOS-style App Store with repositories. A repository is the URL of an
`index.json` (format 1: `name`, `apps[]` with `id`, `version`, `min_hibymod`, localized
`name`/`desc` objects, `size`, `sha256`, `icon`, `hpk`, optional `featured`, `ram_kb`,
`stop_player`, `author`, `category`). The default repository is
`raw.githubusercontent.com/Grohle/hibyos/main/repo/index.json`; its packages are assets of the
GitHub release `apps-1`. Everything is fetched by `hibynet get` in background processes and cached
in `/usr/data/hibymod/cache/store/`. Packages are verified (size + sha256) before installation.
Tabs: Today, Apps, Search and Installed (installed apps, updates and repositories). Apps can be
installed on the SD card (`SD/HiByOS/apps/<id>`, executed in place because exfat is mounted
without `noexec`, with a small stub in `/usr/data`) or on the device, and moved between the two
crash-safely.

## Résumé (français)

**Apps** est une boutique légère façon App Store, avec des dépôts. Un dépôt est l'URL d'un
`index.json` (format ci-dessus). Le dépôt par défaut est `repo/index.json` sur la branche `main` ;
ses paquets sont des fichiers de la release `apps-1`. Tout passe par `hibynet get` en arrière-plan,
avec un cache dans `/usr/data/hibymod/cache/store/`, et la taille ainsi que le sha256 sont vérifiés
avant l'installation. Les apps peuvent s'installer sur la carte SD (exécutées sur place, avec un
petit talon dans `/usr/data`) ou dans le HiBy, et être déplacées de l'un à l'autre sans risque en
cas de coupure.

## Anexo: textos nuevos de la tienda (para `i18n.inc.c`)

Todos pasan por `tr("literal en español")`. Esta tabla da las traducciones al inglés y al francés.

| es | en | fr |
|---|---|---|
| Hoy | Today | Aujourd'hui |
| Buscar | Search | Rechercher |
| Instaladas | Installed | Installées |
| APP DESTACADA | FEATURED APP | APP À LA UNE |
| TODAS LAS APPS | ALL APPS | TOUTES LES APPS |
| Buscar apps | Search apps | Rechercher des apps |
| Nombre o descripción | Name or description | Nom ou description |
| %d resultados | %d results | %d résultats |
| Sin resultados | No results | Aucun résultat |
| Cargando… | Loading… | Chargement… |
| Actualizando… | Updating… | Mise à jour… |
| No hay apps en los repositorios | No apps in the repositories | Aucune app dans les dépôts |
| No se pudo conectar. Comprueba la WiFi y desliza hacia abajo para reintentar. | Couldn't connect. Check Wi-Fi and pull down to retry. | Connexion impossible. Vérifiez le Wi-Fi et tirez vers le bas pour réessayer. |
| Falta el componente de red (hibynet) | The network component (hibynet) is missing | Le composant réseau (hibynet) est absent |
| REPOSITORIOS | REPOSITORIES | DÉPÔTS |
| %s · %d apps | %s · %d apps | %s · %d apps |
| Sin conexión | Offline | Hors ligne |
| Añadir repositorio… / Añadir repositorio | Add repository… / Add repository | Ajouter un dépôt… / Ajouter un dépôt |
| URL del index.json | index.json URL | URL de l'index.json |
| URL no válida | Invalid URL | URL non valide |
| Ese repositorio ya está | That repository is already added | Ce dépôt est déjà ajouté |
| Repositorio añadido | Repository added | Dépôt ajouté |
| Actualizar ahora | Update now | Mettre à jour |
| Eliminar repositorio / Eliminar | Remove repository / Remove | Supprimer le dépôt / Supprimer |
| ¿Eliminar el repositorio? Las apps instaladas se conservan. | Remove the repository? Installed apps are kept. | Supprimer le dépôt ? Les apps installées sont conservées. |
| TAMAÑO / VERSIÓN / MEMORIA | SIZE / VERSION / MEMORY | TAILLE / VERSION / MÉMOIRE |
| INFORMACIÓN | INFORMATION | INFORMATIONS |
| Repositorio / Categoría / Identificador | Repository / Category / Identifier | Dépôt / Catégorie / Identifiant |
| Al abrirla se pausa la música para dejarle memoria. | Music pauses when it opens, to free memory. | La musique se met en pause à l'ouverture pour libérer de la mémoire. |
| Necesita una versión más nueva de HiByOS | Requires a newer version of HiByOS | Nécessite une version plus récente de HiByOS |
| EN PRUEBAS | IN TESTING | EN TEST |
| En pruebas: todavía no se puede instalar | In testing: can't be installed yet | En test : installation pas encore possible |
| Esta app está en pruebas y todavía no se puede abrir | This app is in testing and can't be opened yet | Cette app est en test et ne peut pas encore s'ouvrir |
| ¿Dónde instalar %s (%s)? | Where to install %s (%s)? | Où installer %s (%s) ? |
| En la tarjeta SD (%s libres) | On the SD card (%s free) | Sur la carte SD (%s libres) |
| En el HiBy (%s libres) | On the HiBy (%s free) | Sur le HiBy (%s libres) |
| Espera a que termine la descarga | Wait for the download to finish | Attendez la fin du téléchargement |
| No se pudo descargar | Download failed | Échec du téléchargement |
| La descarga está dañada (sha256) | The download is corrupted (sha256) | Le téléchargement est corrompu (sha256) |
| Versión %s disponible | Version %s available | Version %s disponible |
| Tarjeta SD | SD card | Carte SD |
| Falta la tarjeta SD | SD card missing | Carte SD absente |
| No hay tarjeta SD | No SD card | Pas de carte SD |
| Esta app está en la tarjeta SD: insértala para abrirla | This app is on the SD card: insert it to open the app | Cette app est sur la carte SD : insérez-la pour l'ouvrir |
| Mover a la tarjeta SD / Mover al HiBy | Move to SD card / Move to HiBy | Déplacer vers la carte SD / Déplacer vers le HiBy |
| Moviendo… | Moving… | Déplacement… |
| Movida a la tarjeta SD / Movida al HiBy | Moved to the SD card / Moved to the HiBy | Déplacée sur la carte SD / Déplacée sur le HiBy |
| No se pudo mover la app | Couldn't move the app | Impossible de déplacer l'app |
| La app se cerró por falta de memoria | The app closed: not enough memory | L'app s'est fermée : mémoire insuffisante |
| DOMINGO … SÁBADO | SUNDAY … SATURDAY | DIMANCHE … SAMEDI |
