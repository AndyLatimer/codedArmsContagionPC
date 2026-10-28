# TECHNICAL_NOTES — Coded Arms (PSP) → PC

Solo se documenta aquí lo **verificado** contra los datos reales. Cada entrada indica
cómo se comprobó. Lo que es hipótesis se marca como tal.

Imagen de referencia: `Coded Arms (Europe)` — ULES-00124, ISO de 193 855 488 bytes,
SHA-1 `fabc93b30962f9e4df25c9ecbc7f7098ebe31c4f`, MD5 `7ba77364feed5efbb381660541c95b93`.
PARAM.SFO: TITLE `CODED ARMS`, DISC_ID `ULES00124`, versión de disco 1.00, PSP_SYSTEM_VER 1.50.

## 1. Contenido de la ISO (ISO 9660 estándar)

```
UMD_DATA.BIN                       "ULES-00124|18AF588988526506|0001|G"
PSP_GAME/PARAM.SFO, ICON0.PNG, PIC1.PNG, ICON1.PMF
PSP_GAME/SYSDIR/BOOT.BIN           3 336 488 B  ELF32 MIPS (Allegrex) SIN CIFRAR
PSP_GAME/SYSDIR/EBOOT.BIN          3 336 832 B  "~PSP" cifrado (nombre de módulo "Coded_Arms")
PSP_GAME/SYSDIR/UPDATE/*           actualización de firmware (no relevante)
PSP_GAME/USRDIR/Arc/MAIN.ARC       40.5 MB  475 entradas
PSP_GAME/USRDIR/Arc/DG.ARC         20.9 MB   12 entradas (escenarios)
PSP_GAME/USRDIR/Arc/CASTEST.ARC     4.7 MB    1 entrada  (CASVIEW.BPX)
PSP_GAME/USRDIR/SOUND/SeData.bin   "KSS SE Link Data  04/16/2005"
PSP_GAME/USRDIR/SOUND/TrSeData.bin "KSS BGM Link Data  03/24/2005"
PSP_GAME/USRDIR/SOUND/WaveData.bin "KSS Wave Link Data  04/09/2005" (24 MB)
PSP_GAME/USRDIR/movie/*.pmf        vídeo PSMF ("PSMF0014"): opening, entry, ending, sector00/01
PSP_GAME/USRDIR/module/*.prx       módulos de sistema (red, audio, mpeg)
```

**Consecuencia clave:** `BOOT.BIN` es el ejecutable en claro (cabecera `7F 45 4C 46`,
`file`: "ELF 32-bit LSB, MIPS-II"). Se puede analizar estáticamente (Ghidra + plugin
Allegrex/PSP) **sin descifrar nada** y por tanto sin eludir protecciones.

## 2. Contenedor ARC — "KCEO ARCDT 1.0"  ✅ verificado

Comprobado en los 3 archivos (488 entradas): offsets crecientes, todo dentro del archivo,
el final de la última entrada redondeado a 0x800 coincide con el tamaño del archivo.

| Offset | Tipo | Significado |
|---|---|---|
| 0x00 | char[16] | `KCEO ARCDT 1.0\0\0` |
| 0x10 | u32 | número de entradas N |
| 0x14 | u32 | tamaño de sector (2048) |
| 0x18 | u32 | tamaño de tabla = N·36 |
| ... | | relleno con `-` hasta 0x800 |
| 0x800 | entrada[N] | 36 bytes: char[24] nombre, u32 sector inicial, u32 tamaño, u32 = 0 siempre |

Tras la tabla hay un array de u16 (¿índice ordenado/hash?) — no necesario para extraer.
Herramientas: `tools/arc_tool.py`, `src/assets/Formats.cpp: parseArc`.

## 3. Contenedor BPX  ✅ verificado

Anidable (1506 BPX anidados en total). Cabecera:

| Offset | Tipo | Significado |
|---|---|---|
| 0x00 | char[4] | `BPX\0` |
| 0x04 | u32 | tamaño total |
| 0x08 | char[16] | nombre interno (p.ej. `gamecommon_eur`, `tp`) |
| 0x18 | u32 | número de entradas N |
| 0x1C | u32 | offset de una tabla de N u32 (offsets de datos, redundante) |
| 0x20 | u32 | offset de la tabla de nombres |
| 0x24 | u32 | desconocido (0 o un offset) |
| 0x28 | u32 | inicio de datos |

