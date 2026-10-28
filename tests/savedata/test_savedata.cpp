// Savedata tests for the PPSSPP savedata port (src/psp/ppsspp/SavedataParam.cpp & co.).
//
// Builds SceUtilitySavedataParam requests whose buffers live in guest memory, runs the savedata
// operations the game uses on a scratch memory stick folder and checks the files, PARAM.SFO,
// the encryption/hashes and the result fields. When built with CA_PPSSPP_DIR, the encrypted
// data and every hash are also checked against PPSSPP's own, unmodified sceChnnlsv.cpp
// (tests/savedata/ref_chnnlsv.cpp), and the port's encryption is compared byte for byte with
// PPSSPP's for the same KIRK state.
//
//   test_savedata <scratch-dir>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "psp/host.h"
#include "psp/memory.h"
#include "psp/ppsspp/ParamSFO.h"
#include "psp/ppsspp/SavedataParam.h"
#include "psp/ppsspp/sceChnnlsv.h"

namespace fs = std::filesystem;

// ---- runtime pieces the savedata code links against ---------------------------------------
namespace psp::kernel {
std::string g_hostMsRoot;
void log(const char* fmt, ...) {
    if (!std::getenv("SAVEDATA_TEST_VERBOSE")) return;
    va_list a;
    va_start(a, fmt);
    std::vfprintf(stderr, fmt, a);
    std::fprintf(stderr, "\n");
    va_end(a);
}
void notifyStateChanged() {}
} // namespace psp::kernel
namespace psp::host {
Controller g_ctrl;
}
void UtilityDialogInitialize(UtilityDialogType, int, int, int) {}
void UtilityDialogShutdown(UtilityDialogType, int, int, int) {}
int KernelVolatileMemLock(int, u32, u32) { return 0; }
int KernelVolatileMemUnlock(int) { return 0; }
u64 UtilityNowUs() { return 0; }
std::string StringFromFormat(const char* format, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, format);
    std::vsnprintf(buf, sizeof buf, format, ap);
    va_end(ap);
    return buf;
}

#ifdef CA_HAVE_PPSSPP_REF
// PPSSPP's sceChnnlsv (ref_chnnlsv.cpp)
struct ref__pspChnnlsvContext1 { s32 mode; u8 result[0x10]; u8 key[0x10]; s32 keyLength; };
struct ref__pspChnnlsvContext2 { s32 mode; s32 unkn; u8 cryptedData[0x92]; };
int ref_sceSdMacInit(ref__pspChnnlsvContext1& ctx, int value);
int ref_sceSdMacUpdate(ref__pspChnnlsvContext1& ctx, const u8* data, int length);
int ref_sceSdCipherInit(ref__pspChnnlsvContext2& ctx2, int mode, int uknw, u8* data, const u8* cryptkey);
int ref_sceSdCipherUpdate(ref__pspChnnlsvContext2& ctx, u8* data, int alignedLen);
int ref_sceSdCipherFinal(ref__pspChnnlsvContext2& ctx);
int ref_sceSdMacFinal(ref__pspChnnlsvContext1& ctx, u8* in_hash, const u8* in_key);
void ref_Register_sceChnnlsv();
#endif

// ---- tiny test framework ---------------------------------------------------------------------
static int g_failures = 0, g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { ++g_checks; auto va_ = (a); auto vb_ = (b); if (!(va_ == vb_)) { ++g_failures; std::printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, (long long)va_, (long long)vb_); } } while (0)

static std::vector<u8> readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static int align16(int v) { return (v + 15) & ~15; }

// Guest memory layout for the test.
constexpr u32 kData = 0x08900000, kDataBufSize = 0x8000;
constexpr u32 kIcon = 0x08910000, kLoadBuf = 0x08920000, kLoadIcon = 0x08930000;
constexpr u32 kMsFree = 0x08940000, kMsData = 0x08940100, kUtilData = 0x08940200;
constexpr u32 kIdList = 0x08940300, kIdEntries = 0x08940400, kFileList = 0x08941000;
constexpr u32 kFileEntries = 0x08942000, kSysEntries = 0x08943000, kSecEntries = 0x08944000;
constexpr int kDataSize = 12345;
static const u8 kFakePng[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 1, 2, 3, 4, 5, 6, 7, 8};

