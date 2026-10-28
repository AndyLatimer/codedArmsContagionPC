// Copyright (c) 2012- PPSSPP Project.
// Modified for CodedArmsPC: drawing replaced by the host dialog view (see PSPDialog.h), input from
// the host dialog interface or the emulated controller, CoreTiming ticks replaced by microseconds,
// save states removed.

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

#include <algorithm>

#include "PSPDialog.h"
#include "psp/host.h"

// In seconds. On a PSP, dialog animations advance by animSpeed frames per Update and take about
// this long.
#define FADE_TIME 0.2

constexpr float FONT_SCALE = 0.55f;

const char *UtilityDialogTypeToString(UtilityDialogType type) {
	switch (type) {
	case UtilityDialogType::NONE: return "NONE";
	case UtilityDialogType::SAVEDATA: return "SAVEDATA";
	case UtilityDialogType::MSG: return "MSG";
	case UtilityDialogType::OSK: return "OSK";
	case UtilityDialogType::NET: return "NET";
	case UtilityDialogType::SCREENSHOT: return "SCREENSHOT";
	case UtilityDialogType::GAMESHARING: return "GAMESHARING";
	case UtilityDialogType::GAMEDATAINSTALL: return "GAMEDATAINSTALL";
	case UtilityDialogType::NPSIGNIN: return "NPSIGNIN";
	case UtilityDialogType::HTMLVIEWER: return "HTMLVIEWER";
	default: return "(unknown)";
	}
}

const char *UtilityDialogStatusToString(PSPDialog::DialogStatus status) {
	switch (status) {
	case PSPDialog::SCE_UTILITY_STATUS_NONE: return "STATUS_NONE";
	case PSPDialog::SCE_UTILITY_STATUS_INITIALIZE: return "STATUS_INITIALIZE";
	case PSPDialog::SCE_UTILITY_STATUS_RUNNING: return "STATUS_RUNNING";
	case PSPDialog::SCE_UTILITY_STATUS_FINISHED: return "STATUS_FINISHED";
	case PSPDialog::SCE_UTILITY_STATUS_SHUTDOWN: return "STATUS_SHUTDOWN";
	case PSPDialog::SCE_UTILITY_STATUS_SCREENSHOT_UNKNOWN: return "STATUS_SCREENSHOT_UNKNOWN";
	default: return "(unknown)";
	}
}

void PSPDialog::InitCommon() {
	UpdateCommon();

	if (const pspUtilityDialogCommon *common = GetCommonParam()) {
		// How these compare with the calling thread decides what the game sees from GetStatus right
		// after InitStart/ShutdownStart.
		INFO_LOG(Log::sceUtility, "%s thread priorities: graphics=%d access=%d font=%d sound=%d",
			UtilityDialogTypeToString(DialogType()), common->graphicsThread, common->accessThread,
			common->fontThread, common->soundThread);
	}

	if (GetCommonParam() && GetCommonParam()->language != GetPSPLanguage()) {
		WARN_LOG(Log::sceUtility, "Game requested language %d, ignoring and using user language", GetCommonParam()->language);
	}
}

void PSPDialog::UpdateCommon() {
	okButtonFlag = CTRL_CIRCLE;
	cancelButtonFlag = CTRL_CROSS;
	if (GetCommonParam() && GetCommonParam()->buttonSwap == 1) {
		okButtonFlag = CTRL_CROSS;
		cancelButtonFlag = CTRL_CIRCLE;
	}
}

void PSPDialog::UpdatePendingStatus() {
	if (pendingStatusTicks != 0 && UtilityNowUs() >= pendingStatusTicks) {
		bool changeAllowed = true;
		if (pendingStatus == SCE_UTILITY_STATUS_NONE && status == SCE_UTILITY_STATUS_SHUTDOWN) {
			FinishVolatile();
		} else if (pendingStatus == SCE_UTILITY_STATUS_RUNNING && status == SCE_UTILITY_STATUS_INITIALIZE) {
			if (!volatileLocked_ && LocksVolatileMemory()) {
				volatileLocked_ = KernelVolatileMemLock(0, 0, 0) == 0;
				changeAllowed = volatileLocked_;
			}
		}
		if (changeAllowed) {
			status = pendingStatus;
			pendingStatusTicks = 0;
		}
	}
}

