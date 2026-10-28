// Minimal stand-ins for the PPSSPP headers its utility dialogs (PSPDialog, PSPSaveDialog,
// PSPMsgDialog, SavedataParam), ParamSFO and sceChnnlsv (all GPL-2.0-or-later) depend on, mapped
// onto this port's runtime. Definitions marked "from PPSSPP" are copied from the named PPSSPP
// files (https://github.com/hrydgard/ppsspp, commit 4b71f833d0d5).
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

#include "psp/memory.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
// The PSP and every host this port targets are little endian.
#define COMMON_LITTLE_ENDIAN 1
typedef u16 u16_le;
typedef u32 u32_le;
typedef u64 u64_le;
typedef s16 s16_le;
typedef s32 s32_le;
typedef s64 s64_le;
typedef u32 SceSize;
typedef s32 SceUID;

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

// ---- logging ---------------------------------------------------------------------------------
void ppsspp_utility_log(const char* level, const char* fmt, ...);
namespace PpssppUtilityLog { enum { sceUtility, sceMisc, sceKernel, Loader, IO, HLE }; }
namespace Log = PpssppUtilityLog;
#define ERROR_LOG(cat, ...) ppsspp_utility_log("E", __VA_ARGS__)
#define ERROR_LOG_REPORT(cat, ...) ppsspp_utility_log("E", __VA_ARGS__)
#define WARN_LOG(cat, ...) ppsspp_utility_log("W", __VA_ARGS__)
#define WARN_LOG_REPORT(cat, ...) ppsspp_utility_log("W", __VA_ARGS__)
#define NOTICE_LOG(cat, ...) ppsspp_utility_log("N", __VA_ARGS__)
#define INFO_LOG(cat, ...) ppsspp_utility_log("I", __VA_ARGS__)
#define DEBUG_LOG(cat, ...) do {} while (0)
#define VERBOSE_LOG(cat, ...) do {} while (0)
#define _dbg_assert_(cond) do {} while (0)
#define _dbg_assert_msg_(cond, ...) do {} while (0)