static void baseRequest(SceUtilitySavedataParam& r, int mode) {
    std::memset(&r, 0, sizeof r);
    r.common.size = 1500; // the size Coded Arms uses (SDK 1.x V2 request)
    r.common.language = 1;
    r.common.buttonSwap = 1;
    r.mode = (SceUtilitySavedataType)mode;
    std::strcpy(r.gameName, "ULES00124");
    std::strcpy(r.saveName, "000");
    std::strcpy(r.fileName, "CODED.ARM");
}

static void saveRequest(SceUtilitySavedataParam& r, int mode) {
    baseRequest(r, mode);
    for (int i = 0; i < kDataSize; ++i) psp::write8(kData + i, (u8)(i * 7 + (i >> 8)));
    r.dataBuf = kData;
    r.dataBufSize = kDataBufSize;
    r.dataSize = kDataSize;
    std::strcpy(r.sfoParam.title, "CODED ARMS");
    std::strcpy(r.sfoParam.savedataTitle, "System data");
    std::strcpy(r.sfoParam.detail, "User ID 0000000000\nSector 00");
    r.sfoParam.parentalLevel = 3;
    std::memcpy(psp::hostPtr(kIcon), kFakePng, sizeof kFakePng);
    r.icon0FileData.buf = kIcon;
    r.icon0FileData.bufSize = sizeof kFakePng;
    r.icon0FileData.size = sizeof kFakePng;
}

// Minimal independent PARAM.SFO reader (index entries in file order).
struct SfoEntry { std::string key; u16 fmt; u32 len, maxLen; std::vector<u8> data; u32 dataOffset; };
static std::vector<SfoEntry> parseSfo(const std::vector<u8>& f) {
    std::vector<SfoEntry> out;
    auto rd32 = [&](size_t o) { u32 v; std::memcpy(&v, &f[o], 4); return v; };
    auto rd16 = [&](size_t o) { u16 v; std::memcpy(&v, &f[o], 2); return v; };
    if (f.size() < 20 || rd32(0) != 0x46535000) return out;
    u32 keyStart = rd32(8), dataStart = rd32(12), n = rd32(16);
    for (u32 i = 0; i < n; ++i) {
        size_t e = 20 + i * 16;
        SfoEntry s;
        s.key = (const char*)&f[keyStart + rd16(e)];
        s.fmt = rd16(e + 2);
        s.len = rd32(e + 4);
        s.maxLen = rd32(e + 8);
        s.dataOffset = dataStart + rd32(e + 12);
        s.data.assign(f.begin() + s.dataOffset, f.begin() + s.dataOffset + s.maxLen);
        out.push_back(s);
    }
    return out;
}
static const SfoEntry* sfoGet(const std::vector<SfoEntry>& v, const char* key) {
    for (auto& e : v) if (e.key == key) return &e;
    return nullptr;
}

