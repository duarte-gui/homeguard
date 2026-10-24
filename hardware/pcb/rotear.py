#!/usr/bin/env python3
"""
Roteia a placa gerada por gerar_placa.py e prepara os arquivos para a PCBWay.

    python3 rotear.py --freerouting /caminho/freerouting.jar

Etapas: exporta Specctra DSN -> FreeRouting (sem interface) -> importa a sessão
-> planos de GND nas duas faces -> DRC -> Gerber, furação, BOM e posição.
"""
import argparse
import csv
import json
import shutil
import subprocess
import zipfile
from pathlib import Path

import pcbnew

AQUI = Path(__file__).resolve().parent
PCB = AQUI / "homeguard.kicad_pcb"
FAB = AQUI / "fabricacao"


def mm(v):
    return pcbnew.FromMM(v)


def rotear(jar, passes):
    board = pcbnew.LoadBoard(str(PCB))
    dsn, ses = AQUI / "homeguard.dsn", AQUI / "homeguard.ses"
    if not pcbnew.ExportSpecctraDSN(board, str(dsn)):
        raise SystemExit("falha ao exportar o DSN")
    cmd = ["java", "-Djava.awt.headless=true", "-jar", str(jar), "-de", str(dsn), "-do", str(ses),
           "-mp", str(passes), "--gui.enabled=false"]
    print("FreeRouting:", " ".join(cmd))
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=1800)
    tail = [ln for ln in (r.stdout + r.stderr).splitlines() if "WARNING" not in ln][-12:]
    print("\n".join(tail))
    if not ses.exists():
        raise SystemExit("o FreeRouting não gerou a sessão (.ses)")
    if not pcbnew.ImportSpecctraSES(board, str(ses)):
        raise SystemExit("falha ao importar a sessão")
    return board


def planos_gnd(board, larg=99.8, alt=80.0, margem=0.6):
    """Plano de GND nas duas faces: retorno de corrente curto e blindagem."""
    gnd = board.FindNet("GND")
    for layer in (pcbnew.F_Cu, pcbnew.B_Cu):
        z = pcbnew.ZONE(board)
        z.SetLayer(layer)
        z.SetNet(gnd)
        z.SetLocalClearance(mm(0.4))
        z.SetMinThickness(mm(0.3))
        z.SetThermalReliefGap(mm(0.5))
        z.SetThermalReliefSpokeWidth(mm(0.6))
        # Alívio térmico só nos furos (solda a ferro/onda); SMD liga direto no
        # plano, como é normal em refusão, e não sobra raio "faminto" no DRC.
        z.SetPadConnection(pcbnew.ZONE_CONNECTION_THT_THERMAL)
        z.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)
        o = z.Outline()
        o.NewOutline()
        for x, y in [(margem, margem), (larg - margem, margem), (larg - margem, alt - margem),
                     (margem, alt - margem)]:
            o.Append(mm(x), mm(y))
        board.Add(z)
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())


def drc():
    out = AQUI / "drc.json"
    subprocess.run(["kicad-cli", "pcb", "drc", "--format", "json", "--severity-all",
                    "--output", str(out), str(PCB)], capture_output=True)
    d = json.loads(out.read_text())
    ignorar = {"lib_footprint_issues", "lib_footprint_mismatch"}
    viol = [v for v in d["violations"] if v["type"] not in ignorar]
    return viol, d.get("unconnected_items", [])