// ---- from PPSSPP Core/HLE/ErrorCodes.h ---------------------------------------------------------
enum PSPErrorCode : u32 {
    SCE_KERNEL_ERROR_ERROR = 0x80020001,
    SCE_KERNEL_ERROR_BAD_ARGUMENT = 0x80000004,
    SCE_ERROR_UTILITY_INVALID_STATUS = 0x80110001,
    SCE_ERROR_UTILITY_INVALID_ADDRESS = 0x80110002,
    SCE_ERROR_UTILITY_INVALID_PARAM_SIZE = 0x80110004,
    SCE_ERROR_UTILITY_WRONG_TYPE = 0x80110005,
    SCE_ERROR_UTILITY_INVALID_SYSTEM_PARAM_ID = 0x80110103,
    SCE_ERROR_UTILITY_MSGDIALOG_BADOPTION = 0x80110501,
    SCE_ERROR_UTILITY_MSGDIALOG_ERRORCODEINVALID = 0x80110502,
    SCE_UTILITY_SAVEDATA_ERROR_TYPE = 0x80110300,
    SCE_UTILITY_SAVEDATA_ERROR_LOAD_NO_MS = 0x80110301,
    SCE_UTILITY_SAVEDATA_ERROR_LOAD_EJECT_MS = 0x80110302,
    SCE_UTILITY_SAVEDATA_ERROR_LOAD_ACCESS_ERROR = 0x80110305,
    SCE_UTILITY_SAVEDATA_ERROR_LOAD_DATA_BROKEN = 0x80110306,
    SCE_UTILITY_SAVEDATA_ERROR_LOAD_NO_DATA = 0x80110307,
    SCE_UTILITY_SAVEDATA_ERROR_LOAD_PARAM = 0x80110308,
    SCE_UTILITY_SAVEDATA_ERROR_LOAD_FILE_NOT_FOUND = 0x80110309,
    SCE_UTILITY_SAVEDATA_ERROR_LOAD_INTERNAL = 0x8011030b,
    SCE_UTILITY_SAVEDATA_ERROR_RW_NO_MEMSTICK = 0x80110321,
    SCE_UTILITY_SAVEDATA_ERROR_RW_MEMSTICK_FULL = 0x80110323,
    SCE_UTILITY_SAVEDATA_ERROR_RW_DATA_BROKEN = 0x80110326,
    SCE_UTILITY_SAVEDATA_ERROR_RW_NO_DATA = 0x80110327,
    SCE_UTILITY_SAVEDATA_ERROR_RW_BAD_PARAMS = 0x80110328,
    SCE_UTILITY_SAVEDATA_ERROR_RW_FILE_NOT_FOUND = 0x80110329,
    SCE_UTILITY_SAVEDATA_ERROR_RW_BAD_STATUS = 0x8011032c,
    SCE_UTILITY_SAVEDATA_ERROR_SAVE_NO_MS = 0x80110381,
    SCE_UTILITY_SAVEDATA_ERROR_SAVE_EJECT_MS = 0x80110382,
    SCE_UTILITY_SAVEDATA_ERROR_SAVE_MS_NOSPACE = 0x80110383,
    SCE_UTILITY_SAVEDATA_ERROR_SAVE_MS_PROTECTED = 0x80110384,
    SCE_UTILITY_SAVEDATA_ERROR_SAVE_ACCESS_ERROR = 0x80110385,
    SCE_UTILITY_SAVEDATA_ERROR_SAVE_PARAM = 0x80110388,
    SCE_UTILITY_SAVEDATA_ERROR_SAVE_NO_UMD = 0x80110389,
    SCE_UTILITY_SAVEDATA_ERROR_SAVE_WRONG_UMD = 0x8011038a,
    SCE_UTILITY_SAVEDATA_ERROR_SAVE_INTERNAL = 0x8011038b,
    SCE_UTILITY_SAVEDATA_ERROR_DELETE_NO_MS = 0x80110341,
    SCE_UTILITY_SAVEDATA_ERROR_DELETE_EJECT_MS = 0x80110342,
    SCE_UTILITY_SAVEDATA_ERROR_DELETE_MS_PROTECTED = 0x80110344,
    SCE_UTILITY_SAVEDATA_ERROR_DELETE_ACCESS_ERROR = 0x80110345,
    SCE_UTILITY_SAVEDATA_ERROR_DELETE_NO_DATA = 0x80110347,
    SCE_UTILITY_SAVEDATA_ERROR_DELETE_PARAM = 0x80110348,
    SCE_UTILITY_SAVEDATA_ERROR_DELETE_INTERNAL = 0x8011034b,
    SCE_UTILITY_SAVEDATA_ERROR_SIZES_NO_MS = 0x801103C1,
    SCE_UTILITY_SAVEDATA_ERROR_SIZES_EJECT_MS = 0x801103C2,
    SCE_UTILITY_SAVEDATA_ERROR_SIZES_ACCESS_ERROR = 0x801103C5,
    SCE_UTILITY_SAVEDATA_ERROR_SIZES_NO_DATA = 0x801103C7,
    SCE_UTILITY_SAVEDATA_ERROR_SIZES_PARAM = 0x801103C8,
    SCE_UTILITY_SAVEDATA_ERROR_SIZES_NO_UMD = 0x801103C9,
    SCE_UTILITY_SAVEDATA_ERROR_SIZES_WRONG_UMD = 0x801103Ca,
    SCE_UTILITY_SAVEDATA_ERROR_SIZES_INTERNAL = 0x801103Cb,
};

// ---- from PPSSPP Core/HLE/sceCtrl.h ----------------------------------------------------------
enum {
    CTRL_SELECT = 0x00000001, CTRL_START = 0x00000008, CTRL_UP = 0x00000010, CTRL_RIGHT = 0x00000020,
    CTRL_DOWN = 0x00000040, CTRL_LEFT = 0x00000080, CTRL_LTRIGGER = 0x00000100,
    CTRL_RTRIGGER = 0x00000200, CTRL_TRIANGLE = 0x00001000, CTRL_CIRCLE = 0x00002000,
    CTRL_CROSS = 0x00004000, CTRL_SQUARE = 0x00008000,
};

// ---- from PPSSPP Core/HLE/sceUtility.h -------------------------------------------------------
#define PSP_SYSTEMPARAM_LANGUAGE_JAPANESE 0
#define PSP_SYSTEMPARAM_LANGUAGE_ENGLISH 1
#define PSP_SYSTEMPARAM_BUTTON_CIRCLE 0
#define PSP_SYSTEMPARAM_BUTTON_CROSS 1
enum class UtilityDialogType {
    NONE, SAVEDATA, MSG, OSK, NET, SCREENSHOT, GAMESHARING, GAMEDATAINSTALL, NPSIGNIN, HTMLVIEWER,
};