#ifdef CA_HAVE_PPSSPP_REF
// PPSSPP SavedataParam::DecryptData / BuildHash / UpdateHash / EncryptData on the reference
// sceChnnlsv (copied from PPSSPP Core/Dialog/SavedataParam.cpp).
static int refDecrypt(unsigned mode, u8* data, int* dataLen, int* alignedLen, const u8* cryptkey, u8* hashOut) {
    ref__pspChnnlsvContext1 ctx1{};
    ref__pspChnnlsvContext2 ctx2{};
    if (*alignedLen <= 0x10) return -1;
    *dataLen -= 0x10;
    *alignedLen -= 0x10;
    if (ref_sceSdMacInit(ctx1, mode) < 0) return -2;
    if (ref_sceSdCipherInit(ctx2, mode, 2, data, cryptkey) < 0) return -3;
    if (ref_sceSdMacUpdate(ctx1, data, 0x10) < 0) return -4;
    if (ref_sceSdMacUpdate(ctx1, data + 0x10, *alignedLen) < 0) return -5;
    if (ref_sceSdCipherUpdate(ctx2, data + 0x10, *alignedLen) < 0) return -6;
    if (ref_sceSdCipherFinal(ctx2) < 0) return -7;
    if (ref_sceSdMacFinal(ctx1, hashOut, cryptkey) < 0) return -7;
    std::memmove(data, data + 0x10, *dataLen);
    return 0;
}
static int refEncrypt(unsigned mode, u8* data, int* dataLen, int* alignedLen, u8* hash, const u8* cryptkey) {
    ref__pspChnnlsvContext1 ctx1{};
    ref__pspChnnlsvContext2 ctx2{};
    std::memmove(data + 0x10, data, *alignedLen);
    std::memset(hash, 0, 0x10);
    std::memset(data, 0, 0x10);
    if (ref_sceSdCipherInit(ctx2, mode, 1, data, cryptkey) < 0) return -1;
    if (ref_sceSdMacInit(ctx1, mode) < 0) return -2;
    if (ref_sceSdMacUpdate(ctx1, data, 0x10) < 0) return -3;
    if (ref_sceSdCipherUpdate(ctx2, data + 0x10, *alignedLen) < 0) return -4;
    std::memset(data + 0x10 + *dataLen, 0, *alignedLen - *dataLen);
    if (ref_sceSdMacUpdate(ctx1, data + 0x10, *alignedLen) < 0) return -5;
    if (ref_sceSdCipherFinal(ctx2) < 0) return -6;
    if (ref_sceSdMacFinal(ctx1, hash, cryptkey) < 0) return -7;
    *alignedLen += 0x10;
    *dataLen += 0x10;
    return 0;
}
static int refBuildHash(u8* output, const u8* data, unsigned len, unsigned alignedLen, int mode, const u8* cryptkey) {
    ref__pspChnnlsvContext1 ctx1;
    std::memset(&ctx1, 0, sizeof ctx1);
    std::memset(output, 0, 0x10);
    if (ref_sceSdMacInit(ctx1, mode & 0xFF) < 0) return -1;
    if (ref_sceSdMacUpdate(ctx1, data, alignedLen) < 0) return -2;
    if (ref_sceSdMacFinal(ctx1, output, cryptkey) < 0) { std::memset(output, 0x1, 0x10); return 0; }
    return 0;
}
static int refUpdateHash(u8* sfoData, int sfoSize, int off, int encryptmode) {
    int alignedLen = align16(sfoSize);
    std::memset(sfoData + off, 0, 128);
    u8 filehash[16];
    int firstHashMode = encryptmode & 2 ? 4 : 2;
    int secondHashMode = encryptmode & 2 ? 3 : 0;
    if (encryptmode & 4) { firstHashMode = 6; secondHashMode = 5; }
    if (refBuildHash(filehash, sfoData, sfoSize, alignedLen, firstHashMode, 0) < 0) return -1;
    std::memcpy(sfoData + off + 0x20, filehash, 0x10);
    sfoData[off] |= 0x01;
    if (encryptmode & 6) {
        sfoData[off] |= (encryptmode & 6) << 4;
        if (refBuildHash(filehash, sfoData, sfoSize, alignedLen, secondHashMode, 0) < 0) return -2;
        std::memcpy(sfoData + off + 0x70, filehash, 0x10);
    }
    if (refBuildHash(filehash, sfoData, sfoSize, alignedLen, 1, 0) < 0) return -3;
    std::memcpy(sfoData + off + 0x10, filehash, 0x10);
    return 0;
}
#endif

static fs::path saveDir() { return fs::path(psp::kernel::g_hostMsRoot) / "PSP/SAVEDATA/ULES00124000"; }

