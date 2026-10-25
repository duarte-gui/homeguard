#!/usr/bin/env python3
"""
Gera a placa do Homeguard (KiCad) a partir desta descrição.

    python3 gerar_placa.py          -> homeguard.kicad_pcb + homeguard.kicad_pro

Depois rode rotear.py para as trilhas, as zonas de GND e os arquivos da PCBWay.

Medidas dos módulos (conferidas em footprints publicados, não em foto):
  - ESP32-C3 SuperMini: 2 fileiras de 8 pinos, passo 2,54 mm, fileiras a 15,24 mm.
  - Módulo LM2596 (43 x 21 mm): furos a 40,64 x 17,78 mm; IN+ e OUT+ na mesma borda.
"""
import json
import uuid
from pathlib import Path

AQUI = Path(__file__).resolve().parent
NOME = "homeguard"
LARG, ALT = 99.8, 80.0            # contorno; até 100 mm cai na faixa barata dos fabricantes

# ---------------------------------------------------------------------------
# Redes
# ---------------------------------------------------------------------------
NETS = ["", "GND", "+12V_IN", "+12V_F", "+12V", "+5V_BUCK", "+5V", "+3V3",
        "Z1_IN", "Z2_IN", "Z3_IN", "Z4_IN", "GPIO0", "GPIO1", "GPIO3", "GPIO4",
        "WG_D0", "WG_D1", "GPIO8", "PIX_DIN",
        "GPIO10", "G_SIR", "OUT_SIR", "LED_SIR",
        "GPIO5", "G_TRV", "OUT_TRV", "LED_TRV", "LED_12V"]
NET_ID = {n: i for i, n in enumerate(NETS)}

# Classes: largura de trilha por corrente. Sirene e trava podem puxar ~1 A cada.
CLASSES = {
    "Power": {"track": 1.2, "clear": 0.3, "via": 1.2, "drill": 0.6,
              "nets": ["GND", "+12V_IN", "+12V_F", "+12V", "+5V_BUCK", "+5V",
                       "OUT_SIR", "OUT_TRV"]},
    "Mid":   {"track": 0.6, "clear": 0.25, "via": 0.9, "drill": 0.45,
              "nets": ["+3V3"]},
}
DEFAULT = {"track": 0.3, "clear": 0.2, "via": 0.8, "drill": 0.4}


def u():
    return str(uuid.uuid4())


def net(n):
    return f'(net {NET_ID[n]} "{n}")' if n else ""


def txt(s, x, y, layer="F.SilkS", size=1.0, hide=False, kind="Reference"):
    h = " hide" if hide else ""
    th = round(size * 0.15, 3)
    mir = " (justify mirror)" if layer.startswith("B.") else ""
    return (f'(property "{kind}" "{s}" (at {x} {y} 0) (layer "{layer}") (uuid "{u()}")'
            f' (effects (font (size {size} {size}) (thickness {th})){mir}){h})')


def line(x1, y1, x2, y2, layer="F.SilkS", w=0.12):
    return (f'(fp_line (start {x1} {y1}) (end {x2} {y2}) (stroke (width {w}) (type solid))'
            f' (layer "{layer}") (uuid "{u()}"))')


def rect(x1, y1, x2, y2, layer, w=0.12):
    return "\n".join([line(x1, y1, x2, y1, layer, w), line(x2, y1, x2, y2, layer, w),
                      line(x2, y2, x1, y2, layer, w), line(x1, y2, x1, y1, layer, w)])


def smd_pad(num, x, y, w, h, n, shape="roundrect", rot=0):
    rr = " (roundrect_rratio 0.25)" if shape == "roundrect" else ""
    return (f'(pad "{num}" smd {shape} (at {x} {y} {rot}) (size {w} {h})'
            f' (layers "F.Cu" "F.Paste" "F.Mask"){rr} {net(n)} (uuid "{u()}"))')


def tht_pad(num, x, y, d, drill, n, square=False):
    shp = "rect" if square else "circle"
    return (f'(pad "{num}" thru_hole {shp} (at {x} {y}) (size {d} {d}) (drill {drill})'
            f' (layers "*.Cu" "*.Mask") (remove_unused_layers no) {net(n)} (uuid "{u()}"))')