PSPDialog::DialogStatus PSPDialog::GetStatus() {
	UpdatePendingStatus();

	PSPDialog::DialogStatus retval = status;
	if (UseAutoStatus()) {
		if (status == SCE_UTILITY_STATUS_SHUTDOWN) {
			FinishVolatile();
			status = SCE_UTILITY_STATUS_NONE;
		}
		if (status == SCE_UTILITY_STATUS_INITIALIZE)
			status = SCE_UTILITY_STATUS_RUNNING;
	}
	return retval;
}

bool PSPDialog::IsBusy() {
	UpdatePendingStatus();
	// An auto status dialog in SHUTDOWN is only waiting for the game to see that (FinishAutoShutdown).
	if (status == SCE_UTILITY_STATUS_SHUTDOWN && UseAutoStatus()) {
		return false;
	}
	return status != SCE_UTILITY_STATUS_NONE;
}

void PSPDialog::FinishAutoShutdown() {
	if (status == SCE_UTILITY_STATUS_SHUTDOWN && UseAutoStatus()) {
		FinishVolatile();
		status = SCE_UTILITY_STATUS_NONE;
	}
}

void PSPDialog::ChangeStatus(DialogStatus newStatus, int delayUs) {
	if (delayUs <= 0) {
		if (newStatus == SCE_UTILITY_STATUS_NONE && status == SCE_UTILITY_STATUS_SHUTDOWN) {
			FinishVolatile();
		} else if (newStatus == SCE_UTILITY_STATUS_RUNNING && status == SCE_UTILITY_STATUS_INITIALIZE) {
			if (!volatileLocked_ && LocksVolatileMemory()) {
				// TODO: Should probably make the status pending instead?
				volatileLocked_ = KernelVolatileMemLock(0, 0, 0) == 0;
			}
		}
		status = newStatus;
		pendingStatus = newStatus;
		pendingStatusTicks = 0;
	} else {
		pendingStatus = newStatus;
		pendingStatusTicks = UtilityNowUs() + (u64)delayUs;
	}
}

void PSPDialog::FinishVolatile() {
	if (!volatileLocked_)
		return;

	if (KernelVolatileMemUnlock(0) == 0) {
		volatileLocked_ = false;
		// Simulate modifications to volatile memory.
		Memory::Memset(PSP_GetVolatileMemoryStart(), 0, PSP_GetVolatileMemoryEnd() - PSP_GetVolatileMemoryStart());
	}
}

int PSPDialog::FinishInit() {
	// The thread has locked volatile memory. An auto status dialog may be past INITIALIZE already,
	// and must still let go of it on shutdown.
	if (ReadStatus() == SCE_UTILITY_STATUS_NONE) {
		KernelVolatileMemUnlock(0);
		return -1;
	}
	volatileLocked_ = true;
	if (ReadStatus() == SCE_UTILITY_STATUS_INITIALIZE)
		ChangeStatus(SCE_UTILITY_STATUS_RUNNING, 0);
	return 0;
}

int PSPDialog::FinishShutdown() {
	if (ReadStatus() != SCE_UTILITY_STATUS_SHUTDOWN)
		return -1;
	ChangeStatus(SCE_UTILITY_STATUS_NONE, 0);
	return 0;
}

void PSPDialog::ChangeStatusInit(int delayUs) {
	ChangeStatus(SCE_UTILITY_STATUS_INITIALIZE, 0);

	auto params = GetCommonParam();
	if (params)
		UtilityDialogInitialize(DialogType(), delayUs, params->accessThread, params->graphicsThread);
	else
		ChangeStatus(SCE_UTILITY_STATUS_RUNNING, delayUs);
}

