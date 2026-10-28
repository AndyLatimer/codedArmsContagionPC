# PROGRESS — Coded Arms: Contagion PC port

Procedencia: **[CA1]** reutilizado de `AndyLatimer/CodedArmsPC` (port del primer Coded Arms) ·
**[PPSSPP]** código o datos de PPSSPP (GPL-2.0+) · **[NUEVO]** escrito para este proyecto.

## Arquitectura
Recompilación estática, como CA1: `BOOT.BIN` (MIPS Allegrex, en claro) se traduce a C en `build/`
a partir de la ISO del usuario y se compila con un runtime HLE que sustituye al sistema de la PSP,
un renderizador GE→OpenGL 3.3 y un front end SDL2. El código del juego es el original; nada del
juego se sube al repositorio. Detalles verificados: `TECHNICAL_NOTES.md`.

## Completado (verificado)
- [x] [NUEVO] `tools/inspect/inspect_iso.py` (Fase 1) + `tests/test_inspect_iso.py` (20/20).
- [x] Análisis de la ISO del usuario: ULES-00718, BOOT.BIN en claro, 274 importaciones, motor distinto
  del de CA1 (FMOD 3, SBF, scripts GM/Lua). Hashes en `TECHNICAL_NOTES.md` §1.
- [x] [CA1] Motor integrado: recompilador, runtime HLE, GPU, front end, pruebas `cpu_diff` y `savedata`.
  Lo específico de CA1 está archivado en `docs/ca1/`; lo de Contagion va en `src/games/ULES00718/`
  y `tools/recomp/games/ULES00718/`.
- [x] [NUEVO/PPSSPP] `tools/recomp/gen_imports.py`: tabla de importaciones y `hle_nids.cpp` a partir de las
  tablas HLE de PPSSPP.
- [x] BOOT.BIN recompilado: 14 826 funciones, sin instrucciones sin soporte; `cpu_diff` 75 503 palabras,
  solo `sqrt.s` (signo de NaN, 3 palabras) difiere.
- [x] [NUEVO] Corrección del recompilador para branch-and-link locales (descompresor de Contagion).
- [x] [NUEVO/PPSSPP] HLE añadido: `sceKernelPrintf` (el juego imprime su registro), memoria volátil,
  VTimer, FPL, `sceKernelReferThreadStatus`, `WaitSemaCB`, atributos/prioridad de hilo, versiones de SDK,
  módulos, `sceAudioOutput2*` (canal SRC usado por FMOD), `__sceSasCoreWithMix`, grano/modo/pausa de SAS,
  `sceIoWaitAsync`, `sceIoSync`, `sceDisplayGetFrameBuf`, `sceCtrlGetSamplingMode`,
  `sceRtcGetAccumulativeTime`, `sceUmdWaitDriveStatCB`, `sceUtilityLoad/UnloadModule`.
- [x] **Primer ejecutable nativo** `CodedArmsContagion` (Linux) y `ca_psprun` compilan con el código de
  Contagion. Ejecutado sin ventana: el juego arranca, inicializa pantalla y FMOD, descomprime
  STARTUP.SBF, ejecuta AUTOEXEC.GM y **carga la escena del menú** (MENU.SBF, sonidos, texturas) y la
  música del menú.

- [x] [PPSSPP] `sceAtrac` (AtracCtx2): música del menú continua.
- [x] [NUEVO] Recompilación multimódulo: `PSMF_JK.PRX` (reproductor de vídeo de Konami) recompilado con
  BOOT.BIN, importaciones enlazadas entre módulos; [PPSSPP] `scePsmf` y la ruta YCbCr de `sceMpeg`.
  Los vídeos se ven y suenan.
- [x] [NUEVO] Apropiación de hilos (sondeo en saltos hacia atrás + temporizador).
- [x] Corregidos errores heredados de CA1: modos de event flag, subida de FBO a mitad de fotograma.
- [x] **Primera escena jugable**: del idioma al título, perfil, menú, partida nueva con autoguardado,
  cinemática, terminal y **misión tutorial L0** con movimiento, cámara, disparo, HUD y radio
  (TECHNICAL_NOTES §9). Por ahora con los controles de PSP.

## No funciona todavía
- [ ] Ganchos de Contagion (cámara con ratón, ADS, fps): por localizar; motor distinto de CA1.
- [ ] Compilación de Windows (`scripts/build_windows.sh`) no probada aún para Contagion.
- [ ] Teclado del sistema (Osk) y red: sin implementar (red no prevista). El juego usa su propio teclado.
- [ ] `sqrt.s` de NaN negativo (cpu_diff, 3 palabras).

## Cómo reproducir
```
python3 tests/test_inspect_iso.py
cmake -S . -B build/linux -G Ninja -DCA_GAME_ISO=$PWD/game/Coded.Arms.Contagion.iso
cmake --build build/linux
ctest --test-dir build/linux
./build/linux/cpu_diff
cd build/run && ../linux/ca_psprun --iso ../../game/Coded.Arms.Contagion.iso --seconds 40
```
Variables útiles del runtime: `CA_HLE_STATS=1` (llamadas más frecuentes), `CA_THREAD_DUMP=s` (hilos y
event flags), `--trace` (llamadas al sistema con su resultado), `CA_GPU_DEBUG`, `CA_GPU_XFER`,
`CA_DUMP_UPLOAD=dir`, `CA_ATRAC_VERBOSE`. Control interactivo: `--control FIFO` (press/hold/stick/shot).

## Siguiente paso concreto
1. Compilación de Windows y prueba del front end (ventana, teclado/ratón mapeados a botones).
2. Localizar la actualización de cámara del jugador (yaw/pitch) con el build de vigilancia para el
   apuntado directo con ratón; después el ADS sobre la pistola.

## Registro de decisiones
- 2026-10-28: Recompilación estática reutilizando el motor de CodedArmsPC (sin modificar ese repositorio).
- 2026-10-28: Lo específico de cada juego se separa por DISC_ID; los ganchos de CA1 no se aplican.
- 2026-10-28: PSMF_JK.PRX se recompila (es código del juego y su API no es la de Sony) en vez de HLE.
- 2026-10-28: ATRAC con AtracCtx2 de PPSSPP (la variante que lee la memoria del juego).