// ---- from PPSSPP Core/HLE/sceRtc.h -----------------------------------------------------------
struct ScePspDateTime {
    s16_le year;
    s16_le month;
    s16_le day;
    s16_le hour;
    s16_le minute;
    s16_le second;
    u32_le microsecond;
};
// from PPSSPP Core/HLE/sceIo.cpp
inline void ConvertTmToPspDateTime(ScePspDateTime& date_out, const tm& date_in, int microSeconds) {
    date_out.year = date_in.tm_year + 1900;
    date_out.month = date_in.tm_mon + 1;
    date_out.day = date_in.tm_mday;
    date_out.hour = date_in.tm_hour;
    date_out.minute = date_in.tm_min;
    date_out.second = date_in.tm_sec;
    date_out.microsecond = microSeconds;
}

// ---- guest memory (PPSSPP Core/MemMap.h) -----------------------------------------------------
namespace Memory {
// PPSSPP's MaxSizeAtAddress for the 32 MB memory map this port emulates.
inline u32 MaxSizeAtAddress(u32 address) {
    if ((address & 0x3E000000) == 0x08000000) return 0x0A000000 - (address & 0x3FFFFFFF);
    if ((address & 0xBF800000) == 0x04000000) return 0x04800000 - (address & 0x3FFFFFFF);
    if ((address & 0x3FFFC000) == 0x00010000) return 0x00014000 - (address & 0x3FFFFFFF);
    return 0;
}
inline bool IsValidAddress(u32 address) { return MaxSizeAtAddress(address) != 0; }
inline bool IsValidRange(u32 address, u32 size) {
    return IsValidAddress(address) && MaxSizeAtAddress(address) >= size;
}
inline bool IsValid4AlignedAddress(u32 address) { return IsValidAddress(address) && (address & 3) == 0; }
inline u32 ClampValidSizeAt(u32 address, u32 requestedSize) {
    u32 maxSize = MaxSizeAtAddress(address);
    return requestedSize > maxSize ? maxSize : requestedSize;
}
inline u8* GetPointerUnchecked(u32 address) { return psp::hostPtr(address); }
inline u8* GetPointerWriteUnchecked(u32 address) { return psp::hostPtr(address); }
inline u8* GetPointerWriteOrException(u32 address) { return IsValidAddress(address) ? psp::hostPtr(address) : nullptr; }
inline const u8* GetPointerRangeOrException(u32 address, u32 size) {
    return IsValidRange(address, size) ? psp::hostPtr(address) : nullptr;
}
inline u32 ReadUnchecked_U32(u32 address) { return psp::read32(address); }
inline void WriteUnchecked_U32(u32 value, u32 address) { psp::write32(address, value); }
inline void Memcpy(void* to, u32 from, u32 size, const char* = nullptr) {
    if (IsValidRange(from, size)) std::memcpy(to, psp::hostPtr(from), size);
}
inline void Memcpy(u32 to, const void* from, u32 size, const char* = nullptr) {
    if (IsValidRange(to, size)) std::memcpy(psp::hostPtr(to), from, size);
}
inline void Memset(u32 to, u8 value, u32 size) {
    if (IsValidRange(to, size)) std::memset(psp::hostPtr(to), value, size);
}
} // namespace Memory

// from PPSSPP Core/MemMap.h (the parts the utility code uses)
template <typename T>
struct PSPPointer {
    u32_le ptr;
    T& operator*() const { return *(T*)psp::hostPtr(ptr); }
    const T& operator[](int i) const { return *((const T*)psp::hostPtr(ptr) + i); }
    T& operator[](int i) { return *((T*)psp::hostPtr(ptr) + i); }
    const T* operator->() const { return (const T*)psp::hostPtr(ptr); }
    T* operator->() { return (T*)psp::hostPtr(ptr); }
    PSPPointer<T>& operator=(u32 p) { ptr = p; return *this; }
    operator T*() { return (T*)psp::hostPtr(ptr); }
    operator const T*() const { return (const T*)psp::hostPtr(ptr); }
    bool IsValid() const { return Memory::IsValidRange(ptr, (u32)sizeof(T)); }
    static PSPPointer<T> Create(u32 p) { PSPPointer<T> r; r = p; return r; }
};

constexpr u32 PSP_GetVolatileMemoryStart() { return 0x08400000; }
constexpr u32 PSP_GetVolatileMemoryEnd() { return 0x08800000; }

