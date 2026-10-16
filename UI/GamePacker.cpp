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
#include <atomic>
#include <memory>
#include <thread>

#include "Common/Data/Color/RGBAUtil.h"
#include "Common/Data/Text/I18n.h"
#include "Common/Data/Text/Parsers.h"
#include "Common/File/FileUtil.h"
#include "Common/Log.h"
#include "Common/StringUtils.h"
#include "Common/System/OSD.h"
#include "Common/Thread/ThreadUtil.h"
#include "Common/TimeUtil.h"
#include "Common/UI/Context.h"
#include "Common/UI/ScrollView.h"

#include "Core/Config.h"
#include "Core/System.h"
#include "Core/Util/RecentFiles.h"

#include "UI/BackgroundAudio.h"
#include "UI/GameInfoCache.h"
#include "UI/GamePacker.h"
#include "UI/UnPackScreen.h"

namespace GamePacker {
namespace {

struct Job {
	Path gamePath;     // As the game list knows it, maybe a PSP/GAME/<folder>.
	Path file;         // The image itself.
	Path finalOutput;  // Where the converted game ends up.
	Path writeOutput;  // What gets written: finalOutput, or a temporary name when that's the original's.
	bool replace = false;
	bool toISO = false;
	ImageFormat format = ImageFormat::CSO;
	int preset = 1;

	std::atomic<float> progress{0.0f};
	double startTime = 0.0;
	std::atomic<bool> cancel{false};
	std::atomic<bool> done{false};
	bool success = false;    // Set before done.
	bool cancelled = false;  // Set before done.
	std::string error;       // Set before done.