Tabla de nombres: entradas de 32 bytes = char[24] nombre, u32 offset, u32 tamaño.
Comprobado en TPP_TITLE.BPX: `TEX.PSPIP` @0x100 tamaño 0x280F0 y `FILETBL.BPX` @0x28200
tamaño 0x7F, que terminan exactamente en el tamaño total.

Tipos encontrados dentro (por extensión → magia): PSPM (`PSM\x02`, modelo), PSPIP
(`PSPI01`, texturas), MOT (`MOT1`, animación), BIN (`COLL`, `PSM\x02`, `PSPI01`, `BDB0`…),
CLM (`CLM\0`), WPT, PLT, RLT (`light_room…`), C2B (`CAS2`, interfaz), F2B (`f2b`, texto),
RLVDEFBIN (`BIN ROOT…`), RLVSNAP.

## 4. Textura PSPI ("PSPI01")  ✅ verificado visualmente

```
0x00 char[8]  "PSPI01\0\0"
0x08 u16 nImages, u16 nPalettes
0x0C u32 inicio de datos
0x10 registros de 24 bytes: primero nImages imágenes, luego nPalettes paletas
  imagen : u16 id, u16 paletteId, u16 w, u16 h, u32 bpp, u16 bufW, u16 bufH, u32 dataOff, u32 ?
  paleta : u16 id, u16 nColores, u32 dataOff, 16 bytes a 0
```
- Paletas RGBA8888 (orden de bytes R,G,B,A).
- Píxeles con el **swizzle de la PSP** (bloques de 16 bytes × 8 filas). Sin deshacer el
  swizzle la imagen sale corrupta; deshecho, aparece correcta (logo CODED ARMS,
  fondo del título). Los índices de 4 bpp usan primero el nibble bajo.
- Estadística global: 1803 imágenes de 4 bpp, 192 de 8 bpp, 2 de 32 bpp,
  13 con `bpp = 65540` (variante **no descifrada**: CELLEFFECT, BOX, LEVELMAP, WALLBOX).
- Validado: 2090/2103 imágenes decodifican (`ca_assetcheck`).

## 5. Modelo PSPM ("PSM\x02")  ✅ geometría y texturas verificadas visualmente

```
0x00 char[4] "PSM\x02", u32 tamaño
0x08 float[10]  caja: min xyz, max xyz, centro xyz, radio (centro = (min+max)/2 comprobado)
0x30 u16[4]     desconocidos (p.ej. 5,3,1,2 / 0,2,1,2)
0x38 u32        offset de la primera malla (0x60)
Malla (repetida; se avanza por su tamaño):
  +0x00 float[10] caja de la malla
  +0x28 u32 tamaño total de la malla
  +0x2C u32 tamaño de la cabecera de malla (0x70, 0xC0…)
  +0x30 u16 0x60 (¿tamaño de subcabecera?)   +0x32 u16 número de tiras
  +0x38 u16 flags de material (0x00, 0x0E, 0x10, 0x11, 0x15… — significado pendiente)
  +0x3A u16 = 1 (¿número de texturas?)
  +0x3C u16 índice de imagen en el PSPI del conjunto  (verificado visualmente en 9 escenarios;
            una primera hipótesis con +0x38 era errónea y daba texturas cruzadas)
  Tira: cabecera de 0x50 bytes y luego los vértices
    u32 prim (3 = triángulos, 4 = tira, 5 = abanico — códigos de la GE)
    u32 vtype (registro VTYPE de la GE; visto 0x1FE = UV u16, color 8888, normal float, pos float)
    u32 nVértices, u32 ?, u32 ?, u32 bytes (= n·stride), u32 ?, u32 relleno tras los vértices (0x10)
```
- Las tiras se unen con vértices degenerados repetidos (se descartan al triangular).
- UV u16 normalizadas a /32768. Ejes: **Y arriba** (las marcas de suelo tienen Y constante).
- Validado: 1626/1690 modelos parsean. Fallan 64: modelos de enemigos `EH*` y `SHADOW_HUMAN`
  (probablemente vértices con pesos/skinning o cabecera distinta) → **pendiente**.

## 6. Escenarios (DG.ARC)  ✅ estructura / ⏳ ensamblado

