# libkirk

Open-source implementation of the PSP's KIRK crypto engine, copied unmodified from PPSSPP's
`ext/libkirk` (https://github.com/hrydgard/ppsspp, commit 4b71f833d0d5). The runtime uses it
through `src/psp/ppsspp/sceChnnlsv.cpp` to encrypt/decrypt savedata and to compute the
PARAM.SFO hashes exactly like PPSSPP and the PSP's savedata utility do.

Licences (see each file's header):

- `kirk_engine.c`, `kirk_engine.h`: "Draan proudly presents ... KIRK-ENGINE", GNU GPL version 3
  or later.
- `bn.c`, `ec.c`: Copyright 2007-2022 Segher Boessenkool, GNU GPL version 2 or version 3.
- `AES.c`, `AES.h`: rijndael-alg-fst.c (Rijmen, Bosselaers, Barreto), public domain.
- `SHA1.c`, `SHA1.h`: SHA-1 by David Ireland / A.M. Kuchling / Peter Gutmann, public domain.
- `kirk_common.h`: type definitions, part of the KIRK engine.

Because `kirk_engine.c` is GPL-3.0-or-later, binaries that link it (`ca_psprun`, the savedata
tests) are distributed under the GNU GPL version 3, which the rest of the project
(GPL-2.0-or-later) permits.
