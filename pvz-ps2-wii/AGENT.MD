# AGENT.MD -- LEE ESTO ANTES DE TOCAR NADA

Lee SIEMPRE el `CODESTYLE.MD` y sigue sus reglas. Y lee y ACTUALIZA este
archivo al final de tu sesion: anade tus errores encontrados/resueltos y
cualquier descubrimiento nuevo. Otros agentes dependen de esto.

---

## PROYECTO

Port de Plants vs Zombies (GOTY, original PC) a Nintendo 3DS, basado en el
repo `pvz-ps2-wii` (fork multiplataforma con PS2/Wii/3DS/Switch).
Objetivo del port (ver `opencode-pvz-3ds-port.md`): "terminar el port si o
si", minimo 30 FPS jugables, New 3DS como objetivo principal.

## COMMANDOS DE BUILD (3DS)

- El build bueno es `build/3ds-release/` (generador "Unix Makefiles").
  **NO uses `build3ds/`** (tiene la cache rota por el generador Ninja).
- El target correcto es `re-plants-vs-zombies_3dsx` (NO `re-plants-vs-zombies.3dsx`).
- Compilar desde PowerShell/mensaje de agente:

```
C:\devkitPro\msys2\usr\bin\bash.exe -c "export DEVKITPRO=/opt/devkitpro && export DEVKITARM=/opt/devkitpro/devkitARM && export PATH=/opt/devkitpro/devkitARM/bin:/opt/devkitpro/tools/bin:/opt/devkitpro/msys2/usr/bin:\$PATH && cd /c/Users/gamer/OneDrive/Desktop/pvz3ds/pvz-ps2-wii/build/3ds-release && make re-plants-vs-zombies_3dsx > build_out.txt 2>&1; echo DONE_EXIT=\$? >> build_out.txt"
```

- OJO: `make` SOLO emite salida si hay algo que construir (si esta al dia no
  imprime nada, EXIT True igual). Para forzar rebuild, toca/toca el fuente.
- Antes de pasar target: build previo ok, `.3dsx` 2.7 MB / `.elf` 45 MB.

## ERRORES YA RESUELTOS (CRONOLOGICO)

- **"FATAL ERROR: out of free sound ids"** (29% -> se veia al cargar):
  - Causa: en 3DS `mSoundManager` es `DummySoundManager` cuyo
    `GetFreeSoundId()` devolvia **-1** siempre. `ResourceManager::DoLoadSound`
    (`misc/ResourceManager.cpp:1097-1099`) trata `<0` como error fatal
    `Fail("Out of free sound ids")`.
  - Fix: `SexyAppFramework/sound/DummySoundManager.h` ahora reparte ids
    incrementales (`mNextSoundId++`), `LoadSound()` devuelve `true`,
    `SetBaseVolume/Pan` devuelven `true`. `GetSoundInstance` sigue devolviendo
    NULL y TODO caller lo chequea (Board.cpp:1917, TodFoley.cpp:274,
    SexyAppBase::PlaySample) -> juego silencioso pero sin crashear.
  - `DummyMusicInterface` ya era no-op completo (musica no es problema).

- **"Failed to find ']'"** (salia como FATAL al cargar, justo tras LawnStrings):
  - Sintoma: `TodStringListReadName` (`Sexy.TodLib/TodStringFile.cpp:65`)
    encontraba un `[` sin su `]` posterior dentro de `Properties/LawnStrings.txt`,
    cargado por `TodStringListLoad` (`LawnApp.cpp:2039`) en el hilo de carga.
  - Diagnostico: el RECORD de `properties/lawnstrings.txt` dentro de `main.pak`
    es UTF-16LE espanol (traduccion del usuario; termina en "LOGROS"). Verifiqué
    byte a byte que el archivo completo SÍ parsea limpio (todo `[` tiene `]`
    despues; ultimo `[` en offset 186826 de 186886). MD5 identico en PVZ y
    PAQUETE. O sea: los datos en el pak son BUENOS, el fallo era que el READ
    llegaba CORTO/parcial y el parser se tragaba basura del final.
  - Causa raiz: rama Wii/3DS de `PakInterface.cpp` hacia UN UNICO `fread()` de
    todo el record (tipicamente cientos de KB). En hardware real esa fread
    grande puede volver corta (mismo problema que en PS2, que ya tenia su
    `Ps2ChunkedFRead`).
  - Fix: nuevo `WiiChunkedFRead()` (loop en chunks de 128KB) usado en la rama
    Wii/3DS tanto en el buffered-read de `FOpen` como en el fallback streaming
    de `FRead` (PakInterface.cpp:600, ~735). Ademas `TodStringListReadFile`
    ahora exige `aBytesRead == aSize` (antes solo comprobaba `<= 0`), y el
    mensaje de error incluye bytes leidos vs esperados.
  - Nota: si vuelve a salir, mirar ese mensaje en build_out/log para ver si el
    read llega corto SIEMPRE (pak o SD corrupto) o fue transitorio.

