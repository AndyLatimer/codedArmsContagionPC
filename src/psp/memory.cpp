#include "psp/memory.h"
#include <algorithm>
#include <cstdlib>

namespace psp {

uint8_t* g_mem = nullptr;
Partition g_userPartition;

bool initMemory() {
    if (g_mem) return true;
    g_mem = static_cast<uint8_t*>(std::calloc(1, kMemSize + 64)); // + room for an access straddling the end
    return g_mem != nullptr;
}

void shutdownMemory() {
    std::free(g_mem);
    g_mem = nullptr;
}

std::string readString(uint32_t addr, size_t maxLen) {
    std::string s;
    if (!addr) return s;
    for (size_t i = 0; i < maxLen; ++i) {
        char ch = static_cast<char>(read8(addr + uint32_t(i)));
        if (!ch) break;
        s += ch;
    }
    return s;
}

void writeString(uint32_t addr, const std::string& s) {
    std::memcpy(hostPtr(addr), s.c_str(), s.size() + 1);
}

void Partition::init(uint32_t start, uint32_t end) {
    start_ = start;
    end_ = end;
    count_ = 0;
}

void Partition::reserve(uint32_t addr, uint32_t size) {
    if (count_ < 512) blocks_[count_++] = {addr, size, 0};
    std::sort(blocks_, blocks_ + count_, [](const Block& a, const Block& b) { return a.addr < b.addr; });
}

uint32_t Partition::alloc(uint32_t size, int type, uint32_t addr, uint32_t align, int* idOut) {
    if (count_ >= 512 || size == 0) return 0;
    if (align < 256) align = 256; // the PSP allocates in 256-byte units
    size = (size + 255) & ~255u;
    auto fits = [&](uint32_t a) {
        if (a < start_ || a + size > end_) return false;
        for (int i = 0; i < count_; ++i)
            if (a < blocks_[i].addr + blocks_[i].size && a + size > blocks_[i].addr) return false;
        return true;
    };
    uint32_t found = 0;
    if (type == 2) {
        if (fits(addr)) found = addr;
    } else if (type == 1) { // from the top
        for (uint32_t a = (end_ - size) & ~(align - 1); a >= start_ && a + size <= end_; a -= align)
            if (fits(a)) { found = a; break; }
    } else {
        uint32_t a = (start_ + align - 1) & ~(align - 1);
        // candidate positions: partition start and the end of every block
        for (int i = -1; i < count_; ++i) {
            uint32_t cand = i < 0 ? a : ((blocks_[i].addr + blocks_[i].size + align - 1) & ~(align - 1));
            if (fits(cand)) { found = cand; break; }
        }
    }
    if (!found) return 0;
    int id = nextId_++;
    blocks_[count_++] = {found, size, id};
    std::sort(blocks_, blocks_ + count_, [](const Block& x, const Block& y) { return x.addr < y.addr; });
    if (idOut) *idOut = id;
    return found;
}

bool Partition::free(int id) {
    for (int i = 0; i < count_; ++i)
        if (blocks_[i].id == id && id != 0) {
            blocks_[i] = blocks_[--count_];
            std::sort(blocks_, blocks_ + count_, [](const Block& x, const Block& y) { return x.addr < y.addr; });
            return true;
        }
    return false;
}

uint32_t Partition::blockAddr(int id) const {
    for (int i = 0; i < count_; ++i)
        if (blocks_[i].id == id) return blocks_[i].addr;
    return 0;
}

uint32_t Partition::freeTotal() const {
    uint32_t used = 0;
    for (int i = 0; i < count_; ++i) used += blocks_[i].size;
    return (end_ - start_) - used;
}

uint32_t Partition::largestFree() const {
    uint32_t best = 0, cur = start_;
    for (int i = 0; i < count_; ++i) {
        if (blocks_[i].addr > cur) best = std::max(best, blocks_[i].addr - cur);
        cur = std::max(cur, blocks_[i].addr + blocks_[i].size);
    }
    if (end_ > cur) best = std::max(best, end_ - cur);
    return best;
}

} // namespace psp
