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

#include <cstdint>
#include <string>

// The PSP firmware's look for the system dialogs games open (Core/Dialog), from an installed
// firmware dump (g_Config.nandRootDirectory). Everything is read from the dump at runtime; without
// one the dialogs keep PPSSPP's own look.

// Whether the firmware has what the firmware style needs (its fonts).
bool FirmwareDialogStyleAvailable();
// Whether to draw the message / savedata dialogs in the firmware's style: chosen in the settings
// and available.
bool UseFirmwareMsgDialogStyle();
bool UseFirmwareSaveDialogStyle();

// The message the firmware's msgdialog shows for an error code, from msgdialog_plugin.rco, in the
// dialog's language (PSP_SYSTEMPARAM_LANGUAGE_*). Empty if the code isn't one it knows.
std::string FirmwareErrorMessage(uint32_t errorCode, int language);

// A string of the firmware's common dialog parts (system_plugin.rco: msgsystem_yes, msgsystem_no,
// msgsystem_enter, msgsystem_back, msgsystem_ok...) in a PSP_SYSTEMPARAM_LANGUAGE_* language.
// Empty if missing.
std::string FirmwareSystemText(const char *label, int language);

// A string of the firmware's savedata dialog (savedata_utility.rco: msgsavedata_cmn_save_ask,
// msgshare_wait...) in a PSP_SYSTEMPARAM_LANGUAGE_* language. Empty if missing.
std::string FirmwareSavedataText(const char *label, int language);

// Drops what was loaded, for when the firmware changes.
void FirmwareDialogStyleReset();