// ---- strings (from PPSSPP Common/StringUtils.h/.cpp, Core/Util/PathUtil.cpp) -----------------
size_t truncate_cpy(char* dest, size_t destSize, const char* src);
size_t truncate_cpy(char* dest, size_t destSize, std::string_view src);
template <size_t Count>
inline size_t truncate_cpy(char (&out)[Count], const char* src) { return truncate_cpy(out, Count, src); }
template <size_t Count>
inline size_t truncate_cpy(char (&out)[Count], std::string_view src) { return truncate_cpy(out, Count, src); }
template <size_t Count>
inline std::string_view StringViewFromFixedSizeField(const char (&field)[Count]) {
    return std::string_view(field, strnlen(field, Count));
}
std::string StringFromFormat(const char* format, ...);
bool HasPathTraversal(std::string_view path);

// ---- configuration (PPSSPP Core/Config.cpp defaults; headless forces the same values) -------
struct UtilityConfig {
    bool bEncryptSave = true;              // EncryptSave
    int iMemStickSizeGB = 16;              // MemStickSize
    int iButtonPreference = PSP_SYSTEMPARAM_BUTTON_CROSS;
    int iLanguage = PSP_SYSTEMPARAM_LANGUAGE_ENGLISH;
};
extern UtilityConfig g_Config;
int GetPSPLanguage();
// The game never calls sceKernelSetCompiledSdkVersion*, so PPSSPP reports 0.
inline int sceKernelGetCompiledSdkVersion() { return 0; }

// ---- memory stick (from PPSSPP Core/HW/MemoryStick.cpp, default configuration) --------------
enum MemStickState { PSP_MEMORYSTICK_STATE_INSERTED = 0, PSP_MEMORYSTICK_STATE_NOT_INSERTED = 1 };
inline MemStickState MemoryStick_State() { return PSP_MEMORYSTICK_STATE_INSERTED; }
inline u64 MemoryStick_SectorSize() { return 32 * 1024; }
u64 MemoryStick_FreeSpace(std::string gameID);
inline void MemoryStick_NotifyGameName(const std::string&) {}

// ---- ms0:/ file system (stand-in for PPSSPP's pspFileSystem/DirectoryFileSystem) ------------
enum FileType { FILETYPE_NORMAL = 1, FILETYPE_DIRECTORY = 2 };
struct PSPFileInfo {
    std::string name;
    s64 size = 0;
    u32 access = 0;
    bool exists = false;
    FileType type = FILETYPE_NORMAL;
    tm atime{}; int atimeUs = 0;
    tm ctime{}; int ctimeUs = 0;
    tm mtime{}; int mtimeUs = 0;
};
enum FileAccess { FILEACCESS_NONE = 0, FILEACCESS_READ = 1, FILEACCESS_WRITE = 2, FILEACCESS_APPEND = 4,
                  FILEACCESS_CREATE = 8, FILEACCESS_TRUNCATE = 16, FILEACCESS_EXCL = 32, FILEACCESS_PPSSPP_QUIET = 128 };
enum FileMove { FILEMOVE_BEGIN = 0, FILEMOVE_CURRENT = 1, FILEMOVE_END = 2 };

// Paths are PSP paths on ms0:/ ("ms0:/PSP/SAVEDATA/..."), mapped to the host folder
// psp::kernel::g_hostMsRoot like sceIo does.
class MsFileSystem {
public:
    std::string HostPath(const std::string& pspPath) const;
    PSPFileInfo GetFileInfo(const std::string& path);
    // Like PPSSPP's DirectoryFileSystem::GetDirListing: includes "." and ".." (size 4096), sorted
    // directories first, then case-insensitively by name.
    std::vector<PSPFileInfo> GetDirListing(const std::string& path, bool* exists = nullptr);
    bool MkDir(const std::string& path);   // creates the full path
    bool RmDir(const std::string& path);   // deletes recursively
    bool RemoveFile(const std::string& path);
    int OpenFile(const std::string& path, FileAccess access);
    size_t ReadFile(int handle, u8* data, s64 size);
    size_t WriteFile(int handle, const u8* data, s64 size);
    s64 SeekFile(int handle, s64 position, FileMove type);
    s64 GetSeekPos(int handle);
    void CloseFile(int handle);
    int ReadEntireFile(const std::string& path, std::vector<u8>& data, bool quiet = false);
};
extern MsFileSystem pspFileSystem;
