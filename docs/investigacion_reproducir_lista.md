# Reproducir un fichero o una lista con el motor de fábrica (hiby_player)

Fecha: 2026-11-10. **[V]** = verificado (en el binario, o en el PC sobre una copia de la base); **[I]** = inferido, sin probar en el aparato.

El aparato no estaba conectado por adb durante este trabajo (`adb devices` vacío). Por eso todo sale del análisis del binario `rootfs/usr/bin/hiby_player` (md5 `d1dea413a91ca3c6759c9029744bc52c`) y de copias de la base (`~/hiby/big.db`, `mixtest.db`, `rt2.db`). **Nada se ha probado en el reproductor real**: §7 es el plan de prueba.

Continúa `investigacion_reproduccion.md` §5.

## 0. Resumen

| Pregunta | Respuesta |
|---|---|
| ¿Existe una función interna "carga esta lista y empieza en el índice i"? | **Sí [V].** La cola es una *ruta de lista* (cadena UTF‑16) guardada en el estado del reproductor, junto con un índice. La interfaz de fábrica, al tocar una canción, hace `set_param(0x17,0,0)`, `set_param(0x15, ruta)`, `set_param(8, n)`, `set_param(0x10, actual)` y `play_index(i, -1, 0x10002)` (`0x43e8a0`). |
| ¿Se puede llamar desde la librería? | **Sí [V/I].** El binario es `EXEC` (no PIE, base `0x400000`), así que las direcciones son fijas. Esas funciones toman ellas mismas el cerrojo del reproductor. El servidor **HiBy Link** (la app del móvil) ya las llama **desde su propio hilo**, no desde el de la UI (`playUuid` → `0x4577a0`). Nosotros hacemos lo mismo desde un hilo nuestro. |
| ¿De dónde sale el contenido de la lista? | La ruta de lista es la clave de `MANAGER_TABLE(id, path, count…)`. Las canciones están en `list_tb_<id>`, que se lee con `SELECT * FROM list_tb_%d limit ? offset ?` (`0x71de60`) **cada vez que hace falta una pista** [V]. Ejemplo: `list_tb_1` + `'album_artist\ABBA'` = la cola actual en la copia de la base. |
| Cola propia (b + a) | La librería crea `list_tb_7701` o `list_tb_7702` y su fila en `MANAGER_TABLE`, con ruta `hibymod\q1`/`hibymod\q2`. Para ello usa la **conexión SQLite y el cerrojo del propio reproductor**, sin un segundo proceso escribiendo. Después llama a la secuencia de fábrica. Siguiente, anterior, aleatorio, el Now Playing de fábrica y la reanudación al encender leen esa tabla como cualquier otra lista. |
| ¿Hay IPC que haga esto (c)? | **Ninguna local.** La cola de mensajes del reproductor (`0x462b20`, mensajes 4/5/6/8) solo hace play/pausa. HiBy Link (BT/Wi‑Fi) sí tiene `playUuid`/`playTrackUuid`/`setPlayMode`, pero con un protocolo binario propio y "uuid" con la gramática interna de listas. Es una vía externa posible, pero mucho más cara (§5). |
| Aleatorio / repetir | `setting_set(0x1a, modo)` + `set_param(0xa, modo)`, igual que hace `setPlayMode` de HiBy Link. Modos: **0** en orden, **1** repetir pista, **2** aleatorio, **3** repetir lista [V, por la tabla de nombres]. Es un ajuste persistente. |
| Estado | Código hecho y compilado: `bin/hibymod_disp_v6test.so` (**no instalado**), `pc_play_list()` en `playctl.inc.c` y `hibyplay list…`. La lógica SQL está probada en el PC sobre una copia de la base [V]. Falta la prueba en el aparato (§7). |

## 1. Datos del binario