static void testEncryptedSaveLoad() {
    std::printf("-- SAVE (encrypted, mode 1) / LOAD round trip\n");
    g_Config.bEncryptSave = true;
    SceUtilitySavedataParam req;
    saveRequest(req, 3 /* SAVE */);
    SavedataParam p;
    p.SetPspParam(&req);
    CHECK_EQ(p.Save(&req, "000"), 0);

    std::vector<u8> enc = readFile(saveDir() / "CODED.ARM");
    std::vector<u8> sfo = readFile(saveDir() / "PARAM.SFO");
    std::vector<u8> icon = readFile(saveDir() / "ICON0.PNG");
    CHECK_EQ(enc.size(), (size_t)(kDataSize + 16)); // 16-byte IV/header in front
    CHECK(icon.size() == sizeof kFakePng && std::memcmp(icon.data(), kFakePng, sizeof kFakePng) == 0);
    CHECK(!fs::exists(saveDir() / "ICON1.PMF"));
    CHECK(!fs::exists(saveDir() / "PIC1.PNG"));
    CHECK(!fs::exists(saveDir() / "SND0.AT3"));

    // PARAM.SFO: keys in PSP order (sorted), types and reserved sizes as PPSSPP writes them.
    auto e = parseSfo(sfo);
    const char* keys[] = {"CATEGORY", "PARENTAL_LEVEL", "SAVEDATA_DETAIL", "SAVEDATA_DIRECTORY", "SAVEDATA_FILE_LIST",
                          "SAVEDATA_PARAMS", "SAVEDATA_TITLE", "TITLE"};
    CHECK_EQ(e.size(), (size_t)8);
    for (size_t i = 0; i < e.size() && i < 8; ++i) CHECK(e[i].key == keys[i]);
    if (auto s = sfoGet(e, "CATEGORY")) { CHECK_EQ(s->fmt, 0x0204); CHECK_EQ(s->maxLen, 4u); CHECK(std::string((char*)s->data.data()) == "MS"); }
    if (auto s = sfoGet(e, "PARENTAL_LEVEL")) { CHECK_EQ(s->fmt, 0x0404); CHECK_EQ(s->data[0], 3); }
    if (auto s = sfoGet(e, "SAVEDATA_DETAIL")) { CHECK_EQ(s->maxLen, 1024u); CHECK(std::string((char*)s->data.data()) == "User ID 0000000000\nSector 00"); }
    if (auto s = sfoGet(e, "SAVEDATA_DIRECTORY")) { CHECK_EQ(s->maxLen, 64u); CHECK(std::string((char*)s->data.data()) == "ULES00124000"); }
    if (auto s = sfoGet(e, "SAVEDATA_TITLE")) { CHECK_EQ(s->maxLen, 128u); CHECK(std::string((char*)s->data.data()) == "System data"); }
    if (auto s = sfoGet(e, "TITLE")) { CHECK_EQ(s->maxLen, 128u); CHECK(std::string((char*)s->data.data()) == "CODED ARMS"); }
    const SfoEntry* list = sfoGet(e, "SAVEDATA_FILE_LIST");
    const SfoEntry* params = sfoGet(e, "SAVEDATA_PARAMS");
    CHECK(list && list->fmt == 0x0004 && list->maxLen == 99 * 32);
    CHECK(params && params->fmt == 0x0004 && params->maxLen == 128);
    if (list) CHECK(std::string((char*)list->data.data()) == "CODED.ARM");
    if (params) CHECK_EQ(params->data[0], 0x01); // hash flag for crypt mode 1

    // Our own decryption gives the data back (LOAD).
    SceUtilitySavedataParam ld;
    baseRequest(ld, 2 /* LOAD */);
    std::memset(psp::hostPtr(kLoadBuf), 0, kDataBufSize);
    ld.dataBuf = kLoadBuf;
    ld.dataBufSize = kDataBufSize;
    ld.icon0FileData.buf = kLoadIcon;
    ld.icon0FileData.bufSize = 0x1000;
    SavedataParam pl;
    pl.SetPspParam(&ld);
    CHECK_EQ(pl.Load(&ld, "000", 0), 0);
    CHECK_EQ(ld.dataSize, (u32)kDataSize);
    CHECK(std::memcmp(psp::hostPtr(kLoadBuf), psp::hostPtr(kData), kDataSize) == 0);
    CHECK_EQ(ld.bind, 1021);
    CHECK(std::string(ld.sfoParam.title) == "CODED ARMS");
    CHECK(std::string(ld.sfoParam.savedataTitle) == "System data");
    CHECK_EQ(ld.sfoParam.parentalLevel, 3);
    CHECK_EQ(ld.icon0FileData.size, (u32)sizeof kFakePng);

#ifdef CA_HAVE_PPSSPP_REF
    std::printf("-- checking against PPSSPP's sceChnnlsv\n");
    // Decrypt with PPSSPP's code: same plaintext, MAC equal to the hash in the file list.
    std::vector<u8> buf(align16((int)enc.size()) + 16, 0);
    std::memcpy(buf.data(), enc.data(), enc.size());
    int len = (int)enc.size(), alen = align16(len);
    u8 mac[16];
    CHECK_EQ(refDecrypt(1, buf.data(), &len, &alen, nullptr, mac), 0);
    CHECK_EQ(len, kDataSize);
    CHECK(std::memcmp(buf.data(), psp::hostPtr(kData), kDataSize) == 0);
    if (list) CHECK(std::memcmp(list->data.data() + 13, mac, 16) == 0);
    // PARAM.SFO hashes: recompute SAVEDATA_PARAMS with PPSSPP's UpdateHash.
    if (params) {
        std::vector<u8> copy(align16((int)sfo.size()), 0);
        std::memcpy(copy.data(), sfo.data(), sfo.size());
        CHECK_EQ(refUpdateHash(copy.data(), (int)sfo.size(), (int)params->dataOffset, 1), 0);
        CHECK(std::memcmp(copy.data(), sfo.data(), sfo.size()) == 0);
    }
#endif
}