`DG_00..DG_08.BPX` + `BOSS_0x.BPX`. Cada uno es un **kit modular** de piezas de sala:
```
CLM.BPX       *.CLM   (163 en DG_00; nombres como AO_A_J_00_000T / Y3 / Y4)
MDL.BPX       MODEL.BPX (98 modelos de pieza)  +  TP.BPX/TEX.PSPIP (109 imágenes)
LIFT_MARK.BPX marcas N/E/W/S
WAYPOINT.BPX  *.WPT (20 bytes por pieza)   PLT.BPX  *.PLT (20 bytes por pieza)
AREALIGHT.BPX LIGHT_ROOMxxxx.RLT
MOTION_AO.BPX piezas animadas (MDL + MOT + BDB + COLL)
AOD_POS.BPX   posiciones (12 bytes = 3 floats)
```
- Renderizar todas las piezas de DG_00 en sus coordenadas guardadas produce piezas
  **superpuestas**: son variantes para las mismas posiciones. El montaje lo decide el
  generador aleatorio a partir de `MAIN.ARC/DGxx_*.RLVDEFBIN` (bloques `ROOT`, `PASS`,
  `ROOM`…). **Pendiente de descifrar.**
- **Rejilla de 40 unidades** en X, Y y Z: las cajas de las piezas son múltiplos exactos de 40
  (suelo `GR_` 40×0×40 a y=0; techo `CE_` en y=40; paredes `WA_` 40×40 u 80×40 en el plano z=0).
  Prefijos observados: GR suelo, CE techo, WA pared, DO puerta, DU dintel, ST columna/escalera,
  RA rampa, LI luz, BL bloque, PP tubería(?), SO cartel(?), AO objeto. Sufijo `T`: desconocido.
- Escala: provisionalmente 16 unidades = 1 m (celda = 2,5 m). **No verificada.**

Coincide con Wikipedia: "All of the levels in the game are randomly generated when they
are accessed".

## 6b. Snapshot de nivel RLVB (`MAIN.ARC/DGxx_{L,M,S}_00P.RLVSNAP`)  ✅ rejilla verificada

27 archivos de 153 600 bytes. Cabecera: `RLVB`; 0x2C u32 offset de la rejilla (0x84);
0x30 u16 ancho X, 0x32 u16 capas Y, 0x34 u16 fondo Z; 0x48 u32[4] offsets de las secciones
`TIBKPASS`, `TIBKROOM`, `TIBKRAMP`, `TIBKLAY`. Comprobado en los 27: offset + X·Y·Z·4 = inicio
de la primera sección.

Celda u32, índice `(capa·Z + z)·X + x`. Byte bajo (verificado por la forma de las salas al
imprimir la rejilla de DG00_L_00P: dos salas de 6×5 unidas por pasillo):
- bit 0 pared norte (−Z), bit 1 este (+X), bit 2 oeste (−X), bit 3 sur (+Z)
- 0x10 suelo en esa capa, 0x20 techo, 0x40 aparece en pasarelas de la capa intermedia
- bytes 1–2: probablemente variante de pieza/iluminación — **pendiente**

`TIBKLAY` lista objetos colocados (`LIFT`, `SLOP`, `SSLO`, `AO`, `LVAR9`…) con coordenadas de
celda y rotación en grados como u16 (0x5A, 0xB4, 0x10E). `RLVDEFBIN` (bloques ROOT/PASS/ROOM/
RAMP/LIGH) son los **parámetros** del generador aleatorio, no niveles concretos.

Montaje actual del port **[RE parcial]**: estructura de celdas original; para cada rasgo
(suelo/techo/pared) se usa una pieza representativa de 40×40 elegida por forma. La elección
exacta de pieza por celda y los objetos de TIBKLAY quedan pendientes.

## 7. Datos de diseño verificados (fuente: Wikipedia, artículo "Coded Arms")

- Ambientación: simulador militar de realidad virtual abandonado "AIDA".
- Niveles generados aleatoriamente al acceder.
- Sector 00 (Training, 3 niveles); Sector 01: City, Base, Ruins (6 niveles cada uno);
  Sector 02: 3 campos de 13 niveles con jefe en el 13; Sector 03 "Infinity" ilimitado.
