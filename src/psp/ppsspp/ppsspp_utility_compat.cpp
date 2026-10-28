// Runtime side of ppsspp_utility_compat.h: logging, string helpers, the memory stick model and
// the ms0:/ file system the ported PPSSPP savedata code works on.
#include "ppsspp_utility_compat.h"

#include <cstdarg>
#include <filesystem>
#include <map>
#include <sys/stat.h>

#include "psp/kernel.h"

namespace psp::kernel {
extern std::string g_hostMsRoot;
}

UtilityConfig g_Config;
MsFileSystem pspFileSystem;

void ppsspp_utility_log(const char* level, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    psp::kernel::log("[UTIL %s] %s", level, buf);
}

int GetPSPLanguage() { return g_Config.iLanguage; }

// from PPSSPP Common/StringUtils.cpp
size_t truncate_cpy(char* dest, size_t destSize, const char* src) {
    size_t len = strlen(src);
    if (len >= destSize - 1) {
        memcpy(dest, src, destSize - 1);
        len = destSize - 1;
    } else {
        memcpy(dest, src, len);
    }
    dest[len] = '\0';
    return len;
}

size_t truncate_cpy(char* dest, size_t destSize, std::string_view src) {
    if (src.size() > destSize - 1) {
        memcpy(dest, src.data(), destSize - 1);
        dest[destSize - 1] = 0;
        return destSize - 1;
    } else {
        memcpy(dest, src.data(), src.size());
        dest[src.size()] = 0;
        return src.size();
    }
}

// StringFromFormat is defined in src/psp/vfpu.cpp.

// from PPSSPP Core/Util/PathUtil.cpp
bool HasPathTraversal(std::string_view path) {
    if (path.find_first_of("/\\") != std::string_view::npos)
        return true;
    return path == "." || path == "..";
}

namespace fs = std::filesystem;

namespace {

struct HostStat {
    bool exists = false, dir = false;
    s64 size = 0;
    time_t atime = 0, mtime = 0, ctime = 0;
};

HostStat hostStat(const std::string& path) {
    HostStat s;
    // Windows' _stat fails on a directory path ending in a separator ("SAVEDATA/").
    std::string p = path;
    while (p.size() > 1 && (p.back() == '/' || p.back() == '\\')) p.pop_back();
#ifdef _WIN32
    struct _stat64 st;
    if (_stat64(p.c_str(), &st) != 0) return s;
    s.dir = (st.st_mode & _S_IFDIR) != 0;
#else
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return s;
    s.dir = S_ISDIR(st.st_mode);
#endif
    s.exists = true;
    s.size = (s64)st.st_size;
    s.atime = st.st_atime;
    s.mtime = st.st_mtime;
    s.ctime = st.st_ctime;
    return s;
}

tm localTm(time_t t) {
    tm out{};
#ifdef _WIN32
    localtime_s(&out, &t);
#else
    localtime_r(&t, &out);
#endif
    return out;
}

PSPFileInfo infoFromStat(const std::string& name, const HostStat& s) {
    PSPFileInfo x;
    x.name = name;
    x.exists = s.exists;
    if (!s.exists) return x;
    x.type = s.dir ? FILETYPE_DIRECTORY : FILETYPE_NORMAL;
    x.size = s.dir ? 0 : s.size;
    x.access = s.dir ? 0777 : 0444; // PspAccessBits (PPSSPP reports isWritable = false on Linux)
    x.atime = localTm(s.atime);
    x.ctime = localTm(s.ctime);
    x.mtime = localTm(s.mtime);
    return x;
}

int strcasecmpAscii(const std::string& a, const std::string& b) {
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        int ca = std::tolower((unsigned char)a[i]), cb = std::tolower((unsigned char)b[i]);
        if (ca != cb) return ca - cb;
    }
    return (int)a.size() - (int)b.size();
}

std::map<int, FILE*> g_handles;
int g_nextHandle = 1;

} // namespace

std::string MsFileSystem::HostPath(const std::string& pspPath) const {
    std::string rest = pspPath;
    size_t colon = rest.find(':');
    if (colon != std::string::npos) rest = rest.substr(colon + 1);
    while (!rest.empty() && rest[0] == '/') rest.erase(0, 1);
    std::string root = psp::kernel::g_hostMsRoot;
    if (!root.empty() && root.back() != '/' && root.back() != '\\') root += '/';
    return root + rest;
}

PSPFileInfo MsFileSystem::GetFileInfo(const std::string& path) {
    PSPFileInfo x = infoFromStat(path, hostStat(HostPath(path)));
    x.name = path;
    return x;
}

std::vector<PSPFileInfo> MsFileSystem::GetDirListing(const std::string& path, bool* exists) {
    std::vector<PSPFileInfo> out;
    std::string hp = HostPath(path);
    HostStat ds = hostStat(hp);
    if (!ds.exists || !ds.dir) {
        if (exists) *exists = false;
        return out;
    }
    if (exists) *exists = true;
    std::vector<PSPFileInfo> entries;
    entries.push_back(infoFromStat(".", ds));
    PSPFileInfo parent = infoFromStat("..", hostStat(hp + "/.."));
    parent.size = 4096; // "The PSP reports a size for the parent entry"
    entries.push_back(parent);
    std::error_code ec;
    for (const auto& de : fs::directory_iterator(fs::u8path(hp), ec)) {
        std::string name = de.path().filename().u8string();
        HostStat s = hostStat(hp + "/" + name);
        if (!s.exists) continue;
        entries.push_back(infoFromStat(name, s));
    }
    // File::GetFilesInDir sorts directories first, then by name (case-insensitive).
    std::sort(entries.begin(), entries.end(), [](const PSPFileInfo& a, const PSPFileInfo& b) {
        bool ad = a.type == FILETYPE_DIRECTORY, bd = b.type == FILETYPE_DIRECTORY;
        if (ad != bd) return ad;
        return strcasecmpAscii(a.name, b.name) < 0;
    });
    // GetDirListing doesn't pass the microseconds on.
    return entries;
}

