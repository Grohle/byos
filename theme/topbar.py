#!/usr/bin/env python3
"""Barra de estado de ByOS: glifos para litegui/themeN/topbar y topbar.view.

La barra de serie son solo imagenes PNG (cifras e iconos en celdas de 20 px de alto) que
coloca layout/themeN/topbar/topbar.view; el reproductor de HiBy solo sabe dibujar PNG, asi
que no se puede usar SVG en el aparato. Aqui cada icono se DIBUJA COMO VECTOR (coordenadas
en px con decimales, trazos y curvas) a 8x y se reduce con un filtro de area: el resultado es
el de un SVG rasterizado a mano, pero con las lineas encajadas en la rejilla de pixeles (los
trazos de 1,5-2 px caen en medios pixeles), que es lo que da nitidez a 12 px de alto.

Diseno (iOS 17, docs/UI_GUIDE_FOR_AGENTS.md):
- cifras: Inter SemiBold 15 px (libre, OFL; theme/fonts), en celdas de 9 px (tabulares,
  centradas) con 1 px minimo de separacion (lo necesita el OCR del volumen del shell);
- todo centrado en la misma linea: y = 15 de la barra (celda de 20 px colocada en y = 5);
- iconos de 11-12 px de alto con el mismo grosor de trazo (1,6 px) y el mismo peso visual
  que las cifras; huecos: 6 px entre grupos, 4 px dentro de un grupo; margen lateral 14 px;
- pila horizontal como la de iOS (cuerpo 23 x 11,5, esquinas 3,5, borne 1,5 x 4);
- sin Wi-Fi/Bluetooth: sin icono (iOS).

Tambien genera src/digits.h (plantillas del OCR de volumen) a partir de las cifras.
Uso: python topbar.py [--deploy]
"""
import os, subprocess, sys
from PIL import Image, ImageDraw, ImageFont
import viewlib as V

HERE = os.path.dirname(os.path.abspath(__file__))
OVL = os.path.join(HERE, "..", "overlay", "usr", "resource")
STOCK = r"//wsl.localhost/Ubuntu/home/alber/hiby/rootfs/usr/resource"
INTER = os.path.join(HERE, "fonts", "Inter-SemiBold.ttf")
INTER_B = os.path.join(HERE, "fonts", "Inter-Bold.ttf")
ADB = os.path.expandvars(r"%LOCALAPPDATA%\platform-tools\adb.exe")
DEV = os.environ.get("HIBY_DEV", "192.168.1.147:5555")
S = 8                                     # supermuestreo
WHITE, RED, GREEN, GREY = (255, 255, 255), (255, 69, 58), (52, 199, 89), (142, 142, 147)
CY = 10.0                                 # centro vertical de la celda (y = 15 en la barra)
SW = 1.6                                  # grosor de trazo comun
BATTERY_H = True                          # pila horizontal (iOS); False = vertical como la de serie
files = {}


def canvas(w):
    im = Image.new("RGBA", (w * S, 20 * S), (0, 0, 0, 0))
    return im, ImageDraw.Draw(im)


