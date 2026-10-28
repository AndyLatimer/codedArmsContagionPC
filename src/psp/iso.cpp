#include "psp/iso.h"
#include <algorithm>
#include <cctype>
#include <cstring>

namespace psp {

static constexpr uint32_t kSector = 2048;

static uint32_t rd32le(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }

IsoReader::~IsoReader() {
    if (f_) std::fclose(f_);
}

bool IsoReader::open(const std::string& path) {
    f_ = std::fopen(path.c_str(), "rb");
    if (!f_) return false;
    uint8_t pvd[kSector];
    if (std::fseek(f_, 16 * kSector, SEEK_SET) != 0 || std::fread(pvd, 1, kSector, f_) != kSector) return false;
    if (pvd[0] != 1 || std::memcmp(pvd + 1, "CD001", 5) != 0) return false;
    totalSectors_ = rd32le(pvd + 80);
    root_.lba = rd32le(pvd + 156 + 2);
    root_.size = rd32le(pvd + 156 + 10);
    root_.dir = true;
    return true;
}

size_t IsoReader::read(uint32_t lba, uint64_t offset, void* dst, size_t len) {
    std::lock_guard<std::mutex> lk(mutex_);
    uint64_t pos = uint64_t(lba) * kSector + offset;
#ifdef _WIN32
    if (_fseeki64(f_, (long long)pos, SEEK_SET) != 0) return 0;
#else
    if (fseeko(f_, (off_t)pos, SEEK_SET) != 0) return 0;
#endif
    const size_t got = std::fread(dst, 1, len, f_);
    for (const Patch& p : patches_) {
        const uint64_t lo = std::max<uint64_t>(p.pos, pos), hi = std::min<uint64_t>(p.pos + p.bytes.size(), pos + got);
        if (lo < hi) std::memcpy(static_cast<char*>(dst) + (lo - pos), p.bytes.data() + (lo - p.pos), (size_t)(hi - lo));
    }
    return got;
}

bool IsoReader::addPatch(uint64_t pos, size_t size, uint64_t hash, const std::string& bytes) {
    std::string cur(size, '\0');
    if (read((uint32_t)(pos / kSector), pos % kSector, cur.data(), size) != size) return false;
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : cur) h = (h ^ c) * 0x100000001b3ull;
    if (h != hash) return false;
    std::lock_guard<std::mutex> lk(mutex_);
    patches_.push_back({pos, bytes});
    return true;
}

bool IsoReader::readDir(uint32_t lba, uint32_t size, std::vector<IsoEntry>& out) {
    std::vector<uint8_t> d(size);
    if (read(lba, 0, d.data(), size) != size) return false;
    for (size_t p = 0; p < d.size();) {
        uint8_t len = d[p];
        if (len == 0) {
            p = (p / kSector + 1) * kSector;
            continue;
        }
        uint8_t nameLen = d[p + 32];
        std::string name(reinterpret_cast<const char*>(&d[p + 33]), nameLen);
        size_t semi = name.find(';');
        if (semi != std::string::npos) name.resize(semi);
        if (!(nameLen == 1 && (d[p + 33] == 0 || d[p + 33] == 1))) // skip . and ..
            out.push_back({name, rd32le(&d[p + 2]), rd32le(&d[p + 10]), (d[p + 25] & 2) != 0});
        p += len;
    }
    return true;
}

static bool iequal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::toupper((unsigned char)a[i]) != std::toupper((unsigned char)b[i])) return false;
    return true;
}

bool IsoReader::lookup(const std::string& path, IsoEntry& out) {
    IsoEntry cur = root_;
    size_t start = 0;
    while (start < path.size()) {
        size_t slash = path.find('/', start);
        std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        start = slash == std::string::npos ? path.size() : slash + 1;
        if (part.empty() || part == ".") continue;
        if (!cur.dir) return false;
        std::vector<IsoEntry> entries;
        if (!readDir(cur.lba, cur.size, entries)) return false;
        bool found = false;
        for (const IsoEntry& e : entries)
            if (iequal(e.name, part)) {
                cur = e;
                found = true;
                break;
            }
        if (!found) return false;
    }
    out = cur;
    return true;
}

bool IsoReader::listDir(const std::string& path, std::vector<IsoEntry>& out) {
    IsoEntry e;
    if (!lookup(path, e) || !e.dir) return false;
    return readDir(e.lba, e.size, out);
}

} // namespace psp