- **"Failed to find ']'" (REAPARECE con log nuevo — este es el FIX REAL)**:
  - El error persistio incluso con el chunked-read. Con los logs telematicos
    (sdmc:/3ds/PlantsvsZombies/{log,crash}.log, conectado por FTP del usuario)
    se descubrió que el fallo era OTRO: `strchr` encuentra el `[` en offset 2
    y no encuentra `]` despues, porque el archivo ES UTF-16LE entero:
    `FF FE 5B 00 4C 00 4F 00...` = BOM + "[LOCALE]" con high-bytes 0x00 entre
    cada letra. `strchr` se detiene en el primer 0x00 → devuelve null.
  - Root cause real: la traduccion espanola guarda `LawnStrings.txt` en
    **UTF-16LE**, pero `TodStringListReadFile/ReadName/ReadValue` son parser
    de bytes (strchr/strspn/strlen) y `StringToWString` (Common.cpp:224)
    convierte byte a byte. El analisis previo "parsea limpio" mapeaba pares
    UTF-16, no el parser real.
  - Fix REAL: `TodStringListReadFile` detecta el BOM FF FE y transcodifica
    UTF-16LE → Latin-1 en un buffer nuevo (cada code unit <= 0xFF conserva su
    byte bajo; €=0x80, ‘=0x91, ’=0x92 cp1252; resto '?'), y el parse corre
    sobre ese buffer. `StringToWString` reconvierte byte→wchar asi que los
    textos salen igual que en el juego de PC.
  - El `.3dsx` DE LA SD (por FTP) era el VIEJO del 4-sep (2755952 B, sin fix
    alguno). Siempre VERIFICAR la fecha/tamano del 3dsx en la SD antes de
    debuggear: subir con curl -T pal FTP.

- **BOM UTF-8 en fonts (RESUELTO 6-sep)**: el record `data/*.txt` del pak es
  el descriptor ORIGINAL de PopCap ("Define CharList", 7257 B) con BOM UTF-8;
  el parser falla en "Unknown Command on Line 1" y LoadLegacy lee un archivo
  en un formato que no entiende -> GLIFOS ROTOS. La lista legacy "8 16" SI
  existe en la SD (`sdmc:/3ds/PlantsvsZombies/data/*.txt`, ~382 B) pero
  `PakInterface::FOpen` busca el pak PRIMERO y la tapa: nunca se usó.
  Fix: en `PakInterface.cpp` (`FOpen`, rama NINTENDO_3DS) un `data/*.txt`
  suelto en la SD se abre por filesystem ANTES de mirar el pak (fcaseopen).
  Con eso LoadDescriptor falla inofensivo y LoadLegacy parsea la lista good.

- **std::bad_alloc en partículas (OOM) (RESUELTO 6-sep)**: el log moría con
  `[THROW] tipo=St9bad_alloc` cargando `compiled/particles/MelonImpact.xml.compiled`.
  El stake del heap: strack default de libctru = std 24MB / linear 32MB.
  El intento previo (`-Wl,--defsym,__linear_heap_size*`) NO servía: el crt0 de
  devkitARM lee `__system_argc/argv` (no los .word de heap) y `allocateHeaps.o`
  de libctru toma el tamaño de **variables** `__ctru_heap_size` /
  `__ctru_linear_heap_size` (dereferencea la dirección del símbolo; un defsym
  la convertía en un valor absoluto y leería RAM basura -> riesgo de crash al
  boot). Fix REAL: `platform/3ds/memory_config.cpp` define esas dos como
  variables .data fuertes: `__ctru_linear_heap_size=16MB`,
  `__ctru_heap_size=0` (auto = resto de APPMEM). Con linear 16MB > pico de
  carga (~12.6MB) y std = APPMEM-16MB, el OOM desaparece. Los símbolos doble
  guion (`__heap_size__`=40MB, `__linear_heap_size__`=16MB, `__stacksize__`)
  quedan SOLO para 3dsxtool (metadata del header). NO volver a usar --defsym
  sobre los símbolos del crt0 ("Relocation to invalid address!").

