/* hibytag.h - lectura y escritura de etiquetas (FLAC, MP3/ID3, M4A solo lectura + portada)
 * y actualizacion de la base del reproductor (usrlocal_media.db).
 *
 * Dos formas de usarlo:
 *   - compilar hibytag.c aparte y enlazarlo (API publica normal);
 *   - en el shell (una sola unidad de traduccion):
 *         #define HT_API static
 *         #include "hibytag.c"          (incluye este .h)
 *     Defina HIBYTAG_NO_DB antes si no se enlaza SQLite (quita las funciones ht_db_*).
 * Todos los nombres internos empiezan por ht_ / htx_ para no chocar con el shell.
 *
 * Memoria: ninguna operacion carga el fichero entero. Escribir usa ~80 KB de bufer
 * (+ hasta 128 KB para la zona reescrita en sitio); leer, unos pocos KB.
 */
#ifndef HIBYTAG_H
#define HIBYTAG_H
#include <stddef.h>
#include <stdint.h>

#ifndef HT_API
#define HT_API
#endif

/* campos que se leen y escriben */
enum { HT_TITLE, HT_ARTIST, HT_ALBUM, HT_ALBUMARTIST, HT_GENRE, HT_YEAR, HT_TRACK, HT_DISC, HT_NFIELDS };
/* formatos */
enum { HT_FMT_NONE, HT_FMT_FLAC, HT_FMT_MP3, HT_FMT_M4A };
/* codigos de error (negativos) */
enum { HT_OK = 0, HT_EIO = -1, HT_EFORMAT = -2, HT_EUNSUP = -3, HT_ENOSPC = -4,
       HT_ECANCEL = -5, HT_ENOMEM = -6, HT_EOPEN = -7, HT_EDB = -8,
       HT_ETOOBIG = -9, HT_EIMAGE = -10, HT_EKEY = -11, HT_EEXIST = -12, HT_ENAME = -13 };
/* tamano maximo de la imagen de ht_set_cover */
#define HT_COVER_MAX (1024 * 1024)
/* como se escribio un fichero */
enum { HT_MODE_NONE, HT_MODE_INPLACE, HT_MODE_REWRITE };

/* etiquetas leidas: cadenas UTF-8 en memoria dinamica (NULL = no existe) */
typedef struct { int fmt; char *v[HT_NFIELDS]; } ht_tags;
/* cambios a aplicar: NULL = no tocar, "" = borrar el campo, otro = nuevo valor UTF-8 */
typedef struct { const char *v[HT_NFIELDS]; } ht_changes;

/* progreso de un fichero (bytes hechos / total). Devolver != 0 cancela (el original queda intacto). */
typedef int (*ht_progress_fn)(void *ud, uint64_t done, uint64_t total);
/* progreso de un lote: fichero idx (0..n-1) y bytes globales hechos / total */
typedef int (*ht_batch_fn)(void *ud, int idx, int n, uint64_t done, uint64_t total);

HT_API const char *ht_field_name(int f);                /* "title", "artist", ... */
HT_API int ht_field_parse(const char *name);            /* -1 si no existe; acepta alias (albumartist, date...) */
HT_API const char *ht_strerror(int err);
HT_API const char *ht_fmt_name(int fmt);

HT_API int ht_read(const char *path, ht_tags *t);       /* HT_OK o error; t siempre queda inicializado */
HT_API void ht_tags_free(ht_tags *t);

/* Escribe los cambios. En sitio si caben en el relleno (una sola escritura pequena + fsync);
 * si no, reescribe a un temporal en la misma carpeta (bufer 64 KB, fsync, rename atomico)
 * dejando HT_PAD bytes de relleno nuevo. Ante cualquier error el original queda intacto.
 * mode (opcional) recibe HT_MODE_*. */
HT_API int ht_write(const char *path, const ht_changes *ch, ht_progress_fn cb, void *ud, int *mode);

/* Mismos cambios a n ficheros. res[i] (opcional) recibe el resultado de cada uno.
 * Devuelve cuantos fallaron (0 = todo bien) o HT_ECANCEL si se cancelo. */
