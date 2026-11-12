/* pwrpop.inc.c - the power button held down: an iOS-style "Powering Off" window with HiBy's
 * countdown, instead of HiBy's own grey "Shutting down / 3" box (notice_second.dlg, made
 * invisible by hiby_dlgs_quiet). That box was redrawn by the player once a second straight into
 * the buffer on screen, so over any ByOS screen it flashed through on every tick.
 *
 * The button is read from its own input device (md-gpio-keys, KEY_POWER), never grabbed: the
 * player still gets it and still powers off at the end of its count; releasing the button
 * cancels both. syspop.inc.c presents the window (sp_kind 3) over any screen.
 * API for syspop: pwr_popup_needed() / _ack() / _expired() / _draw(d, progress) / _hit(x, y, p). */
#define PWR_COUNT 3                 /* the player counts 3, 2, 1 from the moment it shows its box */
static int pwr_fd = -2, pwr_down;
static double pwr_t0;

static void pwr_poll(void)
{
    if (pwr_fd == -2) {             /* find "md-gpio-keys" once (event0 on the R3 II) */
        pwr_fd = -1;
        for (int i = 0; i < 8 && pwr_fd < 0; i++) {
            char p[32], name[64] = ""; snprintf(p, sizeof p, "/dev/input/event%d", i);
            int f = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC); if (f < 0) continue;
            if (ioctl(f, EVIOCGNAME(sizeof name), name) >= 0 && strstr(name, "gpio-keys")) pwr_fd = f; else close(f);
        }
    }
    if (pwr_fd < 0) return;
    struct input_event ev[16]; ssize_t n;
    while ((n = read(pwr_fd, ev, sizeof ev)) > 0)
        for (int i = 0; i < n / (ssize_t)sizeof ev[0]; i++)
            if (ev[i].type == EV_KEY && ev[i].code == KEY_POWER) {
                if (ev[i].value == 1 && !pwr_down) { pwr_down = 1; pwr_t0 = now_ms(); }
                else if (ev[i].value == 0) pwr_down = 0;
            }
}
/* a short press (screen off/on) never shows the window */
static int pwr_popup_needed(void) { pwr_poll(); return pwr_down && now_ms() - pwr_t0 > 350; }
static void pwr_popup_ack(void) {}
static int pwr_popup_expired(void) { pwr_poll(); return !pwr_down; }

#define PW_W 236
#define PW_H 176
static void pwr_popup_draw(uint16_t *d, float progress)
{
    pwr_poll();
    float p = progress < 0 ? 0 : progress > 1 ? 1 : progress;
    dim_buf(d, d, W * H, 32 - (int)(12 * p));                      /* the screen behind dims, as iOS */
    int x = (W - PW_W) / 2, y = (H - PW_H) / 2 + (int)((1 - p) * 18);
    int a = (int)(235 * p);
    fill_rrect(d, x, y, PW_W, PW_H, 16, rgb(38, 38, 40), a);
    if (p < 0.35f) return;
    uint16_t fg = blend565(rgb(38, 38, 40), 0xFFFF, (int)(32 * p));
    /* power symbol: a ring open at the top and a bar */
    float cx = W / 2.f, cy = y + 44.f;
    for (float k = 0; k < 6.2832f; k += 0.035f) {
        if (k > 4.15f && k < 5.27f) continue;                       /* the gap at the top */
        fill_circle(d, cx + cosf(k) * 15, cy + sinf(k) * 15, 1.6f, fg, 255);
    }
    fill_rrect(d, (int)cx - 2, (int)cy - 21, 4, 18, 2, fg, 255);
    const char *t = tr("Apagando");
    int tw = tw_n(t, 17, -1); draw_text(d, (W - tw) / 2, y + 92, t, 17, fg, PW_W - 24, 0, H);
    int left = PWR_COUNT - (int)((now_ms() - pwr_t0 - 350) / 1000); if (left < 1) left = 1;
    char num[8]; snprintf(num, sizeof num, "%d", left);
    int nw = tw_n(num, 34, -1); draw_text(d, (W - nw) / 2, y + 134, num, 34, fg, 0, 0, H);
    const char *s = tr("Suelta el botón para cancelar");
    int sw = tw_n(s, 12, -1); if (sw > PW_W - 24) sw = PW_W - 24;
    draw_text(d, (W - sw) / 2, y + 162, s, 12, blend565(rgb(38, 38, 40), rgb(170, 170, 176), (int)(32 * p)), PW_W - 24, 0, H);
}
/* the window takes every touch while it is up (the screen behind is dimmed) */
static int pwr_popup_hit(int x, int y, float progress) { (void)x; (void)y; (void)progress; return 1; }