void PSPDialog::ChangeStatusShutdown(int delayUs) {
	// If we're doing shutdown right away and skipped start, we don't run the dialog thread.
	bool skipDialogShutdown = status == SCE_UTILITY_STATUS_NONE && pendingStatus == SCE_UTILITY_STATUS_NONE;
	ChangeStatus(SCE_UTILITY_STATUS_SHUTDOWN, 0);

	auto params = GetCommonParam();
	if (params && !skipDialogShutdown)
		UtilityDialogShutdown(DialogType(), delayUs, params->accessThread, params->graphicsThread);
	else
		ChangeStatus(SCE_UTILITY_STATUS_NONE, delayUs);
}

void PSPDialog::StartDraw()
{
	view_ = psp::host::DialogView();
	view_.kind = DialogType() == UtilityDialogType::MSG ? psp::host::DialogKind::Message : psp::host::DialogKind::Savedata;
	view_.crossConfirms = okButtonFlag == CTRL_CROSS;
	view_.yesNoChoice = yesnoChoice;
}

void PSPDialog::EndDraw()
{
	// Faded out (FinishFadeOut already took the dialog off the screen): nothing left to show.
	if (ReadStatus() == SCE_UTILITY_STATUS_FINISHED)
		return;
	view_.fade = (int)fadeValue;
	psp::host::publishDialog(view_);
}

int PSPDialog::Shutdown(bool force)
{
	psp::host::closeDialog();
	if (force) {
		ChangeStatus(SCE_UTILITY_STATUS_NONE, 0);
	} else {
		ChangeStatus(SCE_UTILITY_STATUS_SHUTDOWN, 0);
	}
	return 0;
}

void PSPDialog::StartFade(bool fadeIn_)
{
	isFading = true;
	fadeTimer = 0;
	fadeIn = fadeIn_;
}

void PSPDialog::UpdateFade(int animSpeed) {
	if (isFading) {
		// At least a frame per Update, or it would never finish.
		fadeTimer += std::max(animSpeed, 1) / 60.0f;
		if (fadeTimer < FADE_TIME) {
			if (fadeIn)
				fadeValue = (u32) (fadeTimer / FADE_TIME * 255);
			else
				fadeValue = 255 - (u32) (fadeTimer / FADE_TIME * 255);
		} else {
			fadeValue = (fadeIn ? 255 : 0);
			isFading = false;
			if (!fadeIn) {
				FinishFadeOut();
			}
		}
	}
}

void PSPDialog::FinishFadeOut() {
	psp::host::closeDialog();
	ChangeStatus(SCE_UTILITY_STATUS_FINISHED, 0);
}

u32 PSPDialog::CalcFadedColor(u32 inColor) const {
	u32 alpha = inColor >> 24;
	alpha = alpha * fadeValue / 255;
	return (inColor & 0x00FFFFFF) | (alpha << 24);
}

void PSPDialog::ResetState() {
	status = SCE_UTILITY_STATUS_NONE;
	pendingStatus = SCE_UTILITY_STATUS_NONE;
	pendingStatusTicks = 0;
	volatileLocked_ = false;
	isFading = false;
	fadeValue = 0;
}

void PSPDialog::UpdateButtons()
{
	hasHostInput_ = false;
	if (psp::host::dialogUsesController()) {
		// PPSSPP: __CtrlPeekButtons / __CtrlReadLatch (buttons newly pressed since the last read).
		const unsigned int cur = psp::host::g_ctrl.buttons.load();
		lastButtons = cur;
		buttons = cur & ~controllerPrev_;
		controllerPrev_ = cur;
		return;
	}
	lastButtons = 0;
	buttons = 0;
	if (!psp::host::takeDialogInput(hostInput_))
		return;
	hasHostInput_ = true;
	// The host chose with the same buttons a player would press.
	if (hostInput_.yesNoChoice == 0 || hostInput_.yesNoChoice == 1)
		yesnoChoice = hostInput_.yesNoChoice;
	if (hostInput_.confirm)
		buttons |= okButtonFlag;
	if (hostInput_.cancel)
		buttons |= cancelButtonFlag;
}

