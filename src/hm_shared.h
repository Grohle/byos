/* hm_shared.h: memoria compartida entre hibymod_disp.so (dentro de hiby_player) y
 * hibyshell (proceso del sistema de gestos, animaciones y launcher). */
#ifndef HM_SHARED_H
#define HM_SHARED_H
#include <stdint.h>

#define HM_MAGIC      0x34464d48u           /* "HMF4" */
#define HM_STATE      "/dev/shm/hibymod_fbstate"
#define HM_STATE_SIZE 65536
#define HM_FIFO       "/tmp/hibymod_disp"
#define HM_SOCK       "/tmp/hibymod_input.sock"
#define HM_FRAMES     "/dev/shm/hibymod_frames"  /* [0] imagen del shell, [1] ultimo fotograma del reproductor */
#define HM_W 320
#define HM_H 480
#define HM_FRAME_BYTES (HM_W * HM_H * 2)
#define HM_OV_MAX (100 * 100)
#define HM_VLOG 16

/* Ordenes por la FIFO (un byte):
 *  'C' captura: el panel queda en frames[0]; cada fotograma del reproductor va a frames[1]
 *  'P' presentar frames[0] (solo en captura)
 *  'H' retener: el hilo que presenta del reproductor espera
 *  'R' devolver (si venia de captura, se muestra frames[1])
 *  'O' redibujar la capa superpuesta sobre lo visible
 *  'L' (v6) reproducir la lista pedida en /tmp/hibymod_playreq; respuesta en /tmp/hibymod_playres
 *      (la atiende un hilo aparte; ver playctl.inc.c pc_play_list y docs/research_play_list.md)
 *  'S' (v7) leer/cambiar un ajuste del reproductor pedido en /tmp/hibymod_setreq; respuesta en
 *      /tmp/hibymod_setres (hilo aparte; ver hibyset.inc.c y docs/research_settings_api.md)
 */
struct hm_state {
    volatile uint32_t magic;
    volatile uint32_t held, waiting, yoffset, frames;
    volatile uint32_t input_mode;          /* 0 sin puente, 1 paso directo, 2 shell */
    volatile uint32_t capture, cap_frames, presents;
    volatile uint32_t vlog_seq;            /* vistas abiertas: vlog[seq % HM_VLOG] */
    char vlog[HM_VLOG][40];
    /* capa superpuesta (boton de inicio, flecha, aviso de volumen) */
    volatile uint32_t ov_on, ov_x, ov_y, ov_w, ov_h;
    volatile uint32_t ov_saved, ov_ux, ov_uy, ov_uw, ov_uh;
    uint16_t ov_rgb[HM_OV_MAX];
    uint8_t ov_a[HM_OV_MAX];
    uint16_t ov_under[HM_OV_MAX];
    /* vigilancia: el shell incrementa shell_beat al menos cada 300 ms; si se para 4 s,
     * la libreria lo mata (el kernel suelta su EVIOCGRAB) y vuelve el paso directo */
    volatile uint32_t shell_beat, shell_pid;
    /* cerrojos de secuencia (impar = escribiendo): el shell escribe frames[0] y la capa;
     * la libreria nunca copia ninguno de los dos a medias */
    volatile uint32_t fr0_seq, ov_seq;
    /* filas que cambian en la proxima 'P' (y0 < y1); 0,0 = pantalla completa */
    volatile uint32_t dirty_y0, dirty_y1;
    /* sustitucion del inicio de fabrica: si un fotograma del reproductor es su inicio
     * (firma de puntos), la libreria pone la imagen del launcher (HM_HOME) antes de
     * presentarlo y cuenta home_hits; el shell entra en su launcher sin parpadeo */
    volatile uint32_t home_sub_on, home_hits, nsig;
    volatile int32_t sig[8][6];          /* x, y, r, g, b, tolerancia */

    /* ---- v5: lo que suena (lo rellena la libreria; leer con np_get de nowplaying.inc.c).
     * Campos anadidos AL FINAL: un shell viejo no los ve y una libreria vieja deja np_magic = 0.
     * Ficheros de audio abiertos por el reproductor (cualquier hilo): ranura por apertura;
     * al reutilizar se elige la mas antigua ya cerrada, asi el fichero del decodificador
     * (abierto durante toda la cancion) no lo expulsan las lecturas rapidas de etiquetas. */
    volatile uint32_t np_magic;          /* HM_NP_MAGIC si la libreria rellena lo de abajo */
    volatile uint32_t np_seq;            /* cerrojo de secuencia de np_open[] (impar = escribiendo) */
    volatile uint32_t np_gen;            /* contador de aperturas (gen de la ultima) */
    struct hm_np_open {
        char path[400];                  /* ruta tal cual la abrio ("/data/mnt/sd_0/...") */
        int32_t fd;                      /* -1 = ya cerrado */
        uint32_t gen, tid;               /* gen 0 = ranura vacia */
        uint32_t open_ms, close_ms;      /* CLOCK_MONOTONIC en ms */
        uint32_t frames_at_open;         /* pcm_frames al abrir */
        uint32_t nseeks, seek_ms, seek_frames;
        uint32_t seek_lo, seek_hi;       /* desplazamiento de la ultima lseek */
    } np_open[8];
    /* salida PCM (tinyalsa por ioctl en hw:0 o libasound: BT/DAC USB) */
    volatile uint32_t pcm_open;          /* 1 con el PCM abierto (tambien en pausa) */
    volatile uint32_t pcm_frames;        /* tramas escritas en total (da la vuelta) */
    volatile uint32_t pcm_last_ms;       /* ultima escritura: si hace < ~400 ms, esta sonando */
    volatile uint32_t pcm_rate, pcm_bits, pcm_channels;
    volatile uint32_t pcm_drops, pcm_drop_ms;      /* vaciados (saltos, cambios de pista) */
    volatile uint32_t pcm_prepares, pcm_paused;
    /* hilo que escribe el PCM subido a SCHED_FIFO (0 = aun no / desactivado con
     * /usr/data/hibymod/no_rt); rt_err = errno si fallo */
    volatile uint32_t rt_tid, rt_prio, rt_err, rt_count;
};
#define HM_NP_MAGIC 0x3550504eu   /* "NPP5" */
#define HM_NP_SLOTS 8
#define HM_HOME "/dev/shm/hibymod_home"   /* imagen actual del launcher (RGB565 320x480) */

#endif
