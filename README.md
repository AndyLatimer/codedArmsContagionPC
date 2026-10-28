# Coded Arms: Contagion — port nativo para PC

Objetivo: `CodedArmsContagion.exe` (Windows x64) que ejecuta el código original del juego de PSP
(Konami, 2007) recompilado estáticamente, con un runtime que sustituye al sistema de la PSP,
controles FPS de PC y un sistema nuevo de apuntado con la mira (ADS).

Basado en el port del primer juego, [CodedArmsPC](https://github.com/AndyLatimer/CodedArmsPC).
El repositorio **no contiene ningún recurso del juego**: todo se lee de la copia del usuario
(`game/`, ignorada por git) y el código generado se queda en `build/`.

Estado y siguientes pasos: [`PROGRESS.md`](PROGRESS.md).

```
tools/inspect/inspect_iso.py   análisis de Fase 1 de la ISO/CSO del usuario
tests/test_inspect_iso.py      pruebas con una imagen sintética
analysis/                      resultados de análisis (no se suben los derivados del juego)
game/                          tu copia del juego (no se sube)
```