// Input is fine while fading in, but not while fading out: the choice has already been made.
bool PSPDialog::IsFadingOut() const {
	return isFading && !fadeIn;
}

bool PSPDialog::IsButtonPressed(int checkButton)
{
	return !IsFadingOut() && (buttons & checkButton);
}

bool PSPDialog::IsButtonHeld(int checkButton, int &framesHeld, int framesHeldThreshold, int framesHeldRepeatRate)
{
	bool btnWasHeldLastFrame = (lastButtons & checkButton) && ((psp::host::dialogUsesController() ? psp::host::g_ctrl.buttons.load() : 0u) & checkButton);
	if (!IsFadingOut() && btnWasHeldLastFrame) {
		framesHeld++;
	}
	else {
		framesHeld = 0;
		return false;
	}

	// It's considered held for dialog purposes after 30 frames (~0.5 seconds),
	// and set to repeat every 10 frames, by default.
	if (framesHeld >= framesHeldThreshold && ((framesHeld % framesHeldRepeatRate) == 0))
		return true;

	return false;
}

void PSPDialog::DisplayButtons(int flags, std::string_view caption) {
	bool useCaption = false;
	char safeCaption[65] = {0};
	if (!caption.empty()) {
		useCaption = true;
		truncate_cpy(safeCaption, sizeof(safeCaption), caption);
	}

	const pspUtilityDialogCommon *commonParams = GetCommonParam();
	if (!commonParams) {
		return;
	}

	if (flags & DS_BUTTON_OK) {
		view_.confirmButton = true;
		view_.confirmLabel = useCaption ? safeCaption : "Enter";
	}
	if (flags & DS_BUTTON_CANCEL) {
		view_.cancelButton = true;
		view_.cancelLabel = useCaption ? safeCaption : "Back";
	}
}

int PSPDialog::CheckRequest(u32 addr, std::initializer_list<u32> sizes) {
	if (!Memory::IsValidRange(addr, sizeof(pspUtilityDialogCommon))) {
		return SCE_ERROR_UTILITY_INVALID_ADDRESS;
	}
	const u32 size = Memory::ReadUnchecked_U32(addr);
	if (std::find(sizes.begin(), sizes.end(), size) == sizes.end()) {
		return SCE_ERROR_UTILITY_INVALID_PARAM_SIZE;
	}
	if (!Memory::IsValidRange(addr, size)) {
		return SCE_ERROR_UTILITY_INVALID_ADDRESS;
	}
	return 0;
}

int PSPDialog::GetConfirmButton() {
	return g_Config.iButtonPreference == PSP_SYSTEMPARAM_BUTTON_CROSS ? CTRL_CROSS : CTRL_CIRCLE;
}

int PSPDialog::GetCancelButton() {
	return g_Config.iButtonPreference == PSP_SYSTEMPARAM_BUTTON_CROSS ? CTRL_CIRCLE : CTRL_CROSS;
}

PPGeImage::PPGeImage(const std::string &pspFilename) {
	// PPSSPP loads the PNG when the icon is first drawn; the bytes are the same.
	if (pspFileSystem.ReadEntireFile(pspFilename, png_, true) < 0)
		png_.clear();
}

PPGeImage::PPGeImage(u32 pngPointer, size_t pngSize) {
	if (pngSize && Memory::IsValidRange(pngPointer, (u32)pngSize))
		png_.assign(Memory::GetPointerUnchecked(pngPointer), Memory::GetPointerUnchecked(pngPointer) + pngSize);
}
