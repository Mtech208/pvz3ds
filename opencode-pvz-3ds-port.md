# Port de Plants vs Zombies a Nintendo 3DS — informe de proyecto (actualizado 6-sep)

> Documento vivo: problemas, soluciones, pipeline de renderizado, memoria,
> debug y decisones. El log operativo corto está en `pvz-ps2-wii/AGENT.MD`;
> este archivo es la referencia larga.

## Directiva para opencode

- **Terminar el port sí o sí.** No es un experimento. Si una fase se traba,
  no abandonar: iterar, probar otra cosa (referencias `PvZ-Portable`) y seguir.
- **Permiso total** sobre el repo/proyecto (crear/borrar/reescribir, toolchain,
  cualquier archivo: `GLInterface.cpp` incluido) sin pedir confirmación por paso.
- **Mínimo 30 FPS jugables** en gameplay normal. New 3DS es el objetivo principal;
  si Old 3DS no llega ni bajando calidad, priorizar New 3DS y documentarlo.
- El usuario prueba en 3DS real (vía FTP) y ahora tiene el emulador **Azahar**
  instalado para iterar rápido.

## Sinopsis del proyecto

Port de Plants vs Zombies (GOTY PC, decompilación de Patoke) a Nintendo 3DS
sobre el fork multiplataforma `OptiJuegos/pvz-ps2-wii`, que YA tenía un backend
3DS/citro3d empezado (`GLInterface.cpp` ~1942 líneas). El backport 3DS proviene
de la rama 3DS de **`wszqkzqk/PvZ-Portable`** (mismo `ToMortonTexture*`, mismos
shaders PICA200). No se reescribió desde cero: lo que falta ya no es "el
backend", sino que **cargue hasta el título, renderice limpio y no se quede sin
memoria**.

**Estado hoy (7-sep, build v15):** compila y genera `.3dsx`; subido a la 3DS por FTP.
En v14/v15 se identificaron y resolvieron 5 problemas críticos:
1) **Causa del crash (Kernel Panic / Prefetch Abort)**: `TOD_ASSERT(aPtr)` en `DefinitionAlloc`
   al cargar `MelonImpact.xml.compiled`. Se habilitó `DefinitionReadCompiledStream` para 3DS
   (descompresión zlib por stream directo a los objetos sin búfer uncompressed monolítico en heap).
2) **Causa del logo de PvZ y barra corruptos**: se eliminaron llamadas erróneas a
   `DeleteImage("PvZ_Logo")`, `DeleteImage("LoadBar_*")`, etc. que destruían las texturas mientras
   el TitleScreen las dibujaba.
3) **Causa de los controles inactivos ("presiona para Start")**: `TitleScreen` ignora
   los botones hasta que `mLoadingThreadComplete == true`. Al crashear antes de terminar de cargar,
   nunca llegaba a habilitar el inicio.
4) **Líneas del 3D**: `gfxSet3D(false)` desactiva el parallax barrier del hardware.
5) **Fuentes: "Unknown Command on Line 1" en todos los descriptores PopCap** (`BrianneTod16`, `HouseOfTerror28`, etc.). El parser esperaba separador `;` y líneas sin indentación; los archivos del pak usan formato multi-línea indentado (`Define CharList` + continuación). Además, `PakInterface::FOpen` rechazaba archivos sueltos en SD cuyo primer byte era `'8'` (formato legacy), descartando los descriptores PopCap válidos.
   **Fix v15**: `FontData::Load()` setea `mCmdSep = CMDSEP_NO_INDENT` antes de `LoadDescriptor()`. `PakInterface::FOpen` (rama 3DS) ya no rechaza por first-byte; abre loose `data/*.txt` de `sdmc:/3ds/PlantsvsZombies/` con prioridad sobre el pak.

## Build y despliegue (3DS)

