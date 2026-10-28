// sceUtility: the savedata and message dialogs, ported from PPSSPP's Core/HLE/sceUtility.cpp
// (GPL-2.0-or-later). The dialogs themselves are PPSSPP's PSPSaveDialog/PSPMsgDialog/SavedataParam
// (src/psp/ppsspp/), which show their screens through src/psp/host_dialog.h instead of PPGe.
//
// Like PPSSPP, a dialog's start-up and shutdown run on a helper thread ("ScePafJob") that locks
// volatile memory, works for a while at the dialog's accessThread priority, then at its
// graphicsThread priority, and finally moves the status on. The game's own threads interleave
// with it by priority exactly as they do with PPSSPP's MIPS helper, so GetStatus reports the same
// INITIALIZE/RUNNING/FINISHED/SHUTDOWN/NONE sequence at the same points.
//
// The game imports only the savedata, message and (without InitStart) netconf dialogs.
#include <cstdlib>
#include <cstring>
#include <string>

#include "psp/kernel.h"
#include "psp/memory.h"
#include "psp/ppsspp/PSPMsgDialog.h"
#include "psp/ppsspp/PSPSaveDialog.h"
#include "psp/ppsspp/sceChnnlsv.h"

// ---- volatile memory lock (PPSSPP Core/HLE/scePower.cpp) --------------------------------------
namespace {
bool g_volatileMemLocked = false;
constexpr u32 SCE_KERNEL_ERROR_INVALID_MODE = 0x80000107;
constexpr u32 SCE_KERNEL_ERROR_POWER_VMEM_IN_USE = 0x802b0200;
constexpr u32 SCE_KERNEL_ERROR_SEMA_OVF = 0x800201ae;
} // namespace

int KernelVolatileMemLock(int type, u32 paddr, u32 psize) {
    if (type != 0)
        return (int)SCE_KERNEL_ERROR_INVALID_MODE;
    if (g_volatileMemLocked)
        return (int)SCE_KERNEL_ERROR_POWER_VMEM_IN_USE;
    if (Memory::IsValidAddress(paddr))
        Memory::WriteUnchecked_U32(0x08400000, paddr);
    if (Memory::IsValidAddress(psize))
        Memory::WriteUnchecked_U32(0x00400000, psize);
    g_volatileMemLocked = true;
    return 0;
}

int KernelVolatileMemUnlock(int type) {
    if (type != 0)
        return (int)SCE_KERNEL_ERROR_INVALID_MODE;
    if (!g_volatileMemLocked)
        return (int)SCE_KERNEL_ERROR_SEMA_OVF;
    g_volatileMemLocked = false;
    psp::kernel::notifyStateChanged(); // a helper may be waiting in sceKernelVolatileMemLock
    return 0;
}

u64 UtilityNowUs() { return psp::kernel::nowUs(); }