#ifdef CA_HAVE_PPSSPP_REF
static void testEncryptionMatchesPpsspp() {
    std::printf("-- port encryption == PPSSPP encryption for the same KIRK state\n");
    // Both KIRK engines seed their PRNG from time(); initialise them within the same second.
    for (int attempt = 0; attempt < 5; ++attempt) {
        time_t t0 = time(nullptr);
        __ChnnlsvInit();
        ref_Register_sceChnnlsv();
        SceUtilitySavedataParam req;
        saveRequest(req, 3);
        std::strcpy(req.saveName, "009");
        SavedataParam p;
        p.SetPspParam(&req);
        int r = p.Save(&req, "009");
        std::vector<u8> plain(align16(kDataSize) + 0x10, 0);
        std::memcpy(plain.data(), psp::hostPtr(kData), kDataSize);
        int len = kDataSize, alen = align16(kDataSize);
        u8 hash[16];
        int rr = refEncrypt(1, plain.data(), &len, &alen, hash, nullptr);
        if (time(nullptr) != t0) continue; // crossed a second: seeds may differ, retry
        CHECK_EQ(r, 0);
        CHECK_EQ(rr, 0);
        std::vector<u8> ours = readFile(fs::path(psp::kernel::g_hostMsRoot) / "PSP/SAVEDATA/ULES00124009/CODED.ARM");
        CHECK_EQ(ours.size(), (size_t)len);
        CHECK(ours.size() == (size_t)len && std::memcmp(ours.data(), plain.data(), len) == 0);
        auto e = parseSfo(readFile(fs::path(psp::kernel::g_hostMsRoot) / "PSP/SAVEDATA/ULES00124009/PARAM.SFO"));
        if (auto list = sfoGet(e, "SAVEDATA_FILE_LIST")) CHECK(std::memcmp(list->data.data() + 13, hash, 16) == 0);
        fs::remove_all(fs::path(psp::kernel::g_hostMsRoot) / "PSP/SAVEDATA/ULES00124009");
        return;
    }
    CHECK(!"could not initialise both KIRK engines within one second");
}
#endif

static void testSizes() {
    std::printf("-- SIZES\n");
    SceUtilitySavedataParam r;
    baseRequest(r, 8);
    std::memset(r.fileName, 0, sizeof r.fileName);
    std::memset(psp::hostPtr(kMsFree), 0xEE, 0x300);
    r.msFree = kMsFree;
    r.msData = kMsData;
    r.utilityData = kUtilData;
    SceUtilitySavedataMsDataInfo* md = (SceUtilitySavedataMsDataInfo*)psp::hostPtr(kMsData);
    std::memset(md, 0, sizeof *md);
    std::strcpy(md->gameName, "ULES00124");
    std::strcpy(md->saveName, "000");
    r.dataSize = 0;
    SavedataParam p;
    p.SetPspParam(&r);
    CHECK_EQ(p.GetSizes(&r), 0);
    auto* mf = (SceUtilitySavedataMsFreeInfo*)psp::hostPtr(kMsFree);
    // 16 GB stick holding only this game's savedata (PPSSPP's simulated free space).
    u64 used = 0;
    for (auto& de : fs::recursive_directory_iterator(fs::path(psp::kernel::g_hostMsRoot) / "PSP/SAVEDATA"))
        if (de.is_regular_file()) used += de.file_size();
    u64 freeBytes = 16ull * 1024 * 1024 * 1024 - used;
    CHECK_EQ(mf->clusterSize, 32768);
    CHECK_EQ((u32)mf->freeClusters, (u32)(freeBytes / 32768));
    CHECK_EQ((u32)mf->freeSpaceKB, (u32)(freeBytes / 1024));
    CHECK(std::string(mf->freeSpaceStr, strnlen(mf->freeSpaceStr, 8)) == "15 GB");
    // ".", "..", 3 files: ".." counts 4096 bytes (one cluster) like PPSSPP's listing.
    CHECK_EQ(md->info.usedClusters, 4);
    CHECK_EQ(md->info.usedSpaceKB, 128);
    CHECK(std::string(md->info.usedSpaceStr) == "128 KB");
    CHECK_EQ(md->info.usedSpace32KB, 128);
    auto* ud = (SceUtilitySavedataUsedDataInfo*)psp::hostPtr(kUtilData);
    // dir + SFO, no file name -> no data, no icons: 2 clusters (PPSSPP logs 64 KB for the game's call)
    CHECK_EQ(ud->usedClusters, 2);
    CHECK_EQ(ud->usedSpaceKB, 64);
    CHECK(std::string(ud->usedSpaceStr) == "64 KB");

    std::strcpy(md->saveName, "001");
    CHECK_EQ((u32)p.GetSizes(&r), (u32)SCE_UTILITY_SAVEDATA_ERROR_SIZES_NO_DATA);
    CHECK_EQ(md->info.usedClusters, 0);
}

