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
#include <cmath>
#include "Common/Data/Text/I18n.h"

#include "Common/Serialize/Serializer.h"
#include "Common/Serialize/SerializeFuncs.h"
#include "Common/StringUtils.h"
#include "Common/Data/Encoding/Utf8.h"
#include "Core/Config.h"
#include "Core/System.h"
#include "Core/CoreTiming.h"
#include "Core/Dialog/FirmwareDialogStyle.h"
#include "Core/Dialog/PSPDialog.h"
#include "Core/HLE/PopsBoot.h"
#include "Core/HLE/sceCtrl.h"
#include "Core/HLE/sceKernelThread.h"
#include "Core/HLE/scePower.h"
#include "Core/HLE/sceUtility.h"
#include "Core/MemMapHelpers.h"
#include "Core/Util/PPGeDraw.h"

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
		DEBUG_LOG(Log::sceUtility, "%s thread priorities: graphics=%d access=%d font=%d sound=%d (caller %d)",
			UtilityDialogTypeToString(DialogType()), common->graphicsThread, common->accessThread,
			common->fontThread, common->soundThread, KernelCurThreadPriority());
	}

	if (GetCommonParam() && GetCommonParam()->language != GetPSPLanguage()) {
		WARN_LOG(Log::sceUtility, "Game requested language %d, ignoring and using user language", GetCommonParam()->language);
	}
}

void PSPDialog::UpdateCommon() {
	okButtonImg = ImageID("I_CIRCLE");
	cancelButtonImg = ImageID("I_CROSS");
	okButtonFlag = CTRL_CIRCLE;
	cancelButtonFlag = CTRL_CROSS;
	if (GetCommonParam() && GetCommonParam()->buttonSwap == 1) {
		okButtonImg = ImageID("I_CROSS");
		cancelButtonImg = ImageID("I_CIRCLE");
		okButtonFlag = CTRL_CROSS;
		cancelButtonFlag = CTRL_CIRCLE;
	}
}

void PSPDialog::UpdatePendingStatus() {
	if (pendingStatusTicks != 0 && CoreTiming::GetTicks(currentMIPS) >= pendingStatusTicks) {
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
		pendingStatusTicks = CoreTiming::GetTicks(currentMIPS) + usToCycles(delayUs);
	}
}