- `ELF 32-bit LSB executable, MIPS32 rel2`, `Type: EXEC`, `LOAD 0x400000 R E` y `0x8fa000 RW`. Las direcciones del texto y de `.data/.bss` son absolutas en cada arranque [V].
- Cadenas anchas: UTF‑16LE de 2 bytes (`hw16`), no `wchar_t` de glibc. Funciones propias: `0x426040`/`0x426080` (copia y copia con límite), `0x426180` (comparación), `0x4261e0` (comparación de n caracteres) y `0x426300` (longitud).
- **Estado del reproductor `P = 0x969ac8`** (`0x1e68` bytes). `P+0x48`/`P+0x4c` son punteros a las funciones lock/unlock del reproductor.

### 1.1 Parámetros (`set_param` `0x4366a0(id, ptr, val)`, `get_param` `0x4368e0(id, out, out2)`)

Tablas de saltos en `0x7f1c34` (set) y `0x7f1cb0` (get). Las dos funciones toman el cerrojo `P+0x48`/`0x4c`.

| id | campo | significado |
|---|---|---|
| 0x08 | `P+0x2c` | número de pistas de la lista. `get(8, &n, 1)` lo recalcula (`0x433ca0`); para una ruta normal devuelve el valor que se puso |
| 0x0a | `P+0x24` | modo de reproducción; además avisa a HiBy Link (`0x459e00`) |
| 0x0b | `P+0x34` | índice actual en la lista (`play` lo escribe como `i - off - n`, ver 0x17) |
| 0x10 | `P+0x38` | índice anterior (la UI pone aquí el actual antes de cambiar) |
| 0x15 | `P+0x1654` (`0x96b11c`) | **ruta de lista** (UTF‑16, máx. 0x104) |
| 0x17 | `P+0x3c`, `P+0x40` | desplazamiento y cabecera de la vista (carpetas delante de las canciones). 0,0 para una lista simple |
| 0x1b | `0x96a684` | título de la lista (UTF‑8, 64 bytes) |

### 1.2 Arrancar la pista i

```
0043e8a0 play_index(i, pos, flags):
  get_param(0x15, ruta)            ; la lista actual
  0x43e7a0(ruta, i, item)          ; pista i: primero una vista abierta con esa ruta (0x44b620/0x50a9e0),
                                   ;   si no, 0x71de60(item, i, 1, 1, ruta) = consulta a la base
  0x43dfc0(item, i, pos, flags)    ; set 0xb = i-off-n; set 0x13/0x1a = 0; get 8 (n) -> 0x435ce0 (orden aleatorio);
                                   ;   0x4371c0 -> 0x433900(flags, item, pos) bajo el cerrojo del reproductor
```

Llamadas observadas (`callers.py 43e8a0`):

| Dirección | Quién llama | Flags |
|---|---|---|
| `0x4b4634`/`0x4b48a4` | interfaz, tocar una canción en una vista de lista | `0x10002`, pos = -1 |
| `0x457834` | HiBy Link `playUuid` | `0x10002`, pos = -1 |
| `0x457f4c` | HiBy Link `playTrackUuid` | `0x10002`, pos = -1 |
| `0x57031c` | "reproducir todo" (`a:\*`) | `0x10002`, pos = -1 |
| `0x439aec`/`0x509fe8` | reanudación al arrancar | `0x40000`, pos = ajuste `0x1f`; la ruta sale del ajuste `0x20` |

### 1.3 Lo que hace la interfaz de fábrica al tocar una canción (`0x4b4420`)

```
004b45c8  lw s4,0x3b70(s0); lw s1,0x3b84(s0)      ; (solo vistas con cabecera, view->0x40==2)
004b45d8  jal 0x4366a0  (0x17, s4, s1)             ; en las demás: (0x17, 0, 0)  [0x4b4814]
004b45e8  jal 0x4366a0  (0x15, view+0x3de0, 0)     ; ruta de lista de la vista
004b45f8  jal 0x4366a0  (8, view->0x70, 0)         ; número de elementos
004b4610  jal 0x4368e0  (0xb, &cur, 0)
004b4620  jal 0x4366a0  (0x10, cur, 0)
004b4634  jal 0x43e8a0  (i, -1, 0x10002)
```

