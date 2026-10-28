// Copyright (c) 2012- PPSSPP Project.

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
#include <ctime>
#include <thread>

#include "Common/Data/Encoding/Utf8.h"
#include "Common/Data/Text/I18n.h"
#include "Common/System/OSD.h"
#include "Common/File/FileUtil.h"
#include "Common/Serialize/Serializer.h"
#include "Common/Serialize/SerializeFuncs.h"
#include "Common/StringUtils.h"
#include "Common/Thread/ThreadUtil.h"
#include "Core/Dialog/FirmwareDialogStyle.h"
#include "Core/Dialog/PSPSaveDialog.h"
#include "Core/FileSystems/MetaFileSystem.h"
#include "Core/Util/PathUtil.h"
#include "Core/Util/PPGeDraw.h"
#include "Common/TimeUtil.h"
#include "Core/HLE/sceCtrl.h"
#include "Core/HLE/sceUtility.h"
#include "Core/HLE/ErrorCodes.h"
#include "Core/HW/MemoryStick.h"
#include "Core/MemMapHelpers.h"
#include "Core/Config.h"
#include "Core/Reporting.h"
#include "Core/SaveState.h"

static double g_lastSaveTime = -1.0;

// Actually this should be called on both saves and loads, since just after a load it's safe to exit.
void ResetSecondsSinceLastGameSave() {
	g_lastSaveTime = time_now_d();
}

void ShowSaveLoadIndicator(bool save) {
	if (g_Config.bShowSaveLoadIndicator) {
		g_OSD.Show(OSDType::STATUS_ICON, "", "", save ? "I_ROTATE_RIGHT" : "I_ROTATE_LEFT", 1.0f, "save_indicator");
		g_OSD.SetFlags("save_indicator", (save ? OSDMessageFlags::SpinRight : OSDMessageFlags::SpinLeft) | OSDMessageFlags::Transparent);
	}
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
		ERROR_LOG(Log::sceUtility, "A save request is already running, not starting a new one");
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

	yesnoChoice = 1;
	FwReset();
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
	if (firmwareStyle_) {
		DisplayBannerFirmware(which);
		return;
	}
	auto di = GetI18NCategory(I18NCat::DIALOG);
	PPGeDrawRect(0, 0, 480, 23, CalcFadedColor(0x65636358));

	PPGeStyle textStyle = FadedStyle(PPGeAlign::BOX_VCENTER, 0.6f);
	textStyle.hasShadow = false;

	std::string_view title;
	switch (which) {
	case DB_SAVE:
		title = di->T("Save");
		break;
	case DB_LOAD:
		title = di->T("Load");
		break;
	case DB_DELETE:
		title = di->T("Delete");
		break;
	default:
		title = "";
		break;
	}
	// TODO: Draw a hexagon icon
	PPGeDrawImage(10, 6, 12.0f, 12.0f, 1, 10, 1, 10, 10, 10, FadedImageStyle());
	PPGeDrawText(title, 30, 11, textStyle);
}

void PSPSaveDialog::DisplaySaveList(bool canMove) {
	std::lock_guard<std::mutex> guard(paramLock);
	if (firmwareStyle_) {
		DisplaySaveListFirmware();
		if (canMove) {
			MoveSelection();
		}
		return;
	}

	for (int displayCount = 0; displayCount < param.GetFilenameCount(); displayCount++) {
		PPGeImageStyle imageStyle = FadedImageStyle();
		auto fileInfo = param.GetFileInfo(displayCount);

		if (fileInfo.size == 0 && fileInfo.texture && fileInfo.texture->IsValid())
			imageStyle.color = CalcFadedColor(0xFF777777);

		// Calc save image position on screen
		float w, h, x;
		float y = 97;
		if (displayCount != currentSelectedSave) {
			w = 81;
			h = 45;
			x = 58.5f;
		} else {
			w = 144;
			h = 80;
			x = 27;
		}
		if (displayCount < currentSelectedSave)
			y -= 13 + 45 * (currentSelectedSave - displayCount);
		else if (displayCount > currentSelectedSave)
			y += 48 + 45 * (displayCount - currentSelectedSave);

		// Skip if it's well outside the screen.
		if (y > 472.0f || y < -200.0f)
			continue;

		int pad = 0;
		if (fileInfo.texture != nullptr && fileInfo.texture->IsValid()) {
			fileInfo.texture->SetTexture();
			int tw = fileInfo.texture->Width();
			int th = fileInfo.texture->Height();
			float scale = (float)h / (float)th;
			int scaledW = (int)(tw * scale);
			pad = (w - scaledW) / 2;
			w = scaledW;

			// The PSP draws save icons over black, so their transparent parts come out black (#22280).
			PPGeDrawRect(x + pad, y, x + pad + w, y + h, CalcFadedColor(0xFF000000));
			PPGeDrawImage(x + pad, y, w, h, 0, 0, 1, 1, tw, th, imageStyle);
		} else {
			PPGeDrawRect(x, y, x + w, y + h, 0x88666666);
		}
		if (displayCount == currentSelectedSave) {
			float b = 1.2f;
			uint32_t bc = CalcFadedColor(0xD0FFFFFF);
			PPGeDrawRect(x + pad - b, y - b, x + pad + w + b, y, bc); // top border
			PPGeDrawRect(x + pad - b, y, x + pad, y + h, bc); // left border
			PPGeDrawRect(x + pad - b, y + h, x + pad + w + b, y + h + b, bc); //bottom border
			PPGeDrawRect(x + pad + w, y, x + pad + w + b, y + h, bc); //right border
		}
		PPGeSetDefaultTexture();
	}

	if (canMove) {
		MoveSelection();
	}
}

void PSPSaveDialog::DisplaySaveIcon(bool checkExists) {
	std::lock_guard<std::mutex> guard(paramLock);
	if (firmwareStyle_) {
		DisplaySaveIconFirmware();
		return;
	}
	PPGeImageStyle imageStyle = FadedImageStyle();
	auto curSave = param.GetFileInfo(currentSelectedSave);

	if (curSave.size == 0 && checkExists)
		imageStyle.color = CalcFadedColor(0xFF777777);

	// Calc save image position on screen
	float w = 144;
	float h = 80;
	float x = 27;
	float y = 97;

	int tw = 256;
	int th = 256;
	if (curSave.texture != nullptr && curSave.texture->IsValid()) {
		curSave.texture->SetTexture();
		tw = curSave.texture->Width();
		th = curSave.texture->Height();
		float scale = (float)h / (float)th;
		int scaledW = (int)(tw * scale);
		x += (w - scaledW) / 2;
		w = scaledW;
		// Over black, as in DisplaySaveList.
		PPGeDrawRect(x, y, x + w, y + h, CalcFadedColor(0xFF000000));
	} else {
		PPGeDisableTexture();
	}
	PPGeDrawImage(x, y, w, h, 0, 0, 1, 1, tw, th, imageStyle);
	PPGeSetDefaultTexture();
}