- Build bueno: `build/3ds-release/` (Unix Makefiles).
- Comando (desde el repo):
  ```
  C:\devkitPro\msys2\usr\bin\bash.exe -c "export DEVKITPRO=/opt/devkitpro && export DEVKITARM=/opt/devkitpro/devkitARM && export PATH=/opt/devkitpro/devkitARM/bin:/opt/devkitpro/tools/bin:/opt/devkitpro/msys2/usr/bin:\$PATH && cd /c/Users/gamer/OneDrive/Desktop/pvz3ds/pvz-ps2-wii/build/3ds-release && make re-plants-vs-zombies_3dsx -j4 > build_out.txt 2>&1; echo DONE_EXIT=\$? >> build_out.txt"
  ```
- Despliegue por FTP a la 3DS del usuario (`sdmc:/3ds/PlantsvsZombies/` y `sdmc:/3ds/`):
  ```
  curl.exe -v -T build\3ds-release\re-plants-vs-zombies.3dsx "ftp://10.117.118.115:5000/3ds/PlantsvsZombies/re-plants-vs-zombies.3dsx"
  curl.exe -v -T build\3ds-release\re-plants-vs-zombies.3dsx "ftp://10.117.118.115:5000/3ds/re-plants-vs-zombies.3dsx"
  ```
- El `.3dsx` queda en `build/3ds-release/re-plants-vs-zombies.3dsx` (~2.75 MB).
  SHA1 v14: `7C348580C72714842909575EF9362C9C83BEB719`.

## Problemas encontrados y soluciones (cronológico)