### 1.4 HiBy Link (el mismo camino, desde otro hilo)

- Tabla de métodos en `.data.rel.ro`, `{nombre[32], id, manejador, id}`. Por ejemplo `setPlayState 0x1201`, `setPlayMode 0x1202`, `playUuid 0x1204`, `playTrackUuid 0x120a`… El manejador de los 0x12xx es `0x744678`, que llama a las retrollamadas guardadas en `0x9b46c0`.
- Registro: `0x45a64c jal 0x744904(struct)`. Retrollamadas: `+0x00 setPlayState 0x457a80`, `+0x04 setPlayMode 0x4578e0`, `+0x0c playUuid 0x458d80`, `+0x10 playPrevious 0x456ce0`, `+0x14 playNext 0x456ca0`, `+0x18 setProgress 0x457760`, `+0x24 playTrackUuid 0x457e00`…
- `playUuid(lista, pista)` → `0x457d60` (UTF‑8→UTF‑16) → `0x4577a0`:

```
004577d4  jal 0x455c20 (uuid, &tipo, ruta)   ; uuid -> ruta de lista interna
004577e4  jal 0x456380 (tipo, ruta, pista)   ; índice de la pista en la lista
00457810  jal 0x4366a0 (0x17, 0, 0)
00457820  jal 0x4366a0 (0x15, ruta, 0)
00457834  jal 0x43e8a0 (idx, -1, 0x10002)
```

- `setPlayState(1|2|…)` → `0x436d40(5|6|4, …)`, que es poner un mensaje en la cola del reproductor (`0x462b20`, cola en `*(P+0)`).
- `setPlayMode(v)` → `0x42baa0(0x1a, m)` (ajuste) + `0x4366a0(0xa, m)`. Correspondencia: HiBy Link 1→0, 2→3, 3→1, 4→2. `0x459e00` hace la inversa.
- Nombres de los modos internos: en `0x4f8c00` se arma `{"order","single","random","loop"}` = 0..3, es decir *Play playlist*, *Loop track*, *Shuffle* y *Loop list* (`str/english/play_settings.ini`) [V].

Hilo: el servidor corre en sus propios hilos (`hl_thread_begin`, transporte BT/Wi‑Fi), así que el reproductor ya admite estas llamadas desde fuera del hilo de la UI. Es la razón principal para llamarlas desde un hilo de la librería y no esperar a un `FBIOPAN`, que no llega con la pantalla apagada o quieta.

### 1.5 Listas en la base

