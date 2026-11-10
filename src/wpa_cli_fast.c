/* wpa_cli_fast: sustituto minimo de wpa_cli para las consultas que hiby_player lanza
 * cada 3 segundos ("wpa_cli -i wlan0 signal_poll", "status").
 * El wpa_cli original tarda ~23 ms por ejecucion (carga readline/ncurses del squashfs);
 * este binario estatico pregunta directamente al socket de control de wpa_supplicant.
 * Cualquier otra orden, o cualquier error, se delega en el wpa_cli original. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>

#define REAL "/usr/sbin/wpa_cli"
#define CTRL_DIR "/var/run/wpa_supplicant"

static void fallback(char **argv)
{
    argv[0] = (char *)REAL;
    execv(REAL, argv);
    perror(REAL);
    _exit(1);
}

int main(int argc, char **argv)
{
    const char *iface = "wlan0", *cmd = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-i") && i + 1 < argc) { iface = argv[++i]; continue; }
        if (!strncmp(argv[i], "-i", 2) && argv[i][2]) { iface = argv[i] + 2; continue; }
        if (argv[i][0] == '-') fallback(argv);          /* otras opciones: wpa_cli original */
        if (cmd) fallback(argv);                         /* ordenes con argumentos: original */
        cmd = argv[i];
    }
    const char *req = NULL;
    if (cmd && !strcasecmp(cmd, "signal_poll")) req = "SIGNAL_POLL";
    else if (cmd && !strcasecmp(cmd, "status")) req = "STATUS";
    if (!req) fallback(argv);

    int s = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (s < 0) fallback(argv);
    struct sockaddr_un local = { .sun_family = AF_UNIX }, dest = { .sun_family = AF_UNIX };
    snprintf(local.sun_path, sizeof local.sun_path, "/tmp/wpa_ctrl_%d-fast", (int)getpid());
    unlink(local.sun_path);
    snprintf(dest.sun_path, sizeof dest.sun_path, "%s/%s", CTRL_DIR, iface);
    struct timeval tv = { 1, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (bind(s, (struct sockaddr *)&local, sizeof local) ||
        connect(s, (struct sockaddr *)&dest, sizeof dest) ||
        send(s, req, strlen(req), 0) < 0) {
        close(s); unlink(local.sun_path); fallback(argv);
    }
    static char buf[4096];
    ssize_t n;
    /* wpa_supplicant puede enviar mensajes no solicitados (empiezan por '<'): se ignoran */
    do { n = recv(s, buf, sizeof buf - 1, 0); } while (n > 0 && buf[0] == '<');
    close(s); unlink(local.sun_path);
    if (n <= 0) fallback(argv);
    buf[n] = 0;
    if (!strncmp(buf, "FAIL", 4)) fallback(argv);
    fputs(buf, stdout);
    return 0;
}
