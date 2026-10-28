# PROGRESS — Coded Arms: Contagion PC port

Procedencia: **[CA1]** reutilizado de `AndyLatimer/CodedArmsPC` (port del primer Coded Arms,
ULES-00124, recompilación estática ya jugable) · **[NUEVO]** escrito para este proyecto.

## Estado (2026-10-28)

### Completado y verificado
- [x] Inspección del entorno: el directorio de trabajo **no contiene ningún archivo del juego**
  (buscado en `/home/user`, `/mnt/user-data/uploads`, `/mnt/attach` y en todo el disco: ninguna
  ISO/CSO/PBP/BOOT.BIN). Sin la ISO de Contagion no puede empezar la ingeniería inversa.
- [x] Herramientas disponibles: GCC/G++, Clang, CMake, Ninja, Python 3.13, binutils/llvm-objdump
  (sin MIPS), y ya instalados `libsdl2-dev` y `mingw-w64` (compilación cruzada a Windows x64).
  No hay Ghidra ni Wine.
- [x] Estudio del proyecto hermano CodedArmsPC (ver abajo): su arquitectura de recompilación
  estática es la estrategia elegida para Contagion.
- [x] [NUEVO] `tools/inspect/inspect_iso.py`: análisis de Fase 1 de la ISO/CSO del usuario
  (hashes, listado ISO 9660, UMD_DATA.BIN, PARAM.SFO, estado de BOOT.BIN/EBOOT.BIN —en claro,
  a ceros o cifrado—, secciones, tipos de reubicación, información de módulo, todos los NID
  importados y **cuáles no implementa aún el runtime HLE de CodedArmsPC**, y un censo de
  formatos por número mágico). Probado con una imagen sintética: `python3 tests/test_inspect_iso.py`
  → 20/20 comprobaciones correctas (ISO y CSO).

### No funciona / bloqueado
- [ ] **Falta la ISO de Coded Arms: Contagion** (tu copia). Sin ella no hay nada del juego que analizar.
- [ ] Compilar y ejecutar el código de CodedArmsPC en esta sesión necesita tu autorización explícita
  (el entorno bloquea ejecutar código clonado de un repositorio externo sin ella).

## Estrategia elegida: recompilación estática reutilizando CodedArmsPC

Evidencia: CodedArmsPC ya demostró la cadena completa con el primer juego (mismo estudio, misma
plataforma, mismo compilador probable): BOOT.BIN recompilado a C, runtime HLE (hilos, E/S, SAS,
MPEG/ATRAC3+, savedata cifrado), GE→OpenGL 3.3, control de cámara con ratón por gancho en la
actualización de vista del juego, ADS en las 23 armas, 60 fps, panorámico y build de Windows.
Un emulador empaquetado no cumple el objetivo; una reimplementación desde cero no conserva el juego.

| Componente de CodedArmsPC | Reutilización en Contagion |
|---|---|
| `tools/recomp/` (decodificador Allegrex, descubrimiento de funciones, generador de C, cargador PRX) | Directa. Ampliar si Contagion usa reubicaciones o instrucciones nuevas (lo dirá `inspect_iso.py`). |
| `src/psp/` kernel, E/S, SAS, MPEG, utility/savedata, VFPU, GE/GPU | Directa; añadir los NID que falten. `gameName`/fichero de guardado según PARAM.SFO. |
| `src/frontend/` (ventana, entrada, audio, diálogos, menú PC SETUP) | Directa; el menú PC SETUP depende de offsets de texto del disco → por juego. |
| `hooks_ULES00124.tsv`, `game_hooks.cpp` (vista/yaw/pitch, intervalo de fotograma, inventario dev, OPTIONS) | **Por juego**: hay que volver a localizar cada dirección en Contagion con el build de vigilancia (`-DCA_RECOMP_WATCH`). La metodología se conserva (TECHNICAL_NOTES §13, §15, §21 de CA1). |
| `gpu/ads.cpp` (ADS) | Algoritmo reutilizable (cámara propia del arma, medición de la mira, interpolación); la identificación de brazos/arma por número de vértices es **específica de CA1** y se rehará; se añade configuración por arma (`weapons.ini`). |
| `gpu/button_glyphs.cpp`, `gpu/minimap.cpp` | Por juego (hashes de atlas). |

Organización prevista: el motor de CodedArmsPC se integra en `src/` con todo lo específico de un
juego bajo `src/games/<DISC_ID>/` y `tools/recomp/games/<DISC_ID>/` (ganchos, importaciones), de modo
que las mejoras del runtime sirvan a ambos juegos.

## Cómo reproducir
```
python3 tests/test_inspect_iso.py
python3 tools/inspect/inspect_iso.py game/<tu_copia>.iso \
    --known ../andylatimer/codedarmspc/tools/recomp/imports_ULES00124.tsv \
    --json analysis/binary/inspect.json
```

## Siguiente paso concreto
1. Recibir la ISO/CSO → ejecutar `inspect_iso.py`, registrar hashes y DISC_ID en `TECHNICAL_NOTES.md`.
2. Si BOOT.BIN está en claro: pasar el recompilador de CA1 sobre él y listar instrucciones,
   reubicaciones y NID no soportados. Si está a ceros: evaluar el descifrado del EBOOT.BIN del
   propio usuario (p. ej. el PrxDecrypter de PPSSPP) antes de seguir.
3. Integrar el motor de CA1 en este repositorio y conseguir el primer arranque (logo de Konami).
