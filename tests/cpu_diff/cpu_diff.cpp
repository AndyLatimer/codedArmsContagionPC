// Differential CPU test: runs every distinct instruction word of the game through the
// recompiler's translation and through PPSSPP's interpreter with random states, and
// reports any difference in registers, HI/LO, FPU state or memory.
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <string>

#include "psp/memory.h"
#include "psp/recomp.h"
#include "ref_compat.h"

int g_refExceptions = 0;
RefConfig g_Config;

extern "C" {
typedef void (*OpFn)(Ctx*);
typedef struct { unsigned word; OpFn fn; const char* handler; const char* mn; } TestOp;
extern const TestOp g_test_ops[];
extern const unsigned g_test_op_count;
}

// Runtime helpers referenced by generated code that the tested ops never reach.
extern "C" {
void vfpu_exec(Ctx*, uint32_t) {}
void hle_call(Ctx*, int) {}
const GuestFunc g_guest_funcs[1] = {{0, nullptr}};
const unsigned g_guest_func_count = 0;
}
namespace psp::kernel {
[[noreturn]] void fatal(Ctx*, const char* fmt, ...) { std::fprintf(stderr, "fatal: %s\n", fmt); std::abort(); }
bool callImportByAddress(Ctx*, uint32_t) { return false; }
}

using Handler = void (*)(MIPSState*, MIPSOpcode);
static std::map<std::string, Handler> handlers() {
    using namespace MIPSInt;
    return {{"Int_IType", Int_IType}, {"Int_StoreSync", Int_StoreSync}, {"Int_RType3", Int_RType3},
            {"Int_ITypeMem", Int_ITypeMem}, {"Int_FPULS", Int_FPULS}, {"Int_mxc1", Int_mxc1},
            {"Int_RType2", Int_RType2}, {"Int_MulDivType", Int_MulDivType}, {"Int_ShiftType", Int_ShiftType},
            {"Int_Allegrex", Int_Allegrex}, {"Int_Allegrex2", Int_Allegrex2}, {"Int_Special3", Int_Special3},
            {"Int_FPU2op", Int_FPU2op}, {"Int_FPUComp", Int_FPUComp}, {"Int_FPU3op", Int_FPU3op}};
}

static constexpr uint32_t kArea = 0x08900000, kAreaSize = 0x20000;

int main(int argc, char** argv) {
    int trials = argc > 1 ? std::atoi(argv[1]) : 64;
    psp::initMemory();
    RefSpace space;
    auto table = handlers();
    std::mt19937 rng(12345);
    auto rnd = [&] { return (uint32_t)rng(); };
    const float specials[] = {0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 1.5f, 2.5f, -2.5f, 1e10f, -1e10f, 3e9f, -3e9f,
                              INFINITY, -INFINITY, NAN, 1e-40f, 65535.75f, -0.49999997f};
    std::map<std::string, int> failsByMn;
    std::map<std::string, std::string> example;
    int tested = 0, failed = 0;
    static uint8_t snapshot[kAreaSize], after[kAreaSize];

    for (unsigned k = 0; k < g_test_op_count; ++k) {
        const TestOp& t = g_test_ops[k];
        auto h = table.find(t.handler);
        if (h == table.end()) { std::printf("no handler %s\n", t.handler); continue; }
        uint32_t w = t.word;
        int rs = (w >> 21) & 31;
        int16_t simm = (int16_t)(w & 0xFFFF);
        for (int trial = 0; trial < trials; ++trial) {
            Ctx a{};
            for (int r = 1; r < 32; ++r) a.r[r] = rnd();
            if (trial & 1) for (int r = 1; r < 32; ++r) a.r[r] &= 0x1F; // small values: shifts, compares
            a.hi = rnd(); a.lo = rnd();
            for (int f = 0; f < 32; ++f) {
                float v = (rnd() & 3) == 0 ? specials[rnd() % (sizeof(specials) / sizeof(float))]
                                           : (float)((int32_t)rnd()) / (float)(1 + (rnd() & 0xFFFF));
                std::memcpy(&a.fi[f], &v, 4);
                if ((rnd() & 7) == 0) a.fi[f] = rnd(); // raw integer bit patterns
            }
            a.fpcond = rnd() & 1;
            a.fcr31 = a.fpcond << 23;
            // Memory operands point into the scratch area.
            if (rs) {
                // Aligned for the access size, as in the game (lwl/lwr/swl/swr get any byte).
                uint32_t op = w >> 26, mask = 0x3FFC;
                if (op == 0x21 || op == 0x25 || op == 0x29) mask = 0x3FFE;           // lh/lhu/sh
                if (op == 0x20 || op == 0x24 || op == 0x28 || op == 0x22 || op == 0x26 || op == 0x2A || op == 0x2E)
                    mask = 0x3FFF;                                                   // byte / unaligned
                a.r[rs] = ((kArea + 0x8000 + (rnd() & mask)) - (uint32_t)(int32_t)simm);
            }
            a.mem = psp::g_mem;
            for (uint32_t i = 0; i < kAreaSize; i += 4) psp::write32(kArea + i, rnd());
            std::memcpy(snapshot, psp::hostPtr(kArea), kAreaSize);

            MIPSState b{};
            std::memcpy(b.r, a.r, sizeof(a.r));
            std::memcpy(b.fi, a.fi, sizeof(a.fi));
            b.hi = a.hi; b.lo = a.lo; b.fcr31 = a.fcr31; b.fpcond = a.fpcond; b.space = &space;

            t.fn(&a);
            std::memcpy(after, psp::hostPtr(kArea), kAreaSize);
            std::memcpy(psp::hostPtr(kArea), snapshot, kAreaSize);
            g_refExceptions = 0;
            h->second(&b, MIPSOpcode(w));

            std::string why;
            for (int r = 1; r < 32 && why.empty(); ++r)
                if (a.r[r] != b.r[r]) { char buf[96]; std::snprintf(buf, sizeof buf, "r%d gen=%08x ref=%08x", r, a.r[r], b.r[r]); why = buf; }
            if (why.empty() && (a.hi != b.hi || a.lo != b.lo)) why = "hi/lo";
            for (int f = 0; f < 32 && why.empty(); ++f)
                if (a.fi[f] != b.fi[f]) { char buf[96]; std::snprintf(buf, sizeof buf, "f%d gen=%08x ref=%08x", f, a.fi[f], b.fi[f]); why = buf; }
            if (why.empty() && (a.fpcond & 1) != (b.fpcond & 1)) why = "fpcond";
            if (why.empty() && a.fcr31 != b.fcr31) why = "fcr31";
            if (why.empty() && std::memcmp(after, psp::hostPtr(kArea), kAreaSize) != 0) why = "memory";
            ++tested;
            if (!why.empty()) {
                ++failed;
                if (!failsByMn[t.mn]++) {
                    char buf[160];
                    std::snprintf(buf, sizeof buf, "%08x: %s", w, why.c_str());
                    example[t.mn] = buf;
                }
                break; // one failure per instruction word is enough
            }
        }
    }
    std::printf("instruction words: %u, trials run: %d, failing words: %d\n", g_test_op_count, tested, failed);
    for (auto& [mn, n] : failsByMn) std::printf("  %-10s %5d failing words, e.g. %s\n", mn.c_str(), n, example[mn].c_str());
    return failed ? 1 : 0;
}