static void testListAndFiles() {
    std::printf("-- LIST / FILES\n");
    SceUtilitySavedataParam r;
    baseRequest(r, 11);
    std::strcpy(r.saveName, "*");
    r.idList = kIdList;
    auto* il = (SceUtilitySavedataIdListInfo*)psp::hostPtr(kIdList);
    il->maxCount = 10;
    il->resultCount = -1;
    il->entries = kIdEntries;
    SavedataParam p;
    p.SetPspParam(&r);
    CHECK(p.GetList(&r));
    CHECK_EQ(il->resultCount, 1);
    auto* ent = (SceUtilitySavedataIdListEntry*)psp::hostPtr(kIdEntries);
    CHECK_EQ(ent->st_mode, 0x11FF);
    CHECK(std::string(ent->name) == "000");

    SceUtilitySavedataParam f;
    baseRequest(f, 12);
    f.fileList = kFileList;
    auto* fl = (SceUtilitySavedataFileListInfo*)psp::hostPtr(kFileList);
    std::memset(fl, 0, sizeof *fl);
    fl->maxSecureEntries = 5;
    fl->maxNormalEntries = 5;
    fl->maxSystemEntries = 5;
    fl->secureEntries = kSecEntries;
    fl->normalEntries = kFileEntries;
    fl->systemEntries = kSysEntries;
    SavedataParam pf;
    pf.SetPspParam(&f);
    CHECK_EQ(pf.GetFilesList(&f, 0x08950000, "000"), 0);
    CHECK_EQ(fl->resultNumSecureEntries, 1u);
    CHECK_EQ(fl->resultNumNormalEntries, 0u);
    CHECK_EQ(fl->resultNumSystemEntries, 2u); // ICON0.PNG, PARAM.SFO
    auto* se = (SceUtilitySavedataFileListEntry*)psp::hostPtr(kSecEntries);
    CHECK(std::string(se->name) == "CODED.ARM");
    CHECK_EQ(se->st_size, (u64)kDataSize); // the 16-byte header is not counted
    CHECK_EQ(f.bind, 1021);
}