namespace psp::kernel {

namespace {

// from PPSSPP Core/HLE/sceUtility.cpp -----------------------------------------------------------
UtilityDialogType currentDialogType;
bool currentDialogActive;
PSPSaveDialog* saveDialog;
PSPMsgDialog* msgDialog;

int oldStatus = -1;
int accessThread = 0; // helper thread id (0 = none)
bool accessThreadFinished = true;
const char* accessThreadState = "initial";

const char* UtilityDialogStatusToString(int status) {
    switch (status) {
    case PSPDialog::SCE_UTILITY_STATUS_NONE: return "STATUS_NONE";
    case PSPDialog::SCE_UTILITY_STATUS_INITIALIZE: return "STATUS_INITIALIZE";
    case PSPDialog::SCE_UTILITY_STATUS_RUNNING: return "STATUS_RUNNING";
    case PSPDialog::SCE_UTILITY_STATUS_FINISHED: return "STATUS_FINISHED";
    case PSPDialog::SCE_UTILITY_STATUS_SHUTDOWN: return "STATUS_SHUTDOWN";
    default: return "(unknown)";
    }
}

void CleanupDialogThreads(bool force = false) {
    if (accessThread) {
        if (threadStopped(accessThread) || accessThreadFinished) {
            accessThread = 0;
            accessThreadState = "cleaned up";
        } else if (force) {
            log("[UTIL E] Utility access thread still running, state: %s, dialog=%d/%d", accessThreadState,
                (int)currentDialogType, currentDialogActive);
            terminateThread(accessThread);
            accessThread = 0;
            const bool wasInitializing = !strcmp(accessThreadState, "initializing");
            accessThreadState = "force terminated";
            if (wasInitializing) {
                KernelVolatileMemUnlock(0);
            }
        }
    }
}

void ActivateDialog(UtilityDialogType type) {
    CleanupDialogThreads();
    if (!currentDialogActive) {
        currentDialogType = type;
        currentDialogActive = true;
        oldStatus = -1;
    }
}

void DeactivateDialog() {
    CleanupDialogThreads();
    if (currentDialogActive) {
        currentDialogActive = false;
    }
}

PSPDialog* CurrentDialog(UtilityDialogType type) {
    switch (type) {
    case UtilityDialogType::SAVEDATA: return saveDialog;
    case UtilityDialogType::MSG: return msgDialog;
    default: return nullptr;
    }
}

bool UtilityDialogBusy() {
    PSPDialog* current = CurrentDialog(currentDialogType);
    return current && current->IsBusy();
}

template <typename T>
int UtilityInitStart(UtilityDialogType type, T* dialog, u32 paramAddr) {
    if (UtilityDialogBusy()) {
        return SCE_ERROR_UTILITY_INVALID_STATUS;
    }
    PSPDialog* current = CurrentDialog(currentDialogType);
    if (current) {
        current->FinishVolatile();
    }
    dialog->FinishAutoShutdown();
    const bool wasBusy = dialog->IsBusy();
    int result = dialog->Init(paramAddr);
    if (result >= 0 || (!wasBusy && dialog->IsBusy())) {
        if (current && current != dialog) {
            current->FinishAutoShutdown();
        }
        currentDialogActive = false;
        ActivateDialog(type);
    }
    return result;
}

// UtilityWorkUs: "This blocks, but other better priority threads can get time."
void UtilityWorkUs(int us) {
    if (us > 1000) {
        eatUs(1000);
        delayCurrentThread((uint64_t)(us - 1000)); // hleDelayResult
        return;
    }
    eatUs((uint64_t)us);
    yieldIfHigherPriorityReady(); // hleReSchedule
}

// sceKernelVolatileMemLock as the helper calls it: waits while another holder has the lock.
void HelperVolatileMemLock() {
    if (KernelVolatileMemLock(0, 0, 0) == 0) {
        eatUs(5); // hleEatCycles(1200)
        return;
    }
    waitUntil([] { return KernelVolatileMemLock(0, 0, 0) == 0; }, 0);
}

int UtilityInitDialog(int type) {
    PSPDialog* dialog = CurrentDialog((UtilityDialogType)type);
    accessThreadFinished = true;
    accessThreadState = "init finished";
    if (dialog)
        return dialog->FinishInit();
    return 0;
}

int UtilityFinishDialog(int type) {
    PSPDialog* dialog = CurrentDialog((UtilityDialogType)type);
    accessThreadFinished = true;
    accessThreadState = "shutdown finished";
    if (dialog)
        return dialog->FinishShutdown();
    return 0;
}

bool ValidThreadPriority(int priority) { return priority >= 0x08 && priority <= 0x77; }
int PhasePriority(int priority, int fallback) { return ValidThreadPriority(priority) ? priority : fallback; }
int HelperPriority(int priority) { return priority >= 0 && priority <= 0x7F ? priority : 0x20; }

} // namespace

} // namespace psp::kernel

// PPSSPP runs these phases as MIPS code on an HLEHelperThread; here the same steps run as host
// code on a kernel-scheduled helper thread.
void UtilityDialogInitialize(UtilityDialogType type, int delayUs, int accessPriority, int graphicsPriority) {
    using namespace psp::kernel;
    int partDelay = delayUs / 4;
    accessPriority = HelperPriority(accessPriority);
    const int dialogPriority = PhasePriority(graphicsPriority, accessPriority);

    CleanupDialogThreads(true);
    accessThreadFinished = false;
    accessThreadState = "initializing";
    accessThread = startHostThread("ScePafJob", accessPriority, [type, partDelay, dialogPriority] {
        HelperVolatileMemLock();
        // Loading, at accessThread priority.
        UtilityWorkUs(partDelay);
        UtilityWorkUs(partDelay);
        UtilityWorkUs(partDelay);
        // Setting up the dialog, at graphicsThread priority, then the status goes to RUNNING.
        changeCurrentThreadPriority(dialogPriority);
        UtilityWorkUs(partDelay);
        UtilityInitDialog((int)type);
    });
}