def footprint(lib, ref, value, x, y, body, attr="smd", ref_at=(0, -2.2), ref_size=0.9,
              ref_hide=False, rot=0, back=False):
    cu = "B.Cu" if back else "F.Cu"
    return (f'(footprint "HG:{lib}" (layer "{cu}") (uuid "{u()}") (at {x} {y} {rot})\n'
            f'  {txt(ref, ref_at[0], ref_at[1], "B.SilkS" if back else "F.SilkS", ref_size, ref_hide)}\n'
            f'  {txt(value, 0, 0, "B.Fab" if back else "F.Fab", 0.8, kind="Value")}\n'
            f'  (attr {attr})\n  ' + "\n  ".join(body) + "\n)")


# ---------------------------------------------------------------------------
# Footprints
# ---------------------------------------------------------------------------
def two_pad_smd(lib, ref, value, x, y, n1, n2, vertical, pitch, pw, ph, cy=1.0, k_mark=False,
                ref_at=None):
    """0805, 1206, SMA, 1812: dois pads. Pad 1 à esquerda; em pé, pad 1 em cima.

    Em pé é o MESMO footprint girado 270 graus, e não pads trocados de eixo: é a
    rotação que vai para o arquivo de posição, e é por ela que a máquina de
    montagem orienta a peça (e a polaridade dos diodos)."""
    rot = 270 if vertical else 0
    p = [smd_pad("1", -pitch, 0, pw, ph, n1, rot=rot), smd_pad("2", pitch, 0, pw, ph, n2, rot=rot)]
    ext = pitch + pw / 2 + 0.25
    crt = rect(-ext, -cy, ext, cy, "F.CrtYd", 0.05)
    mark = [line(-pitch - pw / 2 - 0.4, -cy, -pitch - pw / 2 - 0.4, cy, w=0.2)] if k_mark else []
    if ref_at and vertical:
        # ref_at vem em coordenadas da placa; converte para o referencial girado
        ref_at = (ref_at[1], -ref_at[0])
    return footprint(lib, ref, value, x, y, p + [crt] + mark, ref_at=ref_at or (0, -cy - 1.0),
                     ref_size=0.8, rot=rot)


def r0805(ref, val, x, y, n1, n2, vertical=False):
    return two_pad_smd("R_0805", ref, val, x, y, n1, n2, vertical, 0.95, 1.0, 1.45)


def c0805(ref, val, x, y, n1, n2, vertical=False, ref_at=None):
    return two_pad_smd("C_0805", ref, val, x, y, n1, n2, vertical, 0.95, 1.0, 1.45, ref_at=ref_at)


def c1206(ref, val, x, y, n1, n2, vertical=False):
    return two_pad_smd("C_1206", ref, val, x, y, n1, n2, vertical, 1.45, 1.15, 1.8, cy=1.2)


def led0805(ref, val, x, y, k, a, vertical=False):
    # pad 1 = catodo (K), marcado na serigrafia
    return two_pad_smd("LED_0805", ref, val, x, y, k, a, vertical, 0.95, 1.0, 1.45, k_mark=True)


def d_sma(ref, val, x, y, k, a, vertical=False):
    # DO-214AC; pad 1 = catodo (K)
    return two_pad_smd("D_SMA", ref, val, x, y, k, a, vertical, 2.0, 2.5, 1.8, cy=1.6, k_mark=True)


def ptc1812(ref, val, x, y, n1, n2, vertical=False, ref_at=None):
    return two_pad_smd("Fuse_1812", ref, val, x, y, n1, n2, vertical, 2.3, 1.4, 3.4, cy=2.0,
                       ref_at=ref_at)


def sot23(ref, val, x, y, g, s, d):
    """AO3400A: 1=G, 2=S, 3=D. Dreno em cima."""
    body = [smd_pad("1", -0.95, 1.1, 0.6, 1.3, g), smd_pad("2", 0.95, 1.1, 0.6, 1.3, s),
            smd_pad("3", 0, -1.1, 0.6, 1.3, d),
            rect(-1.7, -2.1, 1.7, 2.1, "F.CrtYd", 0.05),
            line(-1.55, -0.3, -1.55, 0.3), line(1.55, -0.3, 1.55, 0.3)]
    return footprint("SOT-23", ref, val, x, y, body, ref_at=(2.6, 0), ref_size=0.8)


