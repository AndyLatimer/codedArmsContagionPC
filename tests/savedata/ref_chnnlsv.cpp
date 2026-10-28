// PPSSPP's unmodified Core/HLE/sceChnnlsv.cpp, compiled straight from a PPSSPP checkout
// (CA_PPSSPP_DIR) as the reference the savedata test checks the port's crypto against. Its
// functions are renamed ref_* so they can sit next to the port's copy in one executable.
#define sceSdMacInit ref_sceSdMacInit
#define sceSdMacUpdate ref_sceSdMacUpdate
#define sceSdMacFinal ref_sceSdMacFinal
#define sceSdCipherInit ref_sceSdCipherInit
#define sceSdCipherUpdate ref_sceSdCipherUpdate
#define sceSdCipherFinal ref_sceSdCipherFinal
#define __ChnnlsvKirkState ref___ChnnlsvKirkState
#define Register_sceChnnlsv ref_Register_sceChnnlsv
#define Register_semaphore ref_Register_semaphore
#define pspChnnlsvContext1 ref_pspChnnlsvContext1
#define pspChnnlsvContext2 ref_pspChnnlsvContext2
#define _pspChnnlsvContext1 ref__pspChnnlsvContext1
#define _pspChnnlsvContext2 ref__pspChnnlsvContext2
#include "Core/HLE/sceChnnlsv.cpp"
