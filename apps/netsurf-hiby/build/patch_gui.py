# patch_gui.py GUI_C - desplazamiento exacto en pixeles para la surface "hiby" (MOUSE_5 con
# value.vector.y = dy y value.vector.z = dx, ver nsfb_hiby.c). Idempotente.
import re, sys
p = sys.argv[1]; s = open(p, encoding='utf8').read()
if 'HIBY_SCROLL' not in s:
    pat = re.compile(r'(\t\tcase NSFB_KEY_MOUSE_5:\n\t\t\t/\* scroll down \*/\n)')
    add = ('\t\t\t/* HIBY_SCROLL: desplazamiento exacto (arrastre 1:1 e inercia) */\n'
           '\t\t\tif (cbi->event->value.vector.y || cbi->event->value.vector.z) {\n'
           '\t\t\t\tint hdy = cbi->event->value.vector.y, hdx = cbi->event->value.vector.z;\n'
           '\t\t\t\tif (browser_window_scroll_at_point(gw->bw, x, y, hdx, hdy) == false) {\n'
           '\t\t\t\t\tif (hdy) widget_scroll_y(gw, hdy, false);\n'
           '\t\t\t\t\tif (hdx) widget_scroll_x(gw, hdx, false);\n'
           '\t\t\t\t}\n'
           '\t\t\t\tbreak;\n'
           '\t\t\t}\n')
    s, n = pat.subn(lambda m: m.group(1) + add, s, count=1)
    assert n == 1, 'no se encontro MOUSE_5 en gui.c'
    open(p, 'w', encoding='utf8', newline='\n').write(s)
    print('gui.c parcheado')
