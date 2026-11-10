/* memhog MB - reserva y toca MB megabytes y se queda dormido (simula al reproductor de serie, ~15 MB). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
int main(int c, char **v) {
    long mb = c > 1 ? atol(v[1]) : 15;
    size_t n = (size_t)mb << 20; char *p = malloc(n);
    if (!p) { perror("malloc"); return 1; }
    for (size_t i = 0; i < n; i += 4096) p[i] = (char)i;
    mlock(p, n);
    fprintf(stderr, "memhog: %ld MB residentes (pid %d)\n", mb, getpid());
    for (;;) pause();
}
