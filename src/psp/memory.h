#pragma once
// Guest memory for recompiled PSP code.
//   0x00010000 scratchpad (16 KB)   0x04000000 VRAM (2 MB)
//   0x08000000 kernel RAM (8 MB)    0x08800000 user RAM (24 MB) up to 0x0A000000
// Cached/uncached/kernel mirrors (0x40000000, 0x80000000, ...) are folded by masking.
#include <cstdint>
#include <cstring>
#include <string>

namespace psp {

constexpr uint32_t kAddrMask = 0x0FFFFFFFu;
// The whole masked range is backed, so a stray guest address can never reach host memory
// outside the block (untouched pages cost no RAM).
constexpr uint32_t kMemSize = kAddrMask + 1u;
constexpr uint32_t kScratchpad = 0x00010000u, kScratchpadSize = 0x4000u;
constexpr uint32_t kVram = 0x04000000u, kVramSize = 0x00200000u;
constexpr uint32_t kKernelRam = 0x08000000u, kUserRam = 0x08800000u, kRamEnd = 0x0A000000u;

extern uint8_t* g_mem;

bool initMemory();
void shutdownMemory();

inline uint8_t* hostPtr(uint32_t addr) { return g_mem + (addr & kAddrMask); }
inline bool validAddress(uint32_t addr) {
    uint32_t a = addr & kAddrMask;
    return (a >= kScratchpad && a < kScratchpad + kScratchpadSize) ||
           (a >= kVram && a < kVram + kVramSize * 2) || (a >= kKernelRam && a < kRamEnd);
}
inline uint32_t read32(uint32_t a) { uint32_t v; std::memcpy(&v, hostPtr(a), 4); return v; }
inline uint16_t read16(uint32_t a) { uint16_t v; std::memcpy(&v, hostPtr(a), 2); return v; }
inline uint8_t read8(uint32_t a) { return *hostPtr(a); }
inline void write32(uint32_t a, uint32_t v) { std::memcpy(hostPtr(a), &v, 4); }
inline void write16(uint32_t a, uint16_t v) { std::memcpy(hostPtr(a), &v, 2); }
inline void write8(uint32_t a, uint8_t v) { *hostPtr(a) = v; }
std::string readString(uint32_t addr, size_t maxLen = 1024);
void writeString(uint32_t addr, const std::string& s);

// Simple first-fit block allocator for SysMemUserForUser partitions.
struct Block {
    uint32_t addr, size;
    int id;
};
class Partition {
public:
    void init(uint32_t start, uint32_t end);
    // type: 0 = low, 1 = high, 2 = specific address
    uint32_t alloc(uint32_t size, int type, uint32_t addr, uint32_t align, int* idOut);
    bool free(int id);
    uint32_t blockAddr(int id) const;
    uint32_t freeTotal() const;
    uint32_t largestFree() const;
    void reserve(uint32_t addr, uint32_t size); // mark a range as used (loaded module)

private:
    uint32_t start_ = 0, end_ = 0;
    int nextId_ = 1;
    Block blocks_[512];
    int count_ = 0;
};
extern Partition g_userPartition;

} // namespace psp
