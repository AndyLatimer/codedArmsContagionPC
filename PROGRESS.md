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

- [x] [NUEVO] **Cámara directa con ratón** (gancho en el integrador de giro del jugador, §10):
  sin simular botones, sin aceleración, independiente de los FPS; verificado con el ejecutable real
  (xdotool en Xvfb): +100 cuentas = +0,22 rad, arriba mira arriba.
- [x] [NUEVO/CA1] Controles FPS según el esquema A del juego: WASD, Shift correr, Ctrl sigilo,
  clic izq. disparar, clic der. ADS, Espacio saltar, R recargar, rueda cambiar arma, Esc pausa,
  F1 ajustes del PC. Mando: stick derecho con apuntado directo.
- [x] [NUEVO] **ADS** reescrito para Contagion: arma y brazos se mueven hasta poner la punta del
  punto de mira delantero en el centro (con alabeo corregido), FOV del nivel interpolada,
  sensibilidad reducida, cruz oculta; se dispara normalmente (munición 9→8→7 verificada) y el
  fogonazo sale de la boca del cañón en ADS.
  Configuración `controls.ini [ADS]` con las claves pedidas y `weapons.ini` por arma (nombre
  interno leído del juego, `g01` = pistola).
- [x] [NUEVO] FOV configurable (85 por defecto; sin recortes visibles a 85 y 100).
- [x] 60 FPS (máximo nativo) o límite de 30; la lógica del juego va por tiempo (§10).
- [x] Configuración separada: `graphics.ini`, `controls.ini`, `weapons.ini`, `config.ini`.
- [x] Ayudas de teclas del PC en los pies de menú (× → Intro, △ → Retroceso), verificado.
- [x] Tutorial: tras la radio de diagnóstico la primera puerta se abre y se recorre el pasillo
  siguiente (la puerta solo espera a que termine la radio; no es un fallo del port).

## No funciona todavía
- [ ] Teclas 1–5 para elegir arma directamente: el cambio de arma no pasa por la entrada del control
  del jugador (§10); hay que llegar con varias armas (galería de tiro del tutorial) para localizarlo.
- [ ] Correspondencia g02…g13 ↔ armas sin verificar (solo hay pistola al empezar el tutorial).
- [ ] "E interactuar": el juego no tiene botón de interactuar en el esquema A; sin asignar.
- [x] Compilación de Windows con todo lo anterior; probada bajo Wine en Xvfb: menús, perfil,
  tutorial, ratón directo, ADS con clic derecho y disparo con clic izquierdo.
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
Órdenes del FIFO de `ca_psprun --control`: press/hold/release/stick/shot/memdump/aim/ads/fov/fps/matdump.
Variables útiles del runtime: `CA_HOOK_TRACE` (gancho de giro y posición), `CA_ADS_DEBUG`,
`CA_WATCH_ADDR` + `CA_WATCH_STACK` (build de vigilancia), `CA_HLE_STATS=1` (llamadas más frecuentes), `CA_THREAD_DUMP=s` (hilos y
event flags), `--trace` (llamadas al sistema con su resultado), `CA_GPU_DEBUG`, `CA_GPU_XFER`,
`CA_DUMP_UPLOAD=dir`, `CA_ATRAC_VERBOSE`. Control interactivo: `--control FIFO` (press/hold/stick/shot).

## Siguiente paso concreto
1. Ranuras de arma (teclas 1–5): el inventario es un objeto aparte (vtable 08c1da3c, punteros a
   las armas en +0x50) sin ruta estable desde el personaje todavía; hace falta una segunda arma
   (avanzar el tutorial) para localizar el cambio de arma y verificar g02….

## Registro de decisiones
- 2026-10-28: Recompilación estática reutilizando el motor de CodedArmsPC (sin modificar ese repositorio).
- 2026-10-28: Lo específico de cada juego se separa por DISC_ID; los ganchos de CA1 no se aplican.
- 2026-10-28: PSMF_JK.PRX se recompila (es código del juego y su API no es la de Sony) en vez de HLE.
- 2026-10-28: ATRAC con AtracCtx2 de PPSSPP (la variante que lee la memoria del juego).
- 2026-10-28: Ratón inyectado en el delta de giro del juego (no en la matriz): mismo camino que el mando.
- 2026-10-28: ADS en el renderizador (la lógica del juego no cambia; los disparos van al centro).
- 2026-10-28: Sin parche de frame rate: el juego ya va a 60 y su lógica depende del tiempo.
