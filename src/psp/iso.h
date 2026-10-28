#pragma once
// Random-access ISO 9660 reader backing the guest's disc0:/ and umd0:/ devices.
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace psp {

struct IsoEntry {
    std::string name;
    uint32_t lba = 0, size = 0;
    bool dir = false;
};

class IsoReader {
public:
    ~IsoReader();
    bool open(const std::string& path);
    // Path relative to the disc root, '/' separated, case-insensitive.
    bool lookup(const std::string& path, IsoEntry& out);
    bool listDir(const std::string& path, std::vector<IsoEntry>& out);
    size_t read(uint32_t lba, uint64_t offset, void* dst, size_t len);
    // Replaces bytes of the disc image as they are read (the port's text changes; the file is
    // not modified). Applied only if the `size` bytes at `pos` have the FNV-1a hash `hash`;
    // returns whether it was.
    bool addPatch(uint64_t pos, size_t size, uint64_t hash, const std::string& bytes);
    uint32_t totalSectors() const { return totalSectors_; }

private:
    bool readDir(uint32_t lba, uint32_t size, std::vector<IsoEntry>& out);
    struct Patch { uint64_t pos; std::string bytes; };
    std::vector<Patch> patches_;
    FILE* f_ = nullptr;
    std::mutex mutex_;
    IsoEntry root_;
    uint32_t totalSectors_ = 0;
};

} // namespace psp