def terminal(ref, val, x, y, nets, labels, vertical=False):
    """Borne de parafuso 5,08 mm (KF128/DG128). Entrada do fio voltada para a borda."""
    n = len(nets)
    p, body = 5.08, []
    for i, nn in enumerate(nets):
        px, py = (0, i * p) if vertical else (i * p, 0)
        body.append(tht_pad(str(i + 1), px, py, 2.4, 1.3, nn, square=(i == 0)))
    span = (n - 1) * p
    if vertical:
        body.append(rect(-3.5, -2.6, 4.2, span + 2.6, "F.SilkS"))
        body.append(rect(-3.8, -2.9, 4.5, span + 2.9, "F.CrtYd", 0.05))
        for i, lab in enumerate(labels):
            body.append(f'(fp_text user "{lab}" (at -4.2 {i * p}) (layer "F.SilkS") (uuid "{u()}")'
                        f' (effects (font (size 1 1) (thickness 0.15)) (justify right)))')
        ref_at = (0.4, -4.0)
    else:
        body.append(rect(-2.6, -4.2, span + 2.6, 3.5, "F.SilkS"))
        body.append(rect(-2.9, -4.5, span + 2.9, 3.8, "F.CrtYd", 0.05))
        for i, lab in enumerate(labels):
            body.append(f'(fp_text user "{lab}" (at {i * p} 4.6) (layer "F.SilkS") (uuid "{u()}")'
                        f' (effects (font (size 1 1) (thickness 0.15))))')
        ref_at = (-4.2, 0)
    return footprint(f"Terminal_5.08_{n}P{'_V' if vertical else ''}", ref, val, x, y, body,
                     attr="through_hole", ref_at=ref_at, ref_size=0.9, ref_hide=True)


def header_1x8(ref, x, y, nets, first_square):
    """Barra fêmea 1x8, 2,54 mm, vertical (pino 1 em cima)."""
    body = [tht_pad(str(i + 1), 0, i * 2.54, 1.7, 1.0, nn, square=(first_square and i == 0))
            for i, nn in enumerate(nets)]
    body.append(rect(-1.4, -1.4, 1.4, 19.18, "F.CrtYd", 0.05))
    return footprint("PinSocket_1x8_P2.54", ref, "1x8 female", x, y, body, attr="through_hole",
                     ref_hide=True)


def supermini_graphics(x, y):
    """Contorno do SuperMini (18 x 22,5 mm), da USB-C e os nomes dos pinos."""
    cx = x + 7.62
    g = []
    for x1, y1, x2, y2 in [(cx - 9, y - 3.0, cx + 9, y + 19.5), (cx - 4.5, y + 19.5, cx + 4.5, y + 21.0)]:
        for a, b, c, d in [(x1, y1, x2, y1), (x2, y1, x2, y2), (x2, y2, x1, y2), (x1, y2, x1, y1)]:
            g.append(f'(gr_line (start {a} {b}) (end {c} {d}) (stroke (width 0.12) (type solid))'
                     f' (layer "F.SilkS") (uuid "{u()}"))')
    g.append(gr_text("ESP32-C3 SuperMini", cx, y - 4.2, 1.0))
    g.append(gr_text("USB-C", cx, y + 18, 1.0))
    for i, (l, r) in enumerate(zip(["0", "1", "2", "3", "4", "3V3", "G", "5V"],
                                   ["21", "20", "10", "9", "8", "7", "6", "5"])):
        g.append(gr_text(l, x + 2.2, y + i * 2.54, 0.8, just="left"))
        g.append(gr_text(r, x + 13.04, y + i * 2.54, 0.8, just="right"))
    return g