- Armas/armaduras con atributos (físico… fuego, eléctrico, viral…); mejoras con "Opt_Key".
- Objetos con extensiones `.arm` (armas), `.dfn` (armaduras), `.med` (salud).
- Multijugador wifi 4 jugadores: Deathmatch, Keep the Mark, Last Man Standing.

## 8. Por verificar (no usar como hecho)

- Frecuencia de simulación original (60 Hz es supuesto del port).
- Velocidad de movimiento, salto, gravedad, FOV originales (valores del port provisionales).
- Uso del alfa del color de vértice (algunos vértices tienen A = 0).
- Formatos KSS (sonido), MOT1, COLL, CLM, WPT, PLT, RLT, C2B, F2B, RLVDEFBIN, RLVSNAP.

## 9. Savedata y diálogos del sistema (sceUtility)  ✅ verificado contra PPSSPP
- Front end: los diálogos leen el mando emulado (como PPSSPP: Intro = ✕, Retroceso = ○, flechas = cruceta) y `src/frontend/DialogOverlay.cpp` los dibuja encima del juego (lista de partidas, confirmaciones, mensajes, leyenda de teclas) con una fuente de 8×16 generada de DejaVu Sans Mono (`tools/font/make_font.py`). Sin esto, "Cargar partida" esperaba a un diálogo invisible.
- Error de Windows corregido: `_stat64` falla con rutas de directorio acabadas en `/` (`memstick/PSP/SAVEDATA/`), así que la lista de partidas salía vacía ("There is no data"). Se quita la barra final (también en `sceIoGetstat`).

Fuente: código del juego (llamadas a `sceUtilitySavedataInitStart` en el BOOT.BIN
recompilado) y ejecuciones de PPSSPPHeadless sobre la misma ISO.

- Petición de savedata de **1500 bytes** (formato V2, sin `secureVersion`/clave) → PPSSPP
  cifra con **modo 1** (sin clave del juego); hash de PARAM.SFO con modos 2 y 1
  (`SAVEDATA_PARAMS[0] = 0x01`). gameName `ULES00124`, saveName `000`..`004`, fichero `CODED.ARM`.
- Modos que usa el juego (análisis estático): 8 SIZES (en el arranque), 0 AUTOLOAD, 2 LOAD,
  4 LISTLOAD, 1 AUTOSAVE, 3 SAVE, 6 LISTDELETE, 7 LISTALLDELETE. No importa
  `sceUtilityNetconfInitStart`.
- Arranque: SIZES (usedSpaceKB 64, 2 clusters, igual que PPSSPP) y después un MsgDialog V2
  (580 bytes, opciones YESNO|DEFAULT_NO): *"There is insufficient space on the Memory Stick
  Duo™. At least 160 KB is needed..."*. **PPSSPP lo muestra también** con su tarjeta por
  defecto de 16 GB; con 1 GB (`MemStickSize = 1`) no aparece ni en PPSSPP ni aquí
  (`CA_MEMSTICK_SIZE_GB=1`). Hipótesis: el juego multiplica clusters libres × tamaño de
  cluster en 32 bits y 16 GB desborda. "No" (opción por defecto) repite la comprobación.
- Interfaz para el front end: `src/psp/host_dialog.h`; política de respuesta del runner:
  `CA_DIALOG_POLICY` (default/yes/no/cancel/none/buttons), `CA_DIALOG_DELAY_MS`.

## 10. Recompilación estática de BOOT.BIN  ✅ verificado
- `BOOT.BIN` (ULES-00124) es un PRX ELF en claro, base 0x08804000, compilado con Metrowerks MW MIPS C 2.4.1; reubicaciones de tipo 2/4/5/6.
- Código: `.text` más inicializadores estáticos dentro de `.data` (0x08a89640–0x08a91714) seguidos de una tabla de 51 constructores.
- Funciones descubiertas por: destinos de `jal`, punto de entrada, punteros R_MIPS_32 (función si ya es inicio, si se construye con lui/addiu, si está fuera de `.text` o va precedido de `jr ra`; si no, etiqueta de tabla de saltos y entrada secundaria) y saltos que salen de una función (epílogos compartidos de MW → entradas secundarias).
- Cada función se emite como C sobre `Ctx` (diseño idéntico a `MIPSState` de PPSSPP); `jal` → llamada directa, `jr ra` → retorno, `jr reg` → `switch` sobre las etiquetas de la tabla con `call_indirect` por defecto; importaciones → `hle_call(idx)`; VFPU → `vfpu_exec`.
- Semánticas verificadas con PPSSPP: `div` INT_MIN/-1 y división por cero, `divu`, NaN canónico en `mul.s`/`sqrt.s`, `div.s` actualiza fcr31.

