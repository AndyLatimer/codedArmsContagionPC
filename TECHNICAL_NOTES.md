# TECHNICAL_NOTES — Coded Arms: Contagion (PSP) → PC

Solo se documenta lo **verificado** con la copia del usuario. Las notas del primer juego
(motor distinto) están en `docs/ca1/TECHNICAL_NOTES_CA1.md`.

## 1. Imagen de referencia
`Coded.Arms.Contagion.iso` — ULES-00718 (Europa, multilenguaje), 604 241 920 bytes.
MD5 `b0a5541dea88fbb0a0b0b059b25ebdf2`, SHA-1 `08bbeec07d16d7c284be5d6dde30dc0dd485d7aa`, CRC32 `37a93e82`.
UMD_DATA.BIN `ULES-00718|DB4EE758C851C6DD|0001|G`. PARAM.SFO: TITLE `Coded Arms® Contagion`, DISC_VERSION 1.06,
PSP_SYSTEM_VER 3.80, PARENTAL_LEVEL 7. 583 ficheros. Fuente: `tools/inspect/inspect_iso.py`.

## 2. Ejecutables
- `SYSDIR/BOOT.BIN` 6 020 796 B, SHA-1 `add4f640589c846341bac7b13572b5b4cbedfeb0`: **PRX ELF en claro**
  (tipo 0xFFA0), módulo `CA2`, 26 secciones, un segmento, base 0x08804000, entrada 0x08b79f10,
  gp 0x08c395f0. Reubicaciones de tipo 2/4/5/6 únicamente (20 257 / 67 078 / 50 040 / 50 298):
  el cargador PRX de CA1 vale sin cambios.
- `SYSDIR/EBOOT.BIN` 6 021 136 B: el mismo módulo cifrado (`~PSP`, "CA2"). No hace falta.
- 274 funciones importadas de 29 bibliotecas; 234 con nombre según las tablas HLE de PPSSPP
  (`tools/recomp/games/ULES00718/imports.tsv`, generado por `gen_imports.py`). Las 40 sin nombre
  son de `gamespy_sdk` (juego en línea).
- `USRDIR/MODULE`: dos módulos del juego en claro (`GAMES7BC.PRX` = GameSpy, 283 KB;
  `PSMF_JK.PRX` = reproductor de vídeo `jkPsmfPlayer`, 103 KB) y nueve de Sony cifrados (`~SCE`:
  sceSAScore, sceATRAC3plus_Library, sceAudiocodec, sceMpeg, sceMpegbase, scePsmf, sceFont,
  sceVideocodec, sceNetApDialogDummy) que el runtime sustituye (HLE).

## 3. Motor
Distinto del de CA1. Evidencias en el disco y en los mensajes que imprime el juego (`sceKernelPrintf`):
- Scripts de configuración `USRDIR/CONFIG.GM`, `AUTOEXEC.GM` (directorios `Scenes`, `Textures`,
  `Sound`, `weapons`, `bike`, `man`; variables `REND_ZMin`, `VIDEO_MipMaps`, `Alt+Enter`, `VModeSX`,
  `LuaMemoryHeapSize` = 1 500 000; comentario `D:/Projects/ShadowKing/...`): motor multiplataforma
  con origen en PC y scripts Lua.
- Sonido **FMOD 3** (`FSB3`, mensajes `FSOUND_*`, hilos "FMOD Software Mixer thread", "FMOD
  Streamer thread"); música en ATRAC3 (`SOUND/CD/*.AT3`) por `sceAtrac`.
- Escenas y texturas en `.SBF` (`SCENES/<nivel>/<nivel>.SBF` y `...SCENE.SBF`, `TEXTURES/*.SBF`,
  `STARTUP/STARTUP.SBF`), algunos con cabecera `FBSZ`. Comprimidos: el juego los descomprime con una
  rutina Huffman escrita a mano en ensamblador Allegrex (0x088e0990–0x088e0e54: usa `ext`, `ins`,
  `rotrv`, `bitrev`, `clz` y subrutinas locales llamadas con `bltzal`).
- Pantalla: 480×272, 32 bits, stencil de 8 bits, "2 FB + ZB: 1.3 Mb" (mensaje del juego).
- Niveles L0–L13 con variantes B1–B3, MENU, MP1–MP4 (multijugador).

## 4. Recompilación de BOOT.BIN  ✅
- El recompilador de CA1 descubre 14 826 funciones y traduce todas las instrucciones (ninguna sin
  soporte); el C generado compila con GCC.
- Prueba diferencial (`cpu_diff`, intérprete de referencia de PPSSPP): 75 503 palabras de instrucción
  distintas, 4 832 004 pruebas; solo difieren 3 palabras de `sqrt.s` en el signo del NaN (pendiente).
- **Corrección necesaria para Contagion** (`functions.py`, `codegen.py`): un branch-and-link
  (`bltzal`/`bgezal`) hacia la misma función se emitía como llamada C. El descompresor llama así a
  subrutinas que a veces no vuelven (al acabar un bloque saltan al principio del bucle, 0x088e09c0);
  el `jr ra` final volvía entonces al marco C exterior y la descompresión se repetía sin fin. Ahora:
  el enlace se escribe siempre (también si no se salta, como en el hardware), el salto es un `goto`
  y `jr ra` en esa función va a la dirección de retorno local que corresponda o vuelve.
- Los ganchos del port van por juego (`tools/recomp/games/<DISC_ID>/hooks.tsv`); los de CA1 caían
  en direcciones de Contagion y se han quitado.

## 5. Arranque observado (`ca_psprun`, sin ventana)  ✅
Secuencia actual: tablas de idioma (7 `.TBL`) → `CONFIG.GM` → memoria volátil (0x08400000, 4 MB) →
inicialización de pantalla → módulos de audio → FMOD (`__sceSasInit(256 muestras, 32 voces)`,
salida por `sceAudioOutput2`) → `STARTUP.SBF` (descomprimido) → `AUTOEXEC.GM` → escena del menú
`SCENES/MENU/MENU.SBF`, bancos `MENU.FSB`/`MENU1.FSB`, texturas `TEX_M7EB.SBF` → música
`A_MENU_1.AT3` (falla: `sceAtrac` sin implementar).
El juego busca cada fichero primero en `USRDIR/` y después en su subdirectorio (errores 0x80010002
esperados).
