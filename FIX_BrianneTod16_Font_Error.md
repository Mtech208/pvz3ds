# Fix: "Unknown Command on Line 1" en TODOS los font descriptors PopCap (v15)

## Problema
Al iniciar el juego en 3DS, el log mostraba errores repetidos para **cada fuente**:
```
[FONTDATA ERROR] Font Descriptor Error in data/BrianneTod16.txt
Unknown Command on Line 1:

﻿Define CharList
( 'A', 'B', 'C', ...
```
Mism error para: `HouseOfTerror28/20/16`, `ContinuumBold14/Outback`, `DwarvenTodcraft*`, `Pix118Bold`, `Pico129`.

El parser fallaba y caía a `LoadLegacy()` que no encuentra la imagen `.png` correspondiente → glifos invisibles / texto roto.

## Causa raíz (dos bugs)

1. **`DescParser` usaba separador incorrecto**: por defecto `CMDSEP_SEMICOLON` espera comandos terminados en `;` y sin indentación. Los descriptores PopCap originales usan formato multi-línea indentado:
   ```
   Define CharList
    ( 'A', 'B', 'C', ...
   ```
   Sin `CMDSEP_NO_INDENT`, el parser ve `"Define"` y `"CharList"` como tokens separados y falla.

2. **`PakInterface::FOpen` (rama 3DS) rechazaba archivos sueltos válidos**: tenía un filtro que cerraba cualquier `data/*.txt` cuyo primer byte fuera `'8'` (asumiendo formato legacy "8 16"). Pero los archivos en SD **son los descriptores PopCap originales** (empiezan con `D` de `Define` o BOM UTF-8 `EF BB BF`), no legacy. El filtro los descartaba y forzaba la lectura desde el pak (mismo contenido, mismo fallo).

## Solución real (v15) — **NO crear archivos legacy**

### 1. `SexyAppFramework/graphics/ImageFont.cpp:1148` — `FontData::Load()`
```cpp
mCmdSep = CMDSEP_NO_INDENT;  // ANTES de LoadDescriptor()
mInitialized = LoadDescriptor(theFontDescFileName);
```
Esto hace que `DescParser::ParseDescriptorLine` acepte indentación y líneas continuadas.

### 2. `SexyAppFramework/paklib/PakInterface.cpp:467-511` — `FOpen()` rama 3DS
Eliminado el bloque `if (aFirst == '8') { fclose(aLoose); }`. Ahora:
```cpp
// Cualquier data/*.txt suelto en sdmc:/3ds/PlantsvsZombies/ tiene prioridad
FILE* aLoose = fcaseopen(aLoosePath, "rb");
if (aLoose != NULL) {
    TodTraceAndLog("[FS] opened loose '%s' len=%ld firstbyte=%d", ...);
    // crear PFILE y devolverlo
}
```

### 3. BOM UTF-8 ya se strippeaba en `DescParser.cpp:450-461` (v14) — se mantiene.

## Archivos en SD (`sdmc:/3ds/PlantsvsZombies/data/`)
Los 20 `.txt` + `.png` del pak original (formato PopCap) funcionan **tal cual**. No hace falta convertirlos a legacy.

## Código relevante
- `ImageFont.cpp:1148` — `mCmdSep = CMDSEP_NO_INDENT`
- `ImageFont.cpp:309` — `FontData::HandleCommand` (strip BOM defensivo)
- `DescParser.cpp:424` — `LoadDescriptor` (strip BOM UTF-8 real)
- `PakInterface.cpp:477` — rama loose 3DS sin filtro first-byte

## Build y deploy
```bash
cd pvz-ps2-wii/build/3ds-release
make re-plants-vs-zombies_3dsx -j4
curl -T re-plants-vs-zombies.3dsx ftp://192.168.137.125:5000/3ds/PlantsvsZombies/
```
Copiar `pvz-ps2-wii/data/*.txt *.png` → `sdmc:/3ds/PlantsvsZombies/data/`