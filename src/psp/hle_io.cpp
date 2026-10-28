// IoFileMgrForUser: disc0:/ and umd0:/ read from the user's ISO; ms0:/ maps to a host
// folder (savedata). The data transfer itself happens when a call is made, but completion
// follows PPSSPP's default ("fast") IO timing: an asynchronous operation keeps the fd busy
// (SCE_KERNEL_ERROR_ASYNC_BUSY) until it has finished and its result has been collected with
// sceIoPollAsync / sceIoWaitAsyncCB, and synchronous calls delay the calling thread. Games
// depend on this: the order in which their loader tasks observe finished reads follows it.
#include <cstdio>
#include <map>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "psp/iso.h"
#include "psp/kernel.h"
#include "psp/memory.h"

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0755)
#endif

namespace psp::kernel {

std::string g_hostMsRoot = "memstick/"; // set by the front end

namespace {

constexpr uint32_t ERR_NOFILE = 0x80010002;
constexpr uint32_t ERR_BADF = 0x80020323;
constexpr uint32_t ERR_ASYNC_BUSY = 0x80020329;
constexpr uint32_t ERR_NO_ASYNC = 0x8002032A;

// Delays from PPSSPP Core/HLE/sceIo.cpp (IOTIMING_FAST, its default).
constexpr uint64_t kOpenUmdUs = 4000, kOpenMsUs = 10000, kNotFoundUmdUs = 6000, kSeekUs = 100, kCloseUs = 100;
uint64_t readDelayUs(uint64_t size) { return std::max<uint64_t>(size / 100, 100); }

struct File {
    bool disc = false;
    uint32_t lba = 0;
    uint64_t size = 0, pos = 0;
    FILE* host = nullptr;
    bool asyncPending = false;   // operation in flight until asyncReadyAt
    bool asyncHasResult = false; // finished, result not collected yet
    uint64_t asyncReadyAt = 0;
    uint64_t asyncResult = 0;
    bool closePending = false;
    bool busy() const { return asyncPending || asyncHasResult; }
    std::string path;
};
struct Dir {
    std::vector<IsoEntry> entries;
    size_t next = 0;
};

std::map<int, File> g_files;
std::map<int, Dir> g_dirs;
std::string g_cwd = "disc0:/";

// Splits "disc0:/a/b" into device and path; relative paths use the current directory.
void splitPath(std::string p, std::string& dev, std::string& rest) {
    if (p.find(':') == std::string::npos) p = g_cwd + p;
    size_t colon = p.find(':');
    dev = p.substr(0, colon);
    rest = p.substr(colon + 1);
    while (!rest.empty() && rest[0] == '/') rest.erase(0, 1);
    for (char& ch : dev) ch = (char)std::tolower((unsigned char)ch);
    // umd0:/umd1:/disc0: are the same medium
    if (dev == "umd0" || dev == "umd1" || dev == "umd") dev = "disc0";
}

std::string hostPath(const std::string& rest) { return g_hostMsRoot + rest; }

void mkdirs(const std::string& path) {
    for (size_t i = 0; i < path.size(); ++i)
        if (path[i] == '/') MKDIR(path.substr(0, i).c_str());
}

int openFile(const std::string& path, uint32_t flags) {
    std::string dev, rest;
    splitPath(path, dev, rest);
    File f;
    f.path = path;
    if (dev == "disc0") {
        // sceIoOpen("disc0:/sce_lbn0x...size0x...") style raw sector access
        if (rest.rfind("sce_lbn", 0) == 0) {
            unsigned lbn = 0, size = 0;
            std::sscanf(rest.c_str(), "sce_lbn%xsize%x", &lbn, &size);
            f.disc = true;
            f.lba = lbn;
            f.size = size;
        } else {
            IsoEntry e;
            if (!iso().lookup(rest, e) || e.dir) return (int)ERR_NOFILE;
            f.disc = true;
            f.lba = e.lba;
            f.size = e.size;
        }
    } else if (dev == "ms0" || dev == "fatms0") {
        const char* mode = (flags & 0x0200) ? "w+b" : ((flags & 2) ? "r+b" : "rb"); // PSP_O_CREAT
        std::string hp = hostPath(rest);
        if (flags & 0x0200) mkdirs(hp);
        f.host = std::fopen(hp.c_str(), mode);
        if (!f.host && (flags & 2)) f.host = std::fopen(hp.c_str(), "w+b");
        if (!f.host) return (int)ERR_NOFILE;
        std::fseek(f.host, 0, SEEK_END);
        f.size = (uint64_t)std::ftell(f.host);
        std::fseek(f.host, 0, SEEK_SET);
    } else {
        return (int)ERR_NOFILE;
    }
    // The PSP hands out the lowest free descriptor after stdin/stdout/stderr.
    int fd = 3;
    while (g_files.count(fd) || g_dirs.count(fd)) ++fd;
    g_files[fd] = f;
    return fd;
}

int64_t readFile(int fd, uint32_t dst, uint32_t len) {
    auto it = g_files.find(fd);
    if (it == g_files.end()) return (int32_t)ERR_BADF;
    File& f = it->second;
    if (f.disc) {
        uint64_t n = std::min<uint64_t>(len, f.size > f.pos ? f.size - f.pos : 0);
        size_t got = iso().read(f.lba, f.pos, hostPtr(dst), (size_t)n);
        f.pos += got;
        return (int64_t)got;
    }
    std::fseek(f.host, (long)f.pos, SEEK_SET);
    size_t got = std::fread(hostPtr(dst), 1, len, f.host);
    f.pos += got;
    return (int64_t)got;
}

int64_t seekFile(int fd, int64_t off, int whence) {
    auto it = g_files.find(fd);
    if (it == g_files.end()) return (int32_t)ERR_BADF;
    File& f = it->second;
    int64_t base = whence == 0 ? 0 : (whence == 1 ? (int64_t)f.pos : (int64_t)f.size);
    int64_t np = base + off;
    if (np < 0) return (int32_t)0x80010016;
    f.pos = (uint64_t)np;
    return np;
}

void fillStat(uint32_t addr, bool dir, uint64_t size) {
    std::memset(hostPtr(addr), 0, 0x58);
    write32(addr, dir ? 0x11FF : 0x21FF);     // mode: FIO_S_IFDIR/IFREG | rwx
    write32(addr + 4, dir ? 0x10 : 0x20);     // attr
    write32(addr + 8, (uint32_t)size);
    write32(addr + 12, (uint32_t)(size >> 32));
}

// Starts an asynchronous operation on `f` whose result becomes available after `us`.
void startAsync(File& f, uint64_t result, uint64_t us) {
    f.asyncPending = true;
    f.asyncHasResult = false;
    f.asyncReadyAt = nowUs() + us;
    f.asyncResult = result;
}

void closeNow(int fd) {
    auto it = g_files.find(fd);
    if (it == g_files.end()) return;
    if (it->second.host) std::fclose(it->second.host);
    g_files.erase(it);
}

// Hands the finished result of `fd` to the guest and frees the fd if it was being closed.
void collectAsync(int fd, uint32_t out) {
    File& f = g_files[fd];
    if (out) { write32(out, (uint32_t)f.asyncResult); write32(out + 4, (uint32_t)(f.asyncResult >> 32)); }
    f.asyncPending = f.asyncHasResult = false;
    if (f.closePending) closeNow(fd);
}

bool isUmd(const std::string& path) {
    std::string dev, rest;
    splitPath(path, dev, rest);
    return dev == "disc0";
}

} // namespace

void registerIoHle() {
    registerHle("sceIoOpen", [](Ctx* c) {
        std::string path = readString(A(c, 0));
        int fd = openFile(path, A(c, 1));
        log("sceIoOpen(%s, %x) -> %d", path.c_str(), A(c, 1), fd);
        ret(c, (uint32_t)fd);
        if (fd >= 0) delayCurrentThread(isUmd(path) ? kOpenUmdUs : kOpenMsUs);
        else if (isUmd(path)) delayCurrentThread(kNotFoundUmdUs);
        else delayCurrentThread(kOpenMsUs);
    });
    registerHle("sceIoOpenAsync", [](Ctx* c) {
        std::string path = readString(A(c, 0));
        int fd = openFile(path, A(c, 1));
        log("sceIoOpenAsync(%s) -> %d", path.c_str(), fd);
        if (fd < 0) {
            // The call still returns an fd; the error is its async result (PPSSPP sceIoOpenAsync).
            uint32_t err = (uint32_t)fd;
            fd = 3;
            while (g_files.count(fd) || g_dirs.count(fd)) ++fd;
            File& f = g_files[fd];
            f.path = path;
            f.closePending = true;
            startAsync(f, (uint64_t)(int64_t)(int32_t)err, isUmd(path) ? kNotFoundUmdUs : kOpenMsUs);
        } else {
            startAsync(g_files[fd], (uint64_t)fd, isUmd(path) ? kOpenUmdUs : kOpenMsUs);
        }
        ret(c, (uint32_t)fd);
    });
    registerHle("sceIoClose", [](Ctx* c) {
        int fd = (int)A(c, 0);
        auto it = g_files.find(fd);
        if (it == g_files.end()) { ret(c, ERR_BADF); return; }
        if (it->second.asyncPending) { ret(c, ERR_ASYNC_BUSY); return; }
        closeNow(fd);
        ret(c, 0);
        delayCurrentThread(kCloseUs);
    });
    registerHle("sceIoCloseAsync", [](Ctx* c) {
        auto it = g_files.find((int)A(c, 0));
        if (it == g_files.end()) { ret(c, ERR_BADF); return; }
        if (it->second.busy()) { ret(c, ERR_ASYNC_BUSY); return; }
        it->second.closePending = true;
        startAsync(it->second, 0, 0);
        ret(c, 0);
    });
    registerHle("sceIoRead", [](Ctx* c) {
        int fd = (int)A(c, 0);
        auto it = g_files.find(fd);
        if (it != g_files.end() && it->second.busy()) { ret(c, ERR_ASYNC_BUSY); return; }
        int64_t r = readFile(fd, A(c, 1), A(c, 2));
        log("sceIoRead(%d, %08x, %x) -> %lld", fd, A(c, 1), A(c, 2), (long long)r);
        ret(c, (uint32_t)r);
        if (r >= 0) delayCurrentThread(readDelayUs(A(c, 2)));
    });
    registerHle("sceIoReadAsync", [](Ctx* c) {
        int fd = (int)A(c, 0);
        auto it = g_files.find(fd);
        if (it == g_files.end()) { ret(c, ERR_BADF); return; }
        if (it->second.busy()) { ret(c, ERR_ASYNC_BUSY); return; }
        uint64_t pos = it->second.pos;
        int64_t r = readFile(fd, A(c, 1), A(c, 2));
        log("sceIoReadAsync(%d @%llu, %08x, %x) -> %lld", fd, (unsigned long long)pos, A(c, 1), A(c, 2), (long long)r);
        startAsync(it->second, (uint64_t)r, readDelayUs(A(c, 2)));
        ret(c, 0);
    });
    registerHle("sceIoWrite", [](Ctx* c) {
        int fd = (int)A(c, 0);
        uint32_t src = A(c, 1), len = A(c, 2);
        if (fd == 1 || fd == 2) { // stdout / stderr: the game's debug prints
            std::string s(reinterpret_cast<const char*>(hostPtr(src)), len);
            std::fprintf(stderr, "[GAME] %s", s.c_str());
            ret(c, len);
            return;
        }
        auto it = g_files.find(fd);
        if (it == g_files.end() || !it->second.host) { ret(c, ERR_BADF); return; }
        std::fseek(it->second.host, (long)it->second.pos, SEEK_SET);
        size_t n = std::fwrite(hostPtr(src), 1, len, it->second.host);
        it->second.pos += n;
        it->second.size = std::max(it->second.size, it->second.pos);
        ret(c, (uint32_t)n);
    });
    registerHle("sceIoLseek", [](Ctx* c) {
        // fd in a0, 64-bit offset in a2:a3 (aligned register pair), whence in t0
        int fd = (int)A(c, 0);
        auto it = g_files.find(fd);
        if (it != g_files.end() && it->second.busy()) { ret64(c, (uint64_t)(int64_t)(int32_t)ERR_ASYNC_BUSY); return; }
        int64_t off = (int64_t)(((uint64_t)A(c, 3) << 32) | A(c, 2));
        int64_t r = seekFile(fd, off, (int)A(c, 4));
        log("sceIoLseek(%d, %lld, %d) -> %lld", fd, (long long)off, (int)A(c, 4), (long long)r);
        ret64(c, (uint64_t)r);
    });
    registerHle("sceIoLseekAsync", [](Ctx* c) {
        int fd = (int)A(c, 0);
        auto it = g_files.find(fd);
        if (it == g_files.end()) { ret(c, ERR_BADF); return; }
        if (it->second.busy()) { ret(c, ERR_ASYNC_BUSY); return; }
        int64_t off = (int64_t)(((uint64_t)A(c, 3) << 32) | A(c, 2));
        int64_t r = seekFile(fd, off, (int)A(c, 4));
        log("sceIoLseekAsync(%d, %lld, %d) -> %lld", fd, (long long)off, (int)A(c, 4), (long long)r);
        startAsync(it->second, (uint64_t)r, kSeekUs);
        ret(c, 0);
    });
    registerHle("sceIoPollAsync", [](Ctx* c) {
        int fd = (int)A(c, 0);
        auto it = g_files.find(fd);
        if (it == g_files.end()) { ret(c, ERR_BADF); return; }
        File& f = it->second;
        if (!f.busy()) { ret(c, ERR_NO_ASYNC); return; }
        if (f.asyncPending && nowUs() < f.asyncReadyAt) { ret(c, 1); return; } // not ready
        collectAsync(fd, A(c, 1));
        ret(c, 0);
    });
    registerHle("sceIoWaitAsyncCB", [](Ctx* c) {
        int fd = (int)A(c, 0);
        uint32_t out = A(c, 1);
        auto it = g_files.find(fd);
        if (it == g_files.end()) { ret(c, ERR_BADF); return; }
        if (!it->second.busy()) { ret(c, ERR_NO_ASYNC); runPendingCallbacks(); return; }
        uint64_t now = nowUs(), readyAt = it->second.asyncReadyAt;
        ret(c, 0);
        if (it->second.asyncPending && now < readyAt) delayCurrentThread(readyAt - now, true);
        else runPendingCallbacks();
        if (g_files.count(fd) && g_files[fd].busy()) collectAsync(fd, out);
    });
    registerHle("sceIoWaitAsync", [](Ctx* c) {
        int fd = (int)A(c, 0);
        uint32_t out = A(c, 1);
        auto it = g_files.find(fd);
        if (it == g_files.end()) { ret(c, ERR_BADF); return; }
        if (!it->second.busy()) { ret(c, ERR_NO_ASYNC); return; }
        uint64_t now = nowUs(), readyAt = it->second.asyncReadyAt;
        ret(c, 0);
        if (it->second.asyncPending && now < readyAt) delayCurrentThread(readyAt - now);
        if (g_files.count(fd) && g_files[fd].busy()) collectAsync(fd, out);
    });
    registerHle("sceIoSync", [](Ctx* c) { ret(c, 0); });
    registerHle("sceIoGetstat", [](Ctx* c) {
        std::string dev, rest;
        splitPath(readString(A(c, 0)), dev, rest);
        if (dev == "disc0") {
            IsoEntry e;
            if (!iso().lookup(rest, e)) { ret(c, ERR_NOFILE); return; }
            fillStat(A(c, 1), e.dir, e.size);
            ret(c, 0);
            return;
        }
        struct stat st;
        std::string hp = hostPath(rest);
        while (hp.size() > 1 && hp.back() == '/') hp.pop_back(); // Windows' stat fails on "dir/"
        if (stat(hp.c_str(), &st) != 0) { ret(c, ERR_NOFILE); return; }
        fillStat(A(c, 1), (st.st_mode & S_IFDIR) != 0, (uint64_t)st.st_size);
        ret(c, 0);
    });
    registerHle("sceIoDopen", [](Ctx* c) {
        std::string dev, rest;
        splitPath(readString(A(c, 0)), dev, rest);
        Dir d;
        if (dev != "disc0" || !iso().listDir(rest, d.entries)) { ret(c, ERR_NOFILE); return; }
        int fd = 3;
        while (g_files.count(fd) || g_dirs.count(fd)) ++fd;
        g_dirs[fd] = d;
        ret(c, (uint32_t)fd);
    });
    registerHle("sceIoDread", [](Ctx* c) {
        auto it = g_dirs.find((int)A(c, 0));
        if (it == g_dirs.end()) { ret(c, ERR_BADF); return; }
        if (it->second.next >= it->second.entries.size()) { ret(c, 0); return; }
        const IsoEntry& e = it->second.entries[it->second.next++];
        fillStat(A(c, 1), e.dir, e.size);
        writeString(A(c, 1) + 0x58, e.name);
        ret(c, 1);
    });
    registerHle("sceIoDclose", [](Ctx* c) { ret(c, g_dirs.erase((int)A(c, 0)) ? 0 : ERR_BADF); });
    registerHle("sceIoChdir", [](Ctx* c) {
        g_cwd = readString(A(c, 0));
        if (!g_cwd.empty() && g_cwd.back() != '/') g_cwd += '/';
        ret(c, 0);
    });
    registerHle("sceIoMkdir", [](Ctx* c) {
        std::string dev, rest;
        splitPath(readString(A(c, 0)), dev, rest);
        mkdirs(hostPath(rest) + "/");
        ret(c, 0);
    });
    registerHle("sceIoRmdir", [](Ctx* c) { ret(c, 0); });
    registerHle("sceIoRemove", [](Ctx* c) {
        std::string dev, rest;
        splitPath(readString(A(c, 0)), dev, rest);
        ret(c, std::remove(hostPath(rest).c_str()) == 0 ? 0 : ERR_NOFILE);
    });
    registerHle("sceIoRename", [](Ctx* c) {
        std::string d1, r1, d2, r2;
        splitPath(readString(A(c, 0)), d1, r1);
        splitPath(readString(A(c, 1)), d2, r2);
        ret(c, std::rename(hostPath(r1).c_str(), hostPath(r2).c_str()) == 0 ? 0 : ERR_NOFILE);
    });
    registerHle("sceIoDevctl", [](Ctx* c) {
        // Memory stick commands follow PPSSPP (Core/HLE/sceIo.cpp); the card is always inserted.
        std::string dev = readString(A(c, 0));
        uint32_t cmd = A(c, 1), arg = A(c, 2), out = A(c, 4), outLen = A(c, 5);
        switch (cmd) {
        case 0x02015804: // register insert/eject callback: notify "inserted" right away
            if (arg) notifyCallback((int)read32(arg), 1, 1);
            break;
        case 0x02025801: // driver status: 4 = device inserted
            if (out && outLen >= 4) write32(out, 4);
            break;
        case 0x02025806: // inserted? 1 = yes
            if (out && outLen >= 4) write32(out, 1);
            break;
        case 0x02425818: { // capacity: arg holds a pointer to {maxClusters, freeClusters, maxSectors, sectorSize, sectorCount}
            // PPSSPP reports FAKE_FREE_SPACE (1.5 GB) free, 32 KB clusters of 0x200-byte sectors.
            if (!arg || A(c, 3) < 4) { ret(c, 0x80220081); return; } // SCE_ERROR_MEMSTICK_DEVCTL_BAD_PARAMS
            uint32_t p = read32(arg);
            const uint64_t freeSize = (1024ull + 512) * 1024 * 1024;
            const uint32_t sectorSize = 0x200, sectorCount = 32 * 1024 / sectorSize;
            uint32_t clusters = (uint32_t)((freeSize * 95 / 100) / (sectorSize * sectorCount));
            if (validAddress(p) && validAddress(p + 19)) {
                write32(p, clusters);
                write32(p + 4, clusters);
                write32(p + 8, clusters);
                write32(p + 12, sectorSize);
                write32(p + 16, sectorCount);
            }
            break;
        }
        case 0x02425824: // write protected? 0 = no
            if (out && outLen == 4) write32(out, 0);
            break;
        default:
            log("sceIoDevctl(%s, %08x) unhandled", dev.c_str(), cmd);
            if (out && outLen >= 4) write32(out, 0);
            break;
        }
        ret(c, 0);
    });
}

} // namespace psp::kernel
