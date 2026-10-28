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

#include <mutex>

#include "Common/CommonTypes.h"
#include "Common/File/FileUtil.h"
#include "Common/Log.h"
#include "Core/Config.h"
#include "Core/ConfigValues.h"
#include "Core/Dialog/FirmwareDialogStyle.h"
#include "Core/Util/RCOFile.h"

// What the firmware's msgdialog shows for an error code. In 6.61's msgdialog_plugin.prx the
// error dialog looks the code up in tables of codes and of string labels (into
// msgdialog_plugin.rco), one pair of tables per family of codes, with a fallback label for codes
// of that family it doesn't list. The network (0x8041xxxx, 0x8043xxxx, 0x8055xxxx) and PSN
// (0x801112xx) codes are compared one by one in its code instead, and aren't covered here.
//
// The "em" variants it has for the PSP Go's internal storage aren't used: PPSSPP's saves are on
// the Memory Stick.

namespace {

struct ErrorLabel {
	uint32_t code;
	const char *label;
};

// Savedata (0x801103xx): the tables at 0x1E8C (codes) and 0x21E0 (labels) of the module.
const ErrorLabel savedataErrors[] = {
	{ 0x80110301, "msgshare_error_no_ms" },
	{ 0x80110306, "msgshare_sd_error_load_data_corrupted" },
	{ 0x80110305, "msgerror_load_ms_no_access" },
	{ 0x80110302, "msgerror_sd_error_load_ms_removed" },
	{ 0x80110307, "msgshare_sd_error_load_no_data" },
	{ 0x80110309, "msgshare_sd_error_load_data_corrupted" },
	{ 0x80110383, "msgerror_mu_error_save_ms_full" },
	{ 0x80110381, "msgshare_error_no_ms" },
	{ 0x80110385, "msgerror_sd_error_save_ms_no_access" },
	{ 0x80110382, "msgerror_sd_error_save_ms_removed" },
	{ 0x80110384, "msgshare_sd_error_save_ms_locked" },
	{ 0x80110386, "msgshare_sd_error_save_data_corrupted" },
	{ 0x80110341, "msgshare_error_no_ms" },
	{ 0x80110342, "msgerror_error_del_ms_removed" },
	{ 0x80110344, "msgshare_error_del_ms_locked" },
	{ 0x80110345, "msgerror_error_del_ms_no_access" },
	{ 0x80110347, "msgshare_error_del_nodata" },
	{ 0x80110321, "msgshare_error_no_ms" },
	{ 0x80110322, "msgshare_error_ms_removed" },
	{ 0x80110323, "msgshare_error_ms_full" },
	{ 0x80110324, "msgshare_error_ms_locked" },
	{ 0x80110325, "msgshare_error_notplay_ms" },
	{ 0x80110326, "msgshare_error_content_corrupted" },
	{ 0x80110327, "msgshare_error_no_data" },
	{ 0x80110329, "msgshare_error_content_corrupted" },
	{ 0x8011032A, "msgerror_sleep_processcancelled" },
};

// Game data install (0x80110Dxx).
const ErrorLabel installErrors[] = {
	{ 0x80110D00, "msgshare_error_no_ms" },
	{ 0x80110D01, "msggame_install_error_ms_removed" },
	{ 0x80110D02, "msggame_install_error_ms_full_dialog" },
	{ 0x80110D03, "msggame_install_error_ms_locked" },
	{ 0x80110D04, "msggame_install_error_ms_no_access" },
	{ 0x80110D05, "msggame_install_error_corrupted" },
	{ 0x80110D06, "msggame_install_error_no_data_dialog" },
	{ 0x80110D07, "msggame_install_error_unsupported_data_dialog" },
	{ 0x80110D08, "msggame_install_same_installed_dialog" },
	{ 0x80110D09, "msggame_install_error_lowbattery_dialog" },
	{ 0x80110D0A, "msggame_install_error_systemversion_dialog" },
	{ 0x80110D0B, "msggame_install_error_wrongdata" },
};

// Screenshot / photo saving (0x801119xx).
const ErrorLabel screenshotErrors[] = {
	{ 0x80111900, "msgshare_error_no_ms" },
	{ 0x80111901, "msggame_install_error_ms_removed" },
	{ 0x80111902, "msggame_install_error_ms_full_dialog" },
	{ 0x80111903, "msggame_install_error_ms_locked" },
	{ 0x80111904, "msggame_install_error_ms_no_access" },
	{ 0x80111905, "msg_error_no_umd" },
	{ 0x80111906, "msg_error_install_no_umd" },
	{ 0x80111907, "msggame_install_error_lowbattery_dialog" },
};

const char *LookUp(const ErrorLabel *table, size_t count, uint32_t code) {
	for (size_t i = 0; i < count; i++) {
		if (table[i].code == code) {
			return table[i].label;
		}
	}
	return nullptr;
}

const char *ErrorLabelFor(uint32_t code) {
	switch (code & 0xFFFFFF00) {
	case 0x80110300:
		if (const char *label = LookUp(savedataErrors, ARRAY_SIZE(savedataErrors), code)) {
			return label;
		}
		// The module's fallbacks for the rest of the family, by range (each 0x1F codes long).
		if (code == 0x8011032C) {
			return nullptr;
		}
		if (code - 0x80110300 < 0x1F) {
			return "msgshare_error_load";
		}
		if (code - 0x80110380 < 0x1F) {
			return "msgshare_error_save";
		}
		if (code - 0x80110340 < 0x1F) {
			return "msgshare_error_del";
		}
		if (code - 0x80110320 < 0x1F) {
			return "msgshare_error_internal";
		}
		return nullptr;
	case 0x80110D00:
		if (const char *label = LookUp(installErrors, ARRAY_SIZE(installErrors), code)) {
			return label;
		}
		return "msgshare_error_internal";
	case 0x80111900:
		if (const char *label = LookUp(screenshotErrors, ARRAY_SIZE(screenshotErrors), code)) {
			return label;
		}
		return "msgshare_error_internal";
	default:
		return nullptr;
	}
}

std::mutex lock;
bool rcoTried = false;
RCOFile msgdialogRCO;
bool systemRcoTried = false;
RCOFile systemRCO;

void LoadRCO(const char *name, bool *tried, RCOFile *rco) {
	if (*tried) {
		return;
	}
	*tried = true;
	std::string data;
	const Path path = g_Config.nandRootDirectory / "flash0/vsh/resource" / name;
	if (File::ReadBinaryFileToString(path, &data)) {
		if (!rco->Load(std::vector<uint8_t>(data.begin(), data.end()))) {
			WARN_LOG(Log::sceUtility, "Can't read %s", path.c_str());
		}
	}
}

}  // namespace