def lm2596_module(ref, x, y, vin, gnd, vout):
    """Módulo LM2596 por cima, componentes para cima, IN em cima e OUT embaixo.

    Vista de cima do módulo GIRADA (sem espelhar): com o IN em cima, o IN+ fica à
    DIREITA do IN-, e o OUT+ à direita do OUT-. Uma versão anterior trocava os
    eixos do footprint de referência, o que espelha a peça, e invertia a
    polaridade dos dois lados. Furos a 17,78 x 40,64 mm (7 x 16 passos de 2,54)."""
    body = [tht_pad("1", 17.78, 0, 3.0, 1.6, vin, square=True),      # IN+
            tht_pad("2", 0, 0, 3.0, 1.6, gnd),                        # IN-
            tht_pad("3", 0, 40.64, 3.0, 1.6, gnd),                    # OUT-
            tht_pad("4", 17.78, 40.64, 3.0, 1.6, vout),               # OUT+
            rect(-2.2, -2.2, 19.98, 42.84, "F.SilkS"),
            rect(-2.5, -2.5, 20.28, 43.14, "F.CrtYd", 0.05)]
    for lab, lx, ly in [("IN-", 3.2, 0), ("IN+", 14.6, 0), ("OUT-", 3.2, 40.64), ("OUT+", 14.6, 40.64)]:
        just = "left" if lx < 9 else "right"
        body.append(f'(fp_text user "{lab}" (at {lx} {ly}) (layer "F.SilkS") (uuid "{u()}")'
                    f' (effects (font (size 1 1) (thickness 0.15)) (justify {just})))')
    body.append(f'(fp_text user "LM2596" (at 7.2 20.3 90) (layer "F.SilkS") (uuid "{u()}")'
                f' (effects (font (size 2 2) (thickness 0.3))))')
    body.append(f'(fp_text user "componentes para cima" (at 11.2 20.3 90) (layer "F.SilkS") (uuid "{u()}")'
                f' (effects (font (size 1 1) (thickness 0.15))))')
    return footprint("LM2596_Module", ref, "LM2596 module", x, y, body, attr="through_hole",
                     ref_at=(8.89, -3.6), ref_hide=True)   # "LM2596" já identifica


def keepout_copper(layer, x1, y1, x2, y2):
    """Sem cobre na face encostada no módulo: as soldas dele não tocam em nada."""
    return (f'(zone (net 0) (net_name "") (layer "{layer}") (uuid "{u()}") (name "sob LM2596")'
            f' (hatch edge 0.5) (connect_pads (clearance 0)) (min_thickness 0.25)'
            f' (filled_areas_thickness no)'
            f' (keepout (tracks not_allowed) (vias not_allowed) (pads allowed)'
            f' (copperpour not_allowed) (footprints allowed))'
            f' (fill (thermal_gap 0.5) (thermal_bridge_width 0.5))'
            f' (polygon (pts (xy {x1} {y1}) (xy {x2} {y1}) (xy {x2} {y2}) (xy {x1} {y2}))))')



def mount_hole(ref, x, y):
    body = [f'(pad "" np_thru_hole circle (at 0 0) (size 3.2 3.2) (drill 3.2) (layers "*.Cu" "*.Mask") (uuid "{u()}"))',
            f'(fp_circle (center 0 0) (end 3.2 0) (stroke (width 0.05) (type solid)) (fill none) (layer "F.CrtYd") (uuid "{u()}"))']
    return footprint("MountingHole_M3", ref, "M3", x, y, body, attr="exclude_from_pos_files exclude_from_bom",
                     ref_hide=True)


def gr_text(s, x, y, size=1.2, layer="F.SilkS", just=None, angle=0):
    j = f" (justify {just})" if just else ""
    th = round(size * 0.15, 3)
    return (f'(gr_text "{s}" (at {x} {y} {angle}) (layer "{layer}") (uuid "{u()}")'
            f' (effects (font (size {size} {size}) (thickness {th})){j}))')


# ---------------------------------------------------------------------------
# Placa
# ---------------------------------------------------------------------------
PARTS = []      # (texto do footprint, linha de BOM ou None)
BOM = []        # dicts


def add(fp, ref, value, desc, mpn, package, kind="SMD", dnp=False):
    PARTS.append(fp)
    BOM.append({"ref": ref, "value": value, "desc": desc, "mpn": mpn,
                "package": package, "type": kind, "dnp": dnp})


# --- bornes na borda de cima (y=7) ---
add(terminal("J1", "12V", 12, 7, ["+12V_IN", "GND"], ["+12V", "GND"]),
    "J1", "Screw terminal 2P", "12V power input", "KF128-5.08-2P", "THT 5.08mm", "THT")