- Contexto de la base: `ctx = *(void**)0x991364`. `ctx+0` es el `sqlite3*`; `ctx+0x564`/`+0x568` son los punteros a `0x70eee0`/`0x70ef00`, el lock/unlock (un semáforo, `0x47b460(sem,-1)`) que rodea cada acceso a la base. Se ponen en `0x71a6e8`.
- `0x71de60(out, desde, cuántos, flag, ruta)`: despachador de rutas de lista.
  - Compara la ruta con `all`, `album`, `format`, `m3u`, `book*`, `bookmark=`, `data\`, `collect`, `search`, `step`, `history`, `recent`, `info_table`, `music_search_`, `qobuz`, `sony`…
  - Si no coincide con ninguna: `id = 0x719520(ruta)` (`SELECT id FROM MANAGER_TABLE WHERE path = ?`, con `bind_text16` de `(wcslen+1)*2` bytes, por eso las rutas guardadas terminan en `\0`).
  - Después: `SELECT * FROM list_tb_%d limit ? offset ?` (`0x71ecd4`), sin ORDER BY, así que el orden es el `rowid`. Solo hay orden especial si la ruta empieza por `album` o es de unidad (`a:` …) [V].
- `list_tb_N` tiene **las 27 primeras columnas de MEDIA_TABLE** y `PRIMARY KEY(id, path, cue_id)`. El esquema exacto está en la cadena `CREATE TABLE IF NOT EXISTS %s%d(...)` [V].
- Las tablas las crea `0x7221a0` (escaneo de carpeta/consulta → `INSERT INTO %s%d VALUES …`) al abrir una vista. Si nadie la ha creado, `play_index` sobre una ruta de carpeta o de .m3u no encuentra la pista: `0x719520` devuelve -1. Por eso la librería la crea ella misma.
- API SQLite enlazada estáticamente (3.12.2, `THREADSAFE=1`) [V por uso]:

  | Función | Dirección | Prueba |
  |---|---|---|
  | `prepare_v2` | `0x62e740` | `(db, sql, -1, &st, 0)` |
  | `step` | `0x618480` | devuelve 100/101 |
  | `finalize` | `0x5fbae0` | `beqz a0` |
  | `reset` | `0x5fbbc0` | |
  | `bind_text16` | `0x5db5a0` | `lb SQLITE_UTF16NATIVE` |
  | `bind_int` | `0x5db700` | `j bind_int64; sra a3,a2,31` |
  | `column_int` | `0x5c9840` | |

- Unidades: `a:` = `/data/mnt/sd_0` [V, por datos]; `b:` = `sd_1` [I].

## 2. Opciones evaluadas

| | Opción | Veredicto |
|---|---|---|
| a | Llamar a la función interna | **Elegida**, con la secuencia exacta de la UI y de HiBy Link. No hace falta un "evento de cargar lista": se cambia el estado con `set_param` (con cerrojo) y se llama a `play_index`. |
| b | Escribir la cola en la base y forzar que la relea | No hay evento de "releer". Pero la pista i **siempre** se lee de `list_tb_<id>` en el momento, así que escribir la tabla + (a) basta. Se escribe dentro del reproductor, con su conexión y su cerrojo: sin bloqueos entre procesos y sin la caché de sentencias de otro proceso. `list_tb_1` (la del usuario) no se toca. |
| c | IPC interna | La cola de mensajes `0x462b20` solo lleva play/pausa/stop. `/data/dmr_streamer` (DLNA) está descartado (`investigacion_reproduccion.md` §1). HiBy Link: §5. |

## 3. Diseño implementado (librería v6, orden `'L'`)

```
shell: pc_play_list(paths,n,i,title,mode)
   escribe /tmp/hibymod_playreq (rename atómico) y manda 'L' a /tmp/hibymod_disp
libreria (hilo de control): 'L' -> pl_kick(): hilo propio (512 KB de pila), sin el mutex de pantalla
   pl_check(): 13 firmas de 16 bytes + proyecciones (/proc/self/maps) + ctx->lock==0x70eee0 y unlock==0x70ef00 + P+0x48 != 0
   ranura = la que NO suena (get_param 0x15 == "hibymod\q1" ? 2 : 1)
   db_lock(); BEGIN
     TEMP hm_req(pos, path, cue) <- las rutas (a:\..., con \0 final, bind_text16)
     DROP/CREATE list_tb_770x (esquema de fabrica)
     INSERT OR IGNORE ... SELECT de MEDIA_TABLE LEFT JOIN hm_req ORDER BY pos   (unscanned=1: entran tambien las que no estan)
     indice real = filas antes de la primera fila de la ruta pedida
     MANAGER_TABLE: DELETE id/ruta; INSERT (770x, 'hibymod\qS\0', n, n, 0, 0)
   COMMIT (ROLLBACK si algo falla); db_unlock()
   [mode] setting_set(0x1a,m); set_param(0xa,m)
   [title] set_param(0x1b,title)
   set_param(0x17,0,0); set_param(0x15,ruta); set_param(8,n); get(0xb)->set(0x10); play_index(i, pos|-1, 0x10002)
   -> /tmp/hibymod_playres  "id=N rc=R rows=M index=I slot=S ms=T msg" (+ una linea en /usr/data/hibymod/disp.log)