## 11. Temporización de E/S  ✅ verificado (causa del cuelgue en el cargador de fuentes)
- Con E/S asíncrona instantánea, un segundo objeto de fuente se enlazaba al primero antes de que éste se inicializara (memcpy con `A.font[164]` sin inicializar).
- Igual que PPSSPP (IOTIMING_FAST): open UMD 4000 µs, lectura `max(tamaño/100, 100)` µs, seek 100 µs; una operación asíncrona deja el descriptor ocupado (`0x80020329`) hasta que se recoge su resultado; sin operación → `0x8002032A`; los descriptores libres más bajos se reutilizan.

## 12. Renderizador GE  ✅ escenas verificadas visualmente contra PPSSPP
- Los comandos de la GE se procesan en el hilo del juego: los vértices y texturas se decodifican en el momento (la memoria puede cambiar después) y se graban operaciones GL por fotograma, que el hilo de render ejecuta.
- Framebuffers: un FBO por dirección de VRAM, a escala N; fila 0 de la PSP = y 0 de GL. Un framebuffer mostrado sin dibujos de la GE se toma de memoria solo si su hash cambió (la GE nunca escribe en la RAM): el juego dibuja en un búfer mientras muestra el otro.
- Coded Arms construye la geometría de los niveles con parches bezier/spline: sin teselarlos solo se ven el suelo, los objetos y el arma.
- Esquema de control del juego (tipo A, leído de la pantalla de configuración): stick = mover; △/×/□/○ = mirar arriba/abajo/girar izquierda/derecha; ↑ zoom; ↓ recargar; ←/→ arma anterior/siguiente; R disparar; L saltar; SELECT pausa; START mapa.

## 13. Vista del jugador y apuntado con ratón  ✅ verificado
- Encontrado con el build de vigilancia de escrituras (`-DCA_RECOMP_WATCH`, `CA_WATCH_ADDR`): la función `0x0889ba9c(view, input, ...)` actualiza cada fotograma la vista del jugador (en la misión de entrenamiento `view = 0x08F71570`).
- `view+0x51C`: yaw (rad), integrado como `yaw -= input[+0x78] × factor` y normalizado; `view+0x524`: pitch (rad), integrado desde `input[+0x79]` y limitado a ±1,309 rad (`0x3FA78D37`, en `0x0889bfb8`); `view+0x14`/`+0x10` reciben copias.
- La función se ejecuta una vez por paso de lógica (60 por segundo: N veces por fotograma con el intervalo N de §15) con estructuras de entrada que rotan; el hook (`game_hooks.cpp`, tras `0x0889bad4`) aplica el giro acumulado en la primera y lo pone a cero.
- Esquema A del juego y por qué el ratón ya no emula botones: ver §12.

## 14. Alfa del framebuffer y glow
- En la PSP el canal alfa del framebuffer es el stencil: dibujar no lo modifica salvo con operaciones de stencil (PPSSPP `GPUStateUtils`); ZERO/REPLACE escriben 0/ref.
- La misión usa stencil ALWAYS con ZERO para la geometría normal y REPLACE (ref 255) para luces y neones; un pase de glow reduce la pantalla (con su alfa como máscara) a 128×64 y la suma de vuelta. Escribir el alfa del fragmento convertía el HUD en recuadros blancos.