static void FormatSaveHourMin(char *hour_time, size_t sz, const tm &t) {
	const char *am_pm = "AM";
	int hour = t.tm_hour;
	switch (g_Config.iTimeFormat) {
	case 1:
		if (hour == 12) {
			am_pm = "PM";
		} else if (hour > 12) {
			am_pm = "PM";
			hour -= 12;
		} else if (hour == 0) {
			hour = 12;
		}
		snprintf(hour_time, sz, "%02d:%02d %s", hour, t.tm_min, am_pm);
		break;
	case 0:
	default:
		snprintf(hour_time, sz, "%02d:%02d", hour, t.tm_min);
		break;
	}
}

static void FormatSaveDate(char *date, size_t sz, const tm &t) {
	int year = t.tm_year + 1900;
	int month = t.tm_mon + 1;
	switch (g_Config.iDateFormat) {
	case 1:
		snprintf(date, sz, "%02d/%02d/%04d", month, t.tm_mday, year);
		break;
	case 2:
		snprintf(date, sz, "%02d/%02d/%04d", t.tm_mday, month, year);
		break;
	case 0:
	default:
		snprintf(date, sz, "%04d/%02d/%02d", year, month, t.tm_mday);
		break;
	}
}

// The firmware's look, from screenshots of a PSP-2000 on 6.61 (Brandish: The Dark Revenant's save
// and load dialogs). The game behind is darkened to 30%. The list of icons keeps PPSSPP's
// geometry, the selected icon 144x80 at (26, 96) with tex_f (savedata_utility.rco) pulsing around
// it as a glow. The texts beside it are placed by savedata_utility.rco's page_listdata objects
// (paf's coordinates: origin at the screen's centre, y up; a text's position is where its
// baseline starts), the rest by measuring.
namespace {
const float FW_FONT_SIZE = 10.125f;
const float FW_ICON_X = 26.0f;
const float FW_ICON_Y = 96.0f;
const float FW_ICON_W = 144.0f;
const float FW_ICON_H = 80.0f;
// The message box beside the icon: lines from x 201 to 464.
const float FW_MESSAGE_CENTER_X = 333.0f;
const float FW_MESSAGE_WIDTH = 264.0f;
const float FW_MESSAGE_WRAP = 230.0f;
}

std::string PSPSaveDialog::SaveText(const char *label, std::string_view fallback) {
	if (!firmwareStyle_) {
		return std::string(fallback);
	}
	const pspUtilityDialogCommon *common = GetCommonParam();
	std::string text = FirmwareSavedataText(label, common ? (int)common->language : 1);
	return text.empty() ? std::string(fallback) : text;
}

std::string PSPSaveDialog::NewDataTitle() {
	// newData points to the new save's icon (a PspUtilitySavedataFileData) followed by a pointer to
	// its title.
	const auto &newData = param.GetPspParam()->newData;
	if (!newData.IsValid() || !Memory::IsValidRange(newData.ptr, sizeof(PspUtilitySavedataFileData) + 4)) {
		return std::string();
	}
	const u32 titleAddr = Memory::ReadUnchecked_U32(newData.ptr + sizeof(PspUtilitySavedataFileData));
	if (titleAddr == 0 || !Memory::IsValidNullTerminatedString(titleAddr)) {
		return std::string();
	}
	return SanitizeUTF8(Memory::GetCharPointerUnchecked(titleAddr));
}

void PSPSaveDialog::DisplayBannerFirmware(int which) {
	auto di = GetI18NCategory(I18NCat::DIALOG);
	// tex_infobar (system_plugin_fg.rco) across the top, a white gradient tinted grey-blue.
	PPGeImage *bar = PPGeFirmwareImage("system_plugin_fg.rco", "tex_infobar");
	PPGeImageStyle barStyle;
	barStyle.color = CalcFadedColor(0xFF4D4B40);
	if (bar) {
		PPGeDrawFirmwareImage(bar, 0.0f, 0.0f, 480.0f, 21.0f, barStyle);
	} else {
		PPGeDrawRect(0, 0, 480, 21, CalcFadedColor(0x664D4B40));
	}

	std::string title;
	switch (which) {
	case DB_SAVE:
		title = SaveText("msgsavedata_cmn_save", di->T("Save"));
		break;
	case DB_LOAD:
		title = SaveText("msgsavedata_cmn_load", di->T("Load"));
		break;
	case DB_DELETE:
		title = SaveText("msgshare_opt_del", di->T("Delete"));
		break;
	default:
		break;
	}
	// The hexagon, its shape 4 right of the image's left and 7 down.
	if (PPGeImage *icon = PPGeFirmwareImage("savedata_utility.rco", "tex_infobar_icon")) {
		PPGeDrawFirmwareImage(icon, 4.0f, -2.0f, 0, 0, FadedImageStyle());
	}
	const float scale = 8.2f / FW_FONT_SIZE;
	PPGeSetFirmwareFont(true);
	PPGeDrawText(title, 30.0f, 16.0f - PPGeFirmwareFontAscent() * scale, FirmwareTextStyle(PPGeAlign::BOX_LEFT, scale));
}

void PSPSaveDialog::DrawSaveIconFirmware(const SaveFileInfo &info, float x, float y, float w, float h, u32 color) {
	PPGeImageStyle imageStyle;
	imageStyle.color = color;
	if (info.texture != nullptr && info.texture->IsValid()) {
		info.texture->SetTexture();
		const int tw = info.texture->Width();
		const int th = info.texture->Height();
		const float scaledW = floorf(tw * (h / (float)th));
		x += floorf((w - scaledW) / 2.0f);
		// Over black, as in DisplaySaveList.
		PPGeDrawRect(x, y, x + scaledW, y + h, CalcFadedColor(0xFF000000));
		PPGeDrawImage(x, y, scaledW, h, 0, 0, 1, 1, tw, th, imageStyle);
		PPGeSetDefaultTexture();
		return;
	}
	// The firmware's own pictures: 146x82, an icon's size and a pixel of border around it.
	const char *label = info.broken ? "tex_broken_data" : (info.size == 0 ? "tex_new" : "tex_default_icon");
	if (PPGeImage *image = PPGeFirmwareImage("savedata_utility.rco", label)) {
		const float bx = w / 144.0f, by = h / 80.0f;
		PPGeDrawFirmwareImage(image, x - bx, y - by, w + 2.0f * bx, h + 2.0f * by, imageStyle);
	} else {
		PPGeDrawRect(x, y, x + w, y + h, CalcFadedColor(0x88666666));
	}
}