void PSPDialog::FinishVolatile() {
	if (!volatileLocked_)
		return;

	if (KernelVolatileMemUnlock(0) == 0) {
		volatileLocked_ = false;
		// Simulate modifications to volatile memory. Not under the PS1 emulator, which keeps its heap there.
		if (!PopsBootActive()) {
			Memory::Memset(PSP_GetVolatileMemoryStart(), 0, PSP_GetVolatileMemoryEnd() - PSP_GetVolatileMemoryStart());
		}
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

void PSPDialog::StartDraw(bool dim)
{
	PPGeBegin();
	if (dim) {
		PPGeDrawRect(0, 0, 480, 272, CalcFadedColor(0x20000000));
	}
}

void PSPDialog::EndDraw()
{
	PPGeEnd();
	PPGeSetFirmwareFont(false);
}

int PSPDialog::Shutdown(bool force)
{
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
	pulseFrames_ += std::max(animSpeed, 1);
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

void PSPDialog::DoState(PointerWrap &p) {
	auto s = p.Section("PSPDialog", 1, 3);
	if (!s)
		return;

	Do(p, status);
	Do(p, lastButtons);
	Do(p, buttons);
	Do(p, fadeTimer);
	Do(p, isFading);
	Do(p, fadeIn);
	Do(p, fadeValue);

	// I don't think we should save these two... Let's just ignore them for now for compat.
	int okButtonImg = 0;
	Do(p, okButtonImg);
	int cancelButtonImg = 0;
	Do(p, cancelButtonImg);

	Do(p, okButtonFlag);
	Do(p, cancelButtonFlag);

	if (s >= 2) {
		Do(p, pendingStatus);
		Do(p, pendingStatusTicks);
	} else {
		pendingStatusTicks = 0;
	}

	if (s >= 3) {
		Do(p, volatileLocked_);
	} else {
		volatileLocked_ = false;
	}
}

static void (*g_utilityDialogSoundCallback)(UtilityDialogSound sound);

void SetUtilityDialogSoundCallback(void (*callback)(UtilityDialogSound sound)) {
	g_utilityDialogSoundCallback = callback;
}

void PSPDialog::UpdateButtons()
{
	lastButtons = __CtrlPeekButtons();
	buttons = __CtrlReadLatch();

	// As on a PSP, the dialogs tick when moving around and click when choosing. Once a frame, for
	// what was just pressed.
	if (g_utilityDialogSoundCallback && buttons && !IsFadingOut()) {
		if (buttons & okButtonFlag) {
			g_utilityDialogSoundCallback(UtilityDialogSound::CONFIRM);
		} else if (buttons & cancelButtonFlag) {
			g_utilityDialogSoundCallback(UtilityDialogSound::CANCEL);
		} else if (buttons & (CTRL_UP | CTRL_DOWN | CTRL_LEFT | CTRL_RIGHT)) {
			g_utilityDialogSoundCallback(UtilityDialogSound::CURSOR);
		}
	}
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
	bool btnWasHeldLastFrame = (lastButtons & checkButton) && (__CtrlPeekButtons() & checkButton);
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

PPGeStyle PSPDialog::FadedStyle(PPGeAlign align, float scale) {
	PPGeStyle textStyle;
	textStyle.align = align;
	textStyle.scale = scale;
	textStyle.color = CalcFadedColor(textStyle.color);
	textStyle.hasShadow = true;
	textStyle.shadowColor = CalcFadedColor(textStyle.shadowColor);
	return textStyle;
}

PPGeImageStyle PSPDialog::FadedImageStyle() {
	PPGeImageStyle style;
	style.color = CalcFadedColor(style.color);
	return style;
}

void PSPDialog::DisplayButtons(int flags, std::string_view caption) {
	bool useCaption = false;
	char safeCaption[65] = {0};
	if (!caption.empty()) {
		useCaption = true;
		truncate_cpy(safeCaption, sizeof(safeCaption), caption);
	}

	PPGeStyle textStyle = FadedStyle(PPGeAlign::BOX_LEFT, FONT_SCALE);

	auto di = GetI18NCategory(I18NCat::DIALOG);
	float x1 = 183.5f, x2 = 261.5f;

	const pspUtilityDialogCommon *commonParams = GetCommonParam();
	if (!commonParams) {
		return;
	}

	if (commonParams->buttonSwap == 1) {
		x1 = 261.5f;
		x2 = 183.5f;
	}
	if (flags & DS_BUTTON_OK) {
		std::string_view text = useCaption ? safeCaption : di->T("Enter");
		PPGeDrawImage(okButtonImg, x2, 256, 11.5f, 11.5f, textStyle);
		PPGeDrawText(text, x2 + 14.5f, 252, textStyle);
	}
	if (flags & DS_BUTTON_CANCEL) {
		std::string_view text = useCaption ? safeCaption : di->T("Back");
		PPGeDrawImage(cancelButtonImg, x1, 256, 11.5f, 11.5f, textStyle);
		PPGeDrawText(text, x1 + 14.5f, 252, textStyle);
	}
}

// The firmware's button guide, page_btnnavi in 6.61's system_plugin_fg.rco. Its objects are in
// paf's coordinates (origin at the screen's center, y up): the cross group at (-49, -127), the
// circle group at (29, -127), each the centre of the icon's shadow, with the icon centred 2 left
// and 2 up of it and the label starting 7 right of it. Labels at text size 7.0 (the PGF fonts are
// 10.125). Checked against a screenshot of a PSP's message dialog: the circle's centre at
// (266.5, 260.5), "Back" from x 277 on a baseline at 265.
namespace {
const float BTNNAVI_CROSS_X = 240.0f - 49.0f;
const float BTNNAVI_CIRCLE_X = 240.0f + 29.0f;
const float BTNNAVI_Y = 136.0f + 127.0f;
const float BTNNAVI_LABEL_BASELINE = 265.0f;
const float FIRMWARE_FONT_SIZE = 10.125f;
const float BTNNAVI_LABEL_SCALE = 7.0f / FIRMWARE_FONT_SIZE;
}

PPGeStyle PSPDialog::FirmwareTextStyle(PPGeAlign align, float scale) {
	PPGeStyle style = FadedStyle(align, scale);
	// The firmware's soft text shadow (see PPGeMakeFirmwareTextShadow), tuned until fitting the
	// same model to our render and to a PSP's screenshot gives the same blur, offset and strength.
	style.hasShadow = true;
	style.shadowColor = CalcFadedColor(0xE0000000);
	style.shadowOffsetX = 3.5f;
	style.shadowOffsetY = 3.75f;
	return style;
}

std::string PSPDialog::FirmwareText(const char *label, std::string_view fallback) {
	const pspUtilityDialogCommon *common = GetCommonParam();
	std::string text = FirmwareSystemText(label, common ? (int)common->language : 1);
	return text.empty() ? std::string(fallback) : text;
}

void PSPDialog::DisplayButtonsFirmware(int flags, std::string_view caption) {
	const pspUtilityDialogCommon *commonParams = GetCommonParam();
	if (!commonParams) {
		return;
	}

	auto di = GetI18NCategory(I18NCat::DIALOG);
	PPGeSetFirmwareFont(true);
	const PPGeStyle labelStyle = FirmwareTextStyle(PPGeAlign::BOX_LEFT, BTNNAVI_LABEL_SCALE);
	const float labelTop = BTNNAVI_LABEL_BASELINE - PPGeFirmwareFontAscent() * BTNNAVI_LABEL_SCALE;
	const PPGeImageStyle imageStyle = FadedImageStyle();

	// The cross is always on the left and the circle on the right. What they say follows which one
	// confirms, as the game asked (the same as the input uses).
	const bool circleConfirms = okButtonFlag == CTRL_CIRCLE;
	struct Slot {
		const char *icon;
		const char *shadow;
		float x;
		int button;
	};
	const Slot slots[2] = {
		{ "tex_cross", "tex_cross_shadow", BTNNAVI_CROSS_X, circleConfirms ? DS_BUTTON_CANCEL : DS_BUTTON_OK },
		{ "tex_circle", "tex_circle_shadow", BTNNAVI_CIRCLE_X, circleConfirms ? DS_BUTTON_OK : DS_BUTTON_CANCEL },
	};
	for (const Slot &slot : slots) {
		if (!(flags & slot.button)) {
			continue;
		}
		std::string text;
		if (!caption.empty()) {
			char safeCaption[65]{};
			truncate_cpy(safeCaption, sizeof(safeCaption), caption);
			text = safeCaption;
		} else if (slot.button == DS_BUTTON_OK) {
			text = FirmwareText("msgsystem_enter", di->T("Enter"));
		} else {
			text = FirmwareText("msgsystem_back", di->T("Back"));
		}

		PPGeImage *icon = PPGeFirmwareImage("system_plugin_fg.rco", slot.icon);
		PPGeImage *shadow = PPGeFirmwareImage("system_plugin_fg.rco", slot.shadow);
		if (shadow && icon) {
			shadow->SetTexture();
			const float sw = (float)shadow->ContentWidth(), sh = (float)shadow->Height();
			PPGeDrawFirmwareImage(shadow, slot.x - sw / 2.0f, BTNNAVI_Y - sh / 2.0f, 0, 0, imageStyle);
		}
		if (icon) {
			icon->SetTexture();
			const float iw = (float)icon->ContentWidth(), ih = (float)icon->Height();
			PPGeDrawFirmwareImage(icon, slot.x - 2.0f - iw / 2.0f, BTNNAVI_Y - 2.0f - ih / 2.0f, 0, 0, imageStyle);
		}
		PPGeDrawText(text, slot.x + 7.0f, labelTop, labelStyle);
	}
}

// The firmware's message box, measured from a screenshot and photos of a PSP-2000 on 6.61 (the
// layout is drawn by sceVshCommonGui, not described in an RCO). Two separator lines, one pixel of
// white at about 65%, the box centred at y 135.5. The text is ltn0.pgf at size 7.605 (from
// common_gui.prx), lines 18.86 apart (8 lines of scrolling text span 132), the block centred with
// its lines left aligned. Yes's advance ends 19.5 left of the centre (its ink about 22.5) and No
// starts 19.5 right of it (from a savedata dialog's screenshots, which fit the message dialog's
// too), the chosen one on tex_default_text_focus (system_plugin.rco).
namespace {
const float FW_TEXT_SCALE = 7.605f / FIRMWARE_FONT_SIZE;
const float FW_LINE_STEP = 18.86f;
const float FW_BOX_CENTER_Y = 135.5f;
const float FW_TOP_TO_FIRST_BASELINE = 28.5f;
const float FW_TEXT_TO_BUTTONS = 33.2f;  // Baseline to baseline.
const float FW_LAST_BASELINE_TO_BOTTOM = 15.5f;
// A box of text that scrolls is a little shorter at the bottom (screenshots of PSP Tool's credits).
const float FW_LAST_BASELINE_TO_BOTTOM_SCROLLING = 14.5f;
// It scrolls 12 pixels a step, the last step up or down going all the way.
const float FW_SCROLL_STEP = 12.0f;
// And its text sits a little right of the centre, next to the scroll bar.
const float FW_SCROLLING_TEXT_SHIFT = 2.5f;
// The scroll bar left of the text (system_plugin.rco's tex_default_scroll_*): the bar from y 64 to
// 208 at x 78, the arrows above and below it while there's more to see that way.
const float FW_SCROLLBAR_X = 78.0f;
const float FW_SCROLLBAR_TOP = 64.0f;
const float FW_SCROLLBAR_BOTTOM = 208.0f;
const float FW_YES_RIGHT = -19.5f;
const float FW_NO_LEFT = 19.5f;
const int FW_MAX_VISIBLE_LINES = 8;
// The focus' and selection glow's pulse, from a video of a PSP's save dialog: up and down at an
// even pace, 18 frames (at 30 a second) each way, starting from nothing when the focus moves.
const float FW_PULSE_PERIOD = 1.2f;
// page_busy in system_plugin_fg.rco: busy_icon at (226, -122), 17x17, its shadow 2 right and 2 down.
// tex_busy holds its 60 frames, 17x17 each, two to a row.
const float FW_BUSY_X = 240.0f + 226.0f;
const float FW_BUSY_Y = 136.0f + 122.0f;
const float FW_BUSY_SIZE = 17.0f;
const int FW_BUSY_FRAMES = 60;
}

float PSPDialog::FirmwarePulse() const {
	const float t = (float)pulseFrames_ / (60.0f * FW_PULSE_PERIOD);
	const float phase = t - floorf(t);
	return phase < 0.5f ? phase * 2.0f : 2.0f - phase * 2.0f;
}

void PSPDialog::DisplayBusyFirmware() {
	PPGeImage *icon = PPGeFirmwareImage("system_plugin_fg.rco", "tex_busy");
	PPGeImage *shadow = PPGeFirmwareImage("system_plugin_fg.rco", "tex_busy_shadow");
	// One frame of the animation per frame of video.
	const int frame = pulseFrames_ % FW_BUSY_FRAMES;
	const float half = FW_BUSY_SIZE / 2.0f;
	auto draw = [&](PPGeImage *image, float cx, float cy) {
		if (!image) {
			return;
		}
		image->SetTexture();
		if (!image->IsValid() || image->Width() == 0) {
			return;
		}
		const float tw = (float)image->Width(), th = (float)image->Height();
		const float u = (frame % 2) * FW_BUSY_SIZE, v = (frame / 2) * FW_BUSY_SIZE;
		PPGeDrawImage(cx - half, cy - half, FW_BUSY_SIZE, FW_BUSY_SIZE, u / tw, v / th, (u + FW_BUSY_SIZE) / tw, (v + FW_BUSY_SIZE) / th, image->Width(), image->Height(), FadedImageStyle());
	};
	draw(shadow, FW_BUSY_X + 2.0f, FW_BUSY_Y + 2.0f);
	draw(icon, FW_BUSY_X, FW_BUSY_Y);
	PPGeSetDefaultTexture();
}

void PSPDialog::UpdateScroll(float visibleHeight, float totalHeight, float step) {
	if (visibleHeight >= totalHeight) {
		return;
	}
	const float scrollMax = totalHeight - visibleHeight;
	// A step a press, repeating while held.
	auto buttonDown = [this](int btn, int &held) {
		if (IsButtonPressed(btn)) {
			held = 0;
			return true;
		}
		return IsButtonHeld(btn, held, 24, 6);
	};
	if (buttonDown(CTRL_DOWN, framesDownHeld_) && scrollPos_ < scrollMax) {
		scrollPos_ += step;
		if (scrollMax - scrollPos_ < step) {
			scrollPos_ = scrollMax;
		}
	}
	if (buttonDown(CTRL_UP, framesUpHeld_) && scrollPos_ > 0.0f) {
		scrollPos_ -= step;
		if (scrollPos_ < step) {
			scrollPos_ = 0.0f;
		}
	}
}

void PSPDialog::DisplayScrollBarFirmware(float visibleLines, float totalLines, float scrollPos, float scrollMax) {
	PPGeImage *base = PPGeFirmwareImage("system_plugin.rco", "tex_default_scroll_base");
	PPGeImage *baseShadow = PPGeFirmwareImage("system_plugin.rco", "tex_default_scroll_base_shadow");
	PPGeImage *slider = PPGeFirmwareImage("system_plugin.rco", "tex_default_scroll_slider");
	if (!base || !slider) {
		return;
	}
	const PPGeImageStyle style = FadedImageStyle();
	const float barHeight = FW_SCROLLBAR_BOTTOM - FW_SCROLLBAR_TOP;
	if (baseShadow) {
		baseShadow->SetTexture();
		const float sw = (float)baseShadow->ContentWidth(), sh = (float)baseShadow->Height();
		const float bw = (float)base->ContentWidth(), bh = (float)base->Height();
		// Centred on the bar, 2 right and 2 down.
		PPGeDrawFirmwareImageFrame(baseShadow, FW_SCROLLBAR_X - (sw - bw) / 2.0f + 2.0f, FW_SCROLLBAR_TOP - (sh - bh) / 2.0f + 2.0f, sw, barHeight + sh - bh, sw / 2.0f, sh / 2.0f - 0.5f, style);
	}
	base->SetTexture();
	PPGeDrawFirmwareImageFrame(base, FW_SCROLLBAR_X, FW_SCROLLBAR_TOP, (float)base->ContentWidth(), barHeight, 3.0f, 2.0f, style);

	// Its edge pixels are see-through, as are the bar's: the inner parts line up. Its length is the
	// part of the text in view, counting a line more than there is (which fits the screenshots).
	const float inner = barHeight - 2.0f;
	const float length = std::max(8.0f, roundf(inner * visibleLines / (totalLines + 1.0f)));
	const float top = FW_SCROLLBAR_TOP + roundf((inner - length) * (scrollMax > 0.0f ? scrollPos / scrollMax : 0.0f));
	slider->SetTexture();
	PPGeDrawFirmwareImageFrame(slider, FW_SCROLLBAR_X, top, (float)slider->ContentWidth(), length + 2.0f, 3.0f, 2.0f, style);

	// The arrows, their tips at y 54 and 217.
	struct Arrow {
		const char *image;
		const char *shadow;
		float centerY;
		bool shown;
	};
	const Arrow arrows[] = {
		{ "tex_default_scroll_arrow_up", "tex_default_scroll_arrow_up_shadow", 56.5f, scrollPos > 0.0f },
		{ "tex_default_scroll_arrow_down", "tex_default_scroll_arrow_down_shadow", 214.5f, scrollPos < scrollMax },
	};
	const float centerX = FW_SCROLLBAR_X + 3.0f;
	for (const Arrow &arrow : arrows) {
		if (!arrow.shown) {
			continue;
		}
		if (PPGeImage *shadow = PPGeFirmwareImage("system_plugin.rco", arrow.shadow)) {
			shadow->SetTexture();
			const float w = (float)shadow->ContentWidth(), h = (float)shadow->Height();
			PPGeDrawFirmwareImage(shadow, roundf(centerX - w / 2.0f) + 2.0f, roundf(arrow.centerY - h / 2.0f) + 2.0f, 0, 0, style);
		}
		if (PPGeImage *image = PPGeFirmwareImage("system_plugin.rco", arrow.image)) {
			image->SetTexture();
			const float w = (float)image->ContentWidth(), h = (float)image->Height();
			PPGeDrawFirmwareImage(image, roundf(centerX - w / 2.0f), roundf(arrow.centerY - h / 2.0f), 0, 0, style);
		}
	}
}

void PSPDialog::DisplayMessageBoxFirmware(std::string_view text, float centerX, float boxWidth, float wrapWidth, bool hasYesNo, bool hasOK, bool canScroll, bool interactive, const FirmwareBoxAnim &anim) {
	auto di = GetI18NCategory(I18NCat::DIALOG);
	// The line spacing that puts lines FW_LINE_STEP apart at this size.
	const float fontLineHeight = PPGeFirmwareFontLineHeight() * FW_TEXT_SCALE;
	PPGeSetFirmwareFont(true, fontLineHeight > 0.0f ? FW_LINE_STEP / fontLineHeight : 1.5f);

	const float ascent = PPGeFirmwareFontAscent() * FW_TEXT_SCALE;

	float textWidth = 0.0f, textHeight = 0.0f;
	PPGeMeasureText(&textWidth, &textHeight, text, FW_TEXT_SCALE, PPGE_LINE_WRAP_WORD, (int)wrapWidth);
	float oneLine = 0.0f, twoLines = 0.0f;
	PPGeMeasureText(nullptr, &oneLine, "|", FW_TEXT_SCALE);
	PPGeMeasureText(nullptr, &twoLines, "|\n|", FW_TEXT_SCALE);
	const float lineStep = std::max(1.0f, twoLines - oneLine);
	// The lines as laid out, to draw one at a time when scrolling.
	const std::vector<PPGeTextLine> wrapped = PPGeWrapFirmwareText(text, FW_TEXT_SCALE, wrapWidth);
	const int lines = !wrapped.empty() ? (int)wrapped.size() : std::max(1, (int)((textHeight - oneLine) / lineStep + 0.5f) + 1);
	const int visibleLines = canScroll ? std::min(lines, FW_MAX_VISIBLE_LINES) : lines;
	const bool hasButtons = hasYesNo || hasOK;
	// First to last visible baseline. lineStep is rounded, so use the text's own height when it all
	// fits.
	// Scrolling, the box fits whole lines FW_LINE_STEP apart (which the lines' rounded positions are,
	// near enough).
	const float textSpan = visibleLines == lines ? textHeight - oneLine : (visibleLines - 1) * FW_LINE_STEP;

	const float bottomMargin = visibleLines < lines ? FW_LAST_BASELINE_TO_BOTTOM_SCROLLING : FW_LAST_BASELINE_TO_BOTTOM;
	const float boxHeight = FW_TOP_TO_FIRST_BASELINE + textSpan + (hasButtons ? FW_TEXT_TO_BUTTONS : 0.0f) + bottomMargin;
	const float topLine = roundf(FW_BOX_CENTER_Y - boxHeight / 2.0f);
	const float bottomLine = topLine + roundf(boxHeight);
	const float firstBaseline = topLine + FW_TOP_TO_FIRST_BASELINE;
	const float buttonBaseline = firstBaseline + textSpan + FW_TEXT_TO_BUTTONS;

	const float lineX1 = centerX - boxWidth / 2.0f, lineX2 = centerX + boxWidth / 2.0f;
	{
		AlphaScope lineFade(this, anim.lineAlpha);
		const float inset = anim.lineInset + roundf((1.0f - std::clamp(anim.lineSpread, 0.0f, 1.0f)) * (bottomLine - topLine) / 2.0f);
		for (float y : { topLine + inset, bottomLine - inset }) {
			PPGeDrawRect(lineX1, y, lineX2, y + 1.0f, CalcFadedColor(0xA6FFFFFF));
		}
	}
	AlphaScope contentFade(this, anim.contentAlpha);

	// The text: the block centred, its lines left aligned in it.
	const float textTop = firstBaseline - ascent;
	const float visibleHeight = oneLine + textSpan;
	const float totalHeight = textHeight;
	const PPGeStyle blockStyle = FirmwareTextStyle(PPGeAlign::BOX_LEFT, FW_TEXT_SCALE);
	const bool scrolls = visibleHeight < totalHeight;
	const float textLeft = roundf(centerX - textWidth / 2.0f + (scrolls ? FW_SCROLLING_TEXT_SHIFT : 0.0f));
	if (scrolls && !wrapped.empty()) {
		// Scrolling text: the firmware only shows the lines that fit in the box whole, so nothing is
		// cut off (its soft shadow included).
		for (const PPGeTextLine &line : wrapped) {
			const float top = textTop + line.top - scrollPos_;
			if (top >= textTop - 1.0f && top + oneLine <= textTop + visibleHeight + 1.0f) {
				PPGeDrawText(line.text, textLeft, top, blockStyle);
			}
		}
	} else if (scrolls) {
		PPGeScissor(0, (int)(textTop - 2), 480, (int)(textTop + visibleHeight + 4));
		PPGeDrawTextWrapped(text, textLeft, textTop - scrollPos_, wrapWidth, 0, blockStyle);
		PPGeScissorReset();
	} else {
		PPGeDrawTextWrapped(text, textLeft, textTop - scrollPos_, wrapWidth, 0, blockStyle);
	}
	if (interactive) {
		UpdateScroll(visibleHeight, totalHeight, FW_SCROLL_STEP);
	}
	if (scrolls) {
		DisplayScrollBarFirmware((float)visibleLines, (float)lines, scrollPos_, totalHeight - visibleHeight);
	}

	if (!hasButtons) {
		return;
	}

	PPGeImage *focus = PPGeFirmwareImage("system_plugin.rco", "tex_default_text_focus");
	// left is where the word starts.
	auto drawButton = [&](std::string_view label, float left, bool selected) {
		float w = 0.0f;
		PPGeMeasureText(&w, nullptr, label, FW_TEXT_SCALE);
		if (selected) {
			// The focus image is 60x18 with the box in its middle 30 columns (and rows 1-16): drawn
			// twice as wide as the box, which is the word's ink and 6 either side, from 14 above
			// the baseline to 2 below. It pulses in and out.
			const float boxWidth = std::max(30.0f, roundf(w + 10.0f));
			const float imageWidth = boxWidth * 2.0f;
			const float centre = left + w / 2.0f;
			const u32 alpha = (u32)(FirmwarePulse() * 255.0f);
			if (focus) {
				PPGeImageStyle focusStyle;
				focusStyle.color = CalcFadedColor((alpha << 24) | 0xFFFFFF);
				PPGeDrawFirmwareImage(focus, floorf(centre - imageWidth / 2.0f), buttonBaseline - 15.0f, imageWidth, 18.0f, focusStyle);
			} else {
				PPGeDrawRect(centre - boxWidth / 2.0f, buttonBaseline - 14.0f, centre + boxWidth / 2.0f, buttonBaseline + 2.0f, CalcFadedColor(((alpha / 2) << 24) | 0xFFFFFF));
			}
		}
		PPGeDrawText(label, left, buttonBaseline - ascent, blockStyle);
	};

	if (hasYesNo) {
		const std::string yes = FirmwareText("msgsystem_yes", di->T("Yes"));
		float yesWidth = 0.0f;
		PPGeMeasureText(&yesWidth, nullptr, yes, FW_TEXT_SCALE);
		drawButton(yes, roundf(centerX + FW_YES_RIGHT - yesWidth), yesnoChoice == 1);
		drawButton(FirmwareText("msgsystem_no", di->T("No")), roundf(centerX + FW_NO_LEFT), yesnoChoice == 0);
		if (!interactive) {
			return;
		}
		if (IsButtonPressed(CTRL_LEFT) && yesnoChoice == 0) {
			yesnoChoice = 1;
			pulseFrames_ = 0;
		} else if (IsButtonPressed(CTRL_RIGHT) && yesnoChoice == 1) {
			yesnoChoice = 0;
			pulseFrames_ = 0;
		}
	} else if (hasOK) {
		const std::string ok = FirmwareText("msgsystem_ok", di->T("OK"));
		float okWidth = 0.0f;
		PPGeMeasureText(&okWidth, nullptr, ok, FW_TEXT_SCALE);
		drawButton(ok, roundf(centerX - okWidth / 2.0f), true);
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
	if (PSP_CoreParameter().compat.flags().ForceCircleButtonConfirm) {
		return CTRL_CIRCLE;
	}
	return g_Config.iButtonPreference == PSP_SYSTEMPARAM_BUTTON_CROSS ? CTRL_CROSS : CTRL_CIRCLE;
}

int PSPDialog::GetCancelButton() {
	if (PSP_CoreParameter().compat.flags().ForceCircleButtonConfirm) {
		return CTRL_CROSS;
	}
	return g_Config.iButtonPreference == PSP_SYSTEMPARAM_BUTTON_CROSS ? CTRL_CIRCLE : CTRL_CROSS;
}

void PSPDialog::DisplayMessage2(std::string_view text1, std::string_view text2a, std::string_view text2b, std::string_view text3a, std::string_view text3b, bool hasYesNo, bool hasOK) {
	auto di = GetI18NCategory(I18NCat::DIALOG);

	PPGeStyle buttonStyle = FadedStyle(PPGeAlign::BOX_CENTER, FONT_SCALE);
	PPGeStyle messageStyle = FadedStyle(PPGeAlign::BOX_HCENTER, FONT_SCALE);
	PPGeStyle messageStyleRight = FadedStyle(PPGeAlign::BOX_RIGHT, FONT_SCALE);
	PPGeStyle messageStyleLeft = FadedStyle(PPGeAlign::BOX_LEFT, FONT_SCALE);

	std::string text2 = std::string(text2a) + "  " + std::string(text2b);
	std::string text3 = std::string(text3a) + "  " + std::string(text3b);

	// Without the scrollbar, we have 350 total pixels.
	float WRAP_WIDTH = 300.0f;
	if (UTF8StringNonASCIICount(text1) >= (int)text1.size() / 4) {
		WRAP_WIDTH = 336.0f;
		if (text1.size() > 12) {
			messageStyle.scale = 0.6f;
		}
	}

	float totalHeight1 = 0.0f;
	PPGeMeasureText(nullptr, &totalHeight1, text1, FONT_SCALE, PPGE_LINE_WRAP_WORD, WRAP_WIDTH);
	float totalHeight2 = 0.0f;
	if (text2 != "  ")
		PPGeMeasureText(nullptr, &totalHeight2, text2, FONT_SCALE, PPGE_LINE_USE_ELLIPSIS, WRAP_WIDTH);
	float totalHeight3 = 0.0f;
	if (text3 != "  ")
		PPGeMeasureText(nullptr, &totalHeight3, text3, FONT_SCALE, PPGE_LINE_USE_ELLIPSIS, WRAP_WIDTH);
	float marginTop = 0.0f;
	if (text2 != "  " || text3 != "  ")
		marginTop = 11.0f;
	float totalHeight = totalHeight1 + totalHeight2 + totalHeight3 + marginTop;
	// The PSP normally only shows about 8 lines at a time.
	// For improved UX, we intentionally show part of the next line.
	float visibleHeight = std::min(totalHeight, 175.0f);
	float h2 = visibleHeight / 2.0f;

	float centerY = 135.0f;
	float sy = centerY - h2 - 15.0f;
	float ey = centerY + h2 + 20.0f;
	float buttonY = centerY + h2 + 5.0f;

	auto drawSelectionBoxAndAdjust = [&](float x) {
		// Box has a fixed size.
		float w = 15.0f;
		float h = 8.0f;
		PPGeDrawRect(x - w, buttonY - h, x + w, buttonY + h, CalcFadedColor(0x6DCFCFCF));

		centerY -= h + 5.0f;
		sy -= h + 5.0f;
		ey = buttonY + h * 2.0f + 5.0f;
	};

	if (hasYesNo) {
		if (yesnoChoice == 1) {
			drawSelectionBoxAndAdjust(204.0f);
		} else {
			drawSelectionBoxAndAdjust(273.0f);
		}

		PPGeDrawText(di->T("Yes"), 203.0f, buttonY - 1.0f, buttonStyle);
		PPGeDrawText(di->T("No"), 272.0f, buttonY - 1.0f, buttonStyle);
		if (IsButtonPressed(CTRL_LEFT) && yesnoChoice == 0) {
			yesnoChoice = 1;
		} else if (IsButtonPressed(CTRL_RIGHT) && yesnoChoice == 1) {
			yesnoChoice = 0;
		}
		buttonY += 8.0f + 5.0f;
	}

	if (hasOK) {
		drawSelectionBoxAndAdjust(240.0f);

		PPGeDrawText(di->T("OK"), 239.0f, buttonY - 1.0f, buttonStyle);
		buttonY += 8.0f + 5.0f;
	}

	PPGeScissor(0, (int)(centerY - h2 - 2), 480, (int)(centerY + h2 + 2));
	PPGeDrawTextWrapped(text1, 240.0f, centerY - h2 - scrollPos_, WRAP_WIDTH, 0, messageStyle);
	if (!text2a.empty()) {
		if (!text2b.empty())
			PPGeDrawTextWrapped(text2a, 240.0f - 5.0f, centerY - h2 - scrollPos_ + totalHeight1 + marginTop, WRAP_WIDTH, 0, messageStyleRight);
		else
			PPGeDrawTextWrapped(text2a, 240.0f, centerY - h2 - scrollPos_ + totalHeight1 + marginTop, WRAP_WIDTH, 0, messageStyle);
	}
	if (!text2b.empty())
		PPGeDrawTextWrapped(text2b, 240.0f + 5.0f, centerY - h2 - scrollPos_ + totalHeight1 + marginTop, WRAP_WIDTH, 0, messageStyleLeft);
	if (!text3a.empty()) {
		if (!text3b.empty())
			PPGeDrawTextWrapped(text3a, 240.0f - 5.0f, centerY - h2 - scrollPos_ + totalHeight1 + totalHeight2 + marginTop, WRAP_WIDTH, 0, messageStyleRight);
		else
			PPGeDrawTextWrapped(text3a, 240.0f, centerY - h2 - scrollPos_ + totalHeight1 + totalHeight2 + marginTop, WRAP_WIDTH, 0, messageStyle);
	}
	if (!text3b.empty())
		PPGeDrawTextWrapped(text3b, 240.0f + 5.0f, centerY - h2 - scrollPos_ + totalHeight1 + totalHeight2 + marginTop, WRAP_WIDTH, 0, messageStyleLeft);
	PPGeScissorReset();

	// Do we need a scrollbar?
	if (visibleHeight < totalHeight) {
		float scrollSpeed = 5.0f;
		float scrollMax = totalHeight - visibleHeight;

		float bobHeight = (visibleHeight / totalHeight) * visibleHeight;
		float bobOffset = (scrollPos_ / scrollMax) * (visibleHeight - bobHeight);
		float bobY1 = centerY - h2 + bobOffset;
		PPGeDrawRect(415.0f, bobY1, 420.0f, bobY1 + bobHeight, CalcFadedColor(0xFFCCCCCC));

		auto buttonDown = [this](int btn, int& held) {
			if (IsButtonPressed(btn)) {
				held = 0;
				return true;
			}
			return IsButtonHeld(btn, held, 1, 1);
		};
		if (buttonDown(CTRL_DOWN, framesDownHeld_) && scrollPos_ < scrollMax) {
			scrollPos_ = std::min(scrollMax, scrollPos_ + scrollSpeed);
		}
		if (buttonDown(CTRL_UP, framesUpHeld_) && scrollPos_ > 0.0f) {
			scrollPos_ = std::max(0.0f, scrollPos_ - scrollSpeed);
		}
	}

	PPGeDrawRect(60.0f, sy, 420.0f, sy + 1.0f, CalcFadedColor(0xFFFFFFFF));
	PPGeDrawRect(60.0f, ey, 420.0f, ey + 1.0f, CalcFadedColor(0xFFFFFFFF));
}