static void testUnencrypted() {
    std::printf("-- SAVE without encryption (bEncryptSave off) / LOAD\n");
    g_Config.bEncryptSave = false;
    SceUtilitySavedataParam req;
    saveRequest(req, 1 /* AUTOSAVE */);
    std::strcpy(req.saveName, "001");
    SavedataParam p;
    p.SetPspParam(&req);
    CHECK_EQ(p.Save(&req, "001"), 0);
    fs::path dir = fs::path(psp::kernel::g_hostMsRoot) / "PSP/SAVEDATA/ULES00124001";
    std::vector<u8> raw = readFile(dir / "CODED.ARM");
    CHECK(raw.size() == (size_t)kDataSize && std::memcmp(raw.data(), psp::hostPtr(kData), kDataSize) == 0);
    auto e = parseSfo(readFile(dir / "PARAM.SFO"));
    if (auto s = sfoGet(e, "SAVEDATA_PARAMS")) {
        bool zero = true;
        for (u8 b : s->data) zero = zero && b == 0;
        CHECK(zero);
    }
    if (auto s = sfoGet(e, "SAVEDATA_FILE_LIST")) {
        // PPSSPP still lists the file as secure (secure mode), with an all-zero hash.
        CHECK(std::string((char*)s->data.data()) == "CODED.ARM");
        bool zero = true;
        for (int i = 13; i < 29; ++i) zero = zero && s->data[i] == 0;
        CHECK(zero);
    }
    SceUtilitySavedataParam ld;
    baseRequest(ld, 0 /* AUTOLOAD */);
    std::strcpy(ld.saveName, "001");
    std::memset(psp::hostPtr(kLoadBuf), 0, kDataBufSize);
    ld.dataBuf = kLoadBuf;
    ld.dataBufSize = kDataBufSize;
    SavedataParam pl;
    pl.SetPspParam(&ld);
    CHECK_EQ(pl.Load(&ld, "001", 0), 0);
    CHECK_EQ(ld.dataSize, (u32)kDataSize);
    CHECK(std::memcmp(psp::hostPtr(kLoadBuf), psp::hostPtr(kData), kDataSize) == 0);
    g_Config.bEncryptSave = true;
}

static void testListLoadAndDelete() {
    std::printf("-- LISTLOAD list / missing data / DELETE\n");
    // The game's list request: a saveNameList of five slots.
    const char* names[] = {"000", "001", "002", "003", "004", ""};
    u32 listAddr = 0x08960000;
    for (int i = 0; i < 6; ++i) { std::memset(psp::hostPtr(listAddr + i * 20), 0, 20); std::strcpy((char*)psp::hostPtr(listAddr + i * 20), names[i]); }
    SceUtilitySavedataParam r;
    baseRequest(r, 4 /* LISTLOAD */);
    r.saveNameList = listAddr;
    SavedataParam p;
    p.SetPspParam(&r);
    CHECK_EQ(p.GetFilenameCount(), 2); // LISTLOAD lists existing saves only
    CHECK(p.GetFilename(0) == "000");
    CHECK(p.GetFilename(1) == "001");
    SceUtilitySavedataParam s;
    baseRequest(s, 5 /* LISTSAVE */);
    s.saveNameList = listAddr;
    SavedataParam ps;
    ps.SetPspParam(&s);
    CHECK_EQ(ps.GetFilenameCount(), 5); // LISTSAVE also shows the empty slots
    CHECK_EQ(ps.GetFileInfo(2).size, 0);

    SceUtilitySavedataParam l;
    baseRequest(l, 2);
    std::strcpy(l.saveName, "003");
    l.dataBuf = kLoadBuf;
    l.dataBufSize = kDataBufSize;
    SavedataParam pl;
    pl.SetPspParam(&l);
    CHECK_EQ((u32)pl.Load(&l, "003", 0), (u32)SCE_UTILITY_SAVEDATA_ERROR_LOAD_NO_DATA);

    SceUtilitySavedataParam d;
    baseRequest(d, 10 /* DELETE */);
    std::strcpy(d.saveName, "001");
    SavedataParam pd;
    pd.SetPspParam(&d);
    CHECK(pd.Delete(&d, "ULES00124001"));
    CHECK(!fs::exists(fs::path(psp::kernel::g_hostMsRoot) / "PSP/SAVEDATA/ULES00124001"));
    CHECK(fs::exists(saveDir()));
}

int main(int argc, char** argv) {
    fs::path root = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "ca_savedata_test";
    fs::remove_all(root);
    fs::create_directories(root / "PSP/SAVEDATA");
    psp::kernel::g_hostMsRoot = root.string() + "/";
    if (!psp::initMemory()) { std::printf("no guest memory\n"); return 2; }
    __ChnnlsvInit();
#ifdef CA_HAVE_PPSSPP_REF
    ref_Register_sceChnnlsv();
#else
    std::printf("(built without CA_PPSSPP_DIR: no cross-check against PPSSPP's sceChnnlsv)\n");
#endif
    testEncryptedSaveLoad();
#ifdef CA_HAVE_PPSSPP_REF
    testEncryptionMatchesPpsspp();
#endif
    testSizes();
    testListAndFiles();
    testUnencrypted();
    testListLoadAndDelete();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