// How the parts fade, in seconds, from a video of a PSP saving (at 30 frames a second, so to about
// a frame of it). Going from the list to a question, the list fades out, then the icon's details
// and shadow fade in, then the box's lines (moving apart to their places), then its text. From the
// busy icon to a message, the message comes almost at once.
namespace {
struct FwTiming {
	float delay;
	float duration;
	float out;
};
const FwTiming FW_TIMINGS[] = {
	{ 0.2f, 0.2f, 0.2f },  // FWP_LIST
	{ 0.13f, 0.17f, 0.2f },  // FWP_ICON
	{ 0.2f, 0.2f, 0.2f },  // FWP_GLOW
	{ 0.13f, 0.17f, 0.2f },  // FWP_SHADOW
	{ 0.13f, 0.17f, 0.2f },  // FWP_INFO2
	{ 0.0f, 0.2f, 0.23f },  // FWP_MESSAGE (its own delays, below)
	{ 0.03f, 0.07f, 0.13f },  // FWP_BUTTONS
	{ 0.03f, 0.13f, 0.15f },  // FWP_BUSY
};
const float FW_MESSAGE_DELAY_AFTER_LIST = 0.27f;
const float FW_MESSAGE_DELAY = 0.07f;
const float FW_MESSAGE_LINES = 0.13f;
// How far the box's lines start inside their places.
const float FW_MESSAGE_LINE_INSET = 8.0f;
const int FW_LONG_AGO = -100000;
}

void PSPSaveDialog::FwReset() {
	fwScreen_ = FwScreen();
	fwLastScreen_ = FwScreen();
	for (int i = 0; i < FWP_COUNT; i++) {
		fwOutScreen_[i] = FwScreen();
		fwInStart_[i] = FW_LONG_AGO;
		fwOutStart_[i] = FW_LONG_AGO;
	}
	fwFirstFrame_ = true;
}

float PSPSaveDialog::FwSeconds(int since) const {
	return (fwClock_ - since) / 60.0f;
}

bool PSPSaveDialog::FwSame(FwPart part, const FwScreen &a, const FwScreen &b) const {
	switch (part) {
	case FWP_MESSAGE:
		return a.message == b.message && a.yesNo == b.yesNo && a.besideIcon == b.besideIcon;
	case FWP_BUTTONS:
		return a.buttons == b.buttons;
	default:
		return true;
	}
}

float PSPSaveDialog::FwShow(FwPart part) {
	if (fwDrawingOut_) {
		return 1.0f;
	}
	fwScreen_.has[part] = true;
	if (fwFirstFrame_) {
		// What the dialog opens with is there from the start (the dialog fades in as a whole).
		fwInStart_[part] = FW_LONG_AGO;
	} else if (!fwLastScreen_.has[part] || !FwSame(part, fwScreen_, fwLastScreen_)) {
		fwInStart_[part] = fwClock_;
	}
	const FwTiming &timing = FW_TIMINGS[part];
	return std::clamp((FwSeconds(fwInStart_[part]) - timing.delay) / timing.duration, 0.0f, 1.0f);
}

float PSPSaveDialog::FwOutAlpha(FwPart part) const {
	const bool shown = fwScreen_.has[part];
	if (fwLastScreen_.has[part] && !(shown && FwSame(part, fwScreen_, fwLastScreen_))) {
		// Going this frame.
		return 1.0f;
	}
	if (shown && FwSame(part, fwScreen_, fwOutScreen_[part])) {
		// Back again.
		return 0.0f;
	}
	return std::max(0.0f, 1.0f - FwSeconds(fwOutStart_[part]) / FW_TIMINGS[part].out);
}

void PSPSaveDialog::FwEndFrame() {
	if (!firmwareStyle_) {
		return;
	}
	for (int i = 0; i < FWP_COUNT; i++) {
		const FwPart part = (FwPart)i;
		if (fwLastScreen_.has[part] && (!fwScreen_.has[part] || !FwSame(part, fwScreen_, fwLastScreen_))) {
			fwOutScreen_[part] = fwLastScreen_;
			fwOutStart_[part] = fwClock_;
		}
	}

	fwDrawingOut_ = true;
	float alpha = FwOutAlpha(FWP_LIST);
	if (alpha > 0.0f) {
		std::lock_guard<std::mutex> guard(paramLock);
		AlphaScope fade(this, alpha);
		// The selected icon stays if the next screen has it too, and the glow goes under it.
		DisplaySaveListFirmware(!fwScreen_.has[FWP_ICON]);
		DisplaySaveDataInfo1Firmware();
	}
	alpha = FwOutAlpha(FWP_ICON);
	if (alpha > 0.0f && !fwScreen_.has[FWP_ICON]) {
		std::lock_guard<std::mutex> guard(paramLock);
		AlphaScope fade(this, alpha);
		if (fwOutScreen_[FWP_ICON].has[FWP_SHADOW]) {
			DrawShadowFirmware(1.0f);
		}
		DrawSaveIconFirmware(param.GetFileInfo(currentSelectedSave), FW_ICON_X, FW_ICON_Y, FW_ICON_W, FW_ICON_H, CalcFadedColor(0xFFFFFFFF));
	}
	alpha = FwOutAlpha(FWP_INFO2);
	if (alpha > 0.0f) {
		std::lock_guard<std::mutex> guard(paramLock);
		AlphaScope fade(this, alpha);
		DisplaySaveDataInfo2Firmware(fwOutScreen_[FWP_INFO2].newData);
	}
	alpha = FwOutAlpha(FWP_MESSAGE);
	if (alpha > 0.0f) {
		const FwScreen &out = fwOutScreen_[FWP_MESSAGE];
		AlphaScope fade(this, alpha);
		if (out.besideIcon) {
			DisplayMessageBoxFirmware(out.message, FW_MESSAGE_CENTER_X, FW_MESSAGE_WIDTH, FW_MESSAGE_WRAP, out.yesNo, false, false, false);
		} else {
			DisplayMessageBoxFirmware(out.message, 240.0f, 360.0f, 330.0f, out.yesNo, false, false, false);
		}
	}
	alpha = FwOutAlpha(FWP_BUTTONS);
	if (alpha > 0.0f) {
		AlphaScope fade(this, alpha);
		DisplayButtonsFirmware(fwOutScreen_[FWP_BUTTONS].buttons);
	}
	alpha = FwOutAlpha(FWP_BUSY);
	if (alpha > 0.0f) {
		AlphaScope fade(this, alpha);
		DisplayBusyFirmware();
	}
	fwDrawingOut_ = false;

	fwLastScreen_ = fwScreen_;
	fwScreen_ = FwScreen();
	fwFirstFrame_ = false;
}

void PSPSaveDialog::DrawGlowFirmware(float alpha) {
	// tex_f is a soft white square, 69x70: its middle stretched, its halfway point on the icon's
	// edge, reaching about 12 pixels out at the pulse's brightest.
	if (alpha <= 0.0f) {
		return;
	}
	if (PPGeImage *glow = PPGeFirmwareImage("savedata_utility.rco", "tex_f")) {
		PPGeImageStyle glowStyle;
		glowStyle.color = CalcFadedColor(((u32)(FirmwarePulse() * alpha * 255.0f) << 24) | 0xFFFFFF);
		const float out = 19.0f;
		PPGeDrawFirmwareImageFrame(glow, FW_ICON_X - out, FW_ICON_Y - out, FW_ICON_W + 2.0f * out, FW_ICON_H + 2.0f * out, 34.0f, 34.0f, glowStyle);
	}
}

void PSPSaveDialog::DrawShadowFirmware(float alpha) {
	// A drop shadow, below and right of the icon: tex_s (a black square with soft edges, 45x45),
	// twice, as once is lighter than on a PSP.
	if (alpha <= 0.0f) {
		return;
	}
	if (PPGeImage *shadow = PPGeFirmwareImage("savedata_utility.rco", "tex_s")) {
		AlphaScope fade(this, alpha);
		const float out = 13.0f;
		for (int i = 0; i < 2; i++) {
			PPGeDrawFirmwareImageFrame(shadow, FW_ICON_X - out + 2.0f, FW_ICON_Y - out + 5.0f, FW_ICON_W + 2.0f * out, FW_ICON_H + 2.0f * out, 15.0f, 15.0f, FadedImageStyle());
		}
	}
}

void PSPSaveDialog::DisplaySaveListFirmware(bool drawSelected) {
	const float listAlpha = FwShow(FWP_LIST);
	float glowAlpha = 1.0f, iconAlpha = 1.0f;
	if (drawSelected && param.GetFilenameCount() > 0) {
		glowAlpha = FwShow(FWP_GLOW);
		iconAlpha = FwShow(FWP_ICON);
	}
	if (drawSelected && !fwDrawingOut_) {
		// A question's shadow, fading out under the icon.
		DrawShadowFirmware(FwOutAlpha(FWP_SHADOW));
	}

	for (int displayCount = 0; displayCount < param.GetFilenameCount(); displayCount++) {
		const SaveFileInfo &fileInfo = param.GetFileInfo(displayCount);
		const bool selected = displayCount == currentSelectedSave;
		if (selected && !drawSelected) {
			continue;
		}

		float x = 58.5f, y = 97.0f, w = 81.0f, h = 45.0f;
		if (selected) {
			x = FW_ICON_X;
			y = FW_ICON_Y;
			w = FW_ICON_W;
			h = FW_ICON_H;
		} else if (displayCount < currentSelectedSave) {
			y -= 13 + 45 * (currentSelectedSave - displayCount);
		} else {
			y += 48 + 45 * (displayCount - currentSelectedSave);
		}
		if (y > 472.0f || y < -200.0f) {
			continue;
		}

		if (selected) {
			DrawGlowFirmware(glowAlpha);
		}
		AlphaScope fade(this, selected ? iconAlpha : listAlpha);
		DrawSaveIconFirmware(fileInfo, x, y, w, h, CalcFadedColor(0xFFFFFFFF));
	}
}

void PSPSaveDialog::DisplaySaveIconFirmware() {
	iconShown_ = true;
	const float iconAlpha = FwShow(FWP_ICON);
	const float shadowAlpha = FwShow(FWP_SHADOW);
	// The list's glow, fading out under the icon.
	DrawGlowFirmware(FwOutAlpha(FWP_GLOW));
	DrawShadowFirmware(shadowAlpha);
	AlphaScope fade(this, iconAlpha);
	DrawSaveIconFirmware(param.GetFileInfo(currentSelectedSave), FW_ICON_X, FW_ICON_Y, FW_ICON_W, FW_ICON_H, CalcFadedColor(0xFFFFFFFF));
}

void PSPSaveDialog::DisplaySaveDataInfo1Firmware() {
	auto di = GetI18NCategory(I18NCat::DIALOG);
	AlphaScope fade(this, FwShow(FWP_LIST));
	const SaveFileInfo &saveInfo = param.GetFileInfo(currentSelectedSave);
	PPGeSetFirmwareFont(true);
	const float ascent = PPGeFirmwareFontAscent();
	auto drawText = [&](std::string_view text, float x, float baseline, float size, u32 color) {
		const float scale = size / FW_FONT_SIZE;
		PPGeStyle style = FirmwareTextStyle(PPGeAlign::BOX_LEFT, scale);
		style.color = CalcFadedColor(color);
		PPGeDrawText(text, x, baseline - ascent * scale, style);
	};

	if (saveInfo.broken) {
		drawText(SaveText("msgshare_corrupted_data", di->T("Corrupted Data")), 180.0f, 140.0f, 8.2f, 0xFFFFFFFF);
		drawText(saveInfo.saveTitle, 175.0f, 176.0f, 7.61f, 0xFFFFFFFF);
		return;
	}
	if (saveInfo.size == 0) {
		std::string title = NewDataTitle();
		if (title.empty()) {
			title = SaveText("msgsavedata_cmn_new", di->T("NEW DATA"));
		}
		drawText(title, 180.0f, 140.0f, 8.2f, 0xFFFFFFFF);
		return;
	}

	char hour_time[32];
	FormatSaveHourMin(hour_time, sizeof(hour_time), saveInfo.modif_time);
	char date_year[32];
	FormatSaveDate(date_year, sizeof(date_year), saveInfo.modif_time);
	const s64 sizeK = saveInfo.size / 1024;

	// The game's title in grey over tex_line, then the date and size.
	drawText(saveInfo.title, 180.0f, 131.0f, 8.2f, 0xFFCAC6C2);
	if (PPGeImage *line = PPGeFirmwareImage("savedata_utility.rco", "tex_line")) {
		PPGeDrawFirmwareImage(line, 179.0f, 134.0f, 0, 0, FadedImageStyle());
	}
	drawText(StringFromFormat("%s  %s  %lld KB", date_year, hour_time, sizeK), 180.0f, 151.0f, 6.43f, 0xFFFFFFFF);
	// savedata_title_text at (-65, -40) and detail_text at (-65, -62), 300x56 with lines 14 apart.
	drawText(saveInfo.saveTitle, 175.0f, 176.0f, 7.61f, 0xFFFFFFFF);

	const float detailScale = 6.43f / FW_FONT_SIZE;
	const float lineHeight = PPGeFirmwareFontLineHeight() * detailScale;
	PPGeSetFirmwareFont(true, lineHeight > 0.0f ? 14.0f / lineHeight : 1.0f);
	PPGeStyle detailStyle = FirmwareTextStyle(PPGeAlign::BOX_LEFT, detailScale);
	PPGeDrawTextWrapped(saveInfo.saveDetail, 175.0f, 198.0f - ascent * detailScale, 300.0f, 56.0f + 4.0f, detailStyle);
	PPGeSetFirmwareFont(true);
}

void PSPSaveDialog::DisplaySaveDataInfo2Firmware(bool showNewData) {
	if (!fwDrawingOut_) {
		fwScreen_.newData = showNewData;
	}
	AlphaScope fade(this, FwShow(FWP_INFO2));
	tm modif_time;
	std::string title;
	u32 data_size;
	if (showNewData || param.GetFileInfo(currentSelectedSave).size == 0) {
		time_t t;
		time(&t);
		localtime_r(&t, &modif_time);
		const char *save_title = param.GetPspParam()->sfoParam.savedataTitle;
		title = SanitizeUTF8(std::string(save_title, strnlen(save_title, 128)));
		data_size = param.GetPspParam()->dataSize;
	} else {
		const SaveFileInfo &info = param.GetFileInfo(currentSelectedSave);
		modif_time = info.modif_time;
		title = info.saveTitle;
		data_size = (u32)info.size;
	}

	char hour_time[32];
	FormatSaveHourMin(hour_time, sizeof(hour_time), modif_time);
	char date_year[32];
	FormatSaveDate(date_year, sizeof(date_year), modif_time);

	// mini_title_name, mini_makedate and mini_size: from (-235, -77), 17 apart, at size 7.02.
	PPGeSetFirmwareFont(true);
	const float scale = 7.02f / FW_FONT_SIZE;
	const float top = -PPGeFirmwareFontAscent() * scale;
	const PPGeStyle style = FirmwareTextStyle(PPGeAlign::BOX_LEFT, scale);
	PPGeDrawText(title, 5.0f, 213.0f + top, style);
	PPGeDrawText(StringFromFormat("%s  %s", date_year, hour_time), 5.0f, 230.0f + top, style);
	PPGeDrawText(StringFromFormat("%lld KB", (s64)data_size / 1024), 5.0f, 247.0f + top, style);
}

void PSPSaveDialog::MoveSelection() {
	static int upFramesHeld = 0;
	static int downFramesHeld = 0;
	if ((IsButtonPressed(CTRL_UP) || IsButtonHeld(CTRL_UP, upFramesHeld)) && currentSelectedSave > 0) {
		currentSelectedSave--;
		pulseFrames_ = 0;
	} else if ((IsButtonPressed(CTRL_DOWN) || IsButtonHeld(CTRL_DOWN, downFramesHeld)) && currentSelectedSave < (param.GetFilenameCount() - 1)) {
		currentSelectedSave++;
		pulseFrames_ = 0;
	}
}

void PSPSaveDialog::DisplaySaveDataInfo1() {
	std::lock_guard<std::mutex> guard(paramLock);
	if (firmwareStyle_) {
		DisplaySaveDataInfo1Firmware();
		return;
	}
	const SaveFileInfo &saveInfo = param.GetFileInfo(currentSelectedSave);
	PPGeStyle saveTitleStyle = FadedStyle(PPGeAlign::BOX_LEFT, 0.55f);

	if (saveInfo.broken) {
		auto di = GetI18NCategory(I18NCat::DIALOG);
		PPGeStyle textStyle = FadedStyle(PPGeAlign::BOX_VCENTER, 0.6f);
		PPGeDrawText(di->T("Corrupted Data"), 180, 136, textStyle);
		PPGeDrawText(saveInfo.title, 175, 159, saveTitleStyle);
	} else if (saveInfo.size == 0) {
		auto di = GetI18NCategory(I18NCat::DIALOG);
		PPGeStyle textStyle = FadedStyle(PPGeAlign::BOX_VCENTER, 0.6f);
		PPGeDrawText(di->T("NEW DATA"), 180, 136, textStyle);
	} else {
		char hour_time[32];
		FormatSaveHourMin(hour_time, sizeof(hour_time), saveInfo.modif_time);

		char date_year[32];
		FormatSaveDate(date_year, sizeof(date_year), saveInfo.modif_time);

		s64 sizeK = saveInfo.size / 1024;

		PPGeDrawRect(180, 136, 480, 137, CalcFadedColor(0xFFFFFFFF));
		std::string titleTxt = saveInfo.title;
		std::string timeTxt = StringFromFormat("%s   %s  %lld KB", date_year, hour_time, sizeK);
		std::string saveTitleTxt = saveInfo.saveTitle;
		std::string saveDetailTxt = saveInfo.saveDetail;

		PPGeStyle titleStyle = FadedStyle(PPGeAlign::BOX_BOTTOM, 0.6f);
		titleStyle.color = CalcFadedColor(0xFFC0C0C0);
		PPGeStyle textStyle = FadedStyle(PPGeAlign::BOX_LEFT, 0.5f);

		PPGeDrawText(titleTxt, 180, 136, titleStyle);
		PPGeDrawText(timeTxt, 180, 137, textStyle);
		PPGeDrawText(saveTitleTxt, 175, 159, saveTitleStyle);
		PPGeDrawTextWrapped(saveDetailTxt, 175, 181, 480 - 175, 250 - 181, textStyle);
	}
}

void PSPSaveDialog::DisplaySaveDataInfo2(bool showNewData) {
	std::lock_guard<std::mutex> guard(paramLock);
	if (firmwareStyle_) {
		DisplaySaveDataInfo2Firmware(showNewData);
		return;
	}

	tm modif_time;
	const char *save_title;
	u32 data_size;

	if (showNewData || param.GetFileInfo(currentSelectedSave).size == 0) {
		time_t t;
		time(&t);
		localtime_r(&t, &modif_time);
		save_title = param.GetPspParam()->sfoParam.savedataTitle;
		// TODO: Account for icon, etc., etc.
		data_size = param.GetPspParam()->dataSize;
	} else {
		modif_time = param.GetFileInfo(currentSelectedSave).modif_time;
		save_title = param.GetFileInfo(currentSelectedSave).saveTitle;
		data_size = param.GetFileInfo(currentSelectedSave).size;
	}

	char hour_time[32];
	FormatSaveHourMin(hour_time, sizeof(hour_time), modif_time);

	char date_year[32];
	FormatSaveDate(date_year, sizeof(date_year), modif_time);

	s64 sizeK = data_size / 1024;

	PPGeStyle textStyle = FadedStyle(PPGeAlign::BOX_LEFT, 0.5f);
	std::string title = SanitizeUTF8(std::string(save_title, strnlen(save_title, 128)));
	std::string saveinfoTxt = StringFromFormat("%s\n%s  %s\n%lld KB", title.c_str(), date_year, hour_time, sizeK);
	PPGeDrawText(saveinfoTxt.c_str(), 8, 200, textStyle);
}

void PSPSaveDialog::DisplayMessage(std::string_view text, bool hasYesNo)
{
	if (firmwareStyle_) {
		fwScreen_.message = std::string(text);
		fwScreen_.yesNo = hasYesNo;
		fwScreen_.besideIcon = iconShown_;
		FwShow(FWP_MESSAGE);
		// The lines come first, after anything the list was doing.
		const float delay = fwOutStart_[FWP_LIST] == fwInStart_[FWP_MESSAGE] || fwLastScreen_.has[FWP_LIST] ? FW_MESSAGE_DELAY_AFTER_LIST : FW_MESSAGE_DELAY;
		const float t = FwSeconds(fwInStart_[FWP_MESSAGE]) - delay;
		FirmwareBoxAnim anim;
		anim.lineAlpha = std::clamp(t / FW_MESSAGE_LINES, 0.0f, 1.0f);
		anim.lineInset = roundf((1.0f - anim.lineAlpha) * FW_MESSAGE_LINE_INSET);
		anim.contentAlpha = std::clamp((t - FW_MESSAGE_LINES) / FW_TIMINGS[FWP_MESSAGE].duration, 0.0f, 1.0f);
		// Beside the icon, or in the middle without one.
		if (iconShown_) {
			DisplayMessageBoxFirmware(text, FW_MESSAGE_CENTER_X, FW_MESSAGE_WIDTH, FW_MESSAGE_WRAP, hasYesNo, false, false, true, anim);
		} else {
			DisplayMessageBoxFirmware(text, 240.0f, 360.0f, 330.0f, hasYesNo, false, false, true, anim);
		}
		return;
	}
	PPGeStyle textStyle = FadedStyle(PPGeAlign::BOX_CENTER, FONT_SCALE);

	const float WRAP_WIDTH = 254.0f;
	float y = 136.0f, h;
	PPGeMeasureText(nullptr, &h, text, FONT_SCALE, PPGE_LINE_WRAP_WORD, WRAP_WIDTH);
	float h2 = h / 2.0f;
	if (hasYesNo)
	{
		auto di = GetI18NCategory(I18NCat::DIALOG);
		std::string_view choiceText;
		float x, w;
		if (yesnoChoice == 1) {
			choiceText = di->T("Yes");
			x = 302.0f;
		}
		else {
			choiceText = di->T("No");
			x = 366.0f;
		}
		PPGeMeasureText(&w, &h, choiceText, FONT_SCALE);
		w = w / 2.0f + 5.5f;
		h /= 2.0f;
		float y2 = y + h2 + 4.0f;
		h2 += h + 4.0f;
		y = 132.0f - h;
		PPGeDrawRect(x - w, y2 - h, x + w, y2 + h, CalcFadedColor(0x40C0C0C0));
		PPGeDrawText(di->T("Yes"), 302.0f, y2, textStyle);
		PPGeDrawText(di->T("No"), 366.0f, y2, textStyle);
		if (IsButtonPressed(CTRL_LEFT) && yesnoChoice == 0) {
			yesnoChoice = 1;
		}
		else if (IsButtonPressed(CTRL_RIGHT) && yesnoChoice == 1) {
			yesnoChoice = 0;
		}
	}
	PPGeDrawTextWrapped(text, 334.0f, y, WRAP_WIDTH, 0, textStyle);
	float sy = 122.0f - h2, ey = 150.0f + h2;
	PPGeDrawRect(202.0f, sy, 466.0f, sy + 1.0f, CalcFadedColor(0xFFFFFFFF));
	PPGeDrawRect(202.0f, ey, 466.0f, ey + 1.0f, CalcFadedColor(0xFFFFFFFF));
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

	auto di = GetI18NCategory(I18NCat::DIALOG);

	firmwareStyle_ = UseFirmwareSaveDialogStyle() && PPGeHasFirmwareFont();
	iconShown_ = false;
	fwClock_ += std::max(animSpeed, 1);
	auto startDraw = [&]() {
		StartDraw(!firmwareStyle_);
		if (firmwareStyle_) {
			// The game shows through at about 30%.
			PPGeDrawRect(0, 0, 480, 272, CalcFadedColor(0xB3000000));
		}
	};
	auto displayButtons = [&](int flags) {
		if (firmwareStyle_) {
			fwScreen_.buttons = flags;
			AlphaScope fade(this, FwShow(FWP_BUTTONS));
			DisplayButtonsFirmware(flags);
		} else {
			DisplayButtons(flags);
		}
	};
	auto displayBusy = [&]() {
		AlphaScope fade(this, FwShow(FWP_BUSY));
		DisplayBusyFirmware();
	};
	auto endDraw = [&]() {
		FwEndFrame();
		EndDraw();
	};

	switch (display)
	{
		case DS_SAVE_LIST_CHOICE:
			startDraw();

			DisplaySaveList();
			DisplaySaveDataInfo1();

			displayButtons(DS_BUTTON_OK | DS_BUTTON_CANCEL);
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
			endDraw();
		break;
		case DS_SAVE_CONFIRM:
			startDraw();

			DisplaySaveIcon(false);
			DisplaySaveDataInfo2(true);

			DisplayMessage(SaveText("msgsavedata_cmn_save_ask", di->T("Confirm Save", "Do you want to save this data?")), true);

			displayButtons(DS_BUTTON_OK | DS_BUTTON_CANCEL);
			DisplayBanner(DB_SAVE);

			if (IsButtonPressed(cancelButtonFlag) || (IsButtonPressed(okButtonFlag) && yesnoChoice == 0)) {
				param.GetPspParam()->common.result = SCE_UTILITY_DIALOG_RESULT_CANCEL;
				StartFade(false);
			} else if (IsButtonPressed(okButtonFlag)) {
				display = DS_SAVE_SAVING;
				StartIOThread();
			}

			endDraw();
		break;
		case DS_SAVE_CONFIRM_OVERWRITE:
			startDraw();

			DisplaySaveIcon(true);
			DisplaySaveDataInfo2();

			DisplayMessage(SaveText("msgsavedata_cmn_overwrite_ask", di->T("Confirm Overwrite","Do you want to overwrite the data?")), true);

			displayButtons(DS_BUTTON_OK | DS_BUTTON_CANCEL);
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

			endDraw();
		break;
		case DS_SAVE_SAVING:
			// The dialog keeps drawing while the IO runs, and takes the results once it's done.
			FinishIO(false);

			startDraw();

			DisplaySaveIcon(true);
			DisplaySaveDataInfo2(true);

			// The firmware shows only its busy icon while it works.
			if (firmwareStyle_) {
				displayBusy();
			} else {
				DisplayMessage(di->T("Saving","Saving\nPlease Wait..."));
			}

			DisplayBanner(DB_SAVE);

			endDraw();
		break;
		case DS_SAVE_FAILED:
			startDraw();

			DisplaySaveIcon(true);
			DisplaySaveDataInfo2(true);

			DisplayMessage(SaveText("msgshare_error_save", di->T("SavingFailed", "Unable to save data.")));

			displayButtons(DS_BUTTON_CANCEL);
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

			endDraw();
		break;
		case DS_SAVE_DONE:
			startDraw();

			DisplaySaveIcon(true);
			DisplaySaveDataInfo2();

			DisplayMessage(SaveText("msgsavedata_cmn_saved", di->T("Save completed")));

			displayButtons(DS_BUTTON_CANCEL);
			DisplayBanner(DB_SAVE);

			if (IsButtonPressed(cancelButtonFlag)) {
				param.GetPspParam()->common.result = SCE_UTILITY_DIALOG_RESULT_SUCCESS;
				// Set the save to use for autosave and autoload
				param.SetSelectedSave(param.GetFileInfo(currentSelectedSave).idx);
				StartFade(false);
			}

			endDraw();
		break;

		case DS_LOAD_LIST_CHOICE:
			startDraw();
			
			DisplaySaveList();
			DisplaySaveDataInfo1();

			displayButtons(DS_BUTTON_OK | DS_BUTTON_CANCEL);
			DisplayBanner(DB_LOAD);

			if (IsButtonPressed(cancelButtonFlag)) {
				param.GetPspParam()->common.result = SCE_UTILITY_DIALOG_RESULT_CANCEL;
				StartFade(false);
			} else if (IsButtonPressed(okButtonFlag)) {
				display = DS_LOAD_LOADING;
				StartIOThread();
			}

			endDraw();
		break;
		case DS_LOAD_CONFIRM:
			startDraw();

			DisplaySaveIcon(true);
			DisplaySaveDataInfo2();

			DisplayMessage(SaveText("msgsavedata_cmn_load_ask", di->T("ConfirmLoad", "Load this data?")), true);

			displayButtons(DS_BUTTON_OK | DS_BUTTON_CANCEL);
			DisplayBanner(DB_LOAD);

			if (IsButtonPressed(cancelButtonFlag) || (IsButtonPressed(okButtonFlag) && yesnoChoice == 0)) {
				param.GetPspParam()->common.result = SCE_UTILITY_DIALOG_RESULT_CANCEL;
				StartFade(false);
			} else if (IsButtonPressed(okButtonFlag)) {
				display = DS_LOAD_LOADING;
				StartIOThread();
			}

			endDraw();
		break;
		case DS_LOAD_LOADING:
			// The dialog keeps drawing while the IO runs, and takes the results once it's done.
			FinishIO(false);

			startDraw();

			DisplaySaveIcon(true);
			DisplaySaveDataInfo2();

			// The firmware shows only its busy icon while it works.
			if (firmwareStyle_) {
				displayBusy();
			} else {
				DisplayMessage(di->T("Loading","Loading\nPlease Wait..."));
			}

			DisplayBanner(DB_LOAD);

			endDraw();
		break;
		case DS_LOAD_FAILED:
			startDraw();

			DisplaySaveIcon(true);
			DisplaySaveDataInfo2();

			DisplayMessage(SaveText("msgshare_sd_error_load_data_corrupted", di->T("LoadingFailed", "Load failed\nThe data is corrupted.")));

			displayButtons(DS_BUTTON_CANCEL);
			DisplayBanner(DB_LOAD);

			if (IsButtonPressed(cancelButtonFlag)) {
				// Go back to the list so they can try again.
				if (param.GetPspParam()->mode != SCE_UTILITY_SAVEDATA_TYPE_LOAD) {
					display = DS_LOAD_LIST_CHOICE;
				} else {
					StartFade(false);
				}
			}

			endDraw();
		break;
		case DS_LOAD_DONE:
			startDraw();
			
			DisplaySaveIcon(true);
			DisplaySaveDataInfo2();

			DisplayMessage(SaveText("msgsavedata_cmn_loaded", di->T("Load completed")));

			displayButtons(DS_BUTTON_CANCEL);
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

			endDraw();
		break;
		case DS_LOAD_NODATA:
			startDraw();

			DisplayMessage(SaveText("msgshare_error_no_data", di->T("There is no data")));

			displayButtons(DS_BUTTON_CANCEL);
			DisplayBanner(DB_LOAD);

			if (IsButtonPressed(cancelButtonFlag)) {
				param.GetPspParam()->common.result = SCE_UTILITY_SAVEDATA_ERROR_LOAD_NO_DATA;
				StartFade(false);
			}

			endDraw();
		break;

		case DS_DELETE_LIST_CHOICE:
			startDraw();
			
			DisplaySaveList();
			DisplaySaveDataInfo1();

			displayButtons(DS_BUTTON_OK | DS_BUTTON_CANCEL);
			DisplayBanner(DB_DELETE);

			if (IsButtonPressed(cancelButtonFlag)) {
				param.GetPspParam()->common.result = SCE_UTILITY_DIALOG_RESULT_CANCEL;
				StartFade(false);
			} else if (IsButtonPressed(okButtonFlag)) {
				yesnoChoice = 0;
				display = DS_DELETE_CONFIRM;
			}

			endDraw();
		break;
		case DS_DELETE_CONFIRM:
			startDraw();

			DisplaySaveIcon(true);
			DisplaySaveDataInfo2();

			DisplayMessage(SaveText("msgshare_del_data_ask", di->T("DeleteConfirm", 
						"This save data will be deleted.\nAre you sure you want to continue?")), 
						true);

			displayButtons(DS_BUTTON_OK | DS_BUTTON_CANCEL);
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

			endDraw();
		break;
		case DS_DELETE_DELETING:
			// The dialog keeps drawing while the IO runs, and takes the results once it's done.
			FinishIO(false);

			startDraw();

			// The firmware shows only its busy icon while it works.
			if (firmwareStyle_) {
				displayBusy();
			} else {
				DisplayMessage(di->T("Deleting","Deleting\nPlease Wait..."));
			}

			DisplayBanner(DB_DELETE);

			endDraw();
		break;
		case DS_DELETE_FAILED:
			startDraw();

			DisplayMessage(SaveText("msgshare_error_del", di->T("DeleteFailed", "Unable to delete data.")));

			displayButtons(DS_BUTTON_CANCEL);
			DisplayBanner(DB_DELETE);

			if (IsButtonPressed(cancelButtonFlag)) {
				if (param.GetPspParam()->mode == SCE_UTILITY_SAVEDATA_TYPE_LISTDELETE || param.GetPspParam()->mode == SCE_UTILITY_SAVEDATA_TYPE_LISTALLDELETE)
					display = DS_DELETE_LIST_CHOICE;
				else
					StartFade(false);
			}

			endDraw();
		break;
		case DS_DELETE_DONE:
			startDraw();
			
			DisplayMessage(SaveText("msgshare_deleted", di->T("Delete completed")));

			displayButtons(DS_BUTTON_CANCEL);
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

			endDraw();
		break;
		case DS_DELETE_NODATA:
			startDraw();
			
			DisplayMessage(SaveText("msgshare_error_no_data", di->T("There is no data")));

			displayButtons(DS_BUTTON_CANCEL);
			DisplayBanner(DB_DELETE);

			if (IsButtonPressed(cancelButtonFlag)) {
				param.GetPspParam()->common.result = SCE_UTILITY_SAVEDATA_ERROR_DELETE_NO_DATA;
				StartFade(false);
			}

			endDraw();
		break;

		case DS_NONE: // For action which display nothing
			if (ioThreadStatus == SAVEIO_NONE) {
				StartIOThread();
			} else if (FinishIO(g_Config.iIOTimingMethod != IOTIMING_HOST)) {
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
	ioThread = std::thread([this]() {
		SetCurrentThreadName("SaveIO");

		AndroidJNIThreadContext jniContext;
		this->ExecuteIOAction();
	});
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
		SaveState::NotifySaveData();
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
			SaveState::NotifySaveData();
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

// Version 4 of the dialog's state (never in a release) kept the IO's writes to PSP memory until the
// results were taken. Late is better than never.
static void DoStateOldPendingWrites(PointerWrap &p) {
	auto s = p.Section("SavedataMemory", 1);
	if (!s) {
		return;
	}
	u32 count = 0;
	Do(p, count);
	for (u32 i = 0; i < count; ++i) {
		u32 addr = 0;
		std::string tag;
		std::vector<u8> data;
		Do(p, addr);
		Do(p, tag);
		Do(p, data);
		if (p.mode == PointerWrap::MODE_READ && Memory::IsValidRange(addr, (u32)data.size())) {
			Memory::Memcpy(addr, data.data(), (u32)data.size(), tag.c_str(), tag.size());
		}
	}
}

void PSPSaveDialog::DoState(PointerWrap &p) {
	if (ioThread.joinable()) {
		ioThread.join();
	}
	PSPDialog::DoState(p);
	if (p.mode == p.MODE_READ) {
		// The firmware style's fades aren't kept: the loaded screen shows as it is.
		FwReset();
	}

	// Version 3 activates the s > 2 branch below, so ioThreadStatus survives
	// a savestate. Safe to restore: the IO thread was joined above, so the
	// value is never SAVEIO_PENDING. Without this, loading a state taken
	// while a savedata operation was in flight would restart the operation
	// instead of resuming from its recorded status. Version 4 keeps the
	// results of a finished operation that haven't been taken yet. Version 6 keeps originalRequest,
	// or the first Update after a load would reload the request and lose results kept in request.
	auto s = p.Section("PSPSaveDialog", 1, 6);
	if (!s) {
		return;
	}

	Do(p, display);
	param.DoState(p);
	Do(p, request);
	// Just reset it.
	bool hasParam = param.GetPspParam() != NULL;
	Do(p, hasParam);
	if (hasParam && p.mode == p.MODE_READ) {
		param.SetPspParam(&request);
	}
	Do(p, requestAddr);
	Do(p, currentSelectedSave);
	Do(p, yesnoChoice);
	SaveIOStatus ioStatus = ioThreadStatus;
	if (s > 2) {
		Do(p, ioStatus);
	} else {
		ioStatus = SAVEIO_NONE;
	}
	ioThreadStatus = ioStatus;
	if (s >= 4) {
		Do(p, ioAction_);
		Do(p, ioDisplay_);
		Do(p, ioRequest_);
		Do(p, ioRequestStart_);
	}
	if (s == 4) {
		DoStateOldPendingWrites(p);
	}
	if (s >= 6) {
		Do(p, originalRequest);
	} else if (p.mode == p.MODE_READ) {
		// Best guess: what the game's request holds now, the way Update reads it.
		memset(&originalRequest, 0, sizeof(originalRequest));
		if (Memory::IsValidRange(requestAddr, 4)) {
			const u32 size = std::min((u32)sizeof(originalRequest), Memory::ReadUnchecked_U32(requestAddr));
			if (Memory::IsValidRange(requestAddr, size)) {
				Memory::Memcpy(&originalRequest, requestAddr, size);
			}
		}
	}
}

pspUtilityDialogCommon *PSPSaveDialog::GetCommonParam() {
	return &param.GetPspParam()->common;
}
