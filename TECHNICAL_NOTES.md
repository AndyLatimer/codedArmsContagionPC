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

## 6. Música: sceAtrac  ✅
FMOD reproduce la música con `sceAtrac` en modo streaming. Se porta el reproductor de PPSSPP
(`AtracCtx2`, contextos en memoria del juego). El `AtracCtx` antiguo no sirve: FMOD llama a
`sceAtracAddStreamData` **antes** de lanzar la lectura asíncrona que rellena ese espacio, y AtracCtx
copiaba los datos en ese momento (memoria aún vieja → "Sound Unit id 2f", la música se cortaba a los
10 s). Medido con `--wav`: música continua.

## 7. Vídeos: PSMF_JK.PRX (reproductor de Konami)  ✅
- El juego carga `USRDIR/MODULE/PSMF_JK.PRX` ("jkPsmfP_library", en claro) e importa sus 23 funciones
  `jkPsmfPlayer`. Comparten los NID de `scePsmfPlayer` de Sony pero **no su ABI** (`Create` recibe
  480×272 donde Sony espera buffer/tamaño). Por eso se recompila el módulo original con el juego, como
  hace PPSSPP al ejecutarlo; él usa `sceMpeg`/`scePsmf` (HLE).
- Módulo de 2 segmentos LOAD (código en 0, datos en 0x12540) con reubicaciones relativas al
  segmento 1; sin `module_start` (su syslib solo exporta variables). Se carga en 0x09FE0000 (reservado
  al arrancar). BOOT exporta 469 funciones `sceLibc` que el módulo importa.
- Usa la ruta YCbCr de sceMpeg (`AvcDecodeYCbCr` + `AvcCsc` a VRAM 0x04154000) y el juego dibuja ese
  búfer como textura. Alfa 0 en los píxeles, como en la PSP (PPSSPP igual).

## 8. Errores del runtime de CA1 encontrados con Contagion
- **Event flags**: el modo de espera 0x20 es `PSP_EVENT_WAITCLEAR` (borra los bits esperados) y 0x10
  `WAITCLEARALL`; estaban al revés. Los hilos MC y Decode del reproductor esperan el mismo flag (0x21):
  el primero borraba todos los bits y el segundo no despertaba nunca.
- **Sin apropiación**: un hilo que espera activamente (sin llamadas al sistema) a otro de más prioridad
  bloqueaba todo (controlThread 0x088e1a40 sondea `*(s2+348)`). Ahora los saltos hacia atrás sondean
  `rt_preempt_flag`, que activa un temporizador del host (§5 de recomp.h).
- **GPU**: una subida de VRAM escrita por la CPU a su FBO a mitad de fotograma dejaba sin destino
  enlazado y se perdían los dibujos siguientes (el vídeo salía negro).
- **Recompilador**: `bltzal`/`bgezal` escriben `ra` aunque no salten (§4).
  Todos están corregidos aquí; CodedArmsPC no se ha modificado.

## 9. Flujo de juego verificado (ca_psprun --gl, control por FIFO)
Idioma → vídeos (Konami, GameSpy, Rycar, aviso de autoguardado; START los salta) → título → Create
Profile (teclado en pantalla) → menú principal → Single Player → New Game → dificultad → autoguardado
(diálogo de savedata, "Save completed") → FMV_1 → Main Terminal → Missions → Mission 0: Tutorial →
**nivel L0 jugable**: movimiento con el stick, giro con △/□, disparo con R (la munición baja), radio con
retrato. Los toques cortos (<0,15 s) se pierden: el juego sondea el mando a su ritmo de fotogramas.

## 10. Cámara, control, arma y ritmo del jugador (build de vigilancia, `CA_WATCH_ADDR`)  ✅
- **Vigilancia**: el build `-DCA_RECOMP_WATCH` informa también de los stores VFPU (`sv.s`,
  `sv.q`, `svl/svr.q`, palabra a palabra) y, con `CA_WATCH_STACK=1`, de una traza heurística de
  la pila del invitado (palabras que parecen direcciones de retorno).