HT_API int ht_batch(const char *const *paths, int n, const ht_changes *ch, ht_batch_fn cb, void *ud, int *res);

/* Pone la portada (JPEG, <= HT_COVER_MAX) sustituyendo la portada que hubiera:
 * FLAC: bloque PICTURE tipo 3; MP3: ID3v2 APIC tipo 3 (conserva v2.3/v2.4; sin etiqueta crea v2.3);
 * M4A/MP4: moov/udta/meta/ilst/covr (tipo 13), corrigiendo stco/co64 si moov va antes de mdat.
 * Ogg y ID3v2.2: HT_EUNSUP. Siempre reescritura completa (temporal + fsync + rename), en
 * streaming (~100 KB de RAM); el audio y las demas etiquetas quedan byte a byte.
 * HT_ETOOBIG si la imagen pasa de 1 MB, HT_EIMAGE si no es un JPEG valido. */
HT_API int ht_set_cover(const char *path, const char *jpeg_path);

/* solo para pruebas: si > 0, la reescritura falla adrede tras copiar estos bytes */
extern uint64_t ht_test_fail_at;

/* ------------------------------------------------------------------ todas las etiquetas de texto
 * Claves canonicas (estilo Vorbis, en mayusculas): TITLE ARTIST ALBUM ALBUMARTIST GENRE DATE
 * TRACKNUMBER DISCNUMBER (las 8 de siempre), TRACKTOTAL DISCTOTAL COMPOSER CONDUCTOR LYRICIST
 * COMMENT LYRICS LABEL ISRC BPM COPYRIGHT ... y cualquier clave libre.
 *   FLAC: cada comentario CLAVE=valor (alias: YEAR->DATE, TOTALTRACKS->TRACKTOTAL,
 *         TOTALDISCS->DISCTOTAL, UNSYNCEDLYRICS->LYRICS, ALBUM ARTIST->ALBUMARTIST);
 *         claves repetidas -> un elemento con los valores unidos por "; " (HT_ITEM_MULTI).
 *   MP3:  tramas T*** conocidas (TCOM->COMPOSER, TPE3->CONDUCTOR, TPUB->LABEL, TSRC->ISRC...),
 *         TRCK "3/12" -> TRACKNUMBER 3 + TRACKTOTAL 12 (igual TPOS), COMM -> COMMENT (o
 *         COMMENT:descripcion), USLT -> LYRICS, TXXX:desc -> DESC. Tramas T*** desconocidas:
 *         clave = su id (solo lectura). Imagenes, URL, PRIV... no se listan y no se tocan.
 *   M4A:  solo lectura (fmt == HT_FMT_M4A: ht_write_kv devuelve HT_EUNSUP).
 * Las imagenes incrustadas (PICTURE, APIC, METADATA_BLOCK_PICTURE) nunca se listan. */
#define HT_ITEM_TRUNC 1      /* valor demasiado largo, leido a medias: no editar */
#define HT_ITEM_RO    2      /* solo lectura (REPLAYGAIN_*, iTunNORM..., tramas desconocidas) */
#define HT_ITEM_MULTI 4      /* varios valores en el fichero */
#define HT_LONGMAX 32000     /* tope de un valor de ht_read_all (letras, comentarios) */
typedef struct { char *key, *orig, *val; int flags; } ht_item;      /* key canonica; orig = como esta en el fichero */
typedef struct { int fmt, n, cap; ht_item *it; } ht_all;
HT_API int ht_read_all(const char *path, ht_all *a);    /* HT_OK o error; a siempre queda inicializado */
HT_API void ht_all_free(ht_all *a);
HT_API const ht_item *ht_all_find(const ht_all *a, const char *key);   /* key en cualquier forma */
HT_API void ht_key_canon(const char *key, char *out, size_t outsz);
HT_API int ht_key_valid(const char *key);               /* 1 si vale como clave (ASCII 0x20..0x7D sin '=') */
HT_API int ht_key_base(const char *canon);              /* HT_TITLE.. si es una de las 8, si no -1 */

