/* qr_test.c - prints the QR matrix that src/qr.inc.c produces, for tests/qr_test.sh.
 *   qr_test <mask: 0..7 or -1> <payload in hex>
 * Output: "<version> <mask> <n>" and n rows of 0/1. Exit 2 when the payload does not fit. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/qr.inc.c"

int main(int argc, char **argv)
{
    if (argc < 3) return 1;
    uint8_t buf[512]; int n = 0;
    for (const char *p = argv[2]; p[0] && p[1] && n < (int)sizeof buf; p += 2) { unsigned v; sscanf(p, "%2x", &v); buf[n++] = (uint8_t)v; }
    struct qr q;
    if (!qr_encode(buf, n, &q, atoi(argv[1]))) return 2;
    printf("%d %d %d\n", q.ver, q.mask, q.n);
    for (int y = 0; y < q.n; y++) { for (int x = 0; x < q.n; x++) putchar('0' + q.m[y][x]); putchar('\n'); }
    return 0;
}