bool MsFileSystem::MkDir(const std::string& path) {
    std::error_code ec;
    fs::create_directories(fs::u8path(HostPath(path)), ec);
    return !ec && hostStat(HostPath(path)).dir;
}

bool MsFileSystem::RmDir(const std::string& path) {
    std::error_code ec;
    fs::remove_all(fs::u8path(HostPath(path)), ec);
    return !ec;
}

bool MsFileSystem::RemoveFile(const std::string& path) {
    return std::remove(HostPath(path).c_str()) == 0;
}

int MsFileSystem::OpenFile(const std::string& path, FileAccess access) {
    std::string hp = HostPath(path);
    const char* mode = "rb";
    if (access & FILEACCESS_WRITE) {
        if (access & FILEACCESS_TRUNCATE) mode = "wb";
        else mode = hostStat(hp).exists ? "r+b" : ((access & FILEACCESS_CREATE) ? "w+b" : "r+b");
    }
    FILE* f = std::fopen(hp.c_str(), mode);
    if (!f) return -1;
    int h = g_nextHandle++;
    g_handles[h] = f;
    return h;
}

size_t MsFileSystem::ReadFile(int handle, u8* data, s64 size) {
    auto it = g_handles.find(handle);
    if (it == g_handles.end() || size <= 0) return 0;
    return std::fread(data, 1, (size_t)size, it->second);
}

size_t MsFileSystem::WriteFile(int handle, const u8* data, s64 size) {
    auto it = g_handles.find(handle);
    if (it == g_handles.end() || size <= 0) return 0;
    return std::fwrite(data, 1, (size_t)size, it->second);
}

s64 MsFileSystem::SeekFile(int handle, s64 position, FileMove type) {
    auto it = g_handles.find(handle);
    if (it == g_handles.end()) return -1;
    std::fseek(it->second, (long)position, type == FILEMOVE_BEGIN ? SEEK_SET : (type == FILEMOVE_CURRENT ? SEEK_CUR : SEEK_END));
    return std::ftell(it->second);
}

s64 MsFileSystem::GetSeekPos(int handle) {
    auto it = g_handles.find(handle);
    return it == g_handles.end() ? -1 : (s64)std::ftell(it->second);
}

void MsFileSystem::CloseFile(int handle) {
    auto it = g_handles.find(handle);
    if (it == g_handles.end()) return;
    std::fclose(it->second);
    g_handles.erase(it);
}

// from PPSSPP Core/FileSystems/MetaFileSystem.cpp
int MsFileSystem::ReadEntireFile(const std::string& filename, std::vector<u8>& data, bool quiet) {
    int handle = OpenFile(filename, FILEACCESS_READ);
    if (handle < 0)
        return handle;
    SeekFile(handle, 0, FILEMOVE_END);
    size_t dataSize = (size_t)GetSeekPos(handle);
    SeekFile(handle, 0, FILEMOVE_BEGIN);
    data.resize(dataSize);
    size_t result = ReadFile(handle, data.data(), (s64)dataSize);
    CloseFile(handle);
    if (result != dataSize)
        return (int)SCE_KERNEL_ERROR_ERROR;
    return 0;
}

// from PPSSPP Core/HW/MemoryStick.cpp with its default configuration: a 16 GB stick
// (iMemStickSizeGB), no compat flags for this game, and the simulated free space (no
// SYSPROP_CAN_GET_FREE_SPACE_FAST, bReportAccurateFreeStorageSpace off): the stick holds only
// this game's savedata.
static uint64_t recursiveSize(const fs::path& p) {
    uint64_t total = 0;
    std::error_code ec;
    for (const auto& de : fs::recursive_directory_iterator(p, ec)) {
        std::error_code ec2;
        if (de.is_regular_file(ec2)) total += de.file_size(ec2);
    }
    return total;
}

u64 MemoryStick_FreeSpace(std::string gameID) {
    const u64 memStickSize = (u64)g_Config.iMemStickSizeGB * 1024 * 1024 * 1024;
    // ComputeSizeOfSavedataForGame: every directory in SAVEDATA whose name starts with the game ID.
    u64 currentUse = 0;
    std::error_code ec;
    for (const auto& de : fs::directory_iterator(fs::u8path(pspFileSystem.HostPath("ms0:/PSP/SAVEDATA")), ec)) {
        std::error_code ec2;
        if (!de.is_directory(ec2)) continue;
        if (de.path().filename().u8string().compare(0, gameID.size(), gameID) != 0) continue;
        currentUse += recursiveSize(de.path());
    }
    u64 simulatedFreeSpace = 0;
    if (currentUse < memStickSize)
        simulatedFreeSpace = memStickSize - currentUse;
    INFO_LOG(Log::IO, "Reporting simulated free disk space: %llu", (unsigned long long)simulatedFreeSpace);
    return simulatedFreeSpace;
}