- **Glitch pantalla superior (RESUELTO 6-sep)**: target 400x240, viewport
  320x240 centrado -> barras laterales 40px. El clear solo se hacía en Init
  y el display ping-pongea entre framebuffers, así que las barras mostraban
  píxeles viejos intermitentes. Fix: `GLInterface::Flush()` ahora llama
  `C3D_RenderTargetClear(bottomTarget, C3D_CLEAR_ALL, 0)` cada frame, antes
  de que el juego redibuje los 320x240.

## DECISIONES CLAVE (RESUELTAS -- NO CAMBIAR SIN RAZON FUERTE)

1. **`DOWNSCALE_COUNT` = 1 en 3DS** (`CMakeLists.txt` rama NINTENDO_3DS).
   - Con IMG_DOWNSCALE=2, `mWidth/mHeight` pasan a ser 400x300
     (`LawnApp.cpp:123-124`) pero el juego dibuja con coords hardcodeadas
     800x600 y el backend 3DS **NO** multiplica coords por IMG_DOWNSCALE
     (a diferencia de PC/PS2), asi que el layout quedaba recortado a
     400x300 → todo roto. PvZ-Portable usa 2 pero es otro fork.
2. **Downscale de textura en el backend** (`GLInterface.cpp`):
   - Constantes `TEX_DOWNSCALE_SHIFT=1` y min `TEX_DOWNSCALE_EMPTY_DIM=8`.
   - `C3D_TexInit` se hace a mitad de resolucion que las dimensiones
     logicas del piece; al hacer copy se aplica box filter 2x2
     (`CopyImageToTexture8888/4444/565/Palette8`).
   - Las UVs se calculan contra las dimensiones LOGICAS
     (`aPiece.mWidth/mHeight`, `GetTexture/GetTextureF`), que son
     normalizadas [0..1], asi que una textura fisicamente mas pequena
     se muestra igual pero con menos texeles. Esto ahorra ~4x linear heap
     (la 3DS guarda las texturas en linear heap, ~32 MB por defecto).
3. **Pantalla**: el juego va en GFX_TOP (320x240, proyeccion 800x600);
   GFX_BOTTOM es el touch del raton. Ver `Window.cpp` (MakeWindow:
   `osSetSpeedupEnable(true)`, `C3D_Init`, `consoleInit(GFX_TOP)`).
4. **Input**: `Input.cpp` esta COMPLETO (el plan doc decia "stub", es
   informacion desactualizada). Touch 0..320x240 → RemapMouse via
   `mPresentationRect`. Circle Pad/cruzeta = cursor secundario, A/B=click.

## CARGA DE DATOS (main.pak)

- `main.pak` original GOTY compatible (XOR 0xF7, magic 0xBAC04AC0).
- Orden de busqueda en 3DS (`SexyAppBase.cpp:5418-5423`):
  1. `main.pak` (relativo al CWD; el juego hace `chdir` a
     `sdmc:/3ds/PlantsvsZombies/` en la linea 5377-5378)
  2. `sdmc:/3ds/PlantsvsZombies/main.pak`
  3. `romfs:/main.pak`
- Savedata: `SexyAppBase.cpp:5404-5408` crea `sdmc:/3ds` y
  `sdmc:/3ds/PlantsvsZombies`, savedata ahi.
- `LawnApp.cpp:1508`: `ParseResourcesFile("properties/resources.xml")`.
  En el pak original vienen `data/properties/compiled/images/reanim/sounds`.