	std::thread thread;
};

// Only touched on the UI thread. The worker gets the raw pointer, and the job lives until it's joined.
std::unique_ptr<Job> g_job;
int g_generation = 0;
Result g_lastResult;

void RunJob(Job *job) {
	SetCurrentThreadName("GamePacker");
	INFO_LOG(Log::Loader, "GamePacker: %s %s -> %s", job->toISO ? "Extracting" : "Compressing", job->file.c_str(), job->writeOutput.c_str());

	auto progress = [job](float p) {
		job->progress = p;
		return !job->cancel;
	};
	std::string error;
	bool cancelled = false;
	bool success;
	if (job->toISO) {
		success = ExtractDiscImage(job->file, job->writeOutput, progress, &error, &cancelled);
	} else {
		success = CompressDiscImage(job->format, job->preset, job->file, job->writeOutput, progress, &error, &cancelled);
	}
	if (!success && !cancelled) {
		ERROR_LOG(Log::Loader, "GamePacker: Failed on %s: %s", job->file.c_str(), error.c_str());
	}
	job->success = success;
	job->cancelled = cancelled;
	job->error = error;
	job->done = true;
}

// "<name>" for "<name>.<ext>", or for an EBOOT.PBP the game's disc id.
std::string OutputBaseName(const Path &gamePath, const Path &file) {
	std::string name = file.GetFilename();
	const std::string ext = file.GetFileExtension();
	if (ext.size() <= name.size()) {
		name.resize(name.size() - ext.size());
	}
	if (equalsNoCase(file.GetFilename(), "EBOOT.PBP")) {
		std::shared_ptr<GameInfo> info = g_gameInfoCache->GetInfo(nullptr, gamePath, GameInfoFlags::PARAM_SFO);
		if (info->Ready(GameInfoFlags::PARAM_SFO)) {
			const std::string id = SanitizeString(info->id, StringRestriction::FileName);
			if (!id.empty()) {
				return id;
			}
		}
	}
	return name;
}

Container ContainerOf(ImageFormat format) {
	switch (format) {
	case ImageFormat::CHD: return Container::CHD;
	case ImageFormat::PBP: return Container::PBP;
	default: return Container::CSO;
	}
}

// Where an option puts its result.
Path FinalOutput(const Path &gamePath, const Path &file, Container from, const Option &option) {
	const Container to = option.toISO ? Container::ISO : ContainerOf(option.format);
	if (to == from) {
		return file;
	}
	return file.NavigateUp() / (OutputBaseName(gamePath, file) + (option.toISO ? ".iso" : ImageFormatExtension(option.format)));
}

}  // namespace

Container DetectContainer(const Path &gamePath, Path *file) {
	// Content URIs are fine too (Android's storage), only remote games aren't.
	if (gamePath.Type() == PathType::HTTP) {
		return Container::None;
	}
	Path image = gamePath;
	if (File::IsDirectory(gamePath)) {
		image = gamePath / "EBOOT.PBP";
		if (!File::Exists(image)) {
			return Container::None;
		}
	}
	const std::string ext = image.GetFileExtension();
	Container container = Container::None;
	if (ext == ".iso") {
		container = Container::ISO;
	} else if (ext == ".cso") {
		container = Container::CSO;
	} else if (ext == ".chd") {
		container = Container::CHD;
	} else if (ext == ".pbp") {
		container = Container::PBP;
	}
	if (container != Container::None && file) {
		*file = image;
	}
	return container;
}

std::vector<Option> ListOptions(Container from) {
	auto ga = GetI18NCategory(I18NCat::GAME);

	std::vector<Option> options;
	if (from == Container::None) {
		return options;
	}
	if (from != Container::ISO) {
		Option option;
		option.toISO = true;
		option.title = std::string(ga->T("Extract to ISO"));
		option.description = std::string(ga->T("PackDescISO", "Uncompressed disc image. Largest, reads fastest."));
		options.push_back(option);
	}
	auto add = [&](ImageFormat format, int preset, std::string_view name, std::string_view description) {
		Option option;
		option.format = format;
		option.preset = preset;
		static const char *const formatNames[] = { "CHD", "CSO", "PBP" };  // Indexed by ImageFormat.
		option.title = std::string(formatNames[(int)format]) + " - " + std::string(name);
		option.description = description;
		options.push_back(option);
	};
	// Indexed like iCHDPreset, iCSOPreset and iPBPPreset, in the order the Un/Pack screen lists them.
	add(ImageFormat::CHD, 0, ga->T("Large Hunk"), ga->T("PackDescCHDLarge", "32K hunks. Smallest for most games."));
	add(ImageFormat::CHD, 1, ga->T("Small Hunk"), ga->T("PackDescCHDSmall", "2K hunks. Smallest when a game repeats a lot of data."));
	add(ImageFormat::CHD, 2, ga->T("Quick Access"), ga->T("PackDescCHDQuick", "Zstandard, 2K hunks. Bigger, but fastest to read while playing."));
	add(ImageFormat::CSO, 0, ga->T("Max"), ga->T("PackDescCSOMax", "Zopfli, 256K blocks. Smallest CSO, slow to compress."));
	add(ImageFormat::CSO, 1, ga->T("Great"), ga->T("PackDescCSOGreat", "libdeflate, 64K blocks. Nearly as small, much faster."));
	add(ImageFormat::CSO, 2, ga->T("PSP Compatible"), ga->T("PackDescCSOPSP", "zlib, 2K blocks. Readable by PSP custom firmware."));
	add(ImageFormat::PBP, 0, ga->T("Max"), ga->T("PackDescPBPMax", "Smallest LZRC EBOOT."));
	add(ImageFormat::PBP, 1, ga->T("PSP Compatible"), ga->T("PackDescPBPPSP", "LZRC like sign_np. What real PSPs are known to read."));
	add(ImageFormat::PBP, 2, ga->T("Uncompressed"), ga->T("PackDescPBPNone", "Blocks stored as they are. Largest, fastest."));
	return options;
}

bool MustReplace(const Path &gamePath, const Option &option) {
	Path file;
	const Container from = DetectContainer(gamePath, &file);
	return from != Container::None && FinalOutput(gamePath, file, from, option) == file;
}

bool CanStart() {
	return !IsBusy() && !IsUnPackJobRunning() && !PSP_IsInited();
}

bool Start(const Path &gamePath, const Option &option, bool replaceOriginal) {
	auto ga = GetI18NCategory(I18NCat::GAME);
	if (!CanStart()) {
		return false;
	}

	auto job = std::make_unique<Job>();
	job->gamePath = gamePath;
	const Container from = DetectContainer(gamePath, &job->file);
	if (from == Container::None || (option.toISO && from == Container::ISO)) {
		return false;
	}

	job->toISO = option.toISO;
	job->format = option.format;
	job->preset = option.preset;
	job->finalOutput = FinalOutput(gamePath, job->file, from, option);
	job->replace = replaceOriginal || job->finalOutput == job->file;
	if (job->finalOutput == job->file) {
		const std::string ext = job->file.GetFileExtension();
		std::string name = job->file.GetFilename();
		name.resize(name.size() - ext.size());
		job->writeOutput = job->file.NavigateUp() / (name + ".ppsspp-tmp" + ext);
		// Our own leftover, from a crash or a kill. The compressor never overwrites.
		if (File::Exists(job->writeOutput)) {
			File::Delete(job->writeOutput);
		}
	} else {
		job->writeOutput = job->finalOutput;
		if (File::Exists(job->finalOutput)) {
			g_OSD.Show(OSDType::MESSAGE_ERROR, std::string(ga->T("PackOutputExists", "Output file already exists")) + ": " + job->finalOutput.GetFilename(), 5.0f);
			return false;
		}
	}

	INFO_LOG(Log::Loader, "GamePacker: %s -> %s%s", job->file.c_str(), job->finalOutput.c_str(), job->replace ? " (replacing it)" : "");
	Job *raw = job.get();
	g_job = std::move(job);
	g_job->startTime = time_now_d();
	g_job->thread = std::thread(RunJob, raw);
	return true;
}

bool IsBusy() {
	return g_job != nullptr;
}

bool IsPacking(const Path &gamePath) {
	return g_job && g_job->gamePath == gamePath;
}

void Abort() {
	if (g_job) {
		g_job->cancel = true;
	}
}

bool GetStatus(Status *status) {
	if (!g_job) {
		return false;
	}
	auto ga = GetI18NCategory(I18NCat::GAME);
	const Job &job = *g_job;

	status->gamePath = job.gamePath;
	status->label = std::string(job.toISO ? ga->T("Extracting") : ga->T("Packing")) + " " + job.file.GetFilename();
	status->progress = job.done ? 1.0f : job.progress.load();
	status->aborting = job.cancel;

	const double elapsed = time_now_d() - job.startTime;
	status->etaSeconds = -1.0;
	// The first moments say little.
	if (status->progress > 0.01f && elapsed > 2.0) {
		status->etaSeconds = elapsed * (1.0 - status->progress) / status->progress;
	}
	return true;
}

int Generation() {
	return g_generation;
}

Result LastResult() {
	return g_lastResult;
}

void UpdateOnUIThread() {
	if (!g_job || !g_job->done) {
		return;
	}
	g_job->thread.join();
	std::unique_ptr<Job> job = std::move(g_job);

	auto ga = GetI18NCategory(I18NCat::GAME);
	auto sy = GetI18NCategory(I18NCat::SYSTEM);

	Result result;
	result.oldPath = job->gamePath;
	result.newPath = job->gamePath;
	result.success = job->success;

	if (job->success) {
		const uint64_t before = File::GetFileSize(job->file);
		const uint64_t after = File::GetFileSize(job->writeOutput);
		std::string problem;

		if (job->replace) {
			// Let go of everything that might hold the original open, or Windows won't delete it.
			g_BackgroundAudio.SetGame(Path());
			g_gameInfoCache->Clear();

			if (File::Delete(job->file)) {
				if (job->writeOutput != job->finalOutput && !File::Rename(job->writeOutput, job->finalOutput)) {
					// The original is gone, so keep the converted game under its temporary name.
					problem = std::string(ga->T("PackRenameFailed", "Could not rename the new file"));
					result.newPath = job->writeOutput;
				} else if (job->finalOutput == job->file && job->file != job->gamePath) {
					// Repacked a PSP/GAME/<folder> EBOOT: the folder is still the game.
					result.newPath = job->gamePath;
				} else {
					result.newPath = job->finalOutput;
				}
			} else if (job->writeOutput != job->finalOutput) {
				// Same name as the original, which is still there.
				File::Delete(job->writeOutput);
				problem = std::string(ga->T("PackReplaceFailed", "Could not replace the original"));
				result.success = false;
			} else {
				problem = std::string(ga->T("PackDeleteFailed", "Could not delete the original"));
				result.newPath = job->finalOutput;
			}

			if (result.newPath != result.oldPath && g_recentFiles.ContainsFile(result.oldPath.ToString())) {
				g_recentFiles.Remove(result.oldPath.ToString());
				g_recentFiles.Add(result.newPath.ToString());
			}
		}

		if (result.success) {
			std::string text = std::string(sy->T("Done")) + ": " + job->finalOutput.GetFilename() + "\n" + NiceSizeFormat(before) + " -> " + NiceSizeFormat(after);
			if (before > 0) {
				text += StringFromFormat(" (%.2f%%)", 100.0 * (double)after / (double)before);
			}
			g_OSD.Show(problem.empty() ? OSDType::MESSAGE_SUCCESS : OSDType::MESSAGE_WARNING, text, problem, 6.0f);
		} else {
			g_OSD.Show(OSDType::MESSAGE_ERROR, problem, 6.0f);
		}
	} else if (job->cancelled) {
		g_OSD.Show(OSDType::MESSAGE_INFO, ga->T("Packing aborted"), 3.0f);
	} else {
		g_OSD.Show(OSDType::MESSAGE_ERROR, std::string(sy->T("Failed")) + ": " + job->file.GetFilename(), job->error, 8.0f);
	}

	g_lastResult = result;
	g_generation++;
}

void Shutdown() {
	if (!g_job) {
		return;
	}
	g_job->cancel = true;
	if (g_job->thread.joinable()) {
		g_job->thread.join();
	}
	g_job.reset();
}

}  // namespace GamePacker

