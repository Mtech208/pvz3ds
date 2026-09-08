#!/usr/bin/env python3
"""Pre-compone los companions de alpha ('X_' / '_X') sobre su imagen base X.

Por que: en runtime ImageLib::GetImage busca un companion por cada imagen y
compone el alpha expandiendo la imagen base a 32bpp — un bloque contiguo de
w*h*4. Para los fondos del selector (SELECTORSCREEN_BG_CENTER/LEFT/RIGHT_)
son 1.0-1.6MB contiguos que el heap fragmentado de PS2 no siempre tiene:
'[IMG] OOM expanding ... for alpha compose; alpha dropped' = fondos opacos
con zonas negras + ciclo de purga/refault. Pre-componer al build del pak
elimina ese camino por completo (el PNG paleta con tRNS ya lleva el alpha
integrado y decodifica compacto, 1 byte/pixel).

El alpha se toma del CANAL AZUL del companion, que es lo que lee el motor
(paleta: CLUT&0xFF; RGB: byte azul; 32bpp: bits&0xFF).

Los PNG compuestos se escriben en tools/alphacomposed/ replicando las
subcarpetas. Flujo posterior (igual que jpg_to_png.py):
  1. pngquant sobre tools/alphacomposed/ (paleta 8-bit con alpha/tRNS)
  2. copiar a la media pisando la estructura
  3. BORRAR los companions 'X_'/'_X' compuestos de la media: si siguen ahi,
     el motor los encuentra y vuelve a componer en runtime (el problema
     entero de vuelta). Este script imprime la lista exacta a borrar.
  4. regenerar main.pak (tools/build_pak.py)

Los alpha-only ('_X' SIN imagen base X, p.ej. data/_BrianneTod*) se saltan:
el motor los usa directamente como fuente de alpha de las fuentes.

Uso:
    python tools/compose_alpha_png.py                 # raiz: build/ps2-release
    python tools/compose_alpha_png.py --root <dir>    # otra raiz de assets
"""

import argparse
import os
import shutil
import subprocess
import sys

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(TOOLS_DIR)
DEFAULT_ROOT = os.path.join(REPO_ROOT, "build", "ps2-release")
DEFAULT_OUT = os.path.join(TOOLS_DIR, "alphacomposed")

# Carpetas del build que no son assets del juego.
EXCLUDE_DIRS = {"CMakeFiles", "compiled", "test", "userdata", "savedata",
                "jpg2png", "alphacomposed"}

IMG_EXTS = (".png", ".jpg", ".jpeg")


def find_companion(stem, files_lower):
    """Devuelve el nombre real del companion de 'stem' o None.

    Mismo orden de busqueda que ImageLib::GetImage: primero '_X', luego 'X_'.
    """
    for cand_stem in ("_" + stem, stem + "_"):
        for ext in IMG_EXTS:
            real = files_lower.get(cand_stem.lower() + ext)
            if real is not None:
                return real
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=DEFAULT_ROOT, help="raiz de assets a recorrer")
    parser.add_argument("--out", default=DEFAULT_OUT, help="carpeta destino de los PNG compuestos")
    parser.add_argument("--force", action="store_true", help="recomponer aunque el destino ya exista")
    args = parser.parse_args()

    if shutil.which("magick") is None:
        sys.exit("ERROR: no se encontro 'magick' en el PATH (instalar ImageMagick 7)")
    if not os.path.isdir(args.root):
        sys.exit(f"ERROR: no existe la raiz de assets: {args.root}")

    composed, skipped, failed = 0, 0, []
    companions_to_delete = []

    for dirpath, dirnames, filenames in os.walk(args.root):
        dirnames[:] = [d for d in dirnames if d not in EXCLUDE_DIRS]
        # Busqueda case-insensitive: la media/pak mezcla mayusculas.
        files_lower = {fn.lower(): fn for fn in filenames}

        for fn in filenames:
            stem, ext = os.path.splitext(fn)
            if ext.lower() not in IMG_EXTS:
                continue
            # Un companion no es imagen base ('X_' o '_X').
            if stem.endswith("_") or stem.startswith("_"):
                continue

            comp = find_companion(stem, files_lower)
            if comp is None:
                continue

            src = os.path.join(dirpath, fn)
            comp_src = os.path.join(dirpath, comp)
            rel = os.path.relpath(src, args.root)
            comp_rel = os.path.relpath(comp_src, args.root)
            # La salida siempre es .png: el jpg no puede llevar alpha.
            dst = os.path.join(args.out, os.path.splitext(rel)[0] + ".png")

            companions_to_delete.append(comp_rel)

            if (not args.force and os.path.exists(dst)
                    and os.path.getmtime(dst) >= os.path.getmtime(src)
                    and os.path.getmtime(dst) >= os.path.getmtime(comp_src)):
                skipped += 1
                continue

            os.makedirs(os.path.dirname(dst), exist_ok=True)
            # Canal azul del companion como alpha de la base, igual que el
            # motor. -alpha off primero: CopyOpacity reemplaza, no multiplica.
            result = subprocess.run(
                ["magick", src,
                 "(", comp_src, "-channel", "B", "-separate", "+channel", ")",
                 "-alpha", "off", "-compose", "CopyOpacity", "-composite",
                 "-strip", dst],
                capture_output=True, text=True)
            if result.returncode != 0:
                failed.append((rel, result.stderr.strip()))
                continue
            composed += 1
            print(f"  {rel} + {os.path.basename(comp)} -> {os.path.relpath(dst, TOOLS_DIR)}")

    print(f"\nCompuestos: {composed}   Ya al dia (saltados): {skipped}   Fallidos: {len(failed)}")
    if failed:
        print("\nFALLOS:")
        for rel, err in failed:
            print(f"  {rel}: {err}")
        sys.exit(1)

    if companions_to_delete:
        print("\nCompanions a BORRAR de la media antes de regenerar main.pak")
        print("(si quedan, el motor los recompone en runtime y el OOM vuelve):")
        for rel in sorted(set(companions_to_delete)):
            print(f"  {rel}")

    if composed:
        print(f"\nPNG compuestos en: {args.out}")
        print("Siguiente paso: pngquant sobre esa carpeta, copiar a la media")
        print("pisando la estructura, borrar los companions listados y regenerar")
        print("main.pak (tools/build_pak.py).")


if __name__ == "__main__":
    main()