- **Orientación**: no hay ángulo en la matriz del jugador (`0928f870`, rehecha cada fotograma
  por `SetDirection` 0x088778d4); la dirección sale de `f_089745e8` a partir de **yaw = +0x50 y
  pitch = +0x54** del objeto jugador (radianes): dir = (cos p·sin y, sin p, −cos p·cos y). Más
  yaw gira a la derecha, más pitch mira arriba.
- **Integrador**: el control del jugador `f_0897196c(jugador s3, estado nuevo s2, entrada s1, dt)`
  llama en 0x08971e20 a `TurnInput 0x089c0378(…, &dYaw = sp+236, &dPitch = sp+232)` y suma
  `estado+16 += dYaw`, `estado+20 += dPitch`; después el juego envuelve el yaw (0x0887de3c) y
  limita el pitch. El gancho `ca_hook_player_turn` (tras 0x08971e28) suma ahí el giro del ratón:
  pasa por el mismo camino que el mando. Verificado: +100 cuentas → +0,220 rad exactos.
- **Ritmo**: el juego actualiza una vez por espera de vblank (60/s sin el coste del render) y su
  lógica va por tiempo real: giro con □ = 1,674–1,676 rad/s a 30, 48 y 60 actualizaciones/s.
  El límite de 30 FPS solo alinea las esperas a vblanks pares (`hle_devices.cpp`).
- **Stick**: zona muerta ≈0,3; la velocidad crece con la inclinación hasta ~3,8–4,5 u/s a fondo
  (medidas con ruido por colisiones). No hay "correr" aparte: el front end usa 0,7 (andar),
  1,0 (correr, Shift) y 0,45 (sigilo, Ctrl).
- **Esquemas de control** (Settings > Controls), esquema A por defecto: L saltar, R disparar,
  ◀ cambiar arma, ▶ recargar, ▼ fijar blanco, ▲ mira telescópica, stick mover/lateral,
  △×□○ mirar, SELECT visor, START pausa. Acciones del motor sin botón propio en la PSP: Run,
  Creep, Grenade, Weapon Slot 1–5 (solo el esquema D asigna "Weapon Slot 5").
- **Arma actual**: `*(*(jugador+0x29C)+0x20)` = personaje (vtable 08c20fec), `+0x54` = objeto
  del arma, cuya primera palabra apunta a su nombre interno ("g01" = pistola, verificado). La
  tabla de textos lista Pistol, Rifle, Shotgun, Sniper Rifle, RPG, Submachine Gun, Pulse Rifle,
  Grenade Launcher, Lightning Gun, Gauss Rifle, Data Leech, Nova Generator, Vulcan Cannon.
  Verificado (modo dev): g01 pistola, g02 subfusil, g03 rifle, g04 escopeta, g05 Vulcan,
  g06 francotirador, g07 Gauss, g08 pulsos, g09 rayos, g10 Nova, g11 Data Leech,
  g13 lanzagranadas, g19 RPG. Munición del cargador en arma+0x1C.
- **Render del arma**: vista identidad y la misma proyección que el nivel (`proj[0]` = 1,192,
  FOV horizontal 80° a 480×272) con plano cercano más próximo; brazos = draws con pesos de
  skinning; arma = el resto. Radar: viewport 56×25. Ver `src/psp/gpu/ads.cpp`.
- **Entrada del control del jugador** (`s1` de `f_0897196c`, `CA_INPUT_TRACE=1`): +0x34 indicadores
  (0x40 disparo, 0x20 fijar blanco), +0x30 impulso de salto, +0x14/+0x2c mirada con botones. Cambiar
  de arma, recargar y la mira telescópica no pasan por ahí: los trata otro sistema.
- El objeto de `personaje+0x54` recibe el arma en su constructor (`f_08949624`), así que
  probablemente es el "arma en mano" y se recrea al cambiar de arma.