add(terminal("J2", "ZONAS", 27, 7, ["Z1_IN", "GND", "Z2_IN", "GND", "Z3_IN", "GND", "Z4_IN", "GND"],
             ["Z1", "C", "Z2", "C", "Z3", "C", "Z4", "C"]),
    "J2", "Screw terminal 8P", "Alarm zones 1-4 + common", "KF128-5.08-8P (or 2x KF128-5.08-4P)", "THT 5.08mm", "THT")
add(terminal("J3", "TECLADO", 68.5, 7, ["WG_D0", "WG_D1", "+12V", "GND"], ["D0", "D1", "12V", "GND"]),
    "J3", "Screw terminal 4P", "Wiegand keypad", "KF128-5.08-4P", "THT 5.08mm", "THT")
add(terminal("J4", "RGB", 93, 12, ["PIX_DIN", "+3V3", "GND"], ["DIN", "3V3", "GND"], vertical=True),
    "J4", "Screw terminal 3P", "External RGB pixel", "KF128-5.08-3P", "THT 5.08mm", "THT")

# --- saídas na borda direita (x=103) ---
add(terminal("J5", "SIRENE", 93, 31, ["+12V", "OUT_SIR"], ["+12V", "SIR-"], vertical=True),
    "J5", "Screw terminal 2P", "Siren output (switched 12V)", "KF128-5.08-2P", "THT 5.08mm", "THT")
add(terminal("J6", "TRAVA", 93, 48, ["+12V", "OUT_TRV"], ["+12V", "TRV-"], vertical=True),
    "J6", "Screw terminal 2P", "Lock output (switched 12V)", "KF128-5.08-2P", "THT 5.08mm", "THT")

# --- 12 V sempre ligado, para sensor de presença (PIR) ou outro acessório ---
add(terminal("J9", "AUX 12V", 93, 64, ["+12V", "GND"], ["+12V", "GND"], vertical=True),
    "J9", "Screw terminal 2P", "Auxiliary 12V output (sensors)", "KF128-5.08-2P", "THT 5.08mm", "THT")

# --- entrada de 12 V: fusível rearmável + diodo contra inversão ---
add(ptc1812("F1", "PTC 2A", 11, 14.3, "+12V_IN", "+12V_F", ref_at=(-4.2, 0)),
    "F1", "PTC 2A hold 16V", "Resettable fuse", "Bourns MF-MSMF200/16X-2", "1812")
add(d_sma("D1", "SS54", 20, 14.2, "+12V", "+12V_F"),
    "D1", "SS54", "Schottky 5A 40V, reverse polarity protection", "SS54", "SMA (DO-214AC)")
add(c1206("C1", "10uF 25V", 34, 66, "+12V", "GND"),
    "C1", "10uF 25V X5R", "Decoupling", "CL31A106KAHNNNE", "1206")
add(r0805("R1", "4.7k", 10, 67, "+12V", "LED_12V"),
    "R1", "4.7k 1%", "LED resistor", "0805W8F4701T5E", "0805")
add(led0805("D2", "LED verde", 10, 71, "GND", "LED_12V"),
    "D2", "LED green", "Power indicator", "LTST-C171GKT", "0805")

# --- LM2596 (você solda) e 5 V do ESP ---
PARTS.append(lm2596_module("U2", 5, 20, "+12V", "GND", "+5V_BUCK"))
BOM.append({"ref": "U2", "value": "LM2596 module", "desc": "12V->5V buck module - DO NOT PLACE, customer fits",
            "mpn": "-", "package": "Module 43x21mm", "type": "THT", "dnp": True})
add(d_sma("D3", "SS14", 30, 70.5, "+5V", "+5V_BUCK"),
    "D3", "SS14", "Schottky 1A, USB/buck isolation", "SS14", "SMA (DO-214AC)")
add(c1206("C2", "10uF 25V", 40, 74, "+5V", "GND"),
    "C2", "10uF 25V X5R", "Decoupling", "CL31A106KAHNNNE", "1206")