## 15. Frecuencia de fotogramas  ✅ medido
- Oráculo (PPSSPP, contando cambios de `sceDisplaySetFrameBuf`): el juego original dibuja a **20 fps en las misiones** y 30 en los menús; el port da lo mismo.
- `0x08879ff8(settings, N)` fija el intervalo: `settings+16` = N (s16), `+12` = 60/N, `+8` = (float)(60/N); el bloque lo apunta `0x09bd2004`. Misiones: N = 3.
- El bucle principal espera en `0x08837f70` hasta que el contador de vblanks (`s0+4`) alcanza `settings+16`. El hilo del jugador (`0x0889e128`, prioridad 0x28) espera un semáforo por fotograma y ejecuta su actualización N veces.
- **El intervalo solo puede cambiar en el fijador** (`ca_hook_frame_interval`): parte de la lógica deriva su paso del intervalo al empezar la pantalla. Cambiándolo a mitad de nivel, el jugador avanza 1,08 por fotograma tanto con N = 1 como con N = 2 (medido con `CA_HOOK_TRACE`): con N = 2 iría a la mitad de velocidad. La versión que subía el intervalo sobre la marcha cuando el PC no llegaba causaba "movimiento lento a ratos"; se quitó. Por lo mismo **no hay 45 fps** (exigiría alternar 1 y 2 por fotograma; además 1-2 alterno da 40 fps).
- Medido (entrenamiento, ejecución de la GPU desactivada):

| `frame_rate` | fps | movimiento | giro |
|---|---|---|---|
| 0 (original) | 20 | 43 u/s | 2,02 rad/s |
| 30 | 30 | 65 u/s | 2,02 rad/s |
| 60 | 60 | 65 u/s | 2,00 rad/s |

  El desplazamiento por fotograma del jugador está limitado (2,16): a 20 fps ese límite fija la velocidad original, así que con 30/60 el jugador va 1,5 veces más rápido que en la PSP. Un fotograma que tarda más que su intervalo va a cámara lenta (el juego no recupera tiempo, como en la PSP).
- El front end ejecuta todos los fotogramas pendientes en cada refresco y muestra el último, para que un vsync tardío no haga esperar al juego.
- Error corregido: la espera de vblank calculaba el siguiente vblank con el periodo truncado (16 683 µs en vez de 16 683,33) y volvía al instante.

## 16. Audio  ✅ verificado (0 cortes)
- Antes: las salidas bloqueantes de sceAudio se pacían con el reloj de pared y el front end consumía con el reloj de la tarjeta; la deriva y los retrasos del hilo dejaban la cola vacía → huecos de 1024 muestras (≈4 por minuto, medidos con el driver `disk` de SDL): el sonido "roto".
- Ahora (modelo de PPSSPP): con un consumidor (`host::g_audioConsumer`), una salida bloqueante espera a que la cola del canal baje de max(2048, n) muestras; el callback de audio avisa con `kernel::notifyStateChanged()`. `sceAudioGetChannelRestLength` devuelve lo que queda en la cola. Sin consumidor (runner sin ventana) se mantiene el reloj de pared.

## 17. Teselado de parches
- Coste medido en la misión: ~13 000 parches por segundo, 300–500 ms/s del hilo del juego (más que todo el código del juego, ~170 ms/s).
- Caché por hash de los puntos de control decodificados + parámetros (tipo, divisiones, primitiva, orientación, formato): la geometría de los niveles es estática. La malla sale como una sola tira de triángulos (2 vértices por cuadrado + 2 degenerados por fila, conservando el sentido de giro). Resultado: 300 → 80 ms/s y la mitad de vértices.

## 18. Símbolos de los botones
- El juego dibuja las ayudas de botones desde dos atlas: uno de 256×256 (símbolos ○□×△ de 14×14 en y = 128, x = 0/16/32/48; recuadros L/R de 20×11 en y = 114) y el dibujo de la PSP de 512×512 (círculos de los botones centrados en (211,209) △, (184,236) □, (238,236) ○, (211,262) ×; letras L/R; rótulos SELECT/START en y = 321 con una fuente de 3×5).
- Se reconocen por un hash de la forma (transparente/oscuro/claro) de esas zonas, no por dirección. Al decodificarlos se repintan los símbolos con la tecla de PC asignada (`psp/gpu/button_glyphs.cpp`, fuente de 3×5 propia). Los textos "× button" de la configuración son de la fuente del juego y no se tocan.

