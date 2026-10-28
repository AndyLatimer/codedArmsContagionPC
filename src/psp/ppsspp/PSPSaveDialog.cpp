// Copyright (c) 2012- PPSSPP Project.
// Modified for CodedArmsPC: PPGe drawing replaced by the host dialog view (the Display* functions
// fill it), English strings of PPSSPP's i18n defaults, the IO action runs synchronously when
// started (PPSSPP's IO thread; with its default IO timing a non-visible action is waited for
// anyway), no OSD/save-state notifications, save states removed.

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
#include <chrono>
#include <ctime>

#include "PSPSaveDialog.h"

static double time_now_d() {
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static double g_lastSaveTime = -1.0;

// Actually this should be called on both saves and loads, since just after a load it's safe to exit.
void ResetSecondsSinceLastGameSave() {
	g_lastSaveTime = time_now_d();
}

// PPSSPP shows an on-screen spinner here; the host dialog interface has none.
void ShowSaveLoadIndicator(bool save) {
}

double SecondsSinceLastGameSave() {
	if (g_lastSaveTime < 0) {
		return -1.0;
	} else {
		return time_now_d() - g_lastSaveTime;
	}
}

const static float FONT_SCALE = 0.55f;

// These are rough, it seems to take at least 100ms or so to init, and shutdown depends on threads.
// Some games seem to required slightly longer delays to work, so we try 200ms as a compromise.
const static int SAVEDATA_INIT_DELAY_US = 200000;
const static int SAVEDATA_SHUTDOWN_DELAY_US = 2000;

// These are the only sizes which are allowed.
// TODO: We should test what the different behavior is for each.
const static int SAVEDATA_DIALOG_SIZE_V1 = 1480;
const static int SAVEDATA_DIALOG_SIZE_V2 = 1500;
const static int SAVEDATA_DIALOG_SIZE_V3 = 1536;

static bool IsNotVisibleAction(SceUtilitySavedataType type) {
	switch (type) {
	case SCE_UTILITY_SAVEDATA_TYPE_AUTOLOAD:
	case SCE_UTILITY_SAVEDATA_TYPE_AUTOSAVE:
	case SCE_UTILITY_SAVEDATA_TYPE_SIZES:
	case SCE_UTILITY_SAVEDATA_TYPE_LIST:
	case SCE_UTILITY_SAVEDATA_TYPE_FILES:
	case SCE_UTILITY_SAVEDATA_TYPE_GETSIZE:
	case SCE_UTILITY_SAVEDATA_TYPE_MAKEDATASECURE:
	case SCE_UTILITY_SAVEDATA_TYPE_MAKEDATA:
	case SCE_UTILITY_SAVEDATA_TYPE_WRITEDATASECURE:
	case SCE_UTILITY_SAVEDATA_TYPE_WRITEDATA:
	case SCE_UTILITY_SAVEDATA_TYPE_READDATASECURE:
	case SCE_UTILITY_SAVEDATA_TYPE_READDATA:
	case SCE_UTILITY_SAVEDATA_TYPE_ERASESECURE:
	case SCE_UTILITY_SAVEDATA_TYPE_ERASE:
	case SCE_UTILITY_SAVEDATA_TYPE_DELETEDATA:
	case SCE_UTILITY_SAVEDATA_TYPE_AUTODELETE:
		return true;

	default:
		break;
	}
	return false;
}

PSPSaveDialog::PSPSaveDialog(UtilityDialogType type) : PSPDialog(type) {
	param.SetPspParam(0);
}

PSPSaveDialog::~PSPSaveDialog() {
	if (ioThread.joinable()) {
		ioThread.join();
	}
}

int PSPSaveDialog::Init(int paramAddr) {
	// Ignore if already running
	if (GetStatus() != SCE_UTILITY_STATUS_NONE) {
		ERROR_LOG_REPORT(Log::sceUtility, "A save request is already running, not starting a new one");
		return SCE_ERROR_UTILITY_INVALID_STATUS;
	}

	if (ioThread.joinable()) {
		// Normally shouldn't be the case here.
		ioThread.join();
	}

	ioThreadStatus = SAVEIO_NONE;

	requestAddr = 0;
	const int check = CheckRequest(paramAddr, { SAVEDATA_DIALOG_SIZE_V1, SAVEDATA_DIALOG_SIZE_V2, SAVEDATA_DIALOG_SIZE_V3 });
	if (check < 0) {
		ERROR_LOG(Log::sceUtility, "sceUtilitySavedataInitStart: bad request at %08x: %08x", paramAddr, check);
		return check;
	}
	if (!Memory::IsValid4AlignedAddress(paramAddr)) {
		return SCE_KERNEL_ERROR_BAD_ARGUMENT;  // untested
	}
	requestAddr = paramAddr;

	int size = Memory::ReadUnchecked_U32(requestAddr);
	memset(&request, 0, sizeof(request));
	Memory::Memcpy(&request, requestAddr, size);
	Memory::Memcpy(&originalRequest, requestAddr, size);

	// gameName/saveName/fileName become parts of host filesystem paths.
	// Reject path separators and bare dot components so a crafted request
	// can't escape the save directory (path traversal).
	if (HasPathTraversal(StringViewFromFixedSizeField(request.gameName)) ||
		HasPathTraversal(StringViewFromFixedSizeField(request.saveName)) ||
		HasPathTraversal(StringViewFromFixedSizeField(request.fileName))) {
		ERROR_LOG_REPORT(Log::sceUtility, "sceUtilitySavedataInitStart: path separator in name fields");
		return SCE_ERROR_UTILITY_INVALID_PARAM_SIZE;
	}

	param.SetIgnoreTextures(IsNotVisibleAction((SceUtilitySavedataType)(u32)request.mode));
	param.ClearSFOCache();
	int retval = param.SetPspParam(&request);

	const u32 mode = (u32)param.GetPspParam()->mode;
	const char *modeName = mode < ARRAY_SIZE(utilitySavedataTypeNames) ? utilitySavedataTypeNames[mode] : "UNKNOWN";
	INFO_LOG(Log::sceUtility,"sceUtilitySavedataInitStart(%08x) - %s (%d)", paramAddr, modeName, mode);
	INFO_LOG(Log::sceUtility,"sceUtilitySavedataInitStart(%08x) : Game key (hex): %s", paramAddr, param.GetKey(param.GetPspParam()).c_str());
	// CodedArmsPC: the request as the game made it, for comparing with PPSSPP.
	INFO_LOG(Log::sceUtility, "  size=%d game=%s save=%s list=%08x file=%s buf=%08x/%u data=%u focus=%d secureVersion=%u msFree=%08x msData=%08x utilityData=%08x idList=%08x fileList=%08x sizeInfo=%08x",
		request.common.size, param.GetGameName(&request).c_str(), StringViewFromFixedSizeField(request.saveName).data() ? std::string(StringViewFromFixedSizeField(request.saveName)).c_str() : "",
		request.saveNameList.ptr, param.GetFileName(&request).c_str(), request.dataBuf.ptr, request.dataBufSize, request.dataSize, (int)request.focus,
		request.common.size >= 1536 ? (u32)request.secureVersion : 0u, request.msFree.ptr, request.msData.ptr, request.utilityData.ptr,
		request.idList.ptr, request.fileList.ptr, request.sizeInfo.ptr);

	yesnoChoice = 1;
	switch ((SceUtilitySavedataFocus)(u32)param.GetPspParam()->focus) {
	case SCE_UTILITY_SAVEDATA_FOCUS_NAME:
		currentSelectedSave = param.GetSaveNameIndex(param.GetPspParam());
		break;
	case SCE_UTILITY_SAVEDATA_FOCUS_FIRSTLIST:
		currentSelectedSave = param.GetFirstListSave();
		break;
	case SCE_UTILITY_SAVEDATA_FOCUS_LASTLIST:
		currentSelectedSave = param.GetLastListSave();
		break;
	case SCE_UTILITY_SAVEDATA_FOCUS_LATEST:
		currentSelectedSave = param.GetLatestSave();
		break;
	case SCE_UTILITY_SAVEDATA_FOCUS_OLDEST:
		currentSelectedSave = param.GetOldestSave();
		break;
	case SCE_UTILITY_SAVEDATA_FOCUS_FIRSTDATA:
		currentSelectedSave = param.GetFirstDataSave();
		break;
	case SCE_UTILITY_SAVEDATA_FOCUS_LASTDATA:
		currentSelectedSave = param.GetLastDataSave();
		break;
	case SCE_UTILITY_SAVEDATA_FOCUS_FIRSTEMPTY:
		currentSelectedSave = param.GetFirstEmptySave();
		break;
	case SCE_UTILITY_SAVEDATA_FOCUS_LASTEMPTY:
		currentSelectedSave = param.GetLastEmptySave();
		break;
	default:
		WARN_LOG(Log::sceUtility, "Unknown save list focus option: %d", param.GetPspParam()->focus);
		currentSelectedSave = 0;
		break;
	}

	if (!param.WouldHaveMultiSaveName(param.GetPspParam()))
		currentSelectedSave = 0;

	switch ((SceUtilitySavedataType)(u32)param.GetPspParam()->mode)
	{
		case SCE_UTILITY_SAVEDATA_TYPE_LOAD:
			DEBUG_LOG(Log::sceUtility, "Loading. Title: %s Save: %s File: %s", param.GetGameName(param.GetPspParam()).c_str(), param.GetSaveName(param.GetPspParam()).c_str(), param.GetFileName(param.GetPspParam()).c_str());
			if (param.GetFileInfo(0).size != 0) {
				if (param.GetFileInfo(0).broken) {
					param.GetPspParam()->common.result = SCE_UTILITY_SAVEDATA_ERROR_LOAD_DATA_BROKEN;
					display = DS_LOAD_FAILED;
				} else {
					display = DS_LOAD_CONFIRM;
				}
			} else
				display = DS_LOAD_NODATA;
			break;
		case SCE_UTILITY_SAVEDATA_TYPE_AUTOLOAD:
			DEBUG_LOG(Log::sceUtility, "Loading. Title: %s Save: %s File: %s", param.GetGameName(param.GetPspParam()).c_str(), param.GetSaveName(param.GetPspParam()).c_str(), param.GetFileName(param.GetPspParam()).c_str());
			display = DS_NONE;
			// Is this necessary?
			// currentSelectedSave = param.GetSelectedSave();
			break;
		case SCE_UTILITY_SAVEDATA_TYPE_LISTLOAD:
			DEBUG_LOG(Log::sceUtility, "Loading. Title: %s Save: %s File: %s", param.GetGameName(param.GetPspParam()).c_str(), param.GetGameName(param.GetPspParam()).c_str(), param.GetFileName(param.GetPspParam()).c_str());
			if(param.GetFilenameCount() == 0)
				display = DS_LOAD_NODATA;
			else
				display = DS_LOAD_LIST_CHOICE;
			break;
		case SCE_UTILITY_SAVEDATA_TYPE_SAVE:
			DEBUG_LOG(Log::sceUtility, "Saving. Title: %s Save: %s File: %s", param.GetGameName(param.GetPspParam()).c_str(), param.GetGameName(param.GetPspParam()).c_str(), param.GetFileName(param.GetPspParam()).c_str());
			if (param.GetFileInfo(0).size != 0) {
				yesnoChoice = 0;
				display = DS_SAVE_CONFIRM_OVERWRITE;
			}
			else
				display = DS_SAVE_CONFIRM;
			break;
		case SCE_UTILITY_SAVEDATA_TYPE_AUTOSAVE:
			DEBUG_LOG(Log::sceUtility, "Saving. Title: %s Save: %s File: %s", param.GetGameName(param.GetPspParam()).c_str(), param.GetGameName(param.GetPspParam()).c_str(), param.GetFileName(param.GetPspParam()).c_str());
			display = DS_NONE;
			// Is this necessary?
			// currentSelectedSave = param.GetSelectedSave();
			break;
		case SCE_UTILITY_SAVEDATA_TYPE_LISTSAVE:
			DEBUG_LOG(Log::sceUtility, "Saving. Title: %s Save: %s File: %s", param.GetGameName(param.GetPspParam()).c_str(), param.GetGameName(param.GetPspParam()).c_str(), param.GetFileName(param.GetPspParam()).c_str());
			display = DS_SAVE_LIST_CHOICE;
			break;
		case SCE_UTILITY_SAVEDATA_TYPE_LISTALLDELETE:
			DEBUG_LOG(Log::sceUtility, "Delete. Title: %s Save: %s File: %s", param.GetGameName(param.GetPspParam()).c_str(), param.GetGameName(param.GetPspParam()).c_str(), param.GetFileName(param.GetPspParam()).c_str());
			if(param.GetFilenameCount() == 0)
				display = DS_DELETE_NODATA;
			else
				display = DS_DELETE_LIST_CHOICE;
			break;
		case SCE_UTILITY_SAVEDATA_TYPE_SIZES:
		case SCE_UTILITY_SAVEDATA_TYPE_LIST:
		case SCE_UTILITY_SAVEDATA_TYPE_FILES:
		case SCE_UTILITY_SAVEDATA_TYPE_GETSIZE:
		case SCE_UTILITY_SAVEDATA_TYPE_MAKEDATASECURE:
		case SCE_UTILITY_SAVEDATA_TYPE_MAKEDATA:
		case SCE_UTILITY_SAVEDATA_TYPE_WRITEDATASECURE:
		case SCE_UTILITY_SAVEDATA_TYPE_WRITEDATA:
		case SCE_UTILITY_SAVEDATA_TYPE_READDATASECURE:
		case SCE_UTILITY_SAVEDATA_TYPE_READDATA:
		case SCE_UTILITY_SAVEDATA_TYPE_ERASESECURE:
		case SCE_UTILITY_SAVEDATA_TYPE_ERASE:
		case SCE_UTILITY_SAVEDATA_TYPE_DELETEDATA:
			display = DS_NONE;
			break;

		case SCE_UTILITY_SAVEDATA_TYPE_DELETE:
			DEBUG_LOG(Log::sceUtility, "Delete. Title: %s Save: %s File: %s", param.GetGameName(param.GetPspParam()).c_str(), param.GetGameName(param.GetPspParam()).c_str(), param.GetFileName(param.GetPspParam()).c_str());
			if (param.GetFileInfo(0).size != 0) {
				yesnoChoice = 0;
				display = DS_DELETE_CONFIRM;
			} else
				display = DS_DELETE_NODATA;
			break;

		case SCE_UTILITY_SAVEDATA_TYPE_AUTODELETE:
			DEBUG_LOG(Log::sceUtility, "Delete. Title: %s Save: %s File: %s", param.GetGameName(param.GetPspParam()).c_str(), param.GetGameName(param.GetPspParam()).c_str(), param.GetFileName(param.GetPspParam()).c_str());
			display = DS_NONE;
			break;

		case SCE_UTILITY_SAVEDATA_TYPE_LISTDELETE: 
			DEBUG_LOG(Log::sceUtility, "Delete. Title: %s Save: %s File: %s", param.GetGameName(param.GetPspParam()).c_str(), param.GetGameName(param.GetPspParam()).c_str(), param.GetFileName(param.GetPspParam()).c_str());
			if (param.GetFilenameCount() == 0)
				display = DS_DELETE_NODATA;
			else
				display = DS_DELETE_LIST_CHOICE;
			break;
		default:
		{
			ERROR_LOG_REPORT(Log::sceUtility, "Load/Save function %d not coded. Title: %s Save: %s File: %s", (SceUtilitySavedataType)(u32)param.GetPspParam()->mode, param.GetGameName(param.GetPspParam()).c_str(), param.GetGameName(param.GetPspParam()).c_str(), param.GetFileName(param.GetPspParam()).c_str());
			param.GetPspParam()->common.result = 0;
			ChangeStatusInit(SAVEDATA_INIT_DELAY_US);
			display = DS_NONE;
			return 0; // Return 0 should allow the game to continue, but missing function must be implemented and returning the right value or the game can block.
		}
		break;
	}

	if (retval < 0) {
		ChangeStatusShutdown(SAVEDATA_SHUTDOWN_DELAY_US);
	} else {
		ChangeStatusInit(SAVEDATA_INIT_DELAY_US);
	}

	param.ClearSFOCache();
	InitCommon();
	UpdateButtons();
	StartFade(true);

	/*INFO_LOG(Log::sceUtility,"Dump Param :");
	INFO_LOG(Log::sceUtility,"size : %d",param.GetPspParam()->common.size);
	INFO_LOG(Log::sceUtility,"language : %d",param.GetPspParam()->common.language);
	INFO_LOG(Log::sceUtility,"buttonSwap : %d",param.GetPspParam()->common.buttonSwap);
	INFO_LOG(Log::sceUtility,"result : %d",param.GetPspParam()->common.result);
	INFO_LOG(Log::sceUtility,"mode : %d",param.GetPspParam()->mode);
	INFO_LOG(Log::sceUtility,"bind : %d",param.GetPspParam()->bind);
	INFO_LOG(Log::sceUtility,"overwriteMode : %d",param.GetPspParam()->overwriteMode);
	INFO_LOG(Log::sceUtility,"gameName : %s",param.GetGameName(param.GetPspParam()).c_str());
	INFO_LOG(Log::sceUtility,"saveName : %s",param.GetPspParam()->saveName);
	INFO_LOG(Log::sceUtility,"saveNameList : %08x",*((unsigned int*)&param.GetPspParam()->saveNameList));
	INFO_LOG(Log::sceUtility,"fileName : %s",param.GetPspParam()->fileName);
	INFO_LOG(Log::sceUtility,"dataBuf : %08x",*((unsigned int*)&param.GetPspParam()->dataBuf));
	INFO_LOG(Log::sceUtility,"dataBufSize : %u",param.GetPspParam()->dataBufSize);
	INFO_LOG(Log::sceUtility,"dataSize : %u",param.GetPspParam()->dataSize);

	INFO_LOG(Log::sceUtility,"sfo title : %s",param.GetPspParam()->sfoParam.title);
	INFO_LOG(Log::sceUtility,"sfo savedataTitle : %s",param.GetPspParam()->sfoParam.savedataTitle);
	INFO_LOG(Log::sceUtility,"sfo detail : %s",param.GetPspParam()->sfoParam.detail);

	INFO_LOG(Log::sceUtility,"icon0 data : %08x",*((unsigned int*)&param.GetPspParam()->icon0FileData.buf));
	INFO_LOG(Log::sceUtility,"icon0 size : %u",param.GetPspParam()->icon0FileData.bufSize);

	INFO_LOG(Log::sceUtility,"icon1 data : %08x",*((unsigned int*)&param.GetPspParam()->icon1FileData.buf));
	INFO_LOG(Log::sceUtility,"icon1 size : %u",param.GetPspParam()->icon1FileData.bufSize);

	INFO_LOG(Log::sceUtility,"pic1 data : %08x",*((unsigned int*)&param.GetPspParam()->pic1FileData.buf));
	INFO_LOG(Log::sceUtility,"pic1 size : %u",param.GetPspParam()->pic1FileData.bufSize);

	INFO_LOG(Log::sceUtility,"snd0 data : %08x",*((unsigned int*)&param.GetPspParam()->snd0FileData.buf));
	INFO_LOG(Log::sceUtility,"snd0 size : %u",param.GetPspParam()->snd0FileData.bufSize);*/
	INFO_LOG(Log::sceUtility, "Return value: %d", retval);
	return retval;
}

std::string PSPSaveDialog::GetSelectedSaveDirName() const
{
	switch ((SceUtilitySavedataType)(u32)param.GetPspParam()->mode)
	{
	case SCE_UTILITY_SAVEDATA_TYPE_LOAD:
	case SCE_UTILITY_SAVEDATA_TYPE_AUTOLOAD:
	case SCE_UTILITY_SAVEDATA_TYPE_SAVE:
	case SCE_UTILITY_SAVEDATA_TYPE_AUTOSAVE:
	case SCE_UTILITY_SAVEDATA_TYPE_AUTODELETE:
	case SCE_UTILITY_SAVEDATA_TYPE_DELETE:
		return param.GetSaveDirName(param.GetPspParam());

	case SCE_UTILITY_SAVEDATA_TYPE_MAKEDATASECURE:
	case SCE_UTILITY_SAVEDATA_TYPE_MAKEDATA:
	case SCE_UTILITY_SAVEDATA_TYPE_READDATASECURE:
	case SCE_UTILITY_SAVEDATA_TYPE_READDATA:
	case SCE_UTILITY_SAVEDATA_TYPE_WRITEDATASECURE:
	case SCE_UTILITY_SAVEDATA_TYPE_WRITEDATA:
	case SCE_UTILITY_SAVEDATA_TYPE_ERASESECURE:
	case SCE_UTILITY_SAVEDATA_TYPE_ERASE:
	case SCE_UTILITY_SAVEDATA_TYPE_DELETEDATA:
		return param.GetSaveDirName(param.GetPspParam());

	// SIZES ignores saveName it seems.

	default:
		return param.GetSaveDirName(param.GetPspParam(), currentSelectedSave);
		break;
	}
}

void PSPSaveDialog::DisplayBanner(int which)
{
	std::string_view title;
	switch (which) {
	case DB_SAVE:
		title = "Save";
		break;
	case DB_LOAD:
		title = "Load";
		break;
	case DB_DELETE:
		title = "Delete";
		break;
	default:
		title = "";
		break;
	}
	view_.banner = std::string(title);
}

psp::host::DialogSaveEntry PSPSaveDialog::EntryFromInfo(const SaveFileInfo &info) const {
	psp::host::DialogSaveEntry e;
	e.saveName = info.saveName;
	e.title = info.title;
	e.savedataTitle = info.saveTitle;
	e.detail = info.saveDetail;
	e.sizeBytes = info.size;
	e.broken = info.broken;
	e.modified = info.modif_time;
	if (info.texture && info.texture->IsValid())
		e.iconPng = info.texture->Png();
	return e;
}

void PSPSaveDialog::DisplaySaveList(bool canMove) {
	std::lock_guard<std::mutex> guard(paramLock);
	static int upFramesHeld = 0;
	static int downFramesHeld = 0;

	if (canMove) {
		// The host picked an entry: the PSP gets there with UP/DOWN.
		if (hasHostInput_ && hostInput_.select >= 0 && hostInput_.select < param.GetFilenameCount())
			currentSelectedSave = hostInput_.select;

		if ( (IsButtonPressed(CTRL_UP) || IsButtonHeld(CTRL_UP, upFramesHeld)) && currentSelectedSave > 0)
			currentSelectedSave--;

		else if ( (IsButtonPressed(CTRL_DOWN) || IsButtonHeld(CTRL_DOWN, downFramesHeld)) && currentSelectedSave < (param.GetFilenameCount() - 1))
			currentSelectedSave++;
	}

	view_.entries.clear();
	for (int displayCount = 0; displayCount < param.GetFilenameCount(); displayCount++)
		view_.entries.push_back(EntryFromInfo(param.GetFileInfo(displayCount)));
	view_.selected = currentSelectedSave;
}

void PSPSaveDialog::DisplaySaveIcon(bool checkExists) {
	std::lock_guard<std::mutex> guard(paramLock);
	view_.showsEntry = true;
	view_.entry = EntryFromInfo(param.GetFileInfo(currentSelectedSave));
	view_.selected = currentSelectedSave;
}

void PSPSaveDialog::DisplaySaveDataInfo1() {
	// The list's highlighted entry (title, date, size, detail) is already in view_.entries.
}

void PSPSaveDialog::DisplaySaveDataInfo2(bool showNewData) {
	std::lock_guard<std::mutex> guard(paramLock);

	view_.showsEntry = true;
	if (showNewData || param.GetFileInfo(currentSelectedSave).size == 0) {
		// What is about to be written: the request's SFO strings and data size, dated now.
		const SceUtilitySavedataParam *p = param.GetPspParam();
		psp::host::DialogSaveEntry e = EntryFromInfo(param.GetFileInfo(currentSelectedSave));
		time_t t;
		time(&t);
		tm modif_time{};
#ifdef _WIN32
		localtime_s(&modif_time, &t);
#else
		localtime_r(&t, &modif_time);
#endif
		e.modified = modif_time;
		e.title = std::string(StringViewFromFixedSizeField(p->sfoParam.title));
		e.savedataTitle = std::string(StringViewFromFixedSizeField(p->sfoParam.savedataTitle));
		e.detail = std::string(StringViewFromFixedSizeField(p->sfoParam.detail));
		// TODO (as in PPSSPP): Account for icon, etc., etc.
		e.sizeBytes = p->dataSize;
		view_.entry = e;
	} else {
		view_.entry = EntryFromInfo(param.GetFileInfo(currentSelectedSave));
	}
}

void PSPSaveDialog::DisplayMessage(std::string_view text, bool hasYesNo)
{
	view_.text = std::string(text);
	if (hasYesNo)
	{
		view_.yesNo = true;
		if (IsButtonPressed(CTRL_LEFT) && yesnoChoice == 0) {
			yesnoChoice = 1;
		}
		else if (IsButtonPressed(CTRL_RIGHT) && yesnoChoice == 1) {
			yesnoChoice = 0;
		}
	}
	view_.yesNoChoice = yesnoChoice;
}

int PSPSaveDialog::Update(int animSpeed) {
	if (GetStatus() != SCE_UTILITY_STATUS_RUNNING)
		return SCE_ERROR_UTILITY_INVALID_STATUS;

	if (!param.GetPspParam()) {
		ChangeStatusShutdown(SAVEDATA_SHUTDOWN_DELAY_US);
		return 0;
	}

	if (pendingStatus != SCE_UTILITY_STATUS_RUNNING) {
		// We're actually done, we're just waiting to tell the game that.
		return 0;
	}

	// The struct may have been updated by the game.  This happens in "Where Is My Heart?"
	// Check if it has changed, reload it.
	// TODO: Cut down on preloading?  This rebuilds the list from scratch.
	int size = std::min((u32)sizeof(originalRequest), Memory::ReadUnchecked_U32(requestAddr));
	const u8 *updatedRequest = Memory::GetPointerRangeOrException(requestAddr, size);
	if (updatedRequest && memcmp(updatedRequest, &originalRequest, size) != 0) {
		memset(&request, 0, sizeof(request));
		Memory::Memcpy(&request, requestAddr, size);
		Memory::Memcpy(&originalRequest, requestAddr, size);
		std::lock_guard<std::mutex> guard(paramLock);
		param.SetPspParam(&request);
	}

	param.ClearSFOCache();
	UpdateButtons();
	UpdateFade(animSpeed);

	UpdateCommon();

	switch (display)
	{
		case DS_SAVE_LIST_CHOICE:
			StartDraw();

			DisplaySaveList();
			DisplaySaveDataInfo1();

			DisplayButtons(DS_BUTTON_OK | DS_BUTTON_CANCEL);
			DisplayBanner(DB_SAVE);

			if (IsButtonPressed(cancelButtonFlag)) {
				param.GetPspParam()->common.result = SCE_UTILITY_DIALOG_RESULT_CANCEL;
				StartFade(false);
			} else if (IsButtonPressed(okButtonFlag)) {
				// Save exist, ask user confirm
				if (param.GetFileInfo(currentSelectedSave).size > 0) {
					yesnoChoice = 0;
					display = DS_SAVE_CONFIRM_OVERWRITE;
				} else {
					display = DS_SAVE_SAVING;
					StartIOThread();
				}
			}
			EndDraw();
		break;
		case DS_SAVE_CONFIRM:
			StartDraw();

			DisplaySaveIcon(false);
			DisplaySaveDataInfo2(true);

			DisplayMessage("Do you want to save this data?", true);

			DisplayButtons(DS_BUTTON_OK | DS_BUTTON_CANCEL);
			DisplayBanner(DB_SAVE);

			if (IsButtonPressed(cancelButtonFlag) || (IsButtonPressed(okButtonFlag) && yesnoChoice == 0)) {
				param.GetPspParam()->common.result = SCE_UTILITY_DIALOG_RESULT_CANCEL;
				StartFade(false);
			} else if (IsButtonPressed(okButtonFlag)) {
				display = DS_SAVE_SAVING;
				StartIOThread();
			}

			EndDraw();
		break;
		case DS_SAVE_CONFIRM_OVERWRITE:
			StartDraw();

			DisplaySaveIcon(true);
			DisplaySaveDataInfo2();

			DisplayMessage("Do you want to overwrite the data?", true);

			DisplayButtons(DS_BUTTON_OK | DS_BUTTON_CANCEL);
			DisplayBanner(DB_SAVE);

			if (IsButtonPressed(cancelButtonFlag) || (IsButtonPressed(okButtonFlag) && yesnoChoice == 0)) {
				if (param.GetPspParam()->mode != SCE_UTILITY_SAVEDATA_TYPE_SAVE)
					display = DS_SAVE_LIST_CHOICE;
				else {
					param.GetPspParam()->common.result = SCE_UTILITY_DIALOG_RESULT_CANCEL;
					StartFade(false);
				}
			} else if (IsButtonPressed(okButtonFlag)) {
				display = DS_SAVE_SAVING;
				StartIOThread();
			}

			EndDraw();
		break;
		case DS_SAVE_SAVING:
			// The dialog keeps drawing while the IO runs, and takes the results once it's done.
			FinishIO(false);

			StartDraw();

			DisplaySaveIcon(true);
			DisplaySaveDataInfo2(true);

			DisplayMessage("Saving\nPlease Wait...");
			view_.busy = true;

			DisplayBanner(DB_SAVE);

			EndDraw();
		break;
		case DS_SAVE_FAILED:
			StartDraw();

			DisplaySaveIcon(true);
			DisplaySaveDataInfo2(true);

			DisplayMessage("Unable to save data.");

			DisplayButtons(DS_BUTTON_CANCEL);
			DisplayBanner(DB_SAVE);

			if (IsButtonPressed(cancelButtonFlag)) {
				// Go back to the list so they can try again.
				if (param.GetPspParam()->mode != SCE_UTILITY_SAVEDATA_TYPE_SAVE) {
					display = DS_SAVE_LIST_CHOICE;
				} else {
					param.GetPspParam()->common.result = SCE_UTILITY_DIALOG_RESULT_CANCEL;
					StartFade(false);
				}
			}

			EndDraw();
		break;
		case DS_SAVE_DONE:
			StartDraw();

			DisplaySaveIcon(true);
			DisplaySaveDataInfo2();

			DisplayMessage("Save completed");

			DisplayButtons(DS_BUTTON_CANCEL);
			DisplayBanner(DB_SAVE);

			if (IsButtonPressed(cancelButtonFlag)) {
				param.GetPspParam()->common.result = SCE_UTILITY_DIALOG_RESULT_SUCCESS;
				// Set the save to use for autosave and autoload
				param.SetSelectedSave(param.GetFileInfo(currentSelectedSave).idx);
				StartFade(false);
			}

			EndDraw();
		break;

		case DS_LOAD_LIST_CHOICE:
			StartDraw();
			
			DisplaySaveList();
			DisplaySaveDataInfo1();

			DisplayButtons(DS_BUTTON_OK | DS_BUTTON_CANCEL);
			DisplayBanner(DB_LOAD);

			if (IsButtonPressed(cancelButtonFlag)) {
				param.GetPspParam()->common.result = SCE_UTILITY_DIALOG_RESULT_CANCEL;
				StartFade(false);
			} else if (IsButtonPressed(okButtonFlag)) {
				display = DS_LOAD_LOADING;
				StartIOThread();
			}

			EndDraw();
		break;
		case DS_LOAD_CONFIRM:
			StartDraw();

			DisplaySaveIcon(true);
			DisplaySaveDataInfo2();

			DisplayMessage("Load this data?", true);

			DisplayButtons(DS_BUTTON_OK | DS_BUTTON_CANCEL);
			DisplayBanner(DB_LOAD);

			if (IsButtonPressed(cancelButtonFlag) || (IsButtonPressed(okButtonFlag) && yesnoChoice == 0)) {
				param.GetPspParam()->common.result = SCE_UTILITY_DIALOG_RESULT_CANCEL;
				StartFade(false);
			} else if (IsButtonPressed(okButtonFlag)) {
				display = DS_LOAD_LOADING;
				StartIOThread();
			}

			EndDraw();
		break;
		case DS_LOAD_LOADING:
			// The dialog keeps drawing while the IO runs, and takes the results once it's done.
			FinishIO(false);

			StartDraw();

			DisplaySaveIcon(true);
			DisplaySaveDataInfo2();

			DisplayMessage("Loading\nPlease Wait...");
			view_.busy = true;

			DisplayBanner(DB_LOAD);

			EndDraw();
		break;
		case DS_LOAD_FAILED:
			StartDraw();

			DisplaySaveIcon(true);
			DisplaySaveDataInfo2();

			DisplayMessage("Load failed\nThe data is corrupted.");

			DisplayButtons(DS_BUTTON_CANCEL);
			DisplayBanner(DB_LOAD);

			if (IsButtonPressed(cancelButtonFlag)) {
				// Go back to the list so they can try again.
				if (param.GetPspParam()->mode != SCE_UTILITY_SAVEDATA_TYPE_LOAD) {
					display = DS_LOAD_LIST_CHOICE;
				} else {
					StartFade(false);
				}
			}

			EndDraw();
		break;
		case DS_LOAD_DONE:
			StartDraw();
			
			DisplaySaveIcon(true);
			DisplaySaveDataInfo2();

			DisplayMessage("Load completed");

			// Port: OK closes it too (below), so the legend shows both.
			DisplayButtons(DS_BUTTON_OK | DS_BUTTON_CANCEL);
			DisplayBanner(DB_LOAD);

			// Allow OK to be pressed as well to confirm the save.
			// The PSP only allows cancel, but that's generally not great UX.
			// Allowing this here makes it quicker for most users to get into the actual game.
			if (IsButtonPressed(cancelButtonFlag) || IsButtonPressed(okButtonFlag)) {
				param.GetPspParam()->common.result = SCE_UTILITY_DIALOG_RESULT_SUCCESS;
				// Set the save to use for autosave and autoload
				param.SetSelectedSave(param.GetFileInfo(currentSelectedSave).idx);
				StartFade(false);
			}

			EndDraw();
		break;
		case DS_LOAD_NODATA:
			StartDraw();

			DisplayMessage("There is no data");

			DisplayButtons(DS_BUTTON_CANCEL);
			DisplayBanner(DB_LOAD);

			if (IsButtonPressed(cancelButtonFlag)) {
				param.GetPspParam()->common.result = SCE_UTILITY_SAVEDATA_ERROR_LOAD_NO_DATA;
				StartFade(false);
			}

			EndDraw();
		break;

		case DS_DELETE_LIST_CHOICE:
			StartDraw();
			
			DisplaySaveList();
			DisplaySaveDataInfo1();

			DisplayButtons(DS_BUTTON_OK | DS_BUTTON_CANCEL);
			DisplayBanner(DB_DELETE);

			if (IsButtonPressed(cancelButtonFlag)) {
				param.GetPspParam()->common.result = SCE_UTILITY_DIALOG_RESULT_CANCEL;
				StartFade(false);
			} else if (IsButtonPressed(okButtonFlag)) {
				yesnoChoice = 0;
				display = DS_DELETE_CONFIRM;
			}

			EndDraw();
		break;
		case DS_DELETE_CONFIRM:
			StartDraw();

			DisplaySaveIcon(true);
			DisplaySaveDataInfo2();

			DisplayMessage("This save data will be deleted.\nAre you sure you want to continue?", 
						true);

			DisplayButtons(DS_BUTTON_OK | DS_BUTTON_CANCEL);
			DisplayBanner(DB_DELETE);

			if (IsButtonPressed(cancelButtonFlag) || (IsButtonPressed(okButtonFlag) && yesnoChoice == 0)) {
				if (param.GetPspParam()->mode == SCE_UTILITY_SAVEDATA_TYPE_LISTDELETE || param.GetPspParam()->mode == SCE_UTILITY_SAVEDATA_TYPE_LISTALLDELETE)
					display = DS_DELETE_LIST_CHOICE;
				else {
					param.GetPspParam()->common.result = SCE_UTILITY_DIALOG_RESULT_CANCEL;
					StartFade(false);
				}
			} else if (IsButtonPressed(okButtonFlag)) {
				display = DS_DELETE_DELETING;
				StartIOThread();
			}

			EndDraw();
		break;
		case DS_DELETE_DELETING:
			// The dialog keeps drawing while the IO runs, and takes the results once it's done.
			FinishIO(false);

			StartDraw();

			DisplayMessage("Deleting\nPlease Wait...");
			view_.busy = true;

			DisplayBanner(DB_DELETE);

			EndDraw();
		break;
		case DS_DELETE_FAILED:
			StartDraw();

			DisplayMessage("Unable to delete data.");

			DisplayButtons(DS_BUTTON_CANCEL);
			DisplayBanner(DB_DELETE);

			if (IsButtonPressed(cancelButtonFlag)) {
				if (param.GetPspParam()->mode == SCE_UTILITY_SAVEDATA_TYPE_LISTDELETE || param.GetPspParam()->mode == SCE_UTILITY_SAVEDATA_TYPE_LISTALLDELETE)
					display = DS_DELETE_LIST_CHOICE;
				else
					StartFade(false);
			}

			EndDraw();
		break;
		case DS_DELETE_DONE:
			StartDraw();
			
			DisplayMessage("Delete completed");

			DisplayButtons(DS_BUTTON_CANCEL);
			DisplayBanner(DB_DELETE);

			if (IsButtonPressed(cancelButtonFlag)) {
				if (param.GetFilenameCount() == 0)
					display = DS_DELETE_NODATA;
				else if (param.GetPspParam()->mode == SCE_UTILITY_SAVEDATA_TYPE_LISTDELETE || param.GetPspParam()->mode == SCE_UTILITY_SAVEDATA_TYPE_LISTALLDELETE) {
					if (currentSelectedSave > param.GetFilenameCount() - 1)
						currentSelectedSave = param.GetFilenameCount() - 1;
					display = DS_DELETE_LIST_CHOICE;
				} else {
					param.GetPspParam()->common.result = SCE_UTILITY_DIALOG_RESULT_SUCCESS;
					StartFade(false);
				}
			}

			EndDraw();
		break;
		case DS_DELETE_NODATA:
			StartDraw();
			
			DisplayMessage("There is no data");

			DisplayButtons(DS_BUTTON_CANCEL);
			DisplayBanner(DB_DELETE);

			if (IsButtonPressed(cancelButtonFlag)) {
				param.GetPspParam()->common.result = SCE_UTILITY_SAVEDATA_ERROR_DELETE_NO_DATA;
				StartFade(false);
			}

			EndDraw();
		break;

		case DS_NONE: // For action which display nothing
			if (ioThreadStatus == SAVEIO_NONE) {
				StartIOThread();
			} else if (FinishIO(true /* IOTIMING_FAST, PPSSPP's default */)) {
				ChangeStatus(SCE_UTILITY_STATUS_FINISHED, 0);
			}
		break;

		default:
			ChangeStatus(SCE_UTILITY_STATUS_FINISHED, 0);
		break;
	}

	if (ReadStatus() == SCE_UTILITY_STATUS_FINISHED || pendingStatus == SCE_UTILITY_STATUS_FINISHED)
		Memory::Memcpy(requestAddr, &request, request.common.size, "SaveDialogParam");
	param.ClearSFOCache();
	
	return 0;
}

// Runs on the IO thread. Of the dialog, it only touches the io* members (see StartIOThread and FinishIO).
void PSPSaveDialog::ExecuteIOAction() {
	ioParam_.ClearSFOCache();
	auto &result = ioRequest_.common.result;
	ioDisplay_ = ioAction_;
	switch (ioAction_) {
	case DS_LOAD_LOADING:
		result = ioParam_.Load(&ioRequest_, ioSaveDirName_, ioSaveId_);
		ioDisplay_ = result == 0 ? DS_LOAD_DONE : DS_LOAD_FAILED;
		break;
	case DS_SAVE_SAVING:
		ioDisplay_ = ioParam_.Save(&ioRequest_, ioSaveDirName_) == 0 ? DS_SAVE_DONE : DS_SAVE_FAILED;
		break;
	case DS_DELETE_DELETING:
		if (ioParam_.Delete(&ioRequest_, ioDeleteDir_)) {
			result = 0;
			ioDisplay_ = DS_DELETE_DONE;
		} else {
			//result = SCE_UTILITY_SAVEDATA_ERROR_DELETE_NO_DATA;// What the result should be?
			ioDisplay_ = DS_DELETE_FAILED;
		}
		break;
	case DS_NONE:
		ExecuteNotVisibleIOAction();
		break;

	default:
		// Nothing to do here.
		break;
	}

	ioParam_.ClearSFOCache();
	ioThreadStatus = SAVEIO_READY;
}

// What a mode that shows no dialog returns without a memory stick, or 0 for one not tested on
// hardware. They all start fine and fail when they run (pspautotests utility/savedata/nomemstick).
static u32 NoMemstickResult(SceUtilitySavedataType mode) {
	switch (mode) {
	case SCE_UTILITY_SAVEDATA_TYPE_AUTOLOAD:
		return SCE_UTILITY_SAVEDATA_ERROR_LOAD_NO_MS;
	case SCE_UTILITY_SAVEDATA_TYPE_AUTOSAVE:
		return SCE_UTILITY_SAVEDATA_ERROR_SAVE_NO_MS;
	case SCE_UTILITY_SAVEDATA_TYPE_AUTODELETE:
		return SCE_UTILITY_SAVEDATA_ERROR_DELETE_NO_MS;
	case SCE_UTILITY_SAVEDATA_TYPE_SIZES:
		return SCE_UTILITY_SAVEDATA_ERROR_SIZES_NO_MS;
	case SCE_UTILITY_SAVEDATA_TYPE_LIST:
	case SCE_UTILITY_SAVEDATA_TYPE_FILES:
	case SCE_UTILITY_SAVEDATA_TYPE_MAKEDATASECURE:
	case SCE_UTILITY_SAVEDATA_TYPE_MAKEDATA:
	case SCE_UTILITY_SAVEDATA_TYPE_READDATASECURE:
	case SCE_UTILITY_SAVEDATA_TYPE_READDATA:
	case SCE_UTILITY_SAVEDATA_TYPE_WRITEDATASECURE:
	case SCE_UTILITY_SAVEDATA_TYPE_WRITEDATA:
	case SCE_UTILITY_SAVEDATA_TYPE_ERASESECURE:
	case SCE_UTILITY_SAVEDATA_TYPE_ERASE:
	case SCE_UTILITY_SAVEDATA_TYPE_DELETEDATA:
	case SCE_UTILITY_SAVEDATA_TYPE_GETSIZE:
		return SCE_UTILITY_SAVEDATA_ERROR_RW_NO_MEMSTICK;
	default:
		return 0;
	}
}

void PSPSaveDialog::ExecuteNotVisibleIOAction() {
	auto &result = ioRequest_.common.result;

	SceUtilitySavedataType utilityMode = (SceUtilitySavedataType)(u32)ioRequest_.mode;
	if (MemoryStick_State() != PSP_MEMORYSTICK_STATE_INSERTED) {
		const u32 noMemstickResult = NoMemstickResult(utilityMode);
		if (noMemstickResult != 0) {
			result = noMemstickResult;
			return;
		}
	}

	switch (utilityMode) {
	case SCE_UTILITY_SAVEDATA_TYPE_LOAD: // Only load and exit
	case SCE_UTILITY_SAVEDATA_TYPE_AUTOLOAD:
		result = ioParam_.Load(&ioRequest_, ioSaveDirName_, ioSaveId_);
		break;
	case SCE_UTILITY_SAVEDATA_TYPE_SAVE: // Only save and exit
	case SCE_UTILITY_SAVEDATA_TYPE_AUTOSAVE:
		result = ioParam_.Save(&ioRequest_, ioSaveDirName_);
		break;
	case SCE_UTILITY_SAVEDATA_TYPE_SIZES:
		result = ioParam_.GetSizes(&ioRequest_);
		break;
	case SCE_UTILITY_SAVEDATA_TYPE_LIST:
		ioParam_.GetList(&ioRequest_);
		result = 0;
		break;
	case SCE_UTILITY_SAVEDATA_TYPE_FILES:
		result = ioParam_.GetFilesList(&ioRequest_, requestAddr, ioListSaveDirName_);
		break;
	case SCE_UTILITY_SAVEDATA_TYPE_GETSIZE:
		if (ioParam_.GetSize(&ioRequest_)) {
			result = 0;
		} else {
			result = SCE_UTILITY_SAVEDATA_ERROR_RW_NO_DATA;
		}
		break;
	case SCE_UTILITY_SAVEDATA_TYPE_DELETEDATA:
		DEBUG_LOG(Log::sceUtility, "sceUtilitySavedata DELETEDATA: %s", ioRequest_.saveName);
		if (ioParam_.Delete(&ioRequest_, ioDeleteDir_)) {
			result = 0;
		} else {
			result = SCE_UTILITY_SAVEDATA_ERROR_RW_NO_DATA;
		}
		break;
	case SCE_UTILITY_SAVEDATA_TYPE_AUTODELETE:
	case SCE_UTILITY_SAVEDATA_TYPE_DELETE:
		if (ioParam_.Delete(&ioRequest_, ioDeleteDir_)) {
			result = 0;
		} else {
			result = SCE_UTILITY_SAVEDATA_ERROR_DELETE_NO_DATA;
		}
		break;
	// TODO: Should reset the directory's other files.
	case SCE_UTILITY_SAVEDATA_TYPE_MAKEDATA:
	case SCE_UTILITY_SAVEDATA_TYPE_MAKEDATASECURE:
		result = ioParam_.Save(&ioRequest_, ioSaveDirName_, ioRequest_.mode == SCE_UTILITY_SAVEDATA_TYPE_MAKEDATASECURE);
		if (result == SCE_UTILITY_SAVEDATA_ERROR_SAVE_MS_NOSPACE) {
			result = SCE_UTILITY_SAVEDATA_ERROR_RW_MEMSTICK_FULL;
		}
		break;
	case SCE_UTILITY_SAVEDATA_TYPE_WRITEDATA:
	case SCE_UTILITY_SAVEDATA_TYPE_WRITEDATASECURE:
		result = ioParam_.Save(&ioRequest_, ioSaveDirName_, ioRequest_.mode == SCE_UTILITY_SAVEDATA_TYPE_WRITEDATASECURE);
		break;
	case SCE_UTILITY_SAVEDATA_TYPE_READDATA:
	case SCE_UTILITY_SAVEDATA_TYPE_READDATASECURE:
		result = ioParam_.Load(&ioRequest_, ioSaveDirName_, ioSaveId_, ioRequest_.mode == SCE_UTILITY_SAVEDATA_TYPE_READDATASECURE);
		if (result == SCE_UTILITY_SAVEDATA_ERROR_LOAD_DATA_BROKEN)
			result = SCE_UTILITY_SAVEDATA_ERROR_RW_DATA_BROKEN;
		if (result == SCE_UTILITY_SAVEDATA_ERROR_LOAD_NO_DATA)
			result = SCE_UTILITY_SAVEDATA_ERROR_RW_NO_DATA;
		break;
	case SCE_UTILITY_SAVEDATA_TYPE_ERASE:
	case SCE_UTILITY_SAVEDATA_TYPE_ERASESECURE:
		result = ioParam_.DeleteData(&ioRequest_);
		break;
	default:
		break;
	}
}

void PSPSaveDialog::StartIOThread() {
	if (ioThread.joinable()) {
		WARN_LOG_REPORT(Log::sceUtility, "Starting a save io thread when one already pending, uh oh.");
		ioThread.join();
	}

	// Show save indicator. It's strange how "display" is just as much an action as what to display.
	if (display == DS_SAVE_SAVING || display == DS_LOAD_LOADING) {
		const bool save = display != DS_LOAD_LOADING;
		ResetSecondsSinceLastGameSave();
		ShowSaveLoadIndicator(save);
	}

	// Everything the IO thread needs from the dialog, taken now: it doesn't look at the dialog's state.
	const SceUtilitySavedataType mode = (SceUtilitySavedataType)(u32)request.mode;
	ioAction_ = display;
	ioRequest_ = request;
	ioRequestStart_ = request;
	ioSaveId_ = currentSelectedSave;
	ioSaveDirName_ = GetSelectedSaveDirName();
	ioListSaveDirName_.clear();
	if (display == DS_NONE && mode == SCE_UTILITY_SAVEDATA_TYPE_FILES) {
		ioListSaveDirName_ = param.GetSaveDirName(&request, 0);
	}
	ioDeleteDir_.clear();
	if (display == DS_DELETE_DELETING) {
		ioDeleteDir_ = param.GetSaveDir(currentSelectedSave);
	} else if (display == DS_NONE && (mode == SCE_UTILITY_SAVEDATA_TYPE_DELETEDATA || mode == SCE_UTILITY_SAVEDATA_TYPE_AUTODELETE || mode == SCE_UTILITY_SAVEDATA_TYPE_DELETE)) {
		ioDeleteDir_ = param.GetSaveDir(param.GetSelectedSave());
	}

	ioThreadStatus = SAVEIO_PENDING;
	// PPSSPP runs this on a host thread; here it finishes before StartIOThread returns, and the
	// results are still taken by FinishIO like PPSSPP's.
	this->ExecuteIOAction();
}

// Takes the IO thread's results back. Without wait, only if it's done: returns whether it was.
bool PSPSaveDialog::FinishIO(bool wait) {
	if (ioThreadStatus == SAVEIO_PENDING && !wait) {
		return false;
	}
	if (ioThread.joinable()) {
		ioThread.join();
	}
	if (ioThreadStatus != SAVEIO_READY) {
		return true;
	}

	{
		// Only what the IO changed: the game may have changed its request meanwhile (Update reloads it).
		std::lock_guard<std::mutex> guard(paramLock);
		const u8 *before = (const u8 *)&ioRequestStart_;
		const u8 *after = (const u8 *)&ioRequest_;
		u8 *live = (u8 *)&request;
		for (size_t i = 0; i < sizeof(request); ++i) {
			if (after[i] != before[i]) {
				live[i] = after[i];
			}
		}
	}

	switch (ioAction_) {
	case DS_LOAD_LOADING:
		if (ioDisplay_ == DS_LOAD_DONE) {
			g_lastSaveTime = time_now_d();
		}
		break;
	case DS_SAVE_SAVING:
		if (ioDisplay_ == DS_SAVE_DONE) {
			g_lastSaveTime = time_now_d();
		}
		break;
	case DS_NONE:
		switch ((SceUtilitySavedataType)(u32)request.mode) {
		case SCE_UTILITY_SAVEDATA_TYPE_LOAD:
		case SCE_UTILITY_SAVEDATA_TYPE_AUTOLOAD:
		case SCE_UTILITY_SAVEDATA_TYPE_READDATA:
		case SCE_UTILITY_SAVEDATA_TYPE_READDATASECURE:
			ResetSecondsSinceLastGameSave();
			ShowSaveLoadIndicator(false);
			break;
		case SCE_UTILITY_SAVEDATA_TYPE_MAKEDATA:
		case SCE_UTILITY_SAVEDATA_TYPE_MAKEDATASECURE:
			if (request.common.result == SCE_UTILITY_SAVEDATA_ERROR_RW_MEMSTICK_FULL) {
				break;
			}
			[[fallthrough]];
		case SCE_UTILITY_SAVEDATA_TYPE_SAVE:
		case SCE_UTILITY_SAVEDATA_TYPE_AUTOSAVE:
		case SCE_UTILITY_SAVEDATA_TYPE_WRITEDATA:
		case SCE_UTILITY_SAVEDATA_TYPE_WRITEDATASECURE:
				ResetSecondsSinceLastGameSave();
			ShowSaveLoadIndicator(true);
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}

	if (ioAction_ != DS_NONE) {
		display = ioDisplay_;
		// Show what was saved or deleted in the list.
		if (display == DS_SAVE_DONE || display == DS_DELETE_DONE) {
			std::lock_guard<std::mutex> guard(paramLock);
			param.SetPspParam(param.GetPspParam());
		}
	}

	ioThreadStatus = SAVEIO_DONE;
	return true;
}

void PSPSaveDialog::WaitForIO() {
	if (ioThread.joinable()) {
		ioThread.join();
	}
}

int PSPSaveDialog::Shutdown(bool force) {
	if (GetStatus() != SCE_UTILITY_STATUS_FINISHED && !force)
		return SCE_ERROR_UTILITY_INVALID_STATUS;

	if (ioThread.joinable()) {
		ioThread.join();
	}
	ioThreadStatus = SAVEIO_NONE;

	PSPDialog::Shutdown(force);
	if (!force) {
		ChangeStatusShutdown(SAVEDATA_SHUTDOWN_DELAY_US);
	}
	param.SetPspParam(0);
	param.ClearSFOCache();

	return 0;
}

pspUtilityDialogCommon *PSPSaveDialog::GetCommonParam() {
	return &param.GetPspParam()->common;
}