# --- ESP32-C3 SuperMini em soquete (vista de cima, USB para baixo) ---
ESP_X, ESP_Y = 48.0, 52.0
LEFT = ["GPIO0", "GPIO1", "", "GPIO3", "GPIO4", "+3V3", "GND", "+5V"]
RIGHT = ["", "", "GPIO10", "", "GPIO8", "WG_D1", "WG_D0", "GPIO5"]
add(header_1x8("J7", ESP_X, ESP_Y, LEFT, True),
    "J7", "Female header 1x8", "Socket for ESP32-C3 SuperMini (left row)", "PZ254V-11-08P",
    "THT 2.54mm straight", "THT")
add(header_1x8("J8", ESP_X + 15.24, ESP_Y, RIGHT, False),
    "J8", "Female header 1x8", "Socket for ESP32-C3 SuperMini (right row)", "PZ254V-11-08P",
    "THT 2.54mm straight", "THT")

# --- zonas: 1 k em série, pull-up 10 k, 100 nF ---
ZONAS = [("Z1_IN", "GPIO0"), ("Z2_IN", "GPIO1"), ("Z3_IN", "GPIO3"), ("Z4_IN", "GPIO4")]
for k, (zin, gp) in enumerate(ZONAS):
    zx = 27 + 2 * k * 5.08
    add(r0805(f"R{10 + k}", "1k", zx, 17, zin, gp, vertical=True),
        f"R{10 + k}", "1k 1%", "Zone input series", "0805W8F1001T5E", "0805")
    add(r0805(f"R{20 + k}", "10k", zx + 5.2, 22.5, "+3V3", gp, vertical=True),
        f"R{20 + k}", "10k 1%", "Zone pull-up", "0805W8F1002T5E", "0805")
    add(c0805(f"C{10 + k}", "100nF", zx + 2.6, 22.5, gp, "GND", vertical=True, ref_at=(-1.6, 2.6)),
        f"C{10 + k}", "100nF 50V X7R", "Decoupling/filter", "CL21B104KBCNNNC", "0805")

# --- pixel RGB ---
add(r0805("R30", "330", 70, 62.2, "GPIO8", "PIX_DIN"),
    "R30", "330R 1%", "Pixel data series", "0805W8F3300T5E", "0805")
add(c0805("C30", "100nF", 84, 19.5, "+3V3", "GND", vertical=True),
    "C30", "100nF 50V X7R", "Decoupling/filter", "CL21B104KBCNNNC", "0805")

# --- saídas com MOSFET (sirene e trava) ---
def saida(n, gpio, gate, out, led, y0):
    add(sot23(f"Q{n}", "AO3400A", 78, y0 + 3, gate, "GND", out),
        f"Q{n}", "AO3400A", "N-MOSFET logic level 30V 5.7A", "AO3400A", "SOT-23")
    add(r0805(f"R{40 + n}", "100", 72, y0 + 4.1, gpio, gate),
        f"R{40 + n}", "100R 1%", "Gate resistor", "0805W8F1000T5E", "0805")
    add(r0805(f"R{50 + n}", "100k", 74.5, y0 + 8, gate, "GND", vertical=True),
        f"R{50 + n}", "100k 1%", "Gate pull-down", "0805W8F1003T5E", "0805")
    add(d_sma(f"D{10 + n}", "SS34", 83.5, y0 + 2.5, "+12V", out, vertical=True),
        f"D{10 + n}", "SS34", "Schottky 3A, flyback", "SS34", "SMA (DO-214AC)")
    add(r0805(f"R{60 + n}", "2.2k", 69, y0 - 3, "+12V", led),
        f"R{60 + n}", "2.2k 1%", "LED resistor", "0805W8F2201T5E", "0805")
    add(led0805(f"D{20 + n}", "LED vermelho", 74, y0 - 3, out, led),
        f"D{20 + n}", "LED red", "Output indicator", "LTST-C171KRKT", "0805")


saida(1, "GPIO10", "G_SIR", "OUT_SIR", "LED_SIR", 31)
saida(2, "GPIO5", "G_TRV", "OUT_TRV", "LED_TRV", 48)
add(c0805("C3", "100nF", 66, 42, "+12V", "GND", vertical=True),
    "C3", "100nF 50V X7R", "Decoupling/filter", "CL21B104KBCNNNC", "0805")

# --- furos de fixação ---
for i, (hx, hy) in enumerate([(3.5, 3.5), (96.5, 3.5), (3.5, 76.5), (96.5, 76.5)]):
    PARTS.append(mount_hole(f"H{i + 1}", hx, hy))