namespace {

std::string_view PopupTitle(GamePacker::Container from) {
	auto ga = GetI18NCategory(I18NCat::GAME);
	return from == GamePacker::Container::ISO ? ga->T("Pack") : ga->T("Extract / Repack");
}

std::string_view PopupButton(GamePacker::Container from) {
	auto ga = GetI18NCategory(I18NCat::GAME);
	auto di = GetI18NCategory(I18NCat::DIALOG);
	return from == GamePacker::Container::ISO ? ga->T("Pack") : di->T("Start");
}

// A preset in the list: a radio mark, the name, and a line about it.
class PackOptionView : public UI::ClickableItem {
public:
	PackOptionView(std::string_view title, std::string_view description, UI::LayoutParams *layoutParams = nullptr)
		: UI::ClickableItem(layoutParams), title_(title), description_(description) {}

	void GetContentDimensions(const UIContext &dc, float &w, float &h) const override {
		w = 300.0f;
		h = 70.0f;
	}

	void Draw(UIContext &dc) override {
		UI::Style style = dc.GetTheme().itemStyle;
		if (HasFocus()) style = dc.GetTheme().itemFocusedStyle;
		if (down_) style = dc.GetTheme().itemDownStyle;
		if (!IsEnabled()) style = dc.GetTheme().itemDisabledStyle;
		DrawBG(dc, style);

		const float iconX = bounds_.x + 30.0f;
		dc.Draw()->DrawImage(ImageID(checked_ ? "I_RADIO_SELECTED" : "I_RADIO_EMPTY"), iconX, bounds_.centerY(), 1.0f, style.fgColor, ALIGN_CENTER);

		const float textX = bounds_.x + 60.0f;
		const float textW = bounds_.w - 70.0f;
		dc.SetFontStyle(dc.GetTheme().uiFont);
		dc.DrawTextRectSqueeze(title_, Bounds(textX, bounds_.y + 6.0f, textW, 32.0f), style.fgColor, ALIGN_VCENTER);
		dc.SetFontScale(0.7f, 0.7f);
		dc.DrawTextRectSqueeze(description_, Bounds(textX, bounds_.y + 36.0f, textW, 28.0f), alphaMul(style.fgColor, 0.75f), ALIGN_VCENTER);
		dc.SetFontScale(1.0f, 1.0f);
	}

