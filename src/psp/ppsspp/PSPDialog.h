// Copyright (c) 2012- PPSSPP Project.
// Modified for CodedArmsPC: no PPGe drawing and no save states. The Display*/StartDraw/EndDraw
// calls build a psp::host::DialogView that is published to the front end (src/psp/host_dialog.h),
// and button presses come from the host dialog input (or the emulated controller). Timing uses
// the runtime's clock instead of CoreTiming.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#pragma once

#include <initializer_list>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ppsspp_utility_compat.h"
#include "psp/host_dialog.h"

// Stand-in for PPSSPP's PPGeImage: the PNG bytes of a savedata icon, read from the memory stick
// or from guest memory, handed to the front end instead of being uploaded as a texture.
class PPGeImage {
public:
	explicit PPGeImage(const std::string &pspFilename);
	PPGeImage(u32 pngPointer, size_t pngSize);
	bool IsValid() const { return !png_.empty(); }
	const std::vector<u8> &Png() const { return png_; }
private:
	std::vector<u8> png_;
};

// Implemented in src/psp/hle_utility.cpp (PPSSPP: sceUtility.cpp / sceKernelMemory.cpp).
void UtilityDialogInitialize(UtilityDialogType type, int delayUs, int accessPriority, int graphicsPriority);
void UtilityDialogShutdown(UtilityDialogType type, int delayUs, int accessPriority, int graphicsPriority);
int KernelVolatileMemLock(int type, u32 paddr, u32 psize);
int KernelVolatileMemUnlock(int type);
u64 UtilityNowUs();

#define SCE_UTILITY_DIALOG_RESULT_SUCCESS      0
#define SCE_UTILITY_DIALOG_RESULT_CANCEL       1
#define SCE_UTILITY_DIALOG_RESULT_ABORT        2

struct pspUtilityDialogCommon {
	u32_le size;            /** Size of the structure */
	s32_le language;        /** Language */
	s32_le buttonSwap;      /** Set to 1 for X/O button swap */
	s32_le graphicsThread;  /** Graphics thread priority */
	s32_le accessThread;    /** Access/fileio thread priority (SceJobThread) */
	s32_le fontThread;      /** Font thread priority (ScePafThread) */
	s32_le soundThread;     /** Sound thread priority */
	s32_le result;          /** Result */
	s32_le reserved[4];     /** Set to 0 */
};

class PSPDialog {
public:
	PSPDialog(UtilityDialogType type) : dialogType_(type) {}
	virtual ~PSPDialog() {}

	virtual int Update(int animSpeed) = 0;
	virtual int Shutdown(bool force = false);
	virtual void ResetState();
	virtual pspUtilityDialogCommon *GetCommonParam() {
		// This is returned properly by the derived classes (or should be...).
		return nullptr;
	}

	enum DialogStatus {
		SCE_UTILITY_STATUS_NONE       = 0,
		SCE_UTILITY_STATUS_INITIALIZE = 1,
		SCE_UTILITY_STATUS_RUNNING    = 2,
		SCE_UTILITY_STATUS_FINISHED   = 3,
		SCE_UTILITY_STATUS_SHUTDOWN   = 4,
		SCE_UTILITY_STATUS_SCREENSHOT_UNKNOWN = 5,
	};

	enum DialogStockButton {
		DS_BUTTON_NONE   = 0x00,
		DS_BUTTON_OK     = 0x01,
		DS_BUTTON_CANCEL = 0x02,
		DS_BUTTON_BOTH   = 0x03,
	};

	DialogStatus GetStatus();
	// Whether it keeps another dialog from starting. Applies a status change that's due, but unlike
	// GetStatus doesn't use up the one-time reports of auto status dialogs.
	bool IsBusy();
	// An auto status dialog in SHUTDOWN only waits for the game to see that, which it no longer can
	// once another dialog starts: done.
	void FinishAutoShutdown();
	UtilityDialogType DialogType() { return dialogType_; }

	void StartDraw();
	void EndDraw();

	void FinishVolatile();
	int FinishInit();
	int FinishShutdown();

protected:
	void InitCommon();
	void UpdateCommon();
	void UpdateButtons();
	bool IsFadingOut() const;
	bool IsButtonPressed(int checkButton);
	bool IsButtonHeld(int checkButton, int &framesHeld, int framesHeldThreshold = 30, int framesHeldRepeatRate = 10);
	// The caption override is assumed to have a size of 64 bytes.
	void DisplayButtons(int flags, std::string_view caption = "");
	void ChangeStatus(DialogStatus newStatus, int delayUs);
	void UpdatePendingStatus();
	void ChangeStatusInit(int delayUs);
	void ChangeStatusShutdown(int delayUs);
	DialogStatus ReadStatus() const {
		return status;
	}

	// TODO: Remove this once all dialogs are updated.
	virtual bool UseAutoStatus() = 0;
	// Whether the dialog holds volatile memory while running.
	virtual bool LocksVolatileMemory() const {
		return true;
	}

	static int GetConfirmButton();
	// What every InitStart checks first in sceUtility_Driver: the address, then the size against
	// the ones that type accepts (utility/dialog/sizes), then the rest of the range.
	static int CheckRequest(u32 addr, std::initializer_list<u32> sizes);
	static int GetCancelButton();

	void StartFade(bool fadeIn_);
	void UpdateFade(int animSpeed);
	virtual void FinishFadeOut();
	u32 CalcFadedColor(u32 inColor) const;

	DialogStatus pendingStatus = SCE_UTILITY_STATUS_NONE;
	u64 pendingStatusTicks = 0;

	unsigned int lastButtons = 0;
	unsigned int buttons = 0;

	float fadeTimer = 0.0f;
	bool isFading = false;
	bool fadeIn = false;
	u32 fadeValue = 0;

	// What this frame shows, built by StartDraw/Display*/EndDraw.
	psp::host::DialogView view_;
	// Input taken from the host for this Update.
	psp::host::DialogInput hostInput_;
	bool hasHostInput_ = false;
	unsigned int controllerPrev_ = 0;
	int okButtonFlag = 0;
	int cancelButtonFlag = 0;

	int yesnoChoice = 0;
	float scrollPos_ = 0.0f;
	int framesUpHeld_ = 0;
	int framesDownHeld_ = 0;

private:
	DialogStatus status = SCE_UTILITY_STATUS_NONE;
	UtilityDialogType dialogType_ = UtilityDialogType::NONE;
	bool volatileLocked_ = false;
};

const char *UtilityDialogTypeToString(UtilityDialogType type);
const char *UtilityDialogStatusToString(PSPDialog::DialogStatus status);
