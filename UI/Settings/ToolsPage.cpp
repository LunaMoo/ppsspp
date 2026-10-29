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

#include "Common/Data/Text/I18n.h"
#include "Common/System/System.h"
#include "Common/UI/PopupScreens.h"
#include "Common/UI/ScreenManager.h"
#include "Common/UI/View.h"
#include "Core/Config.h"
#include "UI/DeveloperSettingsPages.h"
#include "UI/RemoteISOScreen.h"
#include "UI/RetroAchievementScreens.h"
#include "UI/SavedataScreen.h"
#include "UI/SettingsPage.h"
#include "UI/Settings/MainSettingsPages.h"
#include "UI/SystemInfoScreen.h"
#if defined(_WIN32) && !PPSSPP_PLATFORM(UWP)
#include "Common/CommonWindows.h"
#include "Windows/MainWindow.h"
#endif

// Settings that should not be changed by most users.
static void BuildOtherSettingsPage(SettingsPage *page) {
	page->Header({ I18NCat::SYSTEM, "Settings that should not be changed by most users" });

	page->Setting(&g_Config.bUnlockCachedScaling).OnChange([](UI::EventParams &) {
		System_PostUIMessage(UIMessage::GPU_CONFIG_CHANGED);
	}).Hint({ I18NCat::GRAPHICS, "UnlockCachedScaling Tip", "Only for testing and in some light cases on powerful desktops, this WILL cause stutter even at max FPS! Hardware (Vulkan) texture scaling is strongly recommended instead." });

	page->Setting(&g_Config.bSavestateScreenshotResLimit);

#if defined(_WIN32) && !PPSSPP_PLATFORM(UWP)
	SettingsPageHost *host = page->Host();
	page->Header({ I18NCat::SYSTEM, "Window" });
	page->Setting(&g_Config.bShowMenuBar).OnChange([](UI::EventParams &) {
		MainWindow::SendApplyWindowDecorations();
	});
	page->Setting(&g_Config.bWindowBorderless).OnChange([host](UI::EventParams &) {
		MainWindow::SendApplyWindowDecorations();
		host->RecreatePageViews();
	});
	if (g_Config.bWindowBorderless) {
		const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
		const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
		auto applyWindow = [](UI::EventParams &) {
			MainWindow::SendApplyWindowDecorations();
		};
		page->IntSlider(&g_Config.iWindowX, 0, screenWidth, { I18NCat::SYSTEM, "Window X position" }).Default(UI::NO_DEFAULT_INT).Format("%d px").OnChange(applyWindow);
		page->IntSlider(&g_Config.iWindowY, 0, screenHeight, { I18NCat::SYSTEM, "Window Y position" }).Default(UI::NO_DEFAULT_INT).Format("%d px").OnChange(applyWindow);
		page->IntSlider(&g_Config.iWindowWidth, 64, screenWidth, { I18NCat::SYSTEM, "Window width" }).Default(UI::NO_DEFAULT_INT).Format("%d px").OnChange(applyWindow);
		page->IntSlider(&g_Config.iWindowHeight, 64, screenHeight, { I18NCat::SYSTEM, "Window height" }).Default(UI::NO_DEFAULT_INT).Format("%d px").OnChange(applyWindow);
	}
#endif
}

void BuildToolsSettings(SettingsPage *page) {
	using namespace UI;
	SettingsPageHost *host = page->Host();

	page->Header("Tools");

	const bool showRetroAchievements = System_GetPropertyInt(SYSPROP_DEVICE_TYPE) != DEVICE_TYPE_VR;
	if (showRetroAchievements) {
		page->Choice({ I18NCat::SYSTEM, "RetroAchievements" }).OpensScreen().OnClick([host](UI::EventParams &) {
			host->PageScreenManager()->push(new RetroAchievementsSettingsScreen(host->PageGamePath()));
		});
	}

	// These were moved here so use the wrong translation objects, to avoid having to change all inis... This isn't a sustainable situation :P
	page->Choice({ I18NCat::SAVEDATA, "Savedata Manager" }).OpensScreen().OnClick([host](UI::EventParams &) {
		host->PageScreenManager()->push(new SavedataScreen(host->PageGamePath()));
	});
	page->Choice({ I18NCat::DEVELOPER, "System Information" }).OpensScreen().OnClick([host](UI::EventParams &) {
		host->PageScreenManager()->push(new SystemInfoScreen(host->PageGamePath()));
	});
	page->Link({ I18NCat::SYSTEM, "Developer Tools" }, I18NCat::DEVELOPER, &BuildDeveloperToolsPage);
	page->Link({ I18NCat::SYSTEM, "Other Settings" }, I18NCat::SYSTEM, &BuildOtherSettingsPage);
	page->Choice({ I18NCat::REMOTEISO, "Remote disc streaming" }).OpensScreen().OnClick([host](UI::EventParams &) {
		host->PageScreenManager()->push(new RemoteISOScreen(host->PageGamePath()));
	});
}
