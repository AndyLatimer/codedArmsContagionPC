/* Interface between recompiled guest code (generated C) and the native runtime.
 * The generated code includes this header only; everything it calls is declared here. */
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Guest CPU state. One per guest thread. The field layout (names, unions, order of
 * vfpuCtrl/pc/lo/hi/fcr31/fpcond) mirrors PPSSPP's MIPSState so its VFPU interpreter
 * (src/psp/ppsspp) can operate on this struct directly. */
typedef struct Ctx {
    uint32_t r[32];
    union { float f[32]; uint32_t fi[32]; int32_t fs[32]; };
    union { float v[128]; uint32_t vi[128]; };  /* PPSSPP "voffset" order */
    uint32_t vfpuCtrl[16];                      /* 0-2 prefixes S/T/D, 3 condition codes */
    uint32_t pc, lo, hi, fcr31, fpcond;
    uint8_t* mem;        /* host base of guest memory */
    void* thread;        /* owning runtime thread */
} Ctx;

typedef void (*GuestFn)(Ctx*);
typedef struct { uint32_t addr; GuestFn fn; } GuestFunc;
typedef struct { const char* lib; uint32_t nid; uint32_t stub; } HleImport;

/* Provided by the generated table.c */
extern const GuestFunc g_guest_funcs[];
extern const unsigned g_guest_func_count;
extern const HleImport g_hle_imports[];
extern const unsigned g_hle_import_count;
extern const uint32_t g_module_base, g_module_entry;

/* Guest memory: user RAM 0x08000000-0x09FFFFFF, VRAM 0x04000000, scratchpad 0x00010000.
 * Cached/uncached/kernel mirrors (0x4..., 0x8..., 0xC...) are folded by the mask. */
#define GUEST_ADDR_MASK 0x0FFFFFFFu
#define GUEST_MEM_SIZE 0x0A000000u
#define HOST(a) (c->mem + ((uint32_t)(a) & GUEST_ADDR_MASK))
static inline uint32_t rd32_(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint16_t rd16_(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline void wr32_(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }
static inline void wr16_(uint8_t* p, uint16_t v) { memcpy(p, &v, 2); }
#define RD8(a) (*HOST(a))
#define RD8S(a) ((int8_t)*HOST(a))
#define RD16(a) rd16_(HOST(a))
#define RD16S(a) ((int16_t)rd16_(HOST(a)))
#define RD32(a) rd32_(HOST(a))
#define WR8(a, v) (*HOST(a) = (uint8_t)(v))
#define WR16(a, v) wr16_(HOST(a), (uint16_t)(v))
#define WR32(a, v) wr32_(HOST(a), (uint32_t)(v))

void rt_watch_store(Ctx* c, uint32_t pc, uint32_t addr);
// Profile build (CA_RECOMP_PROFILE): calls per function, indexed by (address - text base) / 4.
extern uint32_t g_prof_counts[];
#define RT_PROF(a) (++g_prof_counts[((a) - 0x08804000u) >> 2])
void ca_hook_player_view(Ctx* c);
void ca_hook_frame_interval(Ctx* c);
void ca_hook_frame_work_done(Ctx* c);
void ca_hook_frame_start(Ctx* c);
void ca_hook_dev_loadout_mission(Ctx* c);
void ca_hook_dev_loadout_respawn(Ctx* c);

#define R(n) (c->r[n])
#define F(n) (c->f[n])
#define FI(n) (c->fi[n])
#define VFPU_CC (c->vfpuCtrl[3])

#define ROTR(x, s) ((s) ? (((x) >> (s)) | ((x) << (32 - (s)))) : (x))
#define CLZ(x) ((x) ? (uint32_t)__builtin_clz(x) : 32u)
#define BSWAP32(x) __builtin_bswap32(x)
#define MAXI(a, b) ((a) > (b) ? (a) : (b))
#define MINI(a, b) ((a) < (b) ? (a) : (b))
static inline uint32_t BITREV(uint32_t x) {
    uint32_t r = 0;
    for (int i = 0; i < 32; ++i) { r = (r << 1) | (x & 1); x >>= 1; }
    return r;
}

/* Runtime helpers (src/psp/cpu.cpp) */
void rt_div(Ctx* c, uint32_t a, uint32_t b);
void rt_divu(Ctx* c, uint32_t a, uint32_t b);
uint32_t rt_lwl(Ctx* c, uint32_t addr, uint32_t old);
uint32_t rt_lwr(Ctx* c, uint32_t addr, uint32_t old);
void rt_swl(Ctx* c, uint32_t addr, uint32_t v);
void rt_swr(Ctx* c, uint32_t addr, uint32_t v);
uint32_t rt_f2i(float f, int mode); /* 0 round-nearest, 1 truncate, 2 ceil, 3 floor */
uint32_t rt_fcmp(float a, float b, int cond);
float rt_fdiv(Ctx* c, float a, float b); /* updates the divide-by-zero bits of fcr31 */
uint32_t rt_cfc1(Ctx* c, int reg);
void rt_ctc1(Ctx* c, int reg, uint32_t v);
void vfpu_exec(Ctx* c, uint32_t insn); /* every VFPU instruction, incl. loads/stores and mfv/mtv */
void call_indirect(Ctx* c, uint32_t addr);
void hle_call(Ctx* c, int import_index);
void hle_syscall(Ctx* c, uint32_t code);
void rt_break(Ctx* c, uint32_t pc);
void rt_privileged(Ctx* c, uint32_t insn, uint32_t pc);
void rt_unimplemented(Ctx* c, uint32_t insn, uint32_t pc);
void rt_fell_off(Ctx* c, uint32_t addr);
void rt_trace_enter(Ctx* c, uint32_t fn);
void rt_trace_exit(Ctx* c, uint32_t fn);

#ifdef __cplusplus
}
#endif