void UtilityDialogShutdown(UtilityDialogType type, int delayUs, int accessPriority, int graphicsPriority) {
    using namespace psp::kernel;
    accessPriority = HelperPriority(accessPriority);
    int partDelay = delayUs / 4;
    const int dialogPriority = PhasePriority(graphicsPriority, accessPriority);
    // A savedata shutdown ends at priority 0x20, whatever the dialog's thread priorities
    // (pspautotests utility/savedata/shutdownstatus).
    const int finalPriority = type == UtilityDialogType::SAVEDATA ? 0x20 : accessPriority;

    CleanupDialogThreads(true);
    accessThreadFinished = false;
    accessThreadState = "shutting down";
    accessThread = startHostThread("ScePafJob", accessPriority, [type, partDelay, dialogPriority, accessPriority, finalPriority] {
        // Tearing down the dialog, at graphicsThread priority.
        changeCurrentThreadPriority(dialogPriority);
        UtilityWorkUs(partDelay);
        UtilityWorkUs(partDelay);
        // Cleaning up at accessThread priority.
        changeCurrentThreadPriority(accessPriority);
        UtilityWorkUs(partDelay);
        UtilityWorkUs(partDelay);
        // Then the status goes to NONE, from priority finalPriority.
        changeCurrentThreadPriority(finalPriority);
        UtilityFinishDialog((int)type);
    });
}

