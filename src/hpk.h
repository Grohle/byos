/* hpk.h - paquetes de apps de hibymod (.hpk) : interfaz del instalador.
 *
 * Un .hpk es un tar POSIX ustar sin comprimir con manifest.ini + icon.png + ficheros de la app
 * (ver docs/FORMATO_APPS.md). La implementacion esta en hpk.inc.c (se incluye en hibyshell.c, o
 * en el arnes de pruebas de PC). Todo devuelve 0 si va bien o un codigo negativo -HPK_E_*;
 * los textos son CLAVES ("err.traversal") que se traducen con hpk_msg(clave, idioma).
 *
 * Rutas (todas cuelgan de una raiz que se puede cambiar para pruebas: hpk_set_root() o la
 * variable de entorno HPK_ROOT; por defecto /usr/data/hibymod):
 *   <raiz>/apps/<id>/              app instalada (su manifest.ini es el registro)
 *   <raiz>/apps/.tmp-<id>/         extraccion en curso (se borra si algo falla)
 *   <raiz>/apps/.old-<id>/         version anterior durante el cambio atomico
 *   <raiz>/apps/.del-<id>/         app en desinstalacion
 *   <raiz>/appdata/<id>/           datos escribibles de la app (sobreviven a actualizaciones) */
#ifndef HPK_H
#define HPK_H

#define HPK_ID_MAX 48
#define HPK_VER_MAX 24
#define HPK_NAME_MAX 48
#define HPK_DESC_MAX 160

/* mismo orden que LANG_* de i18n.inc.c; los manifiestos solo traen es/en/fr (name[0..2]) */
enum { HPK_LANG_ES, HPK_LANG_EN, HPK_LANG_FR, HPK_NLANG = 13 };

enum {
    HPK_OK = 0,
    HPK_E_OPEN = 1,       /* no se puede abrir el paquete */
    HPK_E_IO,             /* error de lectura/escritura */
    HPK_E_FORMAT,         /* no es un tar ustar valido / truncado */
    HPK_E_CHECKSUM,       /* suma de comprobacion de cabecera erronea */
    HPK_E_TRAVERSAL,      /* ruta absoluta o con .. */
    HPK_E_TYPE,           /* enlace simbolico/duro, dispositivo, fifo... */
    HPK_E_TOOBIG,         /* tamano declarado absurdo o demasiadas entradas */
    HPK_E_NOMANIFEST,     /* falta manifest.ini */
    HPK_E_MANIFEST,       /* campo obligatorio ausente o invalido */
    HPK_E_ID,             /* id invalido */
    HPK_E_VERSION,        /* version no semver */
    HPK_E_ARCH,           /* arch distinto de mipsel-hf */
    HPK_E_NEWER,          /* necesita un hibymod mas nuevo */
    HPK_E_NOEXEC,         /* el ejecutable no esta en el paquete */
    HPK_E_ELF,            /* el ejecutable no es ELF MIPS32 LE estatico de coma flotante hard */
    HPK_E_SPACE,          /* no hay espacio libre en /usr/data */
    HPK_E_ICON,           /* icon.png ilegible o demasiado grande */
    HPK_E_DUP,            /* entrada repetida */
    HPK_E_SWAP,           /* no se pudo activar la nueva version */
    HPK_E_NOTINST,        /* la app no esta instalada */
    HPK_E_NOMEM,
    HPK_E_COUNT
};

/* datos de un paquete (hpk_scan) o de una app instalada (hpk_list_installed) */
struct hpk_info {
    char id[HPK_ID_MAX + 1];
    char version[HPK_VER_MAX];
    char min_hibymod[HPK_VER_MAX];
    char name[3][HPK_NAME_MAX];        /* es, en, fr (si falta uno se copia el de "name") */
    char desc[3][HPK_DESC_MAX];
    char exec[128];
    char category[24];
    char author[HPK_NAME_MAX];
    char homepage[96];
    char input[8];                     /* touch | keys | both */
    int stop_player;                   /* 1: hay que parar la musica antes de abrirla */
    int refresh_full;                  /* refresh=full: la pantalla no baja al refresco de reposo */
    int ram_kb;
    long long size;                    /* bytes del .hpk (scan) o del contenido (instalada) */
    char path[256];                    /* ruta del .hpk o del directorio instalado */
    char installed[HPK_VER_MAX];       /* version instalada ("" si no lo esta) */
    long icon_off, icon_len;           /* icon.png dentro del .hpk (scan; -1 si no hay) */
    int status;                        /* 0 o -HPK_E_* si el paquete esta danado */
};

void hpk_set_root(const char *root);                 /* NULL: HPK_ROOT o /usr/data/hibymod */
const char *hpk_root(void);

int hpk_scan(const char *dir, struct hpk_info *out, int max);
int hpk_install(const char *path, void (*progress)(long done, long total, void *ctx), void *ctx,
                char *err, int errlen);
int hpk_uninstall(const char *id);                   /* conserva appdata/<id> */
#define HPK_PURGE_DATA 1
int hpk_uninstall_ex(const char *id, int flags);
int hpk_list_installed(struct hpk_info *out, int max);
void hpk_recover(void);                              /* termina/deshace cambios interrumpidos */

/* icono ya enmascarado (squircle + brillo + borde) a tamano s de un paquete sin instalar;
 * rgb[s*s] RGB565 y a[s*s] alfa. Las apps instaladas ya tienen apps/<id>/icon_<s>.ov */
int hpk_icon(const struct hpk_info *pkg, int s, unsigned short *rgb, unsigned char *a);

int hpk_vercmp(const char *a, const char *b);        /* <0, 0, >0 (semver) */
const char *hpk_errkey(int code);                    /* -HPK_E_x o HPK_E_x -> "err.x" */
const char *hpk_msg(const char *key, int lang);      /* clave -> texto (13 idiomas; si falta, ingles) */
extern char hpk_err_detail[160];                     /* entrada o campo que provoco el error */

#endif