/* cambio por clave: val NULL = no tocar, "" = borrar, otro = nuevo valor (sustituye a todos) */
typedef struct { const char *key, *val; } ht_kv;
/* Como ht_write, con claves libres. Mismas reglas de seguridad (en sitio si cabe en el relleno;
 * si no, temporal + fsync + rename; audio e imagenes byte a byte). HT_EKEY si una clave no vale. */
HT_API int ht_write_kv(const char *path, const ht_kv *kv, int nkv, ht_progress_fn cb, void *ud, int *mode);
HT_API int ht_batch_kv(const char *const *paths, int n, const ht_kv *kv, int nkv, ht_batch_fn cb, void *ud, int *res);

/* ------------------------------------------------------------------ nombres de fichero */
HT_API int ht_name_valid(const char *name);             /* 1 si vale en FAT/exFAT (sin / \ : * ? " < > |) */
HT_API void ht_name_clean(const char *in, char *out, size_t outsz);   /* lo no valido -> '_' */
enum { HT_PAT_NUM_TITLE, HT_PAT_ARTIST_TITLE, HT_PAT_NUMTITLE, HT_PAT_N };   /* "01 - T", "A - T", "01 T" */
/* nombre nuevo segun el patron (ext con punto, p. ej. ".flac"); multidisc: "1-01". 0 si falta el titulo */
HT_API int ht_pattern_name(int pat, const char *track, const char *disc, int multidisc,
                           const char *artist, const char *title, const char *ext, char *out, size_t outsz);
/* renombra una cancion y su .lrc de al lado (misma raiz). HT_EEXIST si el destino ya existe,
 * HT_ENAME si el nombre no vale. Si el .lrc no se puede mover, deshace el cambio. */
HT_API int ht_rename_song(const char *from, const char *to);
/* renombra/mueve una carpeta (con su cover.jpg, .lrc...). Crea la carpeta madre del destino si
 * falta; rm_parent: borra la madre vieja si queda vacia. HT_EEXIST si el destino ya existe. */
HT_API int ht_rename_dir(const char *from, const char *to, int rm_parent);

#ifndef HIBYTAG_NO_DB
#include "sqlite3.h"
#define HT_SUGLEN 128
/* abre la base del reproductor (NULL = $HIBYTAG_DB o /usr/data/usrlocal_media.db), con busy_timeout */
HT_API sqlite3 *ht_db_open(const char *dbpath);
HT_API void ht_db_close(sqlite3 *db);
/* convierte una ruta del sistema a la de la base: /usr/data/mnt/sd_0/A/b.flac -> a:\A\b.flac
 * (raiz de la SD = $HIBYTAG_SD). Devuelve 0 si la ruta no esta en la SD. */
HT_API int ht_db_path(const char *file, char *out, size_t outsz);
/* relee las etiquetas de los ficheros y actualiza sus filas (MEDIA_TABLE, MEDIA2_TABLE y las
 * demas tablas con el mismo esquema: favoritos, historial, listas) y las tablas de artistas,
 * albumes, generos y artistas de album. Una sola transaccion. rows (opcional) = filas tocadas.
 * Devuelve cuantos ficheros no tenian fila (o error HT_EDB). */
HT_API int ht_db_update(sqlite3 *db, const char *const *paths, int n, int *rows);
/* autocompletado: hasta max valores distintos de field (HT_ARTIST/ALBUM/ALBUMARTIST/GENRE)
 * que empiezan por prefix (sin distinguir mayusculas ni acentos), luego los que lo contienen.
 * Ordenados por numero de pistas. Devuelve cuantos. */
HT_API int ht_db_suggest(sqlite3 *db, int field, const char *prefix, char out[][HT_SUGLEN], int max);
/* rutas tras renombrar: from[i] -> to[i] (ficheros o carpetas; carpeta = todo lo que cuelga).
 * Cambia path, lrc_path y album_pic_path en todas las tablas con el esquema de pistas (MEDIA*,
 * favoritos, historial, listas...). Una sola transaccion: o todo o nada. rows = filas cambiadas. */
HT_API int ht_db_rename(sqlite3 *db, const char *const *from, const char *const *to, int n, int *rows);
#endif

#endif
