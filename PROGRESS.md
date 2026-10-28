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

## No funciona todavía
- [ ] `sceAtrac*` (14 funciones): música ATRAC3 del menú y de los niveles.
- [ ] `jkPsmfPlayer` (11, vídeos FMV): cargar y recompilar `PSMF_JK.PRX` o HLE como PPSSPP.
- [ ] Teclado en pantalla (Osk) y red: sin implementar (red no prevista).
- [ ] No comprobado aún si llega a dibujar (falta ejecutar con `--gl` y capturas).
- [ ] Ganchos de Contagion (cámara con ratón, ADS, fps): por localizar; motor distinto de CA1.
- [ ] Compilación de Windows (`scripts/build_windows.sh`) no probada aún para Contagion.

## Cómo reproducir
```
python3 tests/test_inspect_iso.py
cmake -S . -B build/linux -G Ninja -DCA_GAME_ISO=$PWD/game/Coded.Arms.Contagion.iso
cmake --build build/linux
ctest --test-dir build/linux
./build/linux/cpu_diff
cd build/run && ../linux/ca_psprun --iso ../../game/Coded.Arms.Contagion.iso --seconds 40
```
Variables útiles del runtime: `CA_HLE_STATS=1` (llamadas más frecuentes), `CA_TRACE`, ver `src/psp/psprun.cpp`.

## Siguiente paso concreto
1. Ejecutar con `--gl --shot` bajo Xvfb para ver si el menú se dibuja.
2. Portar `sceAtrac` de PPSSPP (el decodificador ATRAC3/3+ ya está en `src/psp/ppsspp/at3_standalone`).
3. Vídeos (`jkPsmfPlayer`), luego localizar la actualización de cámara del jugador para el ratón.

## Registro de decisiones
- 2026-10-28: Recompilación estática reutilizando el motor de CodedArmsPC (sin modificar ese repositorio).
- 2026-10-28: Lo específico de cada juego se separa por DISC_ID; los ganchos de CA1 no se aplican.