```

- **Inerte** si no llega `'L'`: no se crea ningún hilo ni se lee nada. Las demás órdenes no cambian, salvo que `'L'` ya no pasa por el mutex de pantalla.
- Firmware distinto: `rc=-ENOEXEC` y no se llama a nada. Antes de leer una dirección fija se comprueba en `/proc/self/maps`, para que otro binario no tumbe el reproductor.
- Una petición a la vez: si llega otra mientras tanto, se ignora y queda anotado en el registro.
- Peticiones de experto:
  - `list=RUTA` reproduce una lista que ya existe, sin escribir nada; es la prueba más segura. `n` (param 8) sale de su fila de `MANAGER_TABLE`; si no la tiene (carpetas `a:\…`), se queda el `n` anterior [I].
  - `check=1` comprueba firmas y devuelve el modo, la lista, el índice y el total actuales.
  - `drop=1` borra `list_tb_7701/7702` y sus filas; se niega si suena una de ellas.
- **Probado en el PC [V]** (`tests/pl_sql_test.py`, Python sqlite sobre una copia de `big.db`). Se extraen las sentencias del propio `hibymod_disp.c`:
  - el orden se respeta;
  - un duplicado se descarta;
  - una ruta inexistente se descarta o, con `unscanned=1`, entra en su sitio;
  - el índice se recalcula bien (pedida la 4ª con una descartada delante: `index 2`);
  - `MANAGER_TABLE` queda correcta e `integrity_check ok`.
  - Plan: `SCAN r` + `AUTOMATIC COVERING INDEX (path=?)`, o sea O(n + m), no O(n·m). 1000 pistas: 58 ms en el PC (en el aparato serán decenas o cientos de ms [I]).

### API del shell (`src/playctl.inc.c`)

```c
int pc_play_list(const char *const *paths, int n, int index, const char *title, int mode); // bloquea <=5 s
int pc_play_file(const char *path);                                    // lista de 1
int pc_list_send(paths, n, index, title, mode);  int pc_list_poll(id, &res);  // sin bloquear, para la UI
int pc_listpath_send(const char *listpath, int index);                 // lista ya existente
int pc_list_check_send(void);  int pc_list_drop_send(void);
// mode: -1/0xff no tocar | 0 orden | 1 repetir pista | 2 aleatorio | 3 repetir lista ; | PC_UNSCANNED
// rutas: /data/mnt/sd_0/... o a:\... ; "CUE|ruta" elige una pista de .cue (cue_id)
// devuelve filas (>=0) o <0: -ENXIO sin libreria, -ETIMEDOUT, -ENOEXEC, -EBUSY, < -1000 = error SQLite
```

Herramienta: `bin/hibyplay`:

```
hibyplay list [-i N] [-t T] [-m M] [-u] F...
hibyplay listpath RUTA N
hibyplay listpath check
hibyplay listpath drop
```

**App Música (recomendado)**:

- **Álbum**: las rutas del álbum en orden de pista (de la copia de la base que ya lee la app) + el índice tocado, con `pc_list_send` y `pc_list_poll` en el bucle.
- **Lista**: igual.
- **Canción suelta**: lista de 1, o mejor el álbum con el índice de esa canción, como hace la UI de fábrica.

## 4. Límites y riesgos conocidos

- **Duplicados**: la clave `(id, path, cue_id)` impide repetir la misma pista en una cola. Se descartan, y `rows`/`index` lo reflejan.
- **Pistas no escaneadas**: por defecto se descartan; con `-u` entran con datos mínimos (nombre = ruta, `format=0`). No sé si el reproductor saca entonces el formato de la extensión [I].
- **Transacción en la conexión del reproductor**: dura lo que tarda el INSERT (con `db_lock` tomado, así la UI espera y no se mezcla). Si otro hilo suyo usara la base sin `db_lock` y abriera su propio `BEGIN` justo entonces, el suyo fallaría [I, poco probable: todo el código visto usa `ctx+0x564`]. Si el nuestro choca (transacción abierta), devolvemos `-EBUSY` sin tocar nada.
- **Limpieza de fábrica**: hay código que borra tablas `list_tb_*` (`0x714800`, `DROP TABLE IF EXISTS %s%d`) [I: no he visto cuándo se ejecuta, seguramente al reconstruir la base]. Si borrara la nuestra, la siguiente pista fallaría como si se hubiera borrado el fichero; se arregla volviendo a mandar la cola.
- **Vista "lista actual" de fábrica** con ruta `hibymod\q1`: no sé qué título ni qué tipo de vista elige [I]. Se prueba en §7.
- **`pos=`** (empezar en un punto): `play_index` lo acepta, pero la unidad del ajuste `0x1f` (¿ms?) no está verificada [I]. Por defecto -1.
- **`mode`** cambia un ajuste persistente del usuario, como su menú. Solo se toca si se pide.
- `b:`/`c:` (sd_1, USB) no se han verificado; las rutas `a:\…` sí.

## 5. Alternativa sin librería: cliente HiBy Link (no implementada)

`hl_wifi_server_init` (`0x748f30`) y los servidores BR/EDR y BLE exponen `playUuid(lista, "<path>…")`, `playTrackUuid`, `setPlayMode`, `getPlaylistItems`… El "uuid" de lista es la misma ruta interna (`0x455c20`/`0x45b780`: tipos 1–0x15, `d`+`data\playlist\X.m3u`, `album`, `artist`…). Haría falta:

- reimplementar el transporte (`link_*`, `msg_*`, CRC16 `hl_crc16_calc`);
- tener el servidor Wi‑Fi activo (un ajuste);
- y aun así la lista tendría que existir en la base.

Más trabajo y más frágil que la v6. Útil solo si algún día no se puede usar LD_PRELOAD.

## 6. Ficheros

| Fichero | Qué |
|---|---|
| `src/hibymod_disp.c` | v6: sección final "reproducir una lista" + `'L'` en `control()` (sin el mutex de pantalla) |
| `src/hm_shared.h` | solo el comentario de la orden `'L'` (la estructura no cambia) |
| `src/playctl.inc.c` | `pc_play_list`, `pc_play_file`, `pc_list_send/poll/wait`, `pc_listpath_send`, check/drop |
| `src/hibyplay.c` | `list`, `listpath` (incluido `check`/`drop`) |
| `bin/hibymod_disp.so` = `bin/hibymod_disp_v6test.so` | v6 compilada (md5 `dd921b08c6d0a750b937bc8f27becd5a`), **sin instalar**: v5 + orden `'L'` + corrección del tearing (§8) |
| `bin/hibyplay` | herramienta de prueba (musl estático) |

## 7. Plan de prueba (con el permiso del usuario; necesita un reinicio)

`adb` = `%LOCALAPPDATA%\platform-tools\adb.exe -s "HiBy R3II_2025"`. Antes de empezar, que el usuario deje la música en pausa.

**P0. Copias (sin riesgo)**

```
adb shell "mkdir -p /usr/data/hibymod/t; cp /usr/data/usrlocal_media.db /usr/data/hibymod/t/media_antes_v6.db"
adb shell "ls /usr/data/hibymod/preload/; md5sum /usr/data/hibymod/preload/*.so"
adb shell "cp /usr/data/hibymod/preload/hibymod_disp.so /usr/data/hibymod/t/hibymod_disp.so.bak"
adb push bin/hibymod_disp_v6test.so /usr/data/hibymod/t/
adb push bin/hibyplay /usr/data/hibymod/t/ ; adb shell chmod 755 /usr/data/hibymod/t/hibyplay
```

**P1. Instalar y reiniciar (con permiso)**

```
adb shell "cp /usr/data/hibymod/t/hibymod_disp_v6test.so /usr/data/hibymod/preload/hibymod_disp.so && sync && reboot"
```

**Ojo:** la v6 incluye la v5, que tampoco se ha probado aún en el aparato (en él está la v4).

- Lo que trae la v5: anotación de ficheros y PCM, y el hilo PCM a `SCHED_FIFO`; este último se desactiva con `touch /usr/data/hibymod/no_rt`.
- `bin/hibymod_disp.so` ya es esta misma v6; no queda un binario v5 aparte. Si se quieren separar riesgos, hay que recompilar la v5 desde una copia anterior del fuente.
- El arranque de la v6 es idéntico al de la v5. Lo nuevo solo corre al recibir `'L'`, y si fallara, el reproductor caería y el aparato se reiniciaría (`hiby_player.sh`) sin que nada vuelva a mandar `'L'`.

Tras el arranque, comprobar que todo lo de antes sigue igual: shell, gestos, Now Playing (`hibyplay status`) y `cat /usr/data/hibymod/disp.log`.

**P2. Comprobación sin efectos**

```
adb shell /usr/data/hibymod/t/hibyplay listpath check
```

- Esperado: `rc=0 … firmas ok; modo=M lista='album_artist\ABBA' indice=K total=18`. Apuntar `M`, la lista y el índice para restaurarlos.
- Si sale `rc=-8` (ENOEXEC), el firmware es distinto: parar aquí.

**P3. Lista de fábrica existente, sin escribir nada**

```
adb shell "/usr/data/hibymod/t/hibyplay listpath 'album_artist\ABBA' 3"   # (la ruta que dio P2)
```

- Esperado: suena la pista 4 de esa lista.
- Comprobar `hibyplay status` (ruta = 4ª fila de `list_tb_1`), el Now Playing de fábrica y la pantalla del reproductor (capturar con `C`).
- Después `hibyplay next` → la 5ª y `hibyplay prev` → la 4ª.
- Si no suena: `cat /tmp/hibymod_playres` y `disp.log`.

**P4. Cola propia (escribe solo en `list_tb_7701` + `MANAGER_TABLE`)**

Elegir 3 rutas de dos álbumes distintos sacadas de `MEDIA_TABLE` (de la copia de P0):

```
adb shell "/usr/data/hibymod/t/hibyplay list -i 1 -t Prueba /data/mnt/sd_0/A/B/01.flac /data/mnt/sd_0/A/B/02.flac /data/mnt/sd_0/C/D/05.flac"
```

- Esperado: `rc=0 rows=3 index=1 slot=1 ms=<…>` y suena `02.flac`.
- `next` → `05.flac`; `next` al final: según el modo (en orden se para, repetir lista vuelve a la 1).
- `prev` → `02.flac`.
- Abrir la "lista actual" de fábrica desde su Now Playing: ¿salen 3 elementos y qué título? Capturar.
- Verificar en una copia:

  ```
  adb shell cp /usr/data/usrlocal_media.db /usr/data/hibymod/t/m_p4.db
  adb pull /usr/data/hibymod/t/m_p4.db
  ```

  En el PC: `SELECT * FROM MANAGER_TABLE`, `SELECT rowid,name FROM list_tb_7701` y `pragma integrity_check`.

**P5. Alternancia y repetición**

- Mandar otra cola → `slot=2`.
- Otra más → `slot=1`.
- Las dos veces debe sonar la pista pedida sin cortes raros.

**P6. Rendimiento**

- Una cola de unas 500 pistas (por ejemplo, todas las de un artista grande).
- Anotar `ms=`.
- Mientras tanto, desplazar una lista de fábrica: no debería congelarse más de lo que dure `ms`.

**P7. Errores (no deben cambiar nada)**

- Ruta inexistente → `rc=-5 … lista vacia`.
- Sin `HMPL1` → `rc=-22`.
- Dos peticiones seguidas sin esperar → la segunda se ignora (`pl: ocupado` en `disp.log`) y su `hibyplay` acaba en `-ETIMEDOUT`. El shell debe esperar la respuesta antes de mandar otra.

**P8. (Opcional, cambia un ajuste del usuario) Aleatorio**

- `hibyplay list -m 2 …` → `next` salta al azar.
- Después restaurar el modo `M` de P2 con `hibyplay list -m M …`, o desde su menú.

**P9. (Opcional) Reanudación**

Con una cola nuestra sonando, reiniciar. Al arrancar, ¿reanuda nuestra cola (`check` → `lista='hibymod\q1'`)?

**Vuelta atrás**

1. Poner cualquier lista de fábrica.
2. `hibyplay listpath drop` (borra 7701/7702 y sus filas).
3. `adb shell "cp /usr/data/hibymod/t/hibymod_disp.so.bak /usr/data/hibymod/preload/hibymod_disp.so && sync && reboot"`.

La base original queda en `t/media_antes_v6.db`. **No restaurarla en caliente**: solo con el reproductor parado y si de verdad hiciera falta.

**Qué anotar en cada paso**:

- la salida de `hibyplay` y `/tmp/hibymod_playres`;
- las líneas `pl:` de `disp.log`;
- `hibyplay status`;
- capturas del Now Playing de fábrica.

## 8. Extra en la misma compilación: tearing al presentar el shell

Hay tearing en las transiciones en modo normal.

**Causa.** El panel ST77903 no tiene GRAM: el DPU barre el búfer de la DDR sin parar (`investigacion_60fps.md` §1.4). Las órdenes `'P'`, `'R'` y `'O'` escribían en el búfer **visible** (`buf_at(last_var.yoffset)`) con el barrido en cualquier punto, y después hacían pan, que solo espera al fin de fotograma. El corte quedaba donde estuviera el barrido.

**Corrección** (`frame_edge()` en `hibymod_disp.c`):

- **Orden.** Primero `frame_edge()`: un pan del mismo búfer, que duerme hasta la interrupción de fin de fotograma. Después se copia de arriba abajo, sin segundo pan.
  - La copia completa son 300 KB, del orden de 1 ms; la de filas sucias, menos.
  - Empieza en el vblank (27 líneas, unos 0,8 ms) y va mucho más rápida que el barrido (22,6 ms por pantalla), así que siempre va por delante de él [I: falta medir la latencia de despertar].
  - Se ha usado el pan en vez de `FBIO_WAITFORVSYNC` porque su espera ya está verificada en el driver (`lcdc_pan_display`) y no hace falta otro ioctl.
- **`'P'`.** El cerrojo de secuencia `fr0_seq` se comprueba después del borde; casi siempre está par, así que no retrasa la copia.
- **`'R'`** (salida de captura). `copy_then_ov()` copia las filas hasta el final de la capa, pone la capa y luego copia el resto. Ninguna fila de la capa se barre sin ella.
- **`'O'`**. Tras el borde, `ov_restore` + `ov_apply` seguidos (decenas de µs). No se ve ningún fotograma con la capa a medio quitar.
- **Shell muerto** (`input_thread`). La misma regla.
- **No cambia** el gancho del pan del reproductor: ahí se escribe en el búfer que *va a* presentarse, que aún no se ve. Eso es correcto.

**Latencia.** Antes: copiar y luego esperar el fin de fotograma (se veía en el barrido en curso, con corte). Ahora: esperar y luego copiar (se ve entero en el barrido siguiente). Cada `'P'` sigue bloqueando un periodo como mucho, así que la cadencia no cambia [I].

**Por qué esto no repite el fallo antiguo.** El intento revertido de "vsync y luego escribir" partía de que el panel tenía GRAM: escribía después de un pan que ya había lanzado la transferencia. Aquí no hay transferencia: se escribe en el vblank, justo antes del barrido que la mostrará.

**Medir en el aparato** (cuando se instale):

- `hibyctl pantime 40` y `hibyctl fps` durante una animación: deben seguir en unos 44 fps, sin fotogramas perdidos de más.
- Grabación (`rec`) de una transición, fotograma a fotograma: sin línea de corte.
- Abrir y cerrar el aviso de volumen (`'O'`): sin parpadeo de la capa.