## 19. Apuntar con la mira (función nueva del port)  ✅ verificada con las 23 armas (modo dev)
- El juego dibuja arma y brazos con una cámara propia: matriz de vista girada 180° alrededor del eje vertical (diagonal −1, 1, −1), cerca del origen; el nivel con la cámara del jugador (la vista de más dibujos) y su radar con otra.
- Todo el modelo del arma tiene pesos (skinning). Los brazos son siempre las mismas 7 mallas (101, 186, 305, 85, 143, 61 y 279 vértices), dibujadas después del arma, con las 23 armas (comprobado en el modo dev); el arma = el resto de dibujos de esa cámara. Se descartan los efectos: dibujos de < 12 vértices (partículas, fogonazos), planos (z constante) o enteramente lejos (z < −9: proyectiles). Si no se reconocen los brazos: dibujos estrechos (ancho < 1,2) o enteramente delante (z < −2). Pistola: x 0,84–1,29, y −2,5…−0,49, z −7,7…−4,1; Lightning y Photon_MG tienen una malla ancha (x −0,44…2,4) que llega a z −0,6, por eso la regla de forma fallaba con ellas.
- Al empezar a apuntar se mide la mira = borde superior del arma (12 % más alto de sus vértices) y la dirección del cañón por mínimos cuadrados sobre ese borde; se guarda mientras se apunta (las animaciones, p. ej. el retroceso, se siguen viendo) y se vuelve a medir si cambia el arma (otro número de vértices durante 5 fotogramas).
- `psp/gpu/ads.cpp`: la vista del arma se multiplica por T(mira→eje, 0,8 más cerca)·Rx·Ry·T(−mira) interpolado en 0,15 s; la proyección de la cámara del jugador se estrecha ×1,3; la retícula (dibujo 2D pequeño centrado en 240,136) se oculta. La lógica del juego no cambia: los disparos siguen yendo al centro, donde queda la punta del alza (comprobado con una captura ampliada).
- Front end: `aim_down_sights = mouse:right, pad:lefttrigger`; el zoom del juego (cruceta arriba) pasa al botón central; `[input] ads_sensitivity` (0,6), `ads_toggle`. Los botones del ratón se leen de los eventos de SDL (el sondeo perdía pulsaciones en modo relativo).
- Verificado con las 23 armas en el entrenamiento (modo dev, §21): la mira queda centrada, también al disparar. Con los lanzadores de un disparo se ve su animación de recarga (el arma sube), que es del juego.
- Corregido (temblor): con la transición completa (t = 1) el paso iba en sentido contrario un fotograma (1 → 0,89) y volvía al siguiente: el arma y la vista oscilaban siempre. Ahora t avanza hacia el objetivo sin pasarse.
- Corregido (temblor): el zoom se aplicaba solo si la vista coincidía con la del fotograma anterior, así que al mover la cámara se ponía y quitaba; ahora se aplica a todo dibujo 3D a pantalla completa que no sea del arma (el radar usa un viewport de escala 50, el jugador 240). El arma: se guardan los vértices del borde superior al alinear y cada fotograma se quita el 85 % de su movimiento (balanceo al andar/en reposo), suavizado. La retícula se abre en marcas al moverse: se ocultan las piezas 2D pequeñas dentro de ±56 px del centro.

## 20. Estabilidad
- La memoria del invitado cubre ahora todo el rango enmascarado (0x10000000 + 64 bytes): antes eran 0x0A000000 y un puntero del juego por encima se convertía en un acceso fuera del bloque del host (cierre o corrupción).
- Windows: reserva de pila del ejecutable 16 MB (`-Wl,--stack`); los hilos de `std::thread` (cada hilo del juego) heredan la del ejecutable, 1 MB por defecto.
- Registro: en Windows stderr va a `CodedArms.log` junto al .exe; ante una excepción, `abort` o `std::terminate` se añaden el código, la dirección (`CodedArms.exe+0x…`, traducible con `build/win64/CodedArms.map`), el hilo del juego y su última llamada al sistema (`kernel::describeCrash`), y se avisa al jugador. `CA_TEST_CRASH=1` provoca un fallo a los 5 s para comprobarlo.

