#!/usr/bin/env python3
"""Embute a interface web (data/dist) no firmware como web_assets.h.

Cada arquivo vai comprimido em gzip, e a placa o serve com
Content-Encoding: gzip. Com a interface dentro do firmware, uma única
atualização por OTA leva firmware e página juntos, e o LittleFS fica livre
para o registro de eventos.

Uso: python3 tools/embed_web.py   (depois de `npm run build` em data/)
"""
import gzip
import mimetypes
import pathlib
import sys

root = pathlib.Path(__file__).resolve().parent.parent
dist = root / "data" / "dist"
out = root / "web_assets.h"

if not dist.is_dir():
    sys.exit(f"não achei {dist}: rode `npm run build` em data/ antes")

TYPES = {".js": "application/javascript", ".css": "text/css", ".html": "text/html",
         ".svg": "image/svg+xml", ".ico": "image/x-icon", ".png": "image/png"}

files = sorted(p for p in dist.rglob("*") if p.is_file())
lines = [
    "// GERADO por tools/embed_web.py a partir de data/dist. Não edite à mão.",
    "#pragma once",
    "#include <Arduino.h>",
    "",
    "struct WebAsset { const char* path; const char* mime; const uint8_t* data; size_t len; };",
    "",
]
table = []
total = 0
for i, p in enumerate(files):
    raw = p.read_bytes()
    gz = gzip.compress(raw, compresslevel=9, mtime=0)
    total += len(gz)
    rel = "/" + p.relative_to(dist).as_posix()
    mime = TYPES.get(p.suffix) or mimetypes.guess_type(p.name)[0] or "application/octet-stream"
    lines.append(f"// {rel}: {len(raw)} -> {len(gz)} bytes")
    lines.append(f"static const uint8_t WEB_ASSET_{i}[] PROGMEM = {{")
    for j in range(0, len(gz), 20):
        lines.append("  " + ",".join(str(b) for b in gz[j:j + 20]) + ",")
    lines.append("};")
    table.append(f'  {{"{rel}", "{mime}", WEB_ASSET_{i}, sizeof(WEB_ASSET_{i})}},')

lines.append("")
lines.append("static const WebAsset WEB_ASSETS[] = {")
lines += table
lines.append("};")
lines.append(f"static const size_t WEB_ASSET_COUNT = {len(files)};")
out.write_text("\n".join(lines) + "\n")
print(f"{out.name}: {len(files)} arquivos, {total} bytes comprimidos")