namespace psp::kernel {

void registerUtilityHle() {
    // Simulated memory stick size. PPSSPP defaults to 16 GB, but Coded Arms computes free space
    // in 32 bits and then warns that there is not enough room; a 1 GB stick (like those sold
    // with the game) behaves as on hardware. CA_MEMSTICK_SIZE_GB overrides it.
    g_Config.iMemStickSizeGB = 1;
    if (const char* gb = std::getenv("CA_MEMSTICK_SIZE_GB")) {
        int v = std::atoi(gb);
        if (v > 0) g_Config.iMemStickSizeGB = v;
    }
    __ChnnlsvInit();
    saveDialog = new PSPSaveDialog(UtilityDialogType::SAVEDATA);
    msgDialog = new PSPMsgDialog(UtilityDialogType::MSG);
    currentDialogType = UtilityDialogType::NONE;
    DeactivateDialog();
    SavedataParam::Init();

    // ---- savedata
    registerHle("sceUtilitySavedataInitStart", [](Ctx* c) {
        int r = UtilityInitStart(UtilityDialogType::SAVEDATA, saveDialog, A(c, 0));
        log("sceUtilitySavedataInitStart(%08x) -> %08x", A(c, 0), (uint32_t)r);
        ret(c, (uint32_t)r);
    });
    registerHle("sceUtilitySavedataShutdownStart", [](Ctx* c) {
        if (currentDialogType != UtilityDialogType::SAVEDATA) { ret(c, SCE_ERROR_UTILITY_WRONG_TYPE); return; }
        DeactivateDialog();
        int r = saveDialog->Shutdown();
        log("sceUtilitySavedataShutdownStart() -> %08x", (uint32_t)r);
        ret(c, (uint32_t)r);
        eatUs(135); // hleEatCycles(30000) at 222 MHz
    });
    registerHle("sceUtilitySavedataGetStatus", [](Ctx* c) {
        if (currentDialogType != UtilityDialogType::SAVEDATA) {
            eatUs(1); // hleEatCycles(200)
            ret(c, SCE_ERROR_UTILITY_WRONG_TYPE);
            return;
        }
        const int status = saveDialog->GetStatus();
        eatUs(1);
        CleanupDialogThreads();
        if (oldStatus != status) {
            oldStatus = status;
            log("sceUtilitySavedataGetStatus() -> %d (%s)", status, UtilityDialogStatusToString(status));
        }
        ret(c, (uint32_t)status);
    });
    registerHle("sceUtilitySavedataUpdate", [](Ctx* c) {
        if (currentDialogType != UtilityDialogType::SAVEDATA || !saveDialog) { ret(c, SCE_ERROR_UTILITY_WRONG_TYPE); return; }
        int result = saveDialog->Update((int)A(c, 0));
        ret(c, (uint32_t)result);
        if (result >= 0) delayCurrentThread(300); // hleDelayResult(result, "savedata update", 300)
    });

    // ---- message dialog
    registerHle("sceUtilityMsgDialogInitStart", [](Ctx* c) {
        int r = UtilityInitStart(UtilityDialogType::MSG, msgDialog, A(c, 0));
        log("sceUtilityMsgDialogInitStart(%08x) -> %08x", A(c, 0), (uint32_t)r);
        ret(c, (uint32_t)r);
    });
    registerHle("sceUtilityMsgDialogShutdownStart", [](Ctx* c) {
        if (currentDialogType != UtilityDialogType::MSG) { ret(c, SCE_ERROR_UTILITY_WRONG_TYPE); return; }
        DeactivateDialog();
        int r = msgDialog->Shutdown();
        log("sceUtilityMsgDialogShutdownStart() -> %08x", (uint32_t)r);
        ret(c, (uint32_t)r);
    });
    registerHle("sceUtilityMsgDialogUpdate", [](Ctx* c) {
        if (currentDialogType != UtilityDialogType::MSG) { ret(c, SCE_ERROR_UTILITY_WRONG_TYPE); return; }
        int r = msgDialog->Update((int)A(c, 0));
        ret(c, (uint32_t)r);
        if (r >= 0) delayCurrentThread(800); // hleDelayResult(ret, "msgdialog update", 800)
    });
    registerHle("sceUtilityMsgDialogGetStatus", [](Ctx* c) {
        if (currentDialogType != UtilityDialogType::MSG) { ret(c, SCE_ERROR_UTILITY_WRONG_TYPE); return; }
        const int status = msgDialog->GetStatus();
        CleanupDialogThreads();
        if (oldStatus != status) {
            oldStatus = status;
            log("sceUtilityMsgDialogGetStatus() -> %d (%s)", status, UtilityDialogStatusToString(status));
        }
        ret(c, (uint32_t)status);
    });

    // ---- netconf: the game imports no sceUtilityNetconfInitStart, so the network dialog can never
    // be the current one and PPSSPP answers these with SCE_ERROR_UTILITY_WRONG_TYPE.
    for (const char* n : {"sceUtilityNetconfShutdownStart", "sceUtilityNetconfUpdate", "sceUtilityNetconfGetStatus"})
        registerHle(n, [](Ctx* c) { ret(c, SCE_ERROR_UTILITY_WRONG_TYPE); });

    // ---- system parameters (PPSSPP sceUtilityGetSystemParamInt with its default configuration)
    // Contagion: utility modules (network, AV) are part of the HLE layer.
    registerHle("sceUtilityLoadModule", [](Ctx* c) { log("sceUtilityLoadModule(%04x) -> HLE", A(c, 0)); ret(c, 0); });
    registerHle("sceUtilityUnloadModule", [](Ctx* c) { ret(c, 0); });
    registerHle("sceUtilityGetSystemParamInt", [](Ctx* c) {
        uint32_t id = A(c, 0), dest = A(c, 1);
        uint32_t param = 0;
        switch (id) {
        case 2: param = 0; break;                          // ADHOC_CHANNEL: automatic (adhocctl not up)
        case 3: param = 0; break;                          // WLAN_POWERSAVE: off
        case 4: param = 0; break;                          // DATE_FORMAT: YYYYMMDD
        case 5: param = 0; break;                          // TIME_FORMAT: 24 h
        case 6: param = 0; break;                          // TIMEZONE
        case 7: param = 0; break;                          // DAYLIGHTSAVINGS: standard
        case 8: param = (uint32_t)GetPSPLanguage(); break; // LANGUAGE
        case 9: param = (uint32_t)g_Config.iButtonPreference; break; // BUTTON_PREFERENCE
        case 10: param = 0; break;                         // LOCK_PARENTAL_LEVEL
        default: ret(c, SCE_ERROR_UTILITY_INVALID_SYSTEM_PARAM_ID); return;
        }
        if (validAddress(dest)) write32(dest, param);
        ret(c, 0);
    });
}

} // namespace psp::kernel