## 21. Modo dev (pruebas)  ✅ verificado en el entrenamiento
Para probar todas las armas. `[dev] enabled=1` en config.ini; `loadout` = las 5 armas con que se empieza (ids 0–22 armas, 23–32 granadas, en el orden de la tabla de nombres del juego). F5/F6 cambian el arma actual por la anterior/siguiente que no esté equipada.
- Inventario del perfil (A = 0x09bf3910 en la historia): 52 entradas de 3 bytes (objeto, ranura 1–5 o 0, ?); la entrada i es el objeto i (1–23 armas, 24–33 granadas); cantidad en perfil+268+objeto; objetos-arma en *(perfil+372)[i]; equipadas (máx. 5) en perfil+496; arma actual en perfil+156 (s16); cambio pedido en perfil+500 (s16, −1 nada).
- `0x08927620(perfil, …)` prepara las armas al empezar un nivel y `0x08927498(perfil)` crea un objeto (`0x0884a2b4`, 112 bytes) por arma equipada. El gancho en su entrada (hooks_ULES00124.tsv) llama a `0x08926bd4(perfil, objeto)`, el "añadir al inventario" del juego (el de recoger objetos, `0x088a486c`), para cada objeto 1–33 y equipa los 5 de `loadout`.
- Cambio de arma: se quita la ranura a la actual (perfil+496 −1), se equipa la nueva con `0x08926a04(perfil, objeto, 0, perfil[504])` (o se reutiliza su objeto si ya existía), se escribe perfil+500 = nueva y jugador+1418 = 0 (como al recoger un arma; `0x08894e48` llama entonces a `0x089260d0`, que inicia el cambio) y se rehace la lista del HUD con `0x08900df0(0x089048a8())`. Jugador = objeto de la vista (s3 en 0x0889bad4), perfil en jugador+1244.
- Munición: la reserva del arma id i es un u32 en `0x08928c94()` + 4·i (0x09bf4124 en la historia, 0x09bf418c en el otro modo); el cargador está en el objeto-arma +82 (tamaño en la tabla de armas +94: pistola 12, fusil 32, francotirador 6, escopeta 8, RPG 1). El modo dev mantiene la reserva en ≥ 300 y las granadas en su máximo.
- Las armas dadas pasan al perfil de la historia y a la partida guardada.

## 22. Opciones del PC en el menú OPTIONS del juego  ✅ verificado en el front end
- La pantalla OPTIONS (Terminal y menú de pausa) tiene 5 filas: CONTROL TYPE, FREE LOOK, SOUND, ACCEPT CHALLENGE (multijugador ad hoc, que el port no tiene) y LANGUAGE. La cuarta pasa a ser PC SETUP.
- Textos: tablas de mensajes por idioma en el disco, cadenas con un u32 de longitud delante (incluye el NUL). `IsoReader::addPatch` sustituye bytes al leer el ISO (el fichero no se modifica) si los originales coinciden; el texto nuevo se rellena con NUL hasta la misma longitud. Posiciones en el ISO EUR: etiqueta y ayuda en inglés 74969841 / 74970029, alemán 75024509 / 75024720, español 75081222 / 75081487, francés 75137173 / 75137392, italiano 75192366 / 75192634 (game_hooks.cpp). Codificación del juego para acentos (vista en sus textos): á 83 41, é 83 4E, í 83 56, ó 83 60, É 82 AD, Í 82 B5. El valor ON/OFF de la fila es un texto compartido (Y-AXIS) y no se cambia: la fila sigue mostrando OFF.
- Menú: objeto creado por `0x08966a0c` (vtable 0x08a92f30), fila seleccionada en +158 (s16); `0x08964e1c(menú)` lo actualiza cada fotograma (lee el mando con `0x0895e03c`). Gancho en su entrada → `host::g_gameOptions` (fila y hora). Con la fila 3 activa el front end no pasa al juego aceptar, izquierda/derecha ni cuadrado (la opción de multijugador queda en OFF) y aceptar abre la página PC SETUP.
- Página (`frontend/OptionsMenu.cpp`): dibujada sobre la pantalla con los colores de OPTIONS (verde 77FFAC, naranja FFA737, paneles turquesa translúcidos, fila seleccionada subrayada, línea de ayuda abajo, «PC Setup» abajo a la derecha). Teclado (flechas/WASD, aceptar = teclas de ✕, volver = teclas de ○/△ o Esc), ratón (pasar por encima, clic, rueda) y mando (cruceta, A, B). Remapeo: Intro y la tecla/botón nuevo (F1 cancela); sustituye las del mismo tipo (teclado, ratón o mando) y la quita de otras acciones.
- Pausa: `kernel::setPaused` para el reloj del juego (`nowUs`) y los hilos del juego se detienen en su siguiente llamada al sistema hasta reanudar. Al cerrar, nada llega al juego hasta soltar todas las teclas.

