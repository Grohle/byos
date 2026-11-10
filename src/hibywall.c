/* hibywall - convierte una imagen de la SD en fondo de pantalla (o miniatura) RGB565.
 *   hibywall ENTRADA SALIDA.raw [ANCHO ALTO [fit]]  (por defecto 320x480, recorte)
 * JPEG con libjpeg (IJG 9f) reduciendo al decodificar (1/2, 1/4, 1/8) para que una foto de
 * 12 MP no necesite mas de ~2-3 MB; PNG/BMP/GIF con stb_image (hasta 1,5 MP).
 * Escala "cover" con promedio por area, recorte centrado y tramado ordenado 4x4 a RGB565.
 * Codigos de salida: 0 bien, 2 no se pudo leer, 3 demasiado grande, 4 formato no soportado. */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jpeglib.h"
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_TGA
#define STBI_NO_JPEG
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

struct jerr { struct jpeg_error_mgr pub; jmp_buf jb; };
static void jerr_exit(j_common_ptr c) { longjmp(((struct jerr *)c->err)->jb, 1); }

static unsigned char *load_jpeg(FILE *f, int tw, int th, int *w, int *h)
{
    struct jpeg_decompress_struct c; struct jerr e;
    c.err = jpeg_std_error(&e.pub); e.pub.error_exit = jerr_exit;
    unsigned char *img = 0;
    if (setjmp(e.jb)) { jpeg_destroy_decompress(&c); free(img); return 0; }
    jpeg_create_decompress(&c);
    jpeg_stdio_src(&c, f);
    jpeg_read_header(&c, TRUE);
    int d = 8;   /* la mayor reduccion que aun cubre el destino */
    while (d > 1 && ((int)c.image_width / d < tw || (int)c.image_height / d < th)) d /= 2;
    c.scale_num = 1; c.scale_denom = d;
    c.out_color_space = JCS_RGB;
    c.dct_method = JDCT_IFAST;
    c.do_fancy_upsampling = FALSE;
    jpeg_start_decompress(&c);
    *w = c.output_width; *h = c.output_height;
    if ((long)*w * *h > 3000000L) { jpeg_destroy_decompress(&c); return 0; }
    img = malloc((size_t)*w * *h * 3);
    if (!img) { jpeg_destroy_decompress(&c); return 0; }
    while (c.output_scanline < c.output_height) {
        JSAMPROW row = img + (size_t)c.output_scanline * *w * 3;
        jpeg_read_scanlines(&c, &row, 1);
    }
    jpeg_finish_decompress(&c);
    jpeg_destroy_decompress(&c);
    return img;
}

#include <sys/resource.h>
int main(int argc, char **argv)
{
    /* tope de memoria: un JPEG progresivo enorme necesita todos sus coeficientes en RAM y
     * agotaba la del aparato (el OOM mato al reproductor). Con el tope, malloc falla y la
     * decodificacion termina con error limpio. HIBYWALL_MAX_MB cambia el tope (por defecto 12). */
    { const char *e = getenv("HIBYWALL_MAX_MB"); long mb = e ? atol(e) : 12; if (mb < 4) mb = 4;
      struct rlimit rl = { (rlim_t)mb << 20, (rlim_t)mb << 20 }; setrlimit(RLIMIT_AS, &rl); }
    if (argc < 3) { fprintf(stderr, "uso: hibywall ENTRADA SALIDA.raw [ANCHO ALTO]\n"); return 1; }
    int tw = argc >= 5 ? atoi(argv[3]) : 320, th = argc >= 5 ? atoi(argv[4]) : 480;
    if (tw < 8 || th < 8 || tw > 640 || th > 960) return 1;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    unsigned char m[4] = {0};
    fread(m, 1, 4, f); rewind(f);
    int w = 0, h = 0, n;
    unsigned char *img;
    if (m[0] == 0xFF && m[1] == 0xD8) img = load_jpeg(f, tw, th, &w, &h);
    else {
        if (!stbi_info_from_file(f, &w, &h, &n)) { fclose(f); return 4; }
        if ((long)w * h > 1500000L) { fclose(f); return 3; }
        img = stbi_load_from_file(f, &w, &h, &n, 3);
    }
    fclose(f);
    if (!img) return 2;
    /* cover: escala s = max(tw/w, th/h); "fit" (6o argumento): s = min, con bandas negras */
    int fit = argc >= 6 && !strcmp(argv[5], "fit");
    double sa = (double)tw / w, sb = (double)th / h;
    double s = fit ? (sa < sb ? sa : sb) : (sa > sb ? sa : sb);
    double ox = (w - tw / s) / 2, oy = (h - th / s) / 2, step = 1 / s;
    static const int bayer[4][4] = { {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5} };
    uint16_t *out = malloc((size_t)tw * th * 2);
    if (!out) return 2;
    for (int y = 0; y < th; y++) {
        double sy0 = oy + y * step, sy1 = sy0 + step;
        int y0 = (int)sy0, y1 = (int)(sy1 + 0.999); if (y1 > h) y1 = h; if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < tw; x++) {
            double sx0 = ox + x * step, sx1 = sx0 + step;
            if (sx0 < 0 || sy0 < 0 || sx0 >= w || sy0 >= h) { out[y * tw + x] = 0; continue; }
            int x0 = (int)sx0, x1 = (int)(sx1 + 0.999); if (x1 > w) x1 = w; if (x1 <= x0) x1 = x0 + 1;
            unsigned r = 0, g = 0, b = 0, cnt = 0;
            for (int yy = y0; yy < y1 && yy < h; yy++)
                for (int xx = x0; xx < x1 && xx < w; xx++) {
                    const unsigned char *p = img + ((size_t)yy * w + xx) * 3;
                    r += p[0]; g += p[1]; b += p[2]; cnt++;
                }
            if (!cnt) cnt = 1;
            int t = bayer[y & 3][x & 3];
            int R = (int)(r / cnt) + (t - 8) / 2, G = (int)(g / cnt) + (t - 8) / 4, B = (int)(b / cnt) + (t - 8) / 2;
            if (R < 0) R = 0; if (R > 255) R = 255; if (G < 0) G = 0; if (G > 255) G = 255; if (B < 0) B = 0; if (B > 255) B = 255;
            out[y * tw + x] = (uint16_t)((R >> 3) << 11 | (G >> 2) << 5 | (B >> 3));
        }
    }
    free(img);
    FILE *o = fopen(argv[2], "wb");
    if (!o) return 2;
    fwrite(out, 2, (size_t)tw * th, o);
    fclose(o);
    printf("%dx%d -> %dx%d\n", w, h, tw, th);
    return 0;
}
