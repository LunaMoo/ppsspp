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

#include <algorithm>
#include <initializer_list>

#include "Common/CommonTypes.h"
#include "Common/Render/TextureAtlas.h"
#include "Common/Swap.h"
#include "Core/HLE/sceUtility.h"
#include "Core/Util/PPGeDraw.h"

class PointerWrap;

// The sounds the system dialogs make on a PSP: the XMB's cursor tick and its confirm and cancel
// clicks. The host plays them, if it wants to (see SetUtilityDialogSoundCallback).
enum class UtilityDialogSound {
	CURSOR,
	CONFIRM,
	CANCEL,
};
// Called from the emu thread when a dialog sees a button pressed.
void SetUtilityDialogSoundCallback(void (*callback)(UtilityDialogSound sound));

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

// How far a firmware style message box has got in appearing: its lines fade in while moving
// apart to their places, then the rest fades in.
struct FirmwareBoxAnim {
	float lineAlpha = 1.0f;
	float lineInset = 0.0f;
	// How far the lines have moved apart from the box's middle, 0 to 1 (on top of lineInset).
	float lineSpread = 1.0f;
	float contentAlpha = 1.0f;
};

class PSPDialog {
public:
	PSPDialog(UtilityDialogType type) : dialogType_(type) {}
	virtual ~PSPDialog() {}

	virtual int Update(int animSpeed) = 0;
	virtual int Shutdown(bool force = false);
	virtual void DoState(PointerWrap &p);
	// For loading a state that doesn't have this dialog: back to NONE, without anything Shutdown does
	// on the way (writing results, releasing volatile memory), which would hit the loaded state.
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

	// dim: darken the game a little behind the dialog (PPSSPP's style; the firmware's doesn't).
	void StartDraw(bool dim = true);
	void EndDraw();

	void FinishVolatile();
	int FinishInit();
	int FinishShutdown();

protected:
	void InitCommon();
	void UpdateCommon();
	PPGeStyle FadedStyle(PPGeAlign align, float scale);
	PPGeImageStyle FadedImageStyle();
	void UpdateButtons();
	bool IsFadingOut() const;
	bool IsButtonPressed(int checkButton);
	bool IsButtonHeld(int checkButton, int &framesHeld, int framesHeldThreshold = 30, int framesHeldRepeatRate = 10);
	// The caption override is assumed to have a size of 64 bytes.
	void DisplayButtons(int flags, std::string_view caption = "");
	// The firmware style's pieces (see FirmwareDialogStyle.h). Text styles in the firmware's fonts at
	// the firmware's sizes; call PPGeSetFirmwareFont first.
	PPGeStyle FirmwareTextStyle(PPGeAlign align, float scale);
	// The firmware's button guide at the bottom (system_plugin_fg.rco's page_btnnavi).
	void DisplayButtonsFirmware(int flags, std::string_view caption = "");
	// A string from the firmware's common dialog parts in the dialog's language, or fallback.
	std::string FirmwareText(const char *label, std::string_view fallback);
	// The firmware's message box: text centred on centerX, wrapped at wrapWidth, between two lines
	// boxWidth wide, with Yes/No or OK under it. Text past 8 lines scrolls, if canScroll. Without
	// interactive, it only draws (for one fading out).
	void DisplayMessageBoxFirmware(std::string_view text, float centerX, float boxWidth, float wrapWidth, bool hasYesNo, bool hasOK, bool canScroll, bool interactive = true, const FirmwareBoxAnim &anim = FirmwareBoxAnim());
	// The firmware's highlights pulse: 0 to 1 and back.
	float FirmwarePulse() const;
	void UpdateScroll(float visibleHeight, float totalHeight, float step);
	// The firmware's scroll bar, left of a message box's text.
	void DisplayScrollBarFirmware(float visibleLines, float totalLines, float scrollPos, float scrollMax);
	void DisplayMessage2(std::string_view text1, std::string_view text2a = "", std::string_view text2b = "", std::string_view text3a = "", std::string_view text3b = "", bool hasYesNo = false, bool hasOK = false);
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

	// Makes everything drawn while it lives alpha times as opaque, on top of the dialog's fade.
	class AlphaScope {
	public:
		AlphaScope(PSPDialog *dialog, float alpha) : dialog_(dialog), saved_(dialog->fadeValue) {
			dialog->fadeValue = (u32)(saved_ * std::clamp(alpha, 0.0f, 1.0f) + 0.5f);
		}
		~AlphaScope() {
			dialog_->fadeValue = saved_;
		}

	private:
		PSPDialog *dialog_;
		u32 saved_;
	};

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

	ImageID okButtonImg;
	ImageID cancelButtonImg;
	int okButtonFlag = 0;
	int cancelButtonFlag = 0;

	// DisplayMessage2 variables
	int yesnoChoice = 0;
	float scrollPos_ = 0.0f;
	int framesUpHeld_ = 0;
	int framesDownHeld_ = 0;
	// Frames since the dialog started or the focus last moved, for animations (FirmwarePulse).
	int pulseFrames_ = 0;

private:
	DialogStatus status = SCE_UTILITY_STATUS_NONE;
	UtilityDialogType dialogType_ = UtilityDialogType::NONE;
	bool volatileLocked_ = false;
};

const char *UtilityDialogTypeToString(UtilityDialogType type);
const char *UtilityDialogStatusToString(PSPDialog::DialogStatus status);