bool FirmwareDialogStyleAvailable() {
	const Path fonts = g_Config.nandRootDirectory / "flash0/font";
	return File::Exists(fonts / "ltn0.pgf") || File::Exists(fonts / "jpn0.pgf");
}

bool UseFirmwareMsgDialogStyle() {
	return g_Config.iMsgDialogStyle == (int)DialogStyle::FIRMWARE && FirmwareDialogStyleAvailable();
}

std::string FirmwareErrorMessage(uint32_t errorCode, int language) {
	const char *label = ErrorLabelFor(errorCode);
	if (!label) {
		return std::string();
	}

	std::lock_guard<std::mutex> guard(lock);
	LoadRCO("msgdialog_plugin.rco", &rcoTried, &msgdialogRCO);
	if (!msgdialogRCO.IsLoaded()) {
		return std::string();
	}
	// The PSP's system languages and the RCO's are numbered alike.
	return msgdialogRCO.Text(language, label);
}

std::string FirmwareSystemText(const char *label, int language) {
	std::lock_guard<std::mutex> guard(lock);
	LoadRCO("system_plugin.rco", &systemRcoTried, &systemRCO);
	if (!systemRCO.IsLoaded()) {
		return std::string();
	}
	return systemRCO.Text(language, label);
}

void FirmwareDialogStyleReset() {
	std::lock_guard<std::mutex> guard(lock);
	rcoTried = false;
	msgdialogRCO = RCOFile();
	systemRcoTried = false;
	systemRCO = RCOFile();
}