| # | Problema | Causa raíz | Solución | Estado |
|---|----------|-----------|----------|--------|
| 1 | "FATAL: out of free sound ids" al cargar | `DummySoundManager::GetFreeSoundId()` devolvía siempre -1; `ResourceManager::DoLoadSound` lo trata como fatal | Repartir ids incrementales en `DummySoundManager.h` (`mNextSoundId++`), `LoadSound`->true, `SetBaseVolume/Pan`->true. `GetSoundInstance` sigue NULL y TODO caller lo chequea -> mudo pero no crashea | resuelto |
| 2 | "Failed to find ']'" justo tras LawnStrings | (1º) `fread` grande podía volver corta en hardware real → `WiiChunkedFRead` (chunks 128KB) + exigir `aBytesRead==aSize` | recidiva → era texto **UTF-16LE** (traducción), parser byte-based; `strchr` paraba en el primer 0x00 | detectar BOM `FF FE` y transcodificar UTF-16LE→Latin-1 en un buffer aparte en `TodStringListReadFile` (`TodStringFile.cpp`) | resuelto |
| 3 | Glifos/fuentes rotos (texto ilegible) | El record `data/*.txt` del pak es el descriptor PopCap original (BOM UTF-8, 7257 B); el parser falla; la lista legacy buena (382 B) en la SD quedaba tapada porque FOpen buscaba el pak primero | En la rama NINTENDO_3DS de `PakInterface::FOpen`, un `data/*.txt` suelto en la SD se abre por filesystem ANTES del pak (`fcaseopen`). Además se añadió detección y salto de BOM UTF-8 en `DescParser.cpp` | resuelto |
| 4 | `std::bad_alloc` en `compiled/particles/*.xml.compiled` (kernel panic en crash handler) | Heap std pequeño. En 3DS no estaba habilitado `DefinitionReadCompiledStream` (solo PS2/Wii), intentando asignar el búfer descomprimido monolítico completo en el heap std | Habilitar `DefinitionStreaming` para `defined(__3DS__)`. Ahora descomprime por stream zlib directamente a los objetos sin requerir búfer intermedio gigante | resuelto en v14 |
| 5 | Barras negras/garbage intermitente en el borde (pantalla superior 400x240 con viewport 320x240) | El clear solo se hacía en Init; el display ping-pongea entre framebuffers (double/quad) | `GLInterface::Flush()` ahora hace `C3D_RenderTargetClear(gfxTarget, C3D_CLEAR_ALL, 0, 0)` cada frame | resuelto |
| 6 | Fuentes: `data/*.txt` suelto da "inner EOF at mPos=0" | PFILE según el loader no encuentra/lee el archivo suelto (branch loose) | Rama loose ahora abre `"rb"` + sonda `[FS]` (len+firstbyte). | resuelto |
| 7 | "Líneas y barras" / pantalla shredded por toda la superior | En v10 `C3D_RenderTargetCreate` se llamó con `(400, 240)`. Pero los framebuffers 3DS son PORTRAIT (240x400 para GFX_TOP); pasar 400x240 crea un stride mismatch de 400 vs 240 en el display transfer GX, destrozando la imagen en barras verticales | Cambiar a `C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8)`. (Además se mantiene filtro LINEAR para 2x downscale y `gfxSet3D(false)`) | **resuelto en v12/v13** |
| 8 | (debug) sin visibilidad de: corazón del main thread, progreso por recurso, uso real del heap, estado del FOpen suelto | — | Sondas: `[HB] frame %d alive` cada 240 frames en `Flush`; `[IP] completed '%s' %ld ms` por recurso en `TodResourceManager::TodLoadNextResource`; `[MEM][%s] std free=%u used=%u lin free=%u` (uordblks) en `LawnApp::Log3dsMemoryState`; `[FS]` en rama loose de PakInterface | activo en v10/v11 |
| 9 | `ToMortonTexture32/16/565`: flip vertical `dstY=tex->height-1-y` + canales `(a<<0)|(r<<8)|(g<<16)|(b<<24)` (byte0=alpha en memoria) | — | Un intento de "corregirlos" a RGBA8 recto sin flip (v8) se vio MÁS glitcheado y se revirtió. El código original es idéntico en `wszqkzqk/PvZ-Portable` (fuente del fork). **Mantener la referencia** y validar con Azahar antes de tocar. Los shaders `.pica` pasan `in_col` directo (sin shuffle) | decisión tomada (sin tocar) |
| 10 | Stall: el log se detiene en `about to load 'images/WateringCan'` (v6; log.txt=crash.log=282,349 B) | Sin resolver: ¿hang real en LoadingImages con la SD o lentitud? v7/v8 nunca llegaron a loguear (se corrió copia vieja) | Superado: los recursos cargan normalmente hasta el final de la lista de imágenes | superado |
| 11 | Logo de PvZ y barra de carga glitcheados / franjas horizontales | En `LawnApp.cpp` se insertaron `mResourceManager->DeleteImage(...)` para liberar RAM antes de partículas, borrando imágenes vivas que el `TitleScreen` estaba dibujando | Eliminadas las llamadas destructivas `DeleteImage` | resuelto en v14 |
| 12 | Controles inactivos / No inicia al presionar botones en título | `TitleScreen::KeyDown` y `MouseDown` exigen `mLoadingThreadComplete`. Como el hilo de carga crasheaba en partículas, nunca terminaba | Resuelto el crash en partículas (#4); ahora el hilo completa la carga y habilita los controles | resuelto en v14 |
| 13 | "Unknown Command on Line 1" en TODOS los font descriptors (`BrianneTod16`, `HouseOfTerror28`, `ContinuumBold14`, `DwarvenTodcraft*`, `Pix118Bold`, `Pico129`) | Parser `DescParser` usaba `CMDSEP_SEMICOLON` por defecto → esperaba `;` y sin indentación; archivos PopCap son multi-línea indentados. Además `PakInterface::FOpen` (rama 3DS) rechazaba loose files con first-byte `'8'`, impidiendo que la SD proveyera descriptores válidos. | `FontData::Load()` setea `mCmdSep = CMDSEP_NO_INDENT` (parsea indentación). `PakInterface::FOpen` ya no filtra por first-byte; abre `sdmc:/3ds/PlantsvsZombies/data/*.txt` con prioridad. | **resuelto en v15** |

## Pipeline gráfico 3DS (cómo funciona)

Archivo clave: `SexyAppFramework/platform/3ds/graphics/GLInterface.cpp` (+ `.h`).

- **Pan/Target**: el juego corre en la **pantalla SUPERIOR** (`GFX_TOP`), target
  `C3D_RenderTargetCreate(240, 400)` (orientación física de hardware en GPU) + `C3D_RenderTargetSetOutput(gfxTarget,
  GFX_TOP, GFX_LEFT, ...)`. La **pantalla INFERIOR** es del **console**
  (`consoleInit(GFX_BOTTOM)` en `Window.cpp`) = log en vivo.
- **Viewport/lettermbox**: 4:3 estricto. World 800x600 → viewport 320x240
  centrado en el target 400x240 (barras negras 40px a cada lado). Proyección
  `Mtx_OrthoTilt(0, mWidth-1, mHeight-1, 0, -10, 10, true)` (800x600).
- **Downscale de texturas 2x**: `TEX_DOWNSCALE_SHIFT=1`, min dim 8. Cada pieza
  del atlas se sube a MITAD de resolución (box filter 2x2 en
  `CopyImageToTexture8888/4444/565/Palette8`) y se muestrea normalizada contra
  las dims LÓGICAS. Ahorra ~4x de linear heap. Es la razón del #7: con NEAREST
  da moiré (0.8 texel/px), por eso ahora LINEAR.
- **Swizzle/Morton**: `ToMortonTexture32/16/565` convierten 8x8 tiles en orden
  Z (PICA200). `dst[(mortonX|mortonY) + (tileX*8) + (tileY*tex->width)]` es el
  layout lineal correcto. Se mantiene el flip vertical + swizzle `(a<<0)|(r<<8)`
  de la referencia (ver #9). `C3D_TexInit` dims pow2 (mín 8, máx 512;
  `MAX_TEXTURE_SIZE=1024`).
- **Batching**: vértices `GLVertex` (pos float3, color u32, uv float2) en un
  buffer global (MAX_VERTICES 65536); `GfxEnd()/GfxEnsureCapacity()` hacen
  `C3D_DrawArrays` y, si se llena el buffer, `C3D_FrameEnd/FrameBegin` intermedios.
- **Shaders**: `textured.v.pica` / `colored.v.pica` compilados a `_shbin.h`;
  fragment = citro3d (default env: `GPU_MODULATE` tex*color; sin tex:
  `GPU_REPLACE` color). Attr: v0=pos float3, v1=color ubyte4, v2=uv float2.
  Uniform `projection` por shader.
- **Frame**: `Flush()` = presentar el frame anterior (`C3D_FrameEnd`) + empezar
  (`C3D_FrameBegin(SYNCDRAW)`), `C3D_FrameDrawOn(gfxTarget)`, clear total por
  frame (fix #5), heartbeat `[HB]` cada 240 frames.
- **Transferencia**: `DISPLAY_TRANSFER_FLAGS` = RGBA8→RGB8, sin flip, sin tiled,
  sin scaling (400x240 nativo). citro3d maneja el doble buffer de pantalla.
- **Filtrado**: ahora LINEAR forzado por textura (fix #7). `gLinearFilter` sigue
  existiendo para el toggling del engine pero ya no se usa para el filtro 3DS.
- **Separación de pantallas**: juego = GFX_TOP (C3D), consola = GFX_BOTTOM
  (STDOUT de libctru). No compiten por el framebuffer.

## Memoria

- `memory_config.cpp`: `__ctru_heap_size=32MB` (std, malloc/new) +
  `__ctru_linear_heap_size=20MB` (texturas/VRAM). Símbolos doble-guion para
  3dsxtool: `__heap_size__=32`, `__linear_heap_size__=20`.
- Pico medido de linear en carga: ~12.6MB → 20MB sobra (texturas 2x lejos).
- std heap: cap 32MB sigue fallando con `bad_alloc` en partículas. `fordblks`
  begin=122KB con 24MB y con 32MB (¿el cap real del linker sigue siendo ~24MB?).
  **No zanjado**: hay que ver `[MEM] used=` (uordblks) en una corrida que llegue
  a partículas y dimensionar: subir std a 36MB quitando 4MB a linear, o hallar
  por qué "el heap libre" reporta 122KB al inicio.
- Texturas a mitad de resolución → si hace falta más std, se puede bajar linear
  a 16MB sin riesgo (pico 12.6MB).

## Input (ya completo, no stub)

`SexyAppFramework/platform/3ds/Input.cpp`: `hidScanInput()`; guarda estados
demo/previa; **touch screen → mouse** (0..320x240 → RemapMouse vía
`mPresentationRect`); Circle Pad + D-Pad = cursor secundario; botones A/B =
click; START → RETURN; combinaciones para menú. La consola ocupa la pantalla
inferior ahora, así que el touch físico ya no es el ratón en este setup de
prueba (el Circle Pad sí). Revisar al final según uso real.

## Archivos clave (con líneas)

- `SexyAppFramework/platform/3ds/graphics/GLInterface.cpp`:
  - `DISPLAY_TRANSFER_FLAGS` L38-41; `TEX_DOWNSCALE_SHIFT=1`/`TexDownSize` L18-36.
  - `GfxEnd/GfxAddVertices` L82-169; `SetTexture` (shader+attrs) L171-221.
  - `CalcZOrder` L225-234; `ToMortonTexture32/16/565` L239-323.
  - `TexReadSourcePixel32` L328-333; `CopyImageToTexture8888/4444/565/Palette8`
    L337-557; `CopyImageToTexture` (+ filto LINEAR) L561-584.
  - `GetTexture/GetTextureF` (UVs contra dims lógicas) ~L940-962;
    `SetLinearFilter` L966-972; `TextureData::Blt` L976+.
  - `UpdateViewport` (400x240, 320x240 centrado) ~L1454; `Init` (target GFX_TOP)
    ~L1487; `PreDraw` L1583; `Flush` ([HB] + clear) L1592-1614.
  - `Blt/BltClipF/StretchBlt/BltTransformed` (SetLinearFilter por-blt) L1700+.
- `SexyAppFramework/platform/3ds/graphics/Window.cpp`: `MakeWindow` —
  `gfxInitDefault` + `romfsInit` + CrashHandler + `C3D_Init` + consola en
  inferior (`consoleInit(GFX_BOTTOM)`) + `InitGLInterface`.
- `SexyAppFramework/platform/3ds/memory_config.cpp`: heaps 32/20.
- `SexyAppFramework/platform/3ds/CrashHandler_3ds.cpp`: `threadOnException` +
  logs a `sdmc:/3ds/PlantsvsZombies/{log,crash}.log`.
- `SexyAppFramework/platform/3ds/Input.cpp`: input completo.
- `SexyAppFramework/paklib/PakInterface.cpp`: `WiiChunkedFRead`, rama loose "rb"
  + `[FS]`, FOpen busca SD antes que pak para `data/*.txt` (ya no rechaza por first-byte `'8'`).
- `SexyAppFramework/graphics/ImageFont.cpp`: `FontData::Load()` L1148 — setea `mCmdSep = CMDSEP_NO_INDENT` para parsear descriptores PopCap multi-línea.
- `Sexy.TodLib/TodCommon.cpp`: `TodLoadNextResource` con sonda `[IP]`.
- `LawnApp.cpp`: `Log3dsMemoryState` (used=) L1972-1982; hilo de carga L1991-2230.

## Debug / telemetría

- **Sondas activas en v10/v11**: `[HB]` (main thread vivo), `[IP]` (progreso por
  recurso en ms), `[MEM] used=` (uordblks), `[FS]` (fopen suelto len+firstbyte),
  `[LOADPROBE]`/`[THROW]` (carga/throw). El log va a
  `sdmc:/3ds/PlantsvsZombies/log.txt` y ahora también **en vivo en la pantalla
  inferior** (consola).
- Plantilla de lectura: `[HB]` avanza + `[IP]` avanza = carga OK; `[HB]` sigue
  + `[IP]` parado = algo bloquea el hilo de carga; `[HB]` y `[IP]` parados =
  GPU/hilo main colgado; `[THROW] tipo=St9bad_alloc ra=0x...` = OOM (ver #4).
- Logs viejos útiles: `3ds_log.txt` / `3ds_crash.log` en la raíz del workspace.
- Azahar (emulador): ciclos cortos + capturas de pantalla. Pedir SIEMPRE el
  screenshot del emulador y/o foto de la consola para cerrar el loop visual.

## Decisiones clave (NO cambiar sin razón fuerte)

1. **No reescribir el backend gráfico.** `GLInterface.cpp` viene del fork
   3DS/PvZ-Portable y es la referencia (ver #9: el "fix" de Morton lo empeoró).
2. **`DOWNSCALE_COUNT=1`** (el juego dibuja en coordenadas 800x600 hardcodeadas;
   con 2 el layout quedaba recortado 400x300). El downscale real se hace en el
   backend de texturas (2x).
3. **Filtro LINEAL forzado** en las texturas 3DS (fix #7) — NEAREST con
   escala fraccionaria = moiré.
4. **Pantallas**: juego GFX_TOP (400x240 target, viewport 320x240 4:3) +
   consola GFX_BOTTOM. (En v6-v9 el juego estuvo en bottom; el usuario pidió
   volver a top con el log abajo.)
5. **Memoria**: 32MB std / 20MB linear vía `memory_config.cpp` (no `--defsym`).
6. **Audio mudo** salvo ids dummy (no blquea: minimal de 30fps se mide igual).
7. Touch como mouse quedó implementado; con consola abajo, el cursor real de
   prueba es el Circle Pad.

## Pendientes / próximos pasos (orden sugerido)

1. **Probar v15 en hardware real / Azahar**:
   - Confirmar que la pantalla superior renderiza el logo de PvZ y la barra limpios (sin franjas estáticas).
   - Confirmar que el parallax barrier 3D no genera scanlines interlineadas (`gfxSet3D(false)`).
   - Confirmar que la carga pasa de `MelonImpact` sin crashear gracias al streaming decompression.
   - Confirmar que al completarse la carga aparece el texto de inicio y los botones/touch reaccionan.
2. **Entrar al juego y probar Gameplay**:
   - Validar navegación en el menú principal (Aventura, Minijuegos, etc.).
   - Entrar al nivel 1-1 y medir fluidez (meta: >=30 FPS).
   - Validar controles de colocación de plantas (Circle Pad + A, y pantalla táctil).
3. **Guardado y Persistencia**:
   - Verificar que el progreso del usuario y el perfil se escriban correctamente en `sdmc:/3ds/PlantsvsZombies/userdata/`.
4. **Audio / SFX**:
   - Actualmente mudo mediante `DummySoundManager`; evaluar integración de audio ligero si la memoria lo permite.
5. **Empaquetado final (.cia)**:
   - Crear banner y generar instalable CIA para ejecución directa desde el menú Home.

## Referencias

- Repo de trabajo: `OptiJuegos/pvz-ps2-wii` (remote local)
  https://github.com/OptiJuegos/pvz-ps2-wii.git
- Backend 3DS de referencia / origen del GLInterface:
  `wszqkzqk/PvZ-Portable` https://github.com/wszqkzqk/PvZ-Portable
  (DeepWiki: Nintendo 3DS = citro3d, LOW_MEMORY + 2× downscale, "in development";
  su `ToMortonTexture32` usa el MISMO flip+swizzle que el nuestro).
- Decompilación cruzada: `ruslan831/PlantsVsZombies-decompilation` (v0.9.9).
- Log operativo y build/despliegue: `pvz-ps2-wii/AGENT.MD`.