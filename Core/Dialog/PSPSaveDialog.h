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

#pragma once

#include <atomic>
#include <string>
#include <thread>
#include <mutex>

#include "Core/Dialog/PSPDialog.h"
#include "Core/Dialog/SavedataParam.h"

class PSPSaveDialog : public PSPDialog {
public:
	PSPSaveDialog(UtilityDialogType type);
	~PSPSaveDialog();

	int Init(int paramAddr);
	int Update(int animSpeed) override;
	int Shutdown(bool force = false) override;
	void DoState(PointerWrap &p) override;
	pspUtilityDialogCommon *GetCommonParam() override;
	// Waits for the IO thread, if any, to be done with PSP memory. Its results are still taken as usual.
	void WaitForIO();

protected:
	bool UseAutoStatus() override {
		return false;
	}

private:
	void DisplayBanner(int which);
	void DisplaySaveList(bool canMove = true);
	void DisplaySaveIcon(bool checkExists);
	void DisplaySaveDataInfo1();
	void DisplaySaveDataInfo2(bool showNewData = false);
	void DisplayMessage(std::string_view text, bool hasYesNo = false);
	std::string GetSelectedSaveDirName() const;

	// The firmware's look (see FirmwareDialogStyle.h), from savedata_utility.rco.
	void DisplayBannerFirmware(int which);
	void DisplaySaveListFirmware(bool drawSelected = true);
	void DisplaySaveIconFirmware();
	void DrawGlowFirmware(float alpha);
	void DrawShadowFirmware(float alpha);
	void DisplaySaveDataInfo1Firmware();
	void DisplaySaveDataInfo2Firmware(bool showNewData);
	void DrawSaveIconFirmware(const SaveFileInfo &info, float x, float y, float w, float h, u32 color);
	void MoveSelection();
	// A string of the firmware's savedata dialog in the dialog's language, when drawing in its
	// style; otherwise (or if missing) fallback.
	std::string SaveText(const char *label, std::string_view fallback);
	// The title the game gave its new save, if any.
	std::string NewDataTitle();

	void StartIOThread();
	bool FinishIO(bool wait);
	void ExecuteIOAction();
	void ExecuteNotVisibleIOAction();

	enum DisplayState {
		DS_NONE,

		DS_SAVE_LIST_CHOICE,
		DS_SAVE_CONFIRM,
		DS_SAVE_CONFIRM_OVERWRITE,
		DS_SAVE_SAVING,
		DS_SAVE_DONE,

		DS_LOAD_LIST_CHOICE,
		DS_LOAD_CONFIRM,
		DS_LOAD_LOADING,
		DS_LOAD_DONE,
		DS_LOAD_NODATA,

		DS_DELETE_LIST_CHOICE,
		DS_DELETE_CONFIRM,
		DS_DELETE_DELETING,
		DS_DELETE_DONE,
		DS_DELETE_NODATA,

		DS_SAVE_FAILED,
		DS_LOAD_FAILED,
		DS_DELETE_FAILED,
	};

	enum DialogBanner {
		DB_NONE,
		DB_SAVE,
		DB_LOAD,
		DB_DELETE
	};

	DisplayState display = DS_NONE;

	SavedataParam param;
	SceUtilitySavedataParam request{};
	// For detecting changes made by the game.
	SceUtilitySavedataParam originalRequest{};
	u32 requestAddr = 0;
	int currentSelectedSave = 0;
	// Drawing in the firmware's style this frame.
	bool firmwareStyle_ = false;

	// The firmware's screens change by fading their parts out and in, each at its own pace. What
	// each frame draws is noted, to tell what has come and gone.
	enum FwPart {
		FWP_LIST,  // The other icons and the selected one's details.
		FWP_ICON,  // The selected icon.
		FWP_GLOW,
		FWP_SHADOW,
		FWP_INFO2,
		FWP_MESSAGE,
		FWP_BUTTONS,
		FWP_BUSY,
		FWP_COUNT,
	};
	struct FwScreen {
		bool has[FWP_COUNT]{};
		bool newData = false;
		std::string message;
		bool yesNo = false;
		bool besideIcon = false;
		int buttons = 0;
	};
	void FwReset();
	// Notes that this frame draws part (its parameters in fwScreen_ set first), and returns how far
	// it has faded in.
	float FwShow(FwPart part);
	// How opaque a part that has gone still is (0 once it's faded out).
	float FwOutAlpha(FwPart part) const;
	float FwSeconds(int since) const;
	bool FwSame(FwPart part, const FwScreen &a, const FwScreen &b) const;
	// Draws what's fading out, and moves on to the next frame.
	void FwEndFrame();

	FwScreen fwScreen_;
	FwScreen fwLastScreen_;
	FwScreen fwOutScreen_[FWP_COUNT];
	int fwInStart_[FWP_COUNT]{};
	int fwOutStart_[FWP_COUNT]{};
	int fwClock_ = 0;
	bool fwFirstFrame_ = true;
	bool fwDrawingOut_ = false;
	// Whether the save's icon was drawn this frame (the message box sits beside it).
	bool iconShown_ = false;

	enum SaveIOStatus {
		SAVEIO_NONE,
		SAVEIO_PENDING,
		// Finished, and the results taken.
		SAVEIO_DONE,
		// Finished, but the request changes and bookkeeping haven't been taken yet.
		SAVEIO_READY,
	};

	std::thread ioThread;
	std::mutex paramLock;
	std::atomic<SaveIOStatus> ioThreadStatus{ SAVEIO_NONE };

	// The IO thread uses these instead of the dialog's own state (it writes its results to PSP
	// memory directly). StartIOThread sets them up and FinishIO takes the results back, both on the
	// emulator thread.
	DisplayState ioAction_ = DS_NONE;
	DisplayState ioDisplay_ = DS_NONE;
	SceUtilitySavedataParam ioRequest_{};
	// The request when the IO started, to tell what the IO changed.
	SceUtilitySavedataParam ioRequestStart_{};
	SavedataParam ioParam_;
	int ioSaveId_ = 0;
	std::string ioSaveDirName_;
	std::string ioListSaveDirName_;
	std::string ioDeleteDir_;
};

void ResetSecondsSinceLastGameSave();
double SecondsSinceLastGameSave();
