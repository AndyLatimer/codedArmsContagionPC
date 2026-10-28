#pragma once
// PSP Graphics Engine (GE) display-list processor.
// Walks command lists exactly like the hardware front end (BASE/OFFSET/ORIGIN, JUMP,
// CALL/RET, stall address, SIGNAL/FINISH/END) and hands every other command to the
// renderer, which keeps the full register file and draws with OpenGL.
#include <cstdint>
#include <mutex>
#include <vector>

namespace psp::ge {

// Implemented by the renderer (src/psp/gpu). May be null (headless runs).
class Renderer {
public:
    virtual ~Renderer() = default;
    // Called for every command that is not list control. `pc` is the command address and
    // `relAddr` the data resolved as an address relative to BASE/OFFSET (for VADDR/IADDR).
    virtual void command(uint32_t pc, uint32_t cmd, uint32_t data, uint32_t prevData, uint32_t relAddr) = 0;
    virtual void listFinished() {}
};

struct List {
    int id = 0;
    uint32_t start = 0, pc = 0, stall = 0;
    uint32_t base = 0, offset = 0;
    uint32_t callStack[32];
    int callDepth = 0;
    int cbid = -1;
    bool done = false, finishing = false;
    uint32_t signalArg = 0;
};

void setRenderer(Renderer* r);
uint32_t reg(int cmd); // last data written to a GE register (24 bits)

int enqueue(uint32_t list, uint32_t stall, int cbid, bool head);
int updateStall(int id, uint32_t stall);
bool listDone(int id);
bool allDone();
int setCallback(uint32_t signalFunc, uint32_t signalArg, uint32_t finishFunc, uint32_t finishArg);
void unsetCallback(int cbid);

} // namespace psp::ge
