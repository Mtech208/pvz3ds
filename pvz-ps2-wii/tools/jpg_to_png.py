#!/usr/bin/env python3
"""Convierte todos los .jpg del arbol de assets a .png usando ImageMagick.

Los PNG resultantes NO se dejan junto a los JPG: se escriben en
tools/jpg2png/ replicando las subcarpetas (images/, reanim/, ...), para
compresion posterior con pngquant/oxipng antes de copiarlos a la media y
regenerar main.pak. Los .jpg originales no se tocan.

En PS2 el loader prueba .png antes que .jpg en rutas sin extension
(ImageLib.cpp, bloque PS2_PLATFORM), asi que basta con que el .png exista;
el .jpg puede quedarse o borrarse de la media despues.

Uso:
    python tools/jpg_to_png.py                 # raiz por defecto: build/ps2-release
    python tools/jpg_to_png.py --root <dir>    # otra raiz de assets
"""

import argparse
import os
import shutil
import subprocess
import sys

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(TOOLS_DIR)
DEFAULT_ROOT = os.path.join(REPO_ROOT, "build", "ps2-release")
DEFAULT_OUT = os.path.join(TOOLS_DIR, "jpg2png")

# Carpetas del build que no son assets del juego.
EXCLUDE_DIRS = {"CMakeFiles", "compiled", "test", "userdata", "savedata", "jpg2png"}


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=DEFAULT_ROOT, help="raiz de assets a recorrer")
    parser.add_argument("--out", default=DEFAULT_OUT, help="carpeta destino de los PNG")
    parser.add_argument("--force", action="store_true", help="reconvertir aunque el PNG destino ya exista")
    args = parser.parse_args()

    if shutil.which("magick") is None:
        sys.exit("ERROR: no se encontro 'magick' en el PATH (instalar ImageMagick 7)")
    if not os.path.isdir(args.root):
        sys.exit(f"ERROR: no existe la raiz de assets: {args.root}")

    converted, skipped, failed = 0, 0, []

    for dirpath, dirnames, filenames in os.walk(args.root):
        dirnames[:] = [d for d in dirnames if d not in EXCLUDE_DIRS]
        for fn in filenames:
            if not fn.lower().endswith((".jpg", ".jpeg")):
                continue
            src = os.path.join(dirpath, fn)
            rel = os.path.relpath(src, args.root)
            dst = os.path.join(args.out, os.path.splitext(rel)[0] + ".png")

            if not args.force and os.path.exists(dst) and os.path.getmtime(dst) >= os.path.getmtime(src):
                skipped += 1
                continue

            os.makedirs(os.path.dirname(dst), exist_ok=True)
            # -strip: fuera EXIF/perfiles; pngquant se encarga de la paleta despues.
            result = subprocess.run(["magick", src, "-strip", dst],
                                    capture_output=True, text=True)
            if result.returncode != 0:
                failed.append((rel, result.stderr.strip()))
                continue
            converted += 1
            print(f"  {rel} -> {os.path.relpath(dst, TOOLS_DIR)}")

    print(f"\nConvertidos: {converted}   Ya al dia (saltados): {skipped}   Fallidos: {len(failed)}")
    if failed:
        print("\nFALLOS:")
        for rel, err in failed:
            print(f"  {rel}: {err}")
        sys.exit(1)

    if converted:
        print(f"\nPNG en: {args.out}")
        print("Siguiente paso: pngquant (paleta 8-bit) sobre esa carpeta, copiar a la")
        print("media pisando la estructura, y regenerar main.pak (tools/build_pak.py).")


if __name__ == "__main__":
    main()