	std::string DescribeText() const override {
		return title_ + ", " + description_;
	}

	void SetChecked(bool checked) { checked_ = checked; }

private:
	std::string title_;
	std::string description_;
	bool checked_ = false;
};

}  // namespace

GamePackPopupScreen::GamePackPopupScreen(const Path &gamePath, GamePacker::Container from)
	: UI::PopupScreen(PopupTitle(from), PopupButton(from), GetI18NCategory(I18NCat::DIALOG)->T("Cancel")), gamePath_(gamePath) {
	options_ = GamePacker::ListOptions(from);

	// Start from what was used last, here or on the Un/Pack screen. Extracting is listed first.
	if (from == GamePacker::Container::ISO) {
		const int formatPresets[] = { g_Config.iCHDPreset, g_Config.iCSOPreset, g_Config.iPBPPreset };  // Indexed by ImageFormat.
		const int format = std::clamp(g_Config.iUnPackFormat, 0, 2);
		for (size_t i = 0; i < options_.size(); i++) {
			if (!options_[i].toISO && (int)options_[i].format == format && options_[i].preset == formatPresets[format]) {
				selected_ = (int)i;
				break;
			}
		}
	}
	replaceWanted_ = g_Config.bPackReplaceOriginal;
	replace_ = replaceWanted_;
}

