# Coded Arms: Contagion — port nativo para PC

Objetivo: `CodedArmsContagion.exe` (Windows x64) que ejecuta el código original del juego de PSP
(Konami, 2007) recompilado estáticamente, con un runtime que sustituye al sistema de la PSP,
controles FPS de PC y un sistema nuevo de apuntado con la mira (ADS).

Basado en el port del primer juego, [CodedArmsPC](https://github.com/AndyLatimer/CodedArmsPC).
El repositorio **no contiene ningún recurso del juego**: todo se lee de la copia del usuario
(`game/`, ignorada por git) y el código generado se queda en `build/`.

Estado y siguientes pasos: [`PROGRESS.md`](PROGRESS.md).

Compilar (Linux; necesita CMake, Ninja, GCC, `libsdl2-dev` y tu ISO en `game/`):
```
cmake -S . -B build/linux -G Ninja -DCA_GAME_ISO=$PWD/game/Coded.Arms.Contagion.iso
cmake --build build/linux
```

```
src/psp/                       runtime HLE (sistema de la PSP), GPU GE→OpenGL  [de CodedArmsPC]
src/frontend/                  ventana, entrada, audio, menús del PC            [de CodedArmsPC]
src/games/ULES00718/           ganchos específicos de Contagion
tools/recomp/                  recompilador estático Allegrex → C               [de CodedArmsPC]
tools/recomp/games/ULES00718/  ganchos e importaciones de Contagion
docs/ca1/                      lo específico del primer juego, como referencia
tools/inspect/inspect_iso.py   análisis de Fase 1 de la ISO/CSO del usuario
tests/test_inspect_iso.py      pruebas con una imagen sintética
analysis/                      resultados de análisis (no se suben los derivados del juego)
game/                          tu copia del juego (no se sube)
```

Licencia: GPL-2.0-or-later (incluye código de PPSSPP); los binarios que enlazan libkirk son GPL-3.0.