def fabricacao(bom):
    if FAB.exists():
        shutil.rmtree(FAB)
    ger = FAB / "gerber"
    ger.mkdir(parents=True)
    camadas = "F.Cu,B.Cu,F.Paste,B.Paste,F.Silkscreen,B.Silkscreen,F.Mask,B.Mask,Edge.Cuts"
    subprocess.run(["kicad-cli", "pcb", "export", "gerbers", "--layers", camadas,
                    "--no-protel-ext", "--output", str(ger) + "/", str(PCB)], check=True,
                   capture_output=True)
    subprocess.run(["kicad-cli", "pcb", "export", "drill", "--format", "excellon",
                    "--excellon-separate-th", "--generate-map", "--map-format", "gerberx2",
                    "--output", str(ger) + "/", str(PCB)], check=True, capture_output=True)
    with zipfile.ZipFile(FAB / "homeguard_gerber.zip", "w", zipfile.ZIP_DEFLATED) as z:
        for f in sorted(ger.iterdir()):
            z.write(f, f.name)

    # Posição (centroide) de todas as peças montadas, no formato que a PCBWay pede.
    pos_raw = FAB / "pos_raw.csv"
    subprocess.run(["kicad-cli", "pcb", "export", "pos", "--format", "csv", "--units", "mm",
                    "--side", "front", "--exclude-dnp", "--output", str(pos_raw), str(PCB)],
                   check=True, capture_output=True)
    dnp = {b["ref"] for b in bom if b["dnp"]}
    with open(pos_raw) as fi, open(FAB / "homeguard_pos.csv", "w", newline="") as fo:
        w = csv.writer(fo)
        w.writerow(["Designator", "Mid X", "Mid Y", "Layer", "Rotation"])
        for row in csv.DictReader(fi):
            if row["Ref"] in dnp:
                continue
            w.writerow([row["Ref"], f'{float(row["PosX"]):.3f}mm', f'{float(row["PosY"]):.3f}mm',
                        "Top", row["Rot"]])
    pos_raw.unlink()

    # BOM agrupada por valor/peça, como a PCBWay pede.
    # Mesma peça (valor + código + encapsulamento) numa linha só, mesmo que a
    # função na placa mude; as funções vão juntas na descrição.
    grupos, funcoes = {}, {}
    for b in bom:
        if b["dnp"]:
            continue
        k = (b["value"], b["mpn"], b["package"], b["type"])
        grupos.setdefault(k, []).append(b["ref"])
        funcoes.setdefault(k, [])
        if b["desc"] not in funcoes[k]:
            funcoes[k].append(b["desc"])
    with open(FAB / "homeguard_bom.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Item", "Designator", "Qty", "Value", "Manufacturer Part Number",
                    "Description", "Package", "Type"])
        def ordem(r):
            return (r.rstrip("0123456789"), int("".join(c for c in r if c.isdigit()) or 0))
        for i, (k, refs) in enumerate(sorted(grupos.items(), key=lambda kv: ordem(min(kv[1], key=ordem))), 1):
            val, mpn, pkg, typ = k
            w.writerow([i, ",".join(sorted(refs, key=ordem)), len(refs), val, mpn,
                        "; ".join(funcoes[k]), pkg, typ])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--freerouting", required=True)
    ap.add_argument("--passes", type=int, default=40)
    a = ap.parse_args()

    board = rotear(a.freerouting, a.passes)
    planos_gnd(board)
    board.Save(str(PCB))

    viol, nc = drc()
    soltas = [v for v in viol if v["type"] == "via_dangling"]
    if soltas:
        # O FreeRouting às vezes deixa uma via ao lado de um pad sem usar a outra
        # face. A detecção é do próprio DRC do KiCad; aqui só se removem essas.
        board = pcbnew.LoadBoard(str(PCB))
        alvo = {(round(i["pos"]["x"], 3), round(i["pos"]["y"], 3)) for v in soltas for i in v["items"]}
        for t in list(board.GetTracks()):
            if t.GetClass() == "PCB_VIA":
                p = t.GetPosition()
                if (round(pcbnew.ToMM(p.x), 3), round(pcbnew.ToMM(p.y), 3)) in alvo:
                    board.Remove(t)
        pcbnew.ZONE_FILLER(board).Fill(board.Zones())
        board.Save(str(PCB))
        print(f"{len(soltas)} vias soltas removidas")
        viol, nc = drc()
    print(f"\nDRC: {len(viol)} violações, {len(nc)} ligações faltando")
    for v in viol[:25]:
        print(" -", v["type"], "|", [i["description"][:50] for i in v["items"]])
    for v in nc[:25]:
        print(" - SEM ROTA |", [i["description"][:50] for i in v["items"]])

    bom = json.loads((AQUI / "bom_partes.json").read_text())
    fabricacao(bom)
    print(f"\nArquivos em {FAB}")


if __name__ == "__main__":
    main()