void GamePackPopupScreen::CreatePopupContents(UI::ViewGroup *parent) {
	using namespace UI;

	auto ga = GetI18NCategory(I18NCat::GAME);

	choices_.clear();
	ScrollView *scroll = parent->Add(new ScrollView(ORIENT_VERTICAL, new LinearLayoutParams(FILL_PARENT, FillVertical() ? 0.0f : WRAP_CONTENT, 1.0f)));
	LinearLayout *list = scroll->Add(new LinearLayout(ORIENT_VERTICAL, new LayoutParams(FILL_PARENT, WRAP_CONTENT)));
	list->SetSpacing(0.0f);
	for (size_t i = 0; i < options_.size(); i++) {
		PackOptionView *view = list->Add(new PackOptionView(options_[i].title, options_[i].description));
		view->OnClick.Add([this, i](UI::EventParams &) {
			Select((int)i);
		});
		choices_.push_back(view);
	}

	replaceCheckBox_ = parent->Add(new CheckBox(&replace_, ga->T("Replace original when complete")));
	replaceCheckBox_->OnClick.Add([this](UI::EventParams &) {
		replaceWanted_ = replace_;
	});

	if (!options_.empty()) {
		Select(std::clamp(selected_, 0, (int)options_.size() - 1));
	}
}

void GamePackPopupScreen::Select(int index) {
	selected_ = index;
	for (size_t i = 0; i < choices_.size(); i++) {
		static_cast<PackOptionView *>(choices_[i])->SetChecked((int)i == index);
	}
	// Repacking into the same format writes the game's own file name, so the original has to go.
	const bool forced = GamePacker::MustReplace(gamePath_, options_[index]);
	replace_ = forced || replaceWanted_;
	if (replaceCheckBox_) {
		replaceCheckBox_->SetEnabled(!forced);
	}
}

void GamePackPopupScreen::OnCompleted(DialogResult result) {
	if (result != DR_OK || selected_ < 0 || selected_ >= (int)options_.size()) {
		return;
	}
	const GamePacker::Option &option = options_[selected_];
	if (!option.toISO) {
		g_Config.iUnPackFormat = (int)option.format;
		switch (option.format) {
		case ImageFormat::CHD: g_Config.iCHDPreset = option.preset; break;
		case ImageFormat::CSO: g_Config.iCSOPreset = option.preset; break;
		case ImageFormat::PBP: g_Config.iPBPPreset = option.preset; break;
		}
	}
	if (!GamePacker::MustReplace(gamePath_, option)) {
		g_Config.bPackReplaceOriginal = replace_;
	}
	GamePacker::Start(gamePath_, option, replace_);
}