## 11. Lua del juego, consola de desarrollo y armas  ✅
- **Lua**: variante 5.1-work (bytecode: versión 0x51, números `float`; tras EQ/LT/LE/TEST va
  CALL, sin TESTSET). Biblioteca base en 0x08a2d4d8 (loadstring), `print` vacío. Funciones C:
  luaL_loadbuffer 0x08a2a66c, lua_pcall 0x08a0a7a4, lua_gettop 0x08a0e488, lua_settop 0x08a0e418.
  Pila: TObject de 8 bytes (valor +0, tipo +4: 0 nil, 1 bool, 3 número, 4 cadena).
- **Consola** (`ca_hook_lua_pcall`, tras la primera instrucción de lua_pcall): ejecuta los trozos
  pendientes en el estado Lua del juego y escribe el resultado como `[LUA]`. `string.dump` +
  un desensamblador propio permitieron leer las funciones del juego (sin nombres de locales).
- **Armas**: la tabla global `wpn` (metatabla `__index` 0x08a93ab0 → 0x08a712d8) da el cargador
  C++: `loadWeapon(sbf, ranura0, tipo)`, `setWeapon(ranura0)`, `unloadWeapon`… y los tipos
  PISTOL=1 … VIRAL_GRENADE=18. Recursos `weaponN.sbf` (N = tipo); solo hay 3 búferes de
  0x23200 bytes además de la pistola: **caben 3 armas cargadas a la vez**.
- El HUD de armas (`_import_cached_cache.loader.cfg.objects.powerups.GetHandler().ammo`,
  hud.lua) lleva las ranuras (`slots[1..5]`, nombres "pistol", "rifle"…): `UnlockWeapon(nombre)`
  (lo que hace recoger un arma: busca ranura, caja "loading plugin"), `LoadWpn(ranura, nombre)`
  (descarga la del hueco, carga y cambia, sin caja), `SetWeapon(nombre)` (cambiar). Una ranura
  admite un arma si su `wpn_type` ≤ índice (0-3); la 5.ª es de granadas (`CheckWeaponAvailability`).
- **Modo dev** (F5/F6): `LoadWpn(4, siguiente)` con munición infinita; al terminar de descargar
  el juego llama a `LoadWeapon` (con caja), que se desvía una vez a `LoadWpn`.
  **Teclas 1–5**: `SetWeapon(slots[k])`.
- **ADS por arma** (ajustado con `adsw` en `ca_psprun`): la medida automática (borde superior)
  falla con miras altas o laterales; `weapons.ini` trae Pitch/Roll/SightOffsetY para el rifle,
  la escopeta, Gauss, pulsos, lanzagranadas y RPG; la Vulcan solo hace zoom y el francotirador
  usa la mira del juego (▲: apagada → x2 → x4 → apagada; el front end pulsa ▲ hasta que la
  escala de proyección del nivel, `g_levelProjY`, coincide con el clic derecho).
- **Cadencia**: `rate` de `player_state` son disparos/s (rifle 10, subfusil 15, escopeta 0,97:
  medidos iguales). `SetUpgrades` aplica FIRERATE solo si el árbol de mejoras del arma tiene
  `rate`; la pistola (y el francotirador) no: usan su ritmo interno, 0,77 s (pistola) y ~1 s
  (francotirador) entre disparos, por tiempo y no por fotogramas; `wpn.setWeaponParam(1,
  FIRERATE, …)` no lo cambia. `CA_WEAPON_TRACE=1` muestra los cambios del objeto del arma.

## 12. Hilos y memoria
- Borrar un hilo libera su pila (bloque de la partición de usuario). Antes se perdía: FMOD
  (FSOUND_Close/Init al salir de un nivel, guardar, cargar) crea y borra su mezclador (64 KB) y
  su hilo de streaming cada vez; tras varios reinicios ya no había bloque para la pila y el juego
  seguía sin sonido. `createThread` registra ahora el fallo ("no memory for its stack").
