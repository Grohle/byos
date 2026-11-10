/* evdump - imprime eventos evdev crudos (para verificar el formato goodix/teclas). uso: evdump /dev/input/event1 [n] */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/input.h>
int main(int c, char **v) {
    const char *p = c > 1 ? v[1] : "/dev/input/event1"; int max = c > 2 ? atoi(v[2]) : -1;
    int fd = open(p, O_RDONLY); if (fd < 0) { perror(p); return 1; }
    char name[128] = "?"; ioctl(fd, EVIOCGNAME(sizeof name), name); printf("# %s: \"%s\"\n", p, name); fflush(stdout);
    struct input_event e;
    while (max-- != 0 && read(fd, &e, sizeof e) == sizeof e) {
        printf("%ld.%06ld type=%d code=%d value=%d\n", (long)e.input_event_sec, (long)e.input_event_usec, e.type, e.code, e.value); fflush(stdout);
    }
    return 0;
}