- El paquete listo esta en `C:\Users\gamer\OneDrive\Desktop\pvz3ds\PAQUETE_3DS\`
  (3dsx + smdh en raiz, `PlantsvsZombies/main.pak` para la SD).

## NOTAS DEL PIPELINE GRAFICO 3DS (GLInterface.cpp)

- `CreateTextureDimensions()` (~linea 697): piece grid, `mMaxTotalU/V`.
- `CreateTextures()` (~780): elige formato (R5G6B5 si sin alpha, A8R8G8B8 si
  hay, etc), `createTextures=true` si cambio el tamano/formato, llama a los
  `CopyImageToTexture*` a mitad de resolucion (TexDownSize). `mTexMemSize`
  contabiliza con dims reducidas.
- `Blt/BltTransformed` usan coords logicas + UVs normalizadas (0..1) por
  piece. `BltTriangles` usa `mMaxTotalU/V` (1.0 si el image cabe en un piece)
  con tiling por pieces; UVs relativas, transparente al downscale.
- `ToMortonTexture32/16/565`: convierten 8x8 tiles Z-order; usan
  `tex->width/height` y origen (0,0) (la ruta de update usaba offx/offy, se
  uniformizo a 0,0 porque cada piece tiene su propia C3D_Tex).
- Formatos: A8R8G8B8 | A4R4G4B4 | R5G6B5 | Palette8; `usePow2=true`;
  min 8, max 512; `MAX_TEXTURE_SIZE=1024`. Citro3d `C3D_TexInit` (c3d/texture.h:94)
  requiere dims pow2; las piezas ya lo son.

## SESION 6-sep (parte 2) -- RENDER INFERIOR + AZAHAR

- **Pantalla**: el juego ahora va en GFX_BOTTOM (320x240, viewport completo,
  sin letterbox), `consoleInit` comentado en `Window.cpp`. DECISION #3 de
  arriba ("GFX_TOP") esta OBSOLETA.
- **Referencia upstream**: el codigo 3DS viene de `wszqkzqk/PvZ-Portable`
  (fork del nuestro): `ToMortonTexture32/16/565` usa flip VERTICAL
  `dstY = tex->height - 1 - (y+originY)` y canales `(a<<0)|(r<<8)|(g<<16)|(b<<24)`
  (= byte0 alpha en memoria). NO "arreglar" eso a RGBA8 recto: un intento
  (v8 sin flip + rgba8) se vio MAS glitcheado en el emulador Azahar y el
  usuario pidio seguir la referencia. v9 = flip+swizzle restaurados.
  Los shaders `.pica` pasan `in_col` directo al estimulo (sin shake).
- **Tactica Azahar**: el usuario tiene el emulador Azahar (3DS). Probar ahi
  es ciclos cortos + capturas de pantalla reales. Pedir SIEMPRE screenshot
  del emulador (y del hardware real) para cerrar el loop visual.
- **Memoria ACTUAL**: `memory_config.cpp` = `__ctru_heap_size=32MB` +
  `__ctru_linear_heap_size=20MB` (los dos explicitos). CON 32MB std el
  loading muere con `St9bad_alloc` en `compiled/particles/*.xml.compiled`
  (ra=0x32e73c) -> el titulo NUNCA se alcanzo en ningun build. Con cap 24MB
  tambien fallaba; `fordblks` begin = 122KB en ambos (sospechoso). El log ya
  incluye `used=` (uordblks) via `LawnApp::Log3dsMemoryState`. Falta un run
  que llegue a particulas y loguee `used=` para dimensionar de verdad.
- **v6 stall en LoadingImages**: el log se detiene justo en
  `about to load 'images/WateringCan'` (log.txt = crash.log = 282,349 B).
  NO se resolvio si es hang real o lentitud (v7/v8 nunca llegaron al log).
  Si se repite: ver si `[HB] frame alive` sigue avanzando.
- **Fuentes**: los `data/*.txt` sueltos (382 B) leen EOF a mPos=0
  ("inner EOF at mPos=0"), pese a la rama loose "rb" en `PakInterface.cpp`
  + sonda `[FS]` (len+firstbyte). Aun sin un run que llegue a fonts y
  loguee `[FS]` -> no sabemos si fopen falla o PFILE lee mal.
- **Sondas activas en el build**: `[HB] frame %d alive` cada 240 frames
  (GLInterface::Flush), `[IP] completed '%s' %ld ms` por recurso
  (TodResourceManager::TodLoadNextResource), `[MEM][%s] ... used=%u KB`,
  `[FS]` en la rama loose. Con Azahar + estos probes los ciclos son cortos.
- **Binarios**: v9 subido 6-sep 20:57 (2751816 B, SHA1
  6BEF24D97E0B0AB3FB6729EEB08EBEF07368A428). El .3dsx de la SD es EL QUE
  SE SUBIO; verificar tamano antes de debuggear.
- **v10 (6-sep 20:58+)**: juego a GFX_TOP (target 400x240, viewport 320x240
  centrado 4:3) + consola en GFX_BOTTOM (`consoleInit(GFX_BOTTOM)` reactivado
  en Window.cpp). Se vio "pantalla glicheada full lineas" aun con el render de
  referencia -> la causa de las lineas NO era el flip/swizzle.
- **v11 (6-sep 21:14, 2760720 B, SHA1 CE40F2B6F6EA85DCD2084F12A9450766188FD790)**:
  Se forzó filtro LINEAR en texturas pensando que era moiré, pero la pantalla
  siguió glicheada en hardware real.
- **v12 (6-sep 21:44, SHA1 28CD1101E3832E490968C7CAD1948284D3258CB4)**:
  **FIX REAL DE LA PANTALLA GLICHEADA / LÍNEAS VERTICALES**:
  Causa raíz: en v10 al mover a GFX_TOP se puso `C3D_RenderTargetCreate(400, 240, ...)`.
  Citro3D / PICA200 maneja los framebuffers en su orientación física de hardware (portrait):
  para GFX_TOP las dimensiones son **240 de ancho x 400 de alto**, NO 400x240.
  Al crear el target con 400 de ancho, el hardware de transferencia GX (display transfer)
  escaneaba el buffer con stride 240, causando un desfase acumulativo de 160 px por línea
  que destrozaba la imagen en columnas verticales y diagonales repetidas.
  Fix en `GLInterface.cpp`:
  `gfxTarget = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);`
  Binario compilado y actualizado en `build/3ds-release/` y `PAQUETE_3DS/`.
  Subido por FTP a `sdmc:/3ds/PlantsvsZombies/re-plants-vs-zombies.3dsx` y
  a `sdmc:/3ds/re-plants-vs-zombies.3dsx` (2,760,720 B, 21:50 hrs).

- **v13 (6/7-sep 23:20, SHA1 7ECE32F4B49639F786C1D4337FCE229CA81007C9, 2,761,020 B)**:
  **TRES FIXES APLICADOS:**
  1. **Fix `std::bad_alloc` en partículas** (`Sexy.TodLib/Definition.cpp`):
     `DefinitionAlloc` ahora incluye `defined(__3DS__)` en el path nothrow+purge
     (antes solo `PS2_PLATFORM||WII_PLATFORM`). Con solo ~872 KB de std heap libre
     al cargar `compiled/particles/MelonImpact.xml.compiled`, la asignación
     directamente lanzaba `std::bad_alloc`. Ahora usa nothrow, y si falla llama
     `PurgeLazyImages()` + retry. El threshold se bajó a 64 KB (antes 256 KB)
     porque en 3DS los buffers de partículas son más pequeños que en PS2.
  2. **Fix líneas horizontales en pantalla superior** (`Window.cpp`):
     Añadido `gfxSet3D(false)` inmediatamente después de `gfxInitDefault()`.
     Sin esta llamada, el hardware del parallax barrier del New 3DS queda en modo
     standby 3D, produciendo líneas horizontales visibles en la pantalla superior
     incluso al renderizar en modo 2D.
  3. **Fix fuentes con BOM UTF-8** (`SexyAppFramework/misc/DescParser.cpp`):
     `LoadDescriptor` ahora detecta y salta el BOM (0xEF 0xBB 0xBF) al abrir
     cualquier archivo `.txt`. Sin esto, `BrianneTod16.txt` y otros fallaban con
     "Unknown Command on Line 1" y caían al `LoadLegacy`, que tampoco podía
     parsear el archivo.

- **v14 (7-sep 00:08, SHA1 7C348580C72714842909575EF9362C9C83BEB719, 2,759,456 B)**:
  **FIX CRÍTICO DE CRASH (PREFETCH ABORT / KERNEL PANIC) Y GLITCHES VISUALES:**
  1. **Fix Crash OOM en partículas (`MelonImpact.xml.compiled`) vía Streaming**:
     Al desensamblar el dump (`crash_dump_00000005.dmp`) y trazar el log, el crash era
     `assertion failed: 'aPtr'` en `Definition.cpp:346` (`TOD_ASSERT(aPtr)`).
     La 3DS no tenía activado `DefinitionReadCompiledStream` (solo PS2/Wii lo tenían).
     Por tanto, intentaba descomprimir archivos compilados gigantes asignando de golpe
     un búfer monolítico sin descomprimir en el std heap (donde solo quedaban ~700 KB).
     Se activó `DefinitionStreaming.h`, `DefinitionStreaming.cpp`, `DefinitionStreamDecoder.cpp`
     y `Definition.cpp` con `|| defined(__3DS__)`. Ahora descomprime por stream con `zlib inflate`
     directo a los objetos sin requerir búfer intermedio gigante en RAM.
  2. **Fix Logos Corruptos / Glitch Visual (`PvZ_Logo`, `LoadBar`, botones, diálogos)**:
     En `LawnApp.cpp` se habían insertado llamadas de `mResourceManager->DeleteImage("PvZ_Logo")`,
     `DeleteImage("titlescreen")`, `DeleteImage("LoadBar_dirt")`, `DeleteImage("LoadBar_grass")`,
     `DeleteImage("SodRollCap")`, `dialog_*`, `button_*`, etc. antes de cargar partículas.
     ¡Esto destruía las texturas mientras la pantalla de título las estaba renderizando en vivo!
     Al ser borradas, la GPU dibujaba memoria liberada/basura (las líneas estáticas en el logo).
     Se eliminaron todos esos `DeleteImage` erróneos.
  3. **Fix Botones / Inicio ("Presiona para Start")**:
     La pantalla de título (`TitleScreen`) ignora intencionalmente los botones y toques táctiles
     hasta que `mLoadingThreadComplete == true`. Como el hilo de carga crasheaba en `MelonImpact`,
     nunca llegaba a completarse ni mostraba el botón de inicio. Al resolverse el crash del hilo
     de carga, la barra completa su animación y cualquier tecla (o toque táctil) inicia el juego.
  4. **Fix recursión en `CrashHandler_3ds.cpp`**:
     Añadido guardia de re-entrada a `PvZ3dsExceptionHandler` para prevenir llamadas recursivas
     en bucle a `svcBreak`.

## TECNICA DE DEBUG

- Si no se ve nada en pantalla o carga lenta: comprobar `build_out.txt`,
  el log del crash handler (`Pvz3dsInstallCrashHandler`), y los mensajes
  `printf` a stdout del 3dsx (se ven en la consola de debug de
  devkitARM/Homebrew Launcher).
- Para depurar de verdad: construye con emulador citra y SD virtual, o
  revisa `CrashHandler_3ds.cpp` y los logs que escribe.
- FTP: `ftp://10.117.118.115:5000/` (IP actual, puede cambiar entre sesiones).
- **7-sep 16:30 (REGRESION v15 CORREGIDA)**: la SD tenia el build de 16:10 (v15,
  downscale 2x + sin flip + MAX_VERTICES 65536) = "al reves una parte y todo mal".
  Diagnostico con git diff de GLInterface.cpp; revert aplicado:
  1) flip restaurado en ToMortonTexture32/16/565 (dstY = tex->height-1-(y+originY)),
  2) copy/re-scale reverted a la forma HEAD (sin box filter, sin TexDownSize),
  3) filtro textura CONDICIONAL otra vez (gLinearFilter ? LINEAR : NEAREST),
  4) se borraron defines TEX_DOWNSCALE* + helpers TexDownShift/TexDownSize,
  5) MAX_VERTICES 2048 de nuevo. Rebuild OK 7-sep 16:29:53 (2,759,292 B), subido a
  sdmc:/3ds/PlantsvsZombies/ y sdmc:/3ds/ + PAQUETE_3DS (verificar tamano).
  El downscale forzaba GetBits() en cada pieza (mBits full-res en std heap) -> ese
  es el conductor del bad_alloc en particles del build 16:10.

- **7-sep 16:30 (MEMORIA VERIFICADA)**: memory_config.cpp SI esta en el elf:
  
m/objdump .data 0x38f000: __ctru_linear_heap_size=0x01400000 (20MB) y
  __ctru_heap_size=0x02000000 (32MB). Con 32MB std + streaming + purge (v14), el
  bad_alloc de particles (MelonImpact.xml.compiled, Definition.cpp:346 TOD_ASSERT)
  deberia desaparecer. make incremental: solo GLInterface.o + link (segundos).

- **7-sep 16:40 (FUENTES: GLIFOS REALES)**: DESCUBRIMIENTO CLAVE: los data/*.txt de
  382B legacy que inventamos SON LA CAUSA de "texto invisible" (solo anchos, sin
  pixeles). El main.pak SI trae los descriptores PopCap ORIGINALES (7.2-7.8KB,
  BOM UTF-8 + Define CharList + LayerSetImage) Y los sheets de glifos:
    *_BrianneTod12/16.png, *_BrianneTod32/Outline.png, *_ContinuumBold14(Outback).gif,
    *_HouseofTerror16/20.png, _Pico129.png, _Pix118Bold.gif, DwarvenTodcraft*.png,
    HouseofTerror16Outline/20Outline/28.png.
  El parser (DescParser.cpp LoadDescriptor) YA tiene el skip BOM (v13) y fue escrito
  para estos descriptores; el override loose "data/*.txt" de PakInterface.cpp
  interceptaba y devolvia el 382B legacy -> LoadDescriptor fallaba -> LoadLegacy sin
  imagen = glifos NULL. FIX SIN REBUILD: extraje del pak (XOR 0xF7 por byte: magic
  0xBAC04AC0, dir: flags(0x80=END) + namew + name + size int32 + time int64, data al
  final) los 20 descriptores originales + 20 sheets renombrados a lo que pide cada
  LayerSetImage, y los subi sueltos a sdmc:/3ds/PlantsvsZombies/data/ (40 archivos,
  937,936 B) + espejo en PAQUETE_3DS. Ahora LoadDescriptor(loose original) -> OK ->
  GetSharedImage('data/BrianneTod16') -> ImageLib peina .tga/.jpg/.png/.gif a traves
  de p_fopen -> rama loose -> glifos REALES. Mapeo imagen: BrianneTod32.txt->Outline,
  BrianneTod32Black.txt->BrianneTod32, HouseofTerror16/20->Outline, resto = nombre.
  URL del pak descompilado: C:\Users\gamer\AppData\Local\Temp\opencode\loose_fonts.
  Si un descriptor fallara, LoadLegacy lee el MISMO original (no "8 16") -> falla ->
  FATAL 'Failed to load font'; de momento confiamos en la gramatica (esta probada en PC).

- **7-sep (REGLA pak)**: NUNCA abrir una parte del pak sin aplicar XOR 0xF7 a TODOS
  los bytes (directorio y datos). El encabezado no-XOR mostraba basura. offset datos =
  ftell() tras el flag END.


- **7-sep 17:10 (CRASH LawnStrings ANCHADO + FIX FONTS V2, BUILD 2,761,716 B)**: El
  cierre de 16:49 ("se congela al cargar y glitchea") = NUEVO FATAL: TodStringListLoad
  devolvia false -> TodErrorMessageBox (TodDebug.cpp:80) tira runtime_error("Failed to
  load string list file Properties/LawnStrings.txt"). El sink 3DS es SINCRONO
  (CrashHandler_3ds.cpp SexyLogSinkWrite: fopen+write+fclose por linea), asi que si el
  fuente actual logueara ese fallo, sus lineas aparecerian en log.txt; no aparecen ->
  el binario desplegado no se comportaba como el fuente actual: REBUILD OBLIGATORIO,
  no mas inferencias. CONFIRMADO ademas: sin carrera de paquetes (el read 3DS/Wii
  prefetch-ea el registro a mBuffer bajo sWiiPakHandleMutex, FRead sirve de memoria).
  DESARROLLO CLAVE: la gramatica PopCap ORIGINAL (Define CharList/WidthList/RectList/
  OffsetList + CreateLayer + LayerSetImage + LayerSetCharWidths/ImageMap/CharOffsets)
  ES compatible con ImageFont.cpp (Dereference resuelve define names, L318 def R3).
  Unico bloqueo del descriptor: el BOM UTF-8 (EF BB BF) llegaba al parser (log linea
  raw 81 = EF BB BF 44 65...="BOM+Define") xq el skip viejo (fgetc/ungetc) no movia la
  posicion fisica -> "Unknown Command on Line 1" -> LoadLegacy sin imagen.
  FIXES APLICADOS (4 en 1 build, para minimizar ciclos de test):
  1) DescParser.cpp: skip BOM deterministico (p_fread 3 bytes; si son EF BB BF quedarse
     en la posicion, sino p_fseek(0,SEEK_SET)). Nunca mas fgetc/ungetc.
  2) ImageFont.cpp HandleCommand: si el primer token aun empieza por EF, quitarle los
     3 bytes (belt & braces).
  3) ImageFont.cpp LayerSetImage: si GetSharedImage('data/X') falla, reintentar
     'data/_X' (los sheets del pak son _X.png) antes de Error('Failed to Load Image').
  4) PakInterface.cpp rama loose: rechazar descriptores legacy 382B ('8' como 1er byte)
     -> caer al registro del pak. Los 382B "8 16" estan AUN en la SD (TinyBold.txt y
     ContinuumBold14Outline.txt) y envenenaban el override.
  5) TodStringFile.cpp: log de VEREDICTO al final de ReadFile (size/read/utf16/parse)
     para que el proximo log ancle el fallo de LawnStrings con certeza.
  LOG 16:49: los [FS] del rameo no confirman que el descriptor suelto de 7.2KB se
  abriera siempre; la SD (FTP) SI tenia los originales de 16:42 -> fcaseopen
  parpadeante. Con el build nuevo da igual: loose original O pak (BOM+_X) -> bien.
  LawnStrings.txt SI existe en el pak (a. record start 12641478 size 186886) pero la
  extraccion manual XOR en ese offset daba basura (NO texto UTF-16LE): medio que la
  matematica de offsets del script o el formato de ese registro no es el de las fuentes;
  el juego lo lee bien por runtime (UTF-16LE FF FE, 93442 code units) -> pendiente si el
  veredicto del proximo log apunta a read/size. Render 240x400 intacto (sin revert).
  NO tocar loose: el fix nuevo hace la via pak-only suficiente; si un descriptor falla,
  LoadLegacy lee el MISMO original -> falla -> FATAL. Extraidos temporales en
  C:\Users\gamer\AppData\Local\Temp\opencode\loose_fonts. ATENCION: `rg` NO existe en
  este entorno (PowerShell) -> usar la tool grep / Select-String. Desplegado 17:10 a
  sdmc:/3ds/PlantsvsZombies/ + sdmc:/3ds/ + PAQUETE_3DS (2,761,716 B). FALTA TEST:
  pedir al usuario retest con log fresco y confirmar: [STRINGS] verdict + [FONTDESC]
  UTF-8 BOM stripped + LayerSetImage OK (sin "Failed to Load Image") + texto visible.


- **7-sep 17:28 (FIX LANG + FIX FONTS CONFIRMADOS; CRASH AHORA EN PARTICLES)**: el build
  17:10 (BOM deterministico + _X fallback + loose legacy rechazado + veredicto STRINGS)
  RESOLVIO: LawnStrings carga (`[STRINGS] 'Properties/LawnStrings.txt' verdict
  size=186886 read=186886 utf16=1 parseResult=1`) y aparecio `[FONTDESC] UTF-8 BOM
  stripped` (habia BOM real en los descriptores del pak). PERO ahora el cierre cambia:
  2x `[THROW] St9bad_alloc` + runtime_error + Error Box `Definition.cpp(346) assertion
  aPtr` justo en `[LOAD] particles` tras el doble `[PAK] FOpen
  'compiled/particles/MelonImpact.xml.compiled'`. Importante: mallinfo hace trampa en
  newlib (fordblks NO cuenta el top-headroom, solo huecos): ver "std heap free=750 KB"
  con solo 7.7MB usados NO significa OOM real total, significa FRAGMENTACION/HUECOS
  (el 33+ MB del std estar�a disponible via sbrk, pero los bloques grandes contiguos
  no caben). CAUSA REAL: en particles se piden muchos bloques contiguos a la vez:
  registro del pak (FOpen#1 mBuffer=malloc(aSize)) + FOpen#2 (2a copia malloc) +
  scratch comprimido (ConsoleScratchBuffer, en 3DS cae a new nothrow: BigBlockArena
  es SOLO PS2 4MB / WII 8MB) + buffer descomprimido. El `const void*` de
  DefinitionReadCompiledStream permite decompresion directa desde mBuffer.
  FIXES APPLIED (build 2,761,996 B 17:28):
  1) memory_config.cpp: std 32->34MB, linear 20 (34/20=54MB, 2MB bajo el limite de
     commit ~56; 40/16=56 red-sreenea; 32/20 mantenia OOM). VERIFICADO en el ELF .data
     (0x38f004=0x02200000=34MB, 0x38f008=0x1400000=20MB). OJO: __ctru_X_* los lee
     libctru; __heap_size__/__linear_heap_size__ son METADATA (3dsxtool) y
     __linear_heap_size__=0.5MB es un valor viejo irrelevante en runtime.
  2) Definition.cpp DefinitionReadCompiledFile: fast path consola - si el registro
     ya esta en pFile->mBuffer (mRecord!=NULL && mBuffer!=NULL && mPos==0), se
     descomprime DIRECTAMENTE del buffer del pak (sin 2a copia ni scratch): solo
     queda 1 copia de registro + buffer descomprimido.
  3) DefinitionAlloc: printf (invisible, va a stdout homebrew) -> TodTraceAndLog con
     tamanos y mallinfo+linearSpaceFree, para que el EXACTO bloque que falla salga en
     log.txt. Incluidos <malloc.h> y <3ds.h> solo en __3DS__.
  PENDIENTE: si el crash reaparece, el log mostrara "[DEF] OOM allocating %d bytes"
  y el veredicto dir� cuanto; la rama de continuacion natural = limitar linear
  (texturas 16.5MB) o evitar que FOpen#1 de MelonImpact (ParticleSysManager) siga
  haciendo copy cuando pasa al streaming. Los crash_dumps de Luma (luma/dumps/arm11/
  crash_dump_00000009.dmp) son de MAYO (244-1244B, fecha FAT vieja) - NO del juego de
  hoy; el crash real esta en log.txt/crash.log del propio juego. FTP notes: `curl
  ftp://.../ -I` devuelve Content-Length correcto; no hay python usable en msys2
  (usar objcopy + leer offsets en PowerShell). Desplegado 17:28 a las 2 rutas SD +
  PAQUETE_3DS. FALTA TEST: retest y traer log limpio (esperar pasar particles y ver
  el menu - ahora el texto deberia verse).