# ---------------------------------------------------------------------------
# Escrita
# ---------------------------------------------------------------------------
GRAPHICS = supermini_graphics(ESP_X, ESP_Y) + [
    keepout_copper("F.Cu", 2.8, 17.8, 24.98, 62.84),
    f'(gr_rect (start 0 0) (end {LARG} {ALT}) (stroke (width 0.1) (type default)) (fill none)'
    f' (layer "Edge.Cuts") (uuid "{u()}"))',
    gr_text("HOMEGUARD v1", 78, 76.5, 2.0),
    gr_text("ESP32-C3 SuperMini - alarme 4 zonas", 78, 73.8, 0.9),
    gr_text("SIRENE", 93, 26.6, 1.0),
    gr_text("TRAVA", 93, 44.6, 1.0),
    gr_text("AUX 12V", 93, 60.6, 1.0),
    gr_text("12V", 7.6, 71, 0.8, just="right"),
    gr_text("USB-C pelo lado de baixo do modulo", 55.6, 78.3, 0.8),
]

pcb = f"""(kicad_pcb (version 20240108) (generator "homeguard_gen") (generator_version "1.0")
  (general (thickness 1.6) (legacy_teardrops no))
  (paper "A4")
  (layers
    (0 "F.Cu" signal) (31 "B.Cu" signal)
    (32 "B.Adhes" user "B.Adhesive") (33 "F.Adhes" user "F.Adhesive")
    (34 "B.Paste" user) (35 "F.Paste" user)
    (36 "B.SilkS" user "B.Silkscreen") (37 "F.SilkS" user "F.Silkscreen")
    (38 "B.Mask" user) (39 "F.Mask" user)
    (40 "Dwgs.User" user "User.Drawings") (41 "Cmts.User" user "User.Comments")
    (42 "Eco1.User" user "User.Eco1") (43 "Eco2.User" user "User.Eco2")
    (44 "Edge.Cuts" user) (45 "Margin" user)
    (46 "B.CrtYd" user "B.Courtyard") (47 "F.CrtYd" user "F.Courtyard")
    (48 "B.Fab" user) (49 "F.Fab" user)
  )
  (setup (pad_to_mask_clearance 0))
{chr(10).join(f'  (net {i} "{n}")' for i, n in enumerate(NETS))}
{chr(10).join(PARTS)}
{chr(10).join(GRAPHICS)}
)
"""
(AQUI / f"{NOME}.kicad_pcb").write_text(pcb)


def klass(name, c):
    return {"name": name, "clearance": c["clear"], "track_width": c["track"],
            "via_diameter": c["via"], "via_drill": c["drill"],
            "microvia_diameter": 0.3, "microvia_drill": 0.1,
            "diff_pair_gap": 0.25, "diff_pair_via_gap": 0.25, "diff_pair_width": 0.2,
            "line_style": 0, "pcb_color": "rgba(0, 0, 0, 0.000)",
            "schematic_color": "rgba(0, 0, 0, 0.000)", "wire_width": 6, "bus_width": 12}


pro = {
    "board": {"design_settings": {"rules": {
        "min_clearance": 0.2, "min_track_width": 0.15,   # 6 mil: padrão da PCBWay, sem custo extra
        "min_via_diameter": 0.6,
        "min_through_hole_diameter": 0.3, "min_hole_clearance": 0.25,
        "min_copper_edge_clearance": 0.5, "min_silk_clearance": 0.0}}},
    "net_settings": {
        "classes": [klass("Default", DEFAULT)] + [klass(k, v) for k, v in CLASSES.items()],
        "meta": {"version": 3},
        "netclass_patterns": [{"netclass": k, "pattern": n}
                              for k, v in CLASSES.items() for n in v["nets"]],
    },
    "meta": {"filename": f"{NOME}.kicad_pro", "version": 1},
}
(AQUI / f"{NOME}.kicad_pro").write_text(json.dumps(pro, indent=2))
(AQUI / "bom_partes.json").write_text(json.dumps(BOM, indent=2, ensure_ascii=False))
print(f"{len(PARTS)} footprints, {len(NETS) - 1} redes -> {NOME}.kicad_pcb")