def done(name, im):
    files[name] = im.resize((im.width // S, im.height // S), Image.BOX)


def P(x, y):
    return (x * S, y * S)


def font(px, bold=False):
    return ImageFont.truetype(INTER_B if bold else INTER, int(round(px * S)))


def stroke_line(d, pts, col=WHITE + (255,), w=SW):
    """polilinea con extremos redondeados (como stroke-linecap: round)"""
    d.line([P(*p) for p in pts], fill=col, width=int(round(w * S)), joint="curve")
    r = w / 2
    for x, y in (pts[0], pts[-1]):
        d.ellipse([P(x - r, y - r), P(x + r, y + r)], fill=col)


# ---- cifras: celda 9 px, glifo centrado (tabulares) -----------------------------------
F = font(14.4)
DW = 9


def glyph(ch, w, col, f=F, dy=0.0):
    im, d = canvas(w)
    d.text((w * S / 2, (CY + dy) * S), ch, font=f, fill=col + (255,), anchor="mm")
    return im


for n in "0123456789":
    done(n, glyph(n, DW, WHITE)); done(n + "r", glyph(n, DW, RED))
done("colon", glyph(":", 5, WHITE, dy=-0.9))
done("dot", glyph(".", 5, WHITE)); done("dotr", glyph(".", 5, RED))
done("-", glyph("-", 7, WHITE)); done("slash", glyph("/", 7, WHITE))
done("percent", glyph("%", 11, WHITE, f=font(12.6), dy=0.1))
done("L", glyph("L", 9, WHITE, f=font(12))); done("R", glyph("R", 9, WHITE, f=font(12)))
done("am", glyph("AM", 18, WHITE, f=font(10))); done("pm", glyph("PM", 18, WHITE, f=font(10)))


# ---- iconos ------------------------------------------------------------------------
def speaker():
    """altavoz: cuerpo macizo + dos ondas (14 x 12)"""
    im, d = canvas(16)
    d.polygon([P(1.5, 7.6), P(4.3, 7.6), P(7.6, 4.6), P(7.6, 15.4), P(4.3, 12.4), P(1.5, 12.4)], fill=WHITE)
    d.rounded_rectangle([P(1.0, 7.4), P(4.6, 12.6)], 0.9 * S, fill=WHITE)
    d.arc([P(6.6, 6.8), P(12.0, 13.2)], -52, 52, fill=WHITE, width=int(1.8 * S))
    d.arc([P(6.4, 4.2), P(15.2, 15.8)], -50, 50, fill=WHITE, width=int(1.8 * S))
    return im


def headphones():
    """auriculares: diadema y dos copas (14 x 12)"""
    im, d = canvas(16)
    d.arc([P(1.9, 4.2), P(14.1, 17.4)], 182, 358, fill=WHITE, width=int(SW * S))
    d.rounded_rectangle([P(1.2, 10.4), P(4.8, 15.8)], 1.4 * S, fill=WHITE)
    d.rounded_rectangle([P(11.2, 10.4), P(14.8, 15.8)], 1.4 * S, fill=WHITE)
    return im


def pill(t, w=None, sz=7.6, maxw=None, minsz=5.6, pad=7):
    """capsula con texto en negrita (salida: LO, 4.4, OPT, BT, ADB; codecs Bluetooth). Con maxw,
    la letra se reduce hasta que quepa; pad = margen total (borde incluido)."""
    f = font(sz, bold=True)
    tw = f.getlength(t) / S
    while maxw and tw + pad > maxw and sz > minsz:
        sz -= 0.2; f = font(sz, bold=True); tw = f.getlength(t) / S
    w = w or int(round(tw + pad))
    im, d = canvas(w)
    d.rounded_rectangle([P(0.8, 5.0), P(w - 0.8, 15.0)], 3.0 * S, outline=WHITE, width=int(1.3 * S))
    d.text((w * S / 2, (CY + 0.1) * S), t, font=f, fill=WHITE, anchor="mm")
    return im


def usb():
    """tridente USB (10 x 12)"""
    im, d = canvas(12)
    stroke_line(d, [(6, 5.2), (6, 14.6)])
    d.polygon([P(6, 3.6), P(8.1, 6.4), P(3.9, 6.4)], fill=WHITE)
    stroke_line(d, [(6, 11.6), (2.8, 9.6), (2.8, 8.2)])
    stroke_line(d, [(6, 12.8), (9.2, 10.8), (9.2, 9.4)])
    d.ellipse([P(4.4, 13.6), P(7.6, 16.6)], fill=WHITE)
    return im


def play():
    im, d = canvas(11)
    d.polygon([P(2.6, 5.2), P(9.6, 10), P(2.6, 14.8)], fill=WHITE)
    d.line([P(2.6, 5.2), P(9.6, 10), P(2.6, 14.8), P(2.6, 5.2)], fill=WHITE, width=int(0.9 * S), joint="curve")
    return im


def pause():
    im, d = canvas(11)
    d.rounded_rectangle([P(2.2, 5.0), P(4.6, 15.0)], 0.8 * S, fill=WHITE)
    d.rounded_rectangle([P(6.4, 5.0), P(8.8, 15.0)], 0.8 * S, fill=WHITE)
    return im


def wifi(level):
    """abanico de iOS (15 x 11): sector macizo y dos arcos, los apagados al 35 %"""
    im, d = canvas(17)
    cx, cy = 8.5, 15.4
    for i, r in enumerate((3.6, 7.6, 11.6)):
        col = WHITE + (255,) if i < level else (255, 255, 255, 90)
        if i == 0:
            d.pieslice([P(cx - r, cy - r), P(cx + r, cy + r)], 225, 315, fill=col)
        else:
            w = 2.1
            d.arc([P(cx - r + w / 2, cy - r + w / 2), P(cx + r - w / 2, cy + r - w / 2)], 225, 315, fill=col, width=int(w * S))
    return im


def bluetooth():
    """runa de Bluetooth (7 x 12)"""
    im, d = canvas(10)
    stroke_line(d, [(2.2, 7.4), (7.8, 12.4), (5.0, 15.2), (5.0, 4.8), (7.8, 7.6), (2.2, 12.6)], w=1.5)
    return im


def link():
    """HiBy Link: dos eslabones (13 x 7)"""
    im, d = canvas(15)
    d.rounded_rectangle([P(1.2, 7.2), P(9.0, 12.8)], 2.8 * S, outline=WHITE, width=int(1.5 * S))
    d.rounded_rectangle([P(6.0, 7.2), P(13.8, 12.8)], 2.8 * S, outline=WHITE, width=int(1.5 * S))
    return im


def lock():
    """candado (bloqueo de carga) 9 x 12"""
    im, d = canvas(11)
    d.arc([P(2.6, 4.0), P(8.4, 10.6)], 180, 360, fill=WHITE, width=int(SW * S))
    stroke_line(d, [(3.4, 7.3), (3.4, 9.2)]); stroke_line(d, [(7.6, 7.3), (7.6, 9.2)])
    d.rounded_rectangle([P(1.4, 9.0), P(9.6, 16.0)], 1.6 * S, fill=WHITE)
    return im


blank = lambda w: canvas(w)[0]

# ---- pila ---------------------------------------------------------------------------
if BATTERY_H:
    BW = 27

    def battery_bg(col, a_body=255):
        im, d = canvas(BW)
        body = col + (a_body,)
        d.rounded_rectangle([P(0.8, 4.3), P(23.8, 15.7)], 3.6 * S, outline=body, width=int(1.1 * S))
        d.rounded_rectangle([P(24.6, 8.0), P(26.2, 12.0)], 0.8 * S, fill=col + (150,))      # borne
        return im

    def battery_fill():
        im, d = canvas(BW)
        d.rounded_rectangle([P(2.6, 6.1), P(22.0, 13.9)], 2.0 * S, fill=WHITE)
        return im

    def battery_charge_bg():
        im = battery_bg(GREEN)
        d = ImageDraw.Draw(im)                                       # rayo, fuera del relleno
        d.polygon([P(13.4, 3.0), P(9.6, 10.6), P(12.4, 10.6), P(11.4, 17.0), P(15.4, 9.2), P(12.6, 9.2)],
                  fill=(255, 255, 255, 255), outline=(0, 0, 0, 120))
        return im
else:
    BW = 14

    def battery_bg(col, a_body=235):
        im, d = canvas(14)
        d.rounded_rectangle([P(5, 2.4), P(9, 4.2)], 0.6 * S, fill=col + (a_body,))
        d.rounded_rectangle([P(1.6, 3.6), P(12.4, 17.6)], 2.8 * S, outline=col + (a_body,), width=int(1.3 * S))
        return im

    def battery_fill():
        im, d = canvas(14)
        d.rounded_rectangle([P(3.4, 5.4), P(10.6, 15.8)], 1.4 * S, fill=WHITE)
        return im

    def battery_charge_bg():
        return battery_bg(GREEN, 255)

done("speaker", speaker()); done("po", headphones())
# 19 px (no cabe mas antes del Bluetooth): margen interior 5 en vez de 7, asi "OPT" y "ADB" no
# se encogen hasta que la O toca el borde (se leia "DPT")
done("lo", pill("LO", maxw=18)); done("balance", pill("4.4", maxw=18)); done("spdif", pill("OPT", maxw=18, pad=5))
done("usb", usb()); done("a2dp", pill("BT", maxw=18)); done("adb", pill("ADB", sz=7.0, maxw=18, pad=5))
# codec del Bluetooth (los de serie median 32 px y pisaban al altavoz)
for n, t in (("sbc", "SBC"), ("aptx", "aptX"), ("aptx_hd", "aptX HD"), ("ldac", "LDAC"), ("uat", "UAT"), ("aac", "AAC")):
    done(n, pill(t, maxw=27, minsz=5.0))
# El reproductor NO usa la x de topbar_iv_output: la pone justo detras de las cifras del volumen
# (x de las cifras + ancho real + ~1 px; visto en el aparato). El hueco va dentro del icono.
OUT_PAD = 4
# el reproductor recorta este icono a 19 px de ancho: las capsulas (18 px) llevan solo 1 px de hueco
# (con 4 se cortaba su borde derecho con el volumen de 3 cifras)
PILLS = ("lo", "balance", "spdif", "a2dp", "adb")
for o in ("po", "lo", "balance", "spdif", "usb", "a2dp", "adb"):
    p_ = 1 if o in PILLS else OUT_PAD
    im = files[o]; pad = Image.new("RGBA", (im.width + p_, im.height), (0, 0, 0, 0)); pad.alpha_composite(im, (p_, 0)); files[o] = pad
done("am", blank(1)); done("pm", blank(1))              # como iOS: sin AM/PM en la barra
done("play", play()); done("pause", pause())
for i in range(4): done(f"wifi_connect_{i}", wifi(i if i else 1))
done("wifi_unconnect", blank(17))                       # como iOS: sin Wi-Fi no hay icono
done("bluetooth", bluetooth()); done("bluetooth_unconnect", blank(10))
done("hl_connect", link()); done("hl_disconnect", blank(15))
done("chage_lock", lock())
done("battery_bg", battery_bg(WHITE, 140)); done("battery_charge_bg", battery_charge_bg())
done("battery_low_bg", battery_bg(RED)); done("battery", battery_fill())

# ---- disposicion (topbar.view) -------------------------------------------------------
# izquierda: hora + estado de reproduccion; centro: altavoz + volumen + salida;
# derecha (alineada al margen de 14 px): HiBy Link, Bluetooth, Wi-Fi, "100%" y la pila.
def wd(n): return files[n].width
M = 14
X_BATT = 320 - M - wd("battery_bg")
X_PCT = X_BATT - 3 - (3 * DW + wd("percent"))           # "100%" pegado a la pila
X_NET = X_PCT - 6 - wd("wifi_connect_3")
X_BT = X_NET - 5 - wd("bluetooth")
X_PLAY = M + 4 * DW + wd("colon") + 5                    # estado de reproduccion tras la hora
X_HL = X_PLAY + wd("play") + 4                            # HiBy Link a la izquierda (indicador secundario)
CODECS = ("sbc", "aptx", "aptx_hd", "ldac", "uat", "aac")
OUTS = ("po", "lo", "balance", "spdif", "usb", "a2dp", "adb")
X_CODEC = X_HL + wd("hl_connect") + 4                     # codec del Bluetooth (si se muestra)
X_VOLN = 145                                              # volumen: donde lo lee el OCR del shell (3 cifras)
X_SPK = X_VOLN - 3 - wd("speaker")
X_OUT = X_VOLN + 3 * DW + 1                               # salida tras el volumen de 3 cifras
POS = {
    "topbar_nv_clock": M, "topbar_iv_play_status": X_PLAY, "topbar_iv_hl": X_HL, "topbar_iv_bt_sink_type": X_CODEC,
    "topbar_iv_speaker": X_SPK, "topbar_nv_speaker": X_VOLN, "topbar_iv_output": X_OUT,
    "topbar_iv_bt": X_BT, "topbar_iv_net": X_NET,
    "topbar_nv_battery": X_PCT, "topbar_iv_battery": X_BATT,
    "topbar_iv_clock": X_PLAY,                            # AM/PM: imagen vacia de 1 px
}
# ningun simbolo pisa a otro, en NINGUNA combinacion: cada hueco con el ancho maximo de todo
# lo que puede mostrar (cifras incluidas) y al menos 3 px de aire con el siguiente
GAP = 3
SLOTS = [
    ("hora", POS["topbar_nv_clock"], 4 * DW + wd("colon")),
    ("reproduccion", X_PLAY, max(wd("play"), wd("pause"))),
    ("HiBy Link", X_HL, wd("hl_connect")),
    ("codec BT", X_CODEC, max(wd(c) for c in CODECS)),
    ("altavoz", X_SPK, wd("speaker")),
    ("volumen", X_VOLN, 3 * DW),
    ("salida", X_OUT + OUT_PAD, max(wd(o) for o in OUTS) - OUT_PAD),   # lo visible; el margen va en el PNG
    ("Bluetooth", X_BT, wd("bluetooth")),
    ("Wi-Fi", X_NET, max(wd(f"wifi_connect_{i}") for i in range(4))),
    ("bateria %", X_PCT, 3 * DW + wd("percent")),
    ("pila", X_BATT, max(wd("battery_bg"), wd("battery_charge_bg"), wd("battery_low_bg"), wd("battery"))),
]
for (a, xa, wa), (b, xb, wb) in zip(SLOTS, SLOTS[1:]):
    assert xa + wa + GAP <= xb, f"{a} ({xa}+{wa}) pisa a {b} ({xb})"
assert SLOTS[-1][1] + SLOTS[-1][2] <= 320 - M + 1, "la pila se sale del margen"
for name, x, w in SLOTS: print(f"  {name:13s} x={x:3d} w={w:2d} -> {x + w}")

# Porcentaje de bateria: HiBy lo dibuja pegado a la IZQUIERDA de su caja y coloca el Wi-Fi a la
# izquierda de donde estaria el texto alineado a la derecha (x = pila - 2 - ancho - wifi). La
# libreria lo empujaba a la derecha moviendo columnas y arrastraba medio Wi-Fi (icono roto y mas
# ancho, "%" cortado). Ahora HiBy lo dibuja con cifras TRANSPARENTES de los mismos anchos (su
# colocacion no cambia) y la libreria pinta el texto a la derecha con los glifos de
# src/batt_glyphs.h, sin mover nada.
for k in [*"0123456789", *[c + "r" for c in "0123456789"], "percent", "dot", "dotr", "-"]:
    files["batt/" + k] = Image.new("RGBA", files[k].size, (0, 0, 0, 0))

out = []
for th in ("theme2", "theme3"):
    for n, im in files.items():
        rel = f"litegui/{th}/topbar/{n}.png"
        p = os.path.join(OVL, rel); os.makedirs(os.path.dirname(p), exist_ok=True); im.save(p, optimize=True); out.append((rel, p))
    root = V.load(os.path.join(STOCK, "layout", th, "topbar", "topbar.view"))
    for _k, el in V.walk(root):
        n = V.get(el, "name")
        if n in POS: V.set_(el, "x", POS[n]); V.set_(el, "y", 5)
        if n == "topbar_nv_battery": V.set_(el, "w", 3 * DW + wd("percent")); V.set_(el, "img_path", "topbar\\batt\\")
        if n == "topbar_nv_speaker": V.set_(el, "w", 3 * DW)
        if n == "topbar_nv_clock": V.set_(el, "w", 4 * DW + wd("colon"))
    # HiBy's layout with our changes: never in overlay/ or git (tools/vendor_edits.json rebuilds
    # it from the user's firmware). Written to theme/work/ for --deploy and vendor_edits update.
    rel = f"layout/{th}/topbar/topbar.view"; p = os.path.join(HERE, "work", "topbar", "usr", "resource", rel)
    os.makedirs(os.path.dirname(p), exist_ok=True); V.save(p, root); out.append((rel, p))


# ---- vista previa: tres fondos, a 4x ---------------------------------------------------
def bar(bg, clock="09:41", vol="25", pct="100", play_ic="pause", out_ic="po", net=3, bt=True, hl=False, batt="battery_bg", level=1.0, codec=None):
    im = Image.new("RGBA", (320, 30), bg)
    def put(n, x): im.alpha_composite(files[n], (x, 5))
    x = POS["topbar_nv_clock"]
    for c in clock: n = "colon" if c == ":" else c; put(n, x); x += wd(n)
    put(play_ic, POS["topbar_iv_play_status"])
    put("speaker", POS["topbar_iv_speaker"]); x = POS["topbar_nv_speaker"]
    for c in vol: put(c, x); x += wd(c)
    put(out_ic, POS["topbar_iv_output"])
    if hl: put("hl_connect", POS["topbar_iv_hl"])
    if codec: put(codec, POS["topbar_iv_bt_sink_type"])
    if bt: put("bluetooth", POS["topbar_iv_bt"])
    put(f"wifi_connect_{net}", POS["topbar_iv_net"])
    x = POS["topbar_nv_battery"] + (3 - len(pct)) * DW    # como lo deja la libreria (sb_fix): "%" fijo
    for c in pct: put(c, x); x += wd(c)
    put("percent", x)
    put(batt, POS["topbar_iv_battery"])
    f = files["battery"]; cw = int(round(f.width * level)) if BATTERY_H else f.width
    im.alpha_composite(f.crop((0, 0, max(cw, 1), 20)), (POS["topbar_iv_battery"], 5))
    return im.convert("RGB")


rows = [bar((0, 0, 0, 255)),
        bar((178, 96, 66, 255), clock="01:15", vol="7", pct="68", play_ic="play", out_ic="lo", level=0.68, hl=True),
        bar((40, 44, 70, 255), clock="23:58", vol="100", pct="5", out_ic="usb", net=1, bt=False, batt="battery_low_bg", level=0.05),
        bar((30, 30, 30, 255), clock="12:00", vol="60", pct="82", out_ic="balance", batt="battery_charge_bg", level=0.82),
        # el peor caso: todo a la vez y lo mas ancho de cada hueco
        bar((0, 0, 0, 255), clock="23:58", vol="100", pct="100", play_ic="play", out_ic="spdif", hl=True, codec="aptx_hd", batt="battery_charge_bg"),
        bar((70, 70, 74, 255), clock="10:08", vol="88", pct="100", out_ic="adb", hl=True, codec="ldac", net=2)]
prev = Image.new("RGB", (320, 30 * len(rows) + 3 * (len(rows) - 1)), (255, 0, 255))
for i, r in enumerate(rows): prev.paste(r, (0, i * 33))
os.makedirs(os.path.join(HERE, "out"), exist_ok=True)
prev.resize((prev.width * 4, prev.height * 4), Image.NEAREST).save(os.path.join(HERE, "out", "topbar_preview.png"))
prev.save(os.path.join(HERE, "out", "topbar_preview_1x.png"))

# ---- plantillas del OCR de volumen (src/digits.h) -----------------------------------
rows_h = []
with open(os.path.join(HERE, "..", "src", "digits.h"), "w", newline="\n") as f:
    f.write("/* digits.h: cifras de la barra de estado de ByOS (generado por theme/topbar.py) */\n")
    f.write(f"#define TOPBAR_VOL_X0 {X_VOLN - 2}   /* zona de las cifras del volumen en la barra */\n")
    f.write(f"#define TOPBAR_VOL_X1 {X_VOLN + 3 * DW}\n")
    f.write("struct dtpl { int w, h; const char *rows[16]; };\nstatic const struct dtpl DIGITS[10] = {\n")
    for n in "0123456789":
        a = files[n].getchannel("A").point(lambda v: 255 if v > 140 else 0)
        bb = a.getbbox(); g = a.crop(bb)
        rws = ['"' + "".join("#" if g.getpixel((x, y)) else "." for x in range(g.width)) + '"' for y in range(g.height)]
        rows_h.append(g.height)
        assert g.width <= DW - 1, f"la cifra {n} ocupa {g.width} px: sin hueco para el OCR del volumen"
        f.write(f"    {{ {g.width}, {g.height}, {{ {', '.join(rws)} }} }},   /* {n} */\n")
    f.write("};\n")
# zona del "100%" para la libreria de pantalla (hibymod_disp.c): la barra de HiBy solo alinea
# las cifras a la izquierda, asi que la libreria las empuja a la derecha cuando hay menos de 3
with open(os.path.join(HERE, "..", "src", "topbar_layout.h"), "w", newline="\n") as f:
    f.write("/* topbar_layout.h: generado por theme/topbar.py */\n")
    f.write(f"#define TOPBAR_PCT_X {X_PCT}      /* primera cifra del porcentaje de bateria */\n")
    f.write(f"#define TOPBAR_PCT_W {3 * DW + wd('percent')}       /* 3 cifras + el signo % */\n")
    f.write(f"#define TOPBAR_DW {DW}            /* ancho de celda de una cifra */\n")
    f.write("#define TOPBAR_Y0 5\n#define TOPBAR_Y1 25\n")
    f.write(f"#define TOPBAR_BATT_X {X_BATT}     /* icono de la pila (el rayo de carga se dibuja encima) */\n")
# glifos del porcentaje de bateria para la libreria (alfa 0-255, celdas de 20 px de alto)
with open(os.path.join(HERE, "..", "src", "batt_glyphs.h"), "w", newline="\n") as f:
    f.write("/* batt_glyphs.h: cifras y % de la barra de estado (generado por theme/topbar.py) */\n")
    names = [*"0123456789", "percent"]; mw = max(files[n].width for n in names)
    red = ((RED[0] >> 3) << 11) | ((RED[1] >> 2) << 5) | (RED[2] >> 3)
    f.write(f"#define BG_N 11\n#define BG_H 20\n#define BG_MW {mw}\n#define BG_RED 0x{red:04X}\n")
    f.write("static const unsigned char BG_W[BG_N] = { " + ", ".join(str(files[n].width) for n in names) + " };\n")
    f.write("static const unsigned char BG_A[BG_N][BG_H][BG_MW] = {\n")
    for n in names:
        a = files[n].getchannel("A")
        f.write("  { " + ", ".join("{" + ",".join(str(a.getpixel((x, y)) if x < a.width else 0) for x in range(mw)) + "}" for y in range(20)) + " },\n")
    f.write("};\n")
print("cifras: alto", min(rows_h), "-", max(rows_h), "px;", len(out), "ficheros; POS", POS)

if "--deploy" in sys.argv:
    for rel, src in out:
        if "theme2" in rel: continue   # theme2 solo en el rootfs: en el aparato se montaria incompleto
        dst = "/usr/data/hibymod/theme/" + rel
        subprocess.run([ADB, "-s", DEV, "shell", f"mkdir -p '{os.path.dirname(dst)}'"], check=True)
        subprocess.run([ADB, "-s", DEV, "push", src, dst], check=True, capture_output=True)
    print("desplegado en", len(out), "rutas")
