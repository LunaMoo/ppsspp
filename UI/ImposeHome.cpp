// Copyright (c) 2026- PPSSPP Project.

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
#include <cstring>
#include <string>
#include <vector>

#include "Common/Data/Text/I18n.h"
#include "Common/File/FileUtil.h"
#include "Common/GPU/thin3d.h"
#include "Common/Input/InputState.h"
#include "Common/Log.h"
#include "Common/Render/DrawBuffer.h"
#include "Common/System/System.h"
#include "Common/TimeUtil.h"
#include "Common/UI/Context.h"
#include "Common/UI/View.h"
#include "Core/Config.h"
#include "Core/ConfigValues.h"
#include "Core/Core.h"
#include "Core/System.h"
#include "Core/HLE/PopsBoot.h"
#include "Core/HLE/sceCtrl.h"
#include "Core/HLE/sceKernelModule.h"
#include "Core/HLE/sceKernelThread.h"
#include "Core/HLE/sceRtc.h"
#include "Core/HLE/sceUtility.h"
#include "UI/ImposeHome.h"

static Path ImposeRSCPath() {
	return g_Config.nandRootDirectory / "flash0/kd/resource/impose.rsc";
}

// The artwork of the PSP's own HOME screen ("Do you want to quit the game?"), from the installed
// firmware's flash0:/kd/resource/impose.rsc. impose.prx has no font, so every string is a
// prerendered bitmap, one set per system language, each with its place on the 480x272 screen.
//
// impose.rsc, as of 6.61 (all little-endian u32 unless noted):
//   0x00  palette count (3), language block count (12), shared image count (21)
//   0x0C  offsets from the start of the file: the palettes, then the language blocks (in PSP
//         system language order), then the shared images
//   palette: 16 u16 colors, ABGR1555. 0 is normal, 1 is the highlighted button, 2 a dimmer
//         highlight.
//   language block: image count (10), the circle button icon's x, y, the cross button
//         icon's x, y, padding to 0x44, then per image: palette, byte size, x, y, width, height,
//         offset of its 4bpp pixels (low nibble first) from 0x44 into the block. The images are
//         the question, "Enter", "Back", "Please wait...", then Yes and No, each in palette 0, 1
//         and 2.
//   shared image: byte size, then ABGR1555 pixels, no dimensions (impose.prx knows them).
//         The ones used here: 0 the battery (28x17, four levels stacked, full first), 2 and 3 the
//         circle and cross button icons (16x16), 8 the line above and below the question (328x9),
//         16 a volume step, set and unset (8x24 each), 18 the speaker (32x28, then muted and
//         dimmed versions), 10 the digits (16x15 each, stacked), 12 "/" and
//         ":" (12x15 each), 14 "AM" and "PM" (32x16 each). 11, 13 and 15 are the same shifted by
//         a pixel, since impose.prx copies 32 bits at a time and needs them for odd x. The rest are
//         for impose's other overlays (volume, brightness, mute, hold).
//
// The status bar at the top, the lines and the volume bar aren't placed by impose.rsc: impose.prx
// draws them with these images, at the positions DrawStatusBar and DrawFrame use (read from
// impose_02g.prx, 6.61). The same goes for the button guide: the cross icon is always on the left
// and the circle on the right, and "Enter" and "Back" go next to whichever confirms and cancels.
class ImposeArt {
public:
	enum class Item {
		QUESTION,
		ENTER,
		BACK,
		PLEASE_WAIT,
		YES,
		YES_SELECTED,
		YES_DIM,
		NO,
		NO_SELECTED,
		NO_DIM,
		CIRCLE_ICON,
		CROSS_ICON,
		BATTERY_FULL,  // Then 2 bars, 1 bar, empty.
		DIGIT_0 = BATTERY_FULL + 4,
		SLASH = DIGIT_0 + 10,
		COLON,
		AM,
		PM,
		LINE,
		VOLUME_SET,
		VOLUME_UNSET,
		SPEAKER,
		// The volume bar's sprites with their background cut out, for drawing straight over a game.
		VOLUME_SET_CLEAR,
		VOLUME_UNSET_CLEAR,
		SPEAKER_CLEAR,
		COUNT,
	};

	~ImposeArt();

	// Reads impose.rsc from the installed firmware (g_Config.nandRootDirectory) for this PSP
	// system language. Only touches the file on the first call or when the language changes.
	// False if there's no firmware or the file isn't the expected format.
	bool Load(int pspLanguage);
	// Where an item goes on the PSP screen (480x272), mapped onto screen.
	Bounds ItemBounds(Item item, const Bounds &screen) const;
	// The button guide: where the circle or cross icon goes, and where "Enter" or "Back" goes
	// when its button is the circle or the cross.
	Bounds IconBounds(bool circle, const Bounds &screen) const;
	Bounds LabelBounds(Item label, bool circle, const Bounds &screen) const;
	void Draw(UIContext &dc, Item item, const Bounds &where, uint32_t color = 0xFFFFFFFF);

	// The date and time at the top right, as impose.prx draws them, with the battery to their
	// right (batteryPercent < 0: none, as on a PSP running off the adapter with its battery out). dayFirst is the DD/MM/YYYY date format, twelveHour the 12-hour clock.
	void DrawStatusBar(UIContext &dc, const Bounds &screen, int month, int day, int hour, int minute, bool dayFirst, bool twelveHour, int batteryPercent, uint32_t color);
	// The lines above and below the question, and the volume bar (volume 0-30) when volume >= 0.
	// spread: how far apart the lines are, from 0 (both in the middle) to 1 (in place).
	void DrawFrame(UIContext &dc, const Bounds &screen, int volume, float spread, uint32_t color);
	// Just the volume bar (volume 0-30), which impose also shows over a game when the volume keys
	// are pressed. clear: without the background the firmware's sprites have.
	void DrawVolumeBar(UIContext &dc, const Bounds &screen, int volume, uint32_t color = 0xFFFFFFFF, bool clear = false);
	// The same text, for drawing it without the firmware's glyphs: "12/31 9:05" or "31/12 9:05 PM".
	static std::string ClockText(int month, int day, int hour, int minute, bool dayFirst, bool twelveHour);
	// Which battery level impose.prx shows for a charge (0 full .. 3 empty).
	static int BatteryLevel(int percent);

	void DeviceLost();

	// The color behind everything on the screen, which the text bitmaps are drawn on (ABGR8888).
	static constexpr uint32_t BACKGROUND = 0xFF393939;

private:
	struct Sprite {
		int atlasY = 0;
		int w = 0;
		int h = 0;
		int x = 0;  // On the PSP screen.
		int y = 0;
	};

	bool Parse(const std::vector<uint8_t> &file, int pspLanguage);
	Draw::Texture *GetTexture(Draw::DrawContext *draw);

	bool tried_ = false;
	bool loaded_ = false;
	int language_ = -1;
	Sprite sprites_[(int)Item::COUNT]{};
	int circleIconX_ = 0, circleIconY_ = 0;
	int crossIconX_ = 0, crossIconY_ = 0;
	// All the sprites, stacked vertically, RGBA8888.
	std::vector<uint32_t> atlas_;
	int atlasW_ = 0;
	int atlasH_ = 0;
	Draw::Texture *texture_ = nullptr;
};

namespace {

constexpr int PSP_W = 480;
constexpr int PSP_H = 272;
constexpr uint32_t NUM_PALETTES = 3;
constexpr uint32_t NUM_LANGUAGES = 12;
constexpr uint32_t NUM_TEXT_IMAGES = 10;
constexpr uint32_t TEXT_RECORDS = 0x44;
constexpr int BATTERY_SHARED_IMAGE = 0;
constexpr int CIRCLE_SHARED_IMAGE = 2;
constexpr int CROSS_SHARED_IMAGE = 3;
constexpr int LINE_SHARED_IMAGE = 8;
constexpr int DIGITS_SHARED_IMAGE = 10;
constexpr int VOLUME_SHARED_IMAGE = 16;
constexpr int SPEAKER_SHARED_IMAGE = 18;
constexpr int SEPARATORS_SHARED_IMAGE = 12;
constexpr int AMPM_SHARED_IMAGE = 14;
constexpr int ICON_SIZE = 16;
// The lines' tops once in place are 96 and 175.
constexpr float LINES_MIDDLE = 135.5f;
constexpr float LINES_HALF_GAP = 39.5f;

uint32_t ABGR1555ToRGBA(uint16_t c) {
	uint32_t r = ((c >> 0) & 31) * 255 / 31;
	uint32_t g = ((c >> 5) & 31) * 255 / 31;
	uint32_t b = ((c >> 10) & 31) * 255 / 31;
	uint32_t a = (c & 0x8000) ? 255 : 0;
	return r | (g << 8) | (b << 16) | (a << 24);
}

// Bounds-checked reads from the file.
struct Reader {
	const std::vector<uint8_t> &data;
	bool ok = true;

	uint32_t U32(size_t offset) {
		if (offset + 4 > data.size()) {
			ok = false;
			return 0;
		}
		uint32_t v;
		memcpy(&v, &data[offset], 4);
		return v;
	}
	uint16_t U16(size_t offset) {
		if (offset + 2 > data.size()) {
			ok = false;
			return 0;
		}
		return (uint16_t)(data[offset] | (data[offset + 1] << 8));
	}
	bool Has(size_t offset, size_t size) {
		if (offset > data.size() || size > data.size() - offset) {
			ok = false;
		}
		return ok;
	}
};

}  // namespace

ImposeArt::~ImposeArt() {
	DeviceLost();
}

void ImposeArt::DeviceLost() {
	if (texture_) {
		texture_->Release();
		texture_ = nullptr;
	}
}

bool ImposeArt::Load(int pspLanguage) {
	if (pspLanguage < 0 || pspLanguage >= (int)NUM_LANGUAGES) {
		pspLanguage = 1;  // English.
	}
	if (tried_ && language_ == pspLanguage) {
		return loaded_;
	}
	tried_ = true;
	language_ = pspLanguage;
	loaded_ = false;
	DeviceLost();

	if (g_Config.nandRootDirectory.empty()) {
		return false;
	}
	const Path path = ImposeRSCPath();
	std::string contents;
	if (!File::ReadBinaryFileToString(path, &contents)) {
		INFO_LOG(Log::System, "No %s, drawing the HOME screen ourselves", path.c_str());
		return false;
	}
	std::vector<uint8_t> file(contents.begin(), contents.end());
	loaded_ = Parse(file, pspLanguage);
	if (loaded_) {
		INFO_LOG(Log::System, "HOME screen: using the firmware's artwork from %s", path.c_str());
	} else {
		WARN_LOG(Log::System, "HOME screen: %s isn't in the expected format, drawing it ourselves", path.c_str());
	}
	return loaded_;
}

bool ImposeArt::Parse(const std::vector<uint8_t> &file, int pspLanguage) {
	Reader r{ file };
	const uint32_t numPalettes = r.U32(0);
	const uint32_t numLanguages = r.U32(4);
	const uint32_t numShared = r.U32(8);
	if (!r.ok || numPalettes != NUM_PALETTES || numLanguages < NUM_LANGUAGES || numShared <= SPEAKER_SHARED_IMAGE || numShared > 256) {
		return false;
	}
	auto tableEntry = [&](uint32_t index) { return r.U32(0x0C + 4 * index); };

	uint32_t palettes[NUM_PALETTES][16];
	for (uint32_t p = 0; p < NUM_PALETTES; p++) {
		const uint32_t offset = tableEntry(p);
		for (int i = 0; i < 16; i++) {
			palettes[p][i] = ABGR1555ToRGBA(r.U16(offset + 2 * i));
		}
	}

	// Decoded sprites, before they go into the atlas.
	struct Decoded {
		int w = 0, h = 0, x = 0, y = 0;
		std::vector<uint32_t> pixels;
	};
	Decoded decoded[(int)Item::COUNT];

	const uint32_t block = tableEntry(NUM_PALETTES + pspLanguage);
	if (!r.ok || r.U32(block) < NUM_TEXT_IMAGES) {
		return false;
	}
	circleIconX_ = (int)r.U32(block + 4);
	circleIconY_ = (int)r.U32(block + 8);
	crossIconX_ = (int)r.U32(block + 12);
	crossIconY_ = (int)r.U32(block + 16);
	if (circleIconX_ > PSP_W - ICON_SIZE || circleIconY_ > PSP_H - ICON_SIZE || crossIconX_ > PSP_W - ICON_SIZE || crossIconY_ > PSP_H - ICON_SIZE) {
		return false;
	}

	for (uint32_t i = 0; i < NUM_TEXT_IMAGES; i++) {
		const uint32_t rec = block + TEXT_RECORDS + 28 * i;
		const uint32_t palette = r.U32(rec + 0);
		const uint32_t size = r.U32(rec + 4);
		const uint32_t x = r.U32(rec + 8);
		const uint32_t y = r.U32(rec + 12);
		const uint32_t w = r.U32(rec + 16);
		const uint32_t h = r.U32(rec + 20);
		const uint32_t offset = r.U32(rec + 24);
		if (!r.ok || palette >= NUM_PALETTES || w == 0 || h == 0 || x + w > PSP_W || y + h > PSP_H || (w * h + 1) / 2 != size) {
			return false;
		}
		const size_t pixels = block + TEXT_RECORDS + (size_t)offset;
		if (!r.Has(pixels, size)) {
			return false;
		}
		Decoded &d = decoded[i];
		d.w = (int)w;
		d.h = (int)h;
		d.x = (int)x;
		d.y = (int)y;
		d.pixels.resize(w * h);
		for (uint32_t p = 0; p < w * h; p++) {
			const uint8_t byte = file[pixels + p / 2];
			d.pixels[p] = palettes[palette][(p & 1) ? (byte >> 4) : (byte & 0xF)];
		}
	}

	// Shared images, cut into the frames they hold (stacked vertically).
	struct SharedImage {
		int index;
		int w, h;  // Of one frame.
		int frames;
		Item first;
	};
	static const SharedImage sharedImages[] = {
		{ BATTERY_SHARED_IMAGE, 28, 17, 4, Item::BATTERY_FULL },
		{ CIRCLE_SHARED_IMAGE, ICON_SIZE, ICON_SIZE, 1, Item::CIRCLE_ICON },
		{ CROSS_SHARED_IMAGE, ICON_SIZE, ICON_SIZE, 1, Item::CROSS_ICON },
		{ DIGITS_SHARED_IMAGE, 16, 15, 10, Item::DIGIT_0 },
		{ SEPARATORS_SHARED_IMAGE, 12, 15, 2, Item::SLASH },
		{ AMPM_SHARED_IMAGE, 32, 16, 2, Item::AM },
		{ LINE_SHARED_IMAGE, 328, 9, 1, Item::LINE },
		{ VOLUME_SHARED_IMAGE, 8, 24, 2, Item::VOLUME_SET },
		{ SPEAKER_SHARED_IMAGE, 32, 28, 3, Item::SPEAKER },
	};
	for (const SharedImage &img : sharedImages) {
		if ((uint32_t)img.index >= numShared) {
			return false;
		}
		const uint32_t offset = tableEntry(NUM_PALETTES + numLanguages + img.index);
		const uint32_t frameBytes = (uint32_t)(img.w * img.h * 2);
		// The speaker has more frames than we use (only the first).
		const int frames = img.first == Item::SPEAKER ? 1 : img.frames;
		if (!r.ok || r.U32(offset) != frameBytes * img.frames || !r.Has(offset + 4, frameBytes * img.frames)) {
			return false;
		}
		for (int f = 0; f < frames; f++) {
			Decoded &d = decoded[(int)img.first + f];
			d.w = img.w;
			d.h = img.h;
			d.pixels.resize(img.w * img.h);
			const uint32_t frame = offset + 4 + frameBytes * f;
			for (int p = 0; p < img.w * img.h; p++) {
				d.pixels[p] = ABGR1555ToRGBA(r.U16(frame + 2 * p));
			}
		}
	}
	if (!r.ok) {
		return false;
	}

	// The volume bar's sprites again, with the color in their corner (their background) made
	// transparent, and anything darker than it (the shadows) half so.
	static const Item clearCopies[][2] = {
		{ Item::VOLUME_SET, Item::VOLUME_SET_CLEAR },
		{ Item::VOLUME_UNSET, Item::VOLUME_UNSET_CLEAR },
		{ Item::SPEAKER, Item::SPEAKER_CLEAR },
	};
	for (const auto &copy : clearCopies) {
		Decoded &d = decoded[(int)copy[1]];
		d = decoded[(int)copy[0]];
		const uint32_t background = d.pixels[0];
		for (uint32_t &p : d.pixels) {
			if (p == background) {
				p = 0;
			} else if ((p & 0xFF) < (background & 0xFF)) {
				p = (p & 0x00FFFFFF) | 0x80000000;
			}
		}
	}

	// Stack them into one texture, a pixel apart so linear filtering doesn't bleed.
	atlasW_ = 0;
	atlasH_ = 0;
	for (const Decoded &d : decoded) {
		atlasW_ = std::max(atlasW_, d.w);
		atlasH_ += d.h + 1;
	}
	atlas_.assign((size_t)atlasW_ * atlasH_, 0);
	int y = 0;
	for (int i = 0; i < (int)Item::COUNT; i++) {
		const Decoded &d = decoded[i];
		for (int row = 0; row < d.h; row++) {
			memcpy(&atlas_[(size_t)(y + row) * atlasW_], &d.pixels[(size_t)row * d.w], d.w * 4);
		}
		sprites_[i] = Sprite{ y, d.w, d.h, d.x, d.y };
		y += d.h + 1;
	}
	return true;
}

Draw::Texture *ImposeArt::GetTexture(Draw::DrawContext *draw) {
	if (!texture_ && loaded_) {
		Draw::TextureDesc desc{};
		desc.type = Draw::TextureType::LINEAR2D;
		desc.format = Draw::DataFormat::R8G8B8A8_UNORM;
		desc.width = atlasW_;
		desc.height = atlasH_;
		desc.depth = 1;
		desc.mipLevels = 1;
		desc.generateMips = false;
		desc.tag = "impose_home";
		desc.initData.push_back((const uint8_t *)atlas_.data());
		texture_ = draw->CreateTexture(desc);
	}
	return texture_;
}

Bounds ImposeArt::ItemBounds(Item item, const Bounds &screen) const {
	const Sprite &s = sprites_[(int)item];
	const float sx = screen.w / PSP_W;
	const float sy = screen.h / PSP_H;
	return Bounds(screen.x + s.x * sx, screen.y + s.y * sy, s.w * sx, s.h * sy);
}

Bounds ImposeArt::IconBounds(bool circle, const Bounds &screen) const {
	const float sx = screen.w / PSP_W;
	const float sy = screen.h / PSP_H;
	const int x = circle ? circleIconX_ : crossIconX_;
	const int y = circle ? circleIconY_ : crossIconY_;
	return Bounds(screen.x + x * sx, screen.y + y * sy, ICON_SIZE * sx, ICON_SIZE * sy);
}

Bounds ImposeArt::LabelBounds(Item label, bool circle, const Bounds &screen) const {
	const Sprite &s = sprites_[(int)label];
	const float sx = screen.w / PSP_W;
	const float sy = screen.h / PSP_H;
	const int x = (circle ? circleIconX_ : crossIconX_) + ICON_SIZE;
	return Bounds(screen.x + x * sx, screen.y + s.y * sy, s.w * sx, s.h * sy);
}

void ImposeArt::Draw(UIContext &dc, Item item, const Bounds &where, uint32_t color) {
	Draw::Texture *texture = GetTexture(dc.GetDrawContext());
	if (!texture) {
		return;
	}
	const Sprite &s = sprites_[(int)item];
	dc.Flush();
	dc.GetDrawContext()->BindTexture(0, texture);
	const float u1 = 0.0f;
	const float u2 = (float)s.w / atlasW_;
	const float v1 = (float)s.atlasY / atlasH_;
	const float v2 = (float)(s.atlasY + s.h) / atlasH_;
	dc.Draw()->DrawTexRect(where, u1, v1, u2, v2, color);
	dc.Flush();
	dc.RebindTexture();
}

namespace {

// The status bar's glyphs, as impose.prx lays them out: what to draw and how far to move on.
enum class Glyph { DIGIT, SLASH, COLON, SPACE, THIN_SPACE, AM, PM };

struct StatusGlyph {
	Glyph glyph;
	int digit;
};

// The firmware builds the string the same way: no leading zero on the month, the day or the hour,
// a 5 pixel space after the date and at the end, a 1 pixel one before AM/PM.
std::vector<StatusGlyph> StatusBarGlyphs(int month, int day, int hour, int minute, bool dayFirst, bool twelveHour) {
	std::vector<StatusGlyph> glyphs;
	auto number = [&](int n, bool leadingZero) {
		if (n / 10 != 0 || leadingZero) {
			glyphs.push_back({ Glyph::DIGIT, (n / 10) % 10 });
		}
		glyphs.push_back({ Glyph::DIGIT, n % 10 });
	};
	number(dayFirst ? day : month, false);
	glyphs.push_back({ Glyph::SLASH, 0 });
	number(dayFirst ? month : day, false);
	glyphs.push_back({ Glyph::SPACE, 0 });
	bool pm = false;
	if (twelveHour) {
		pm = hour >= 12;
		hour %= 12;
		if (hour == 0) {
			hour = 12;
		}
	}
	number(hour, false);
	glyphs.push_back({ Glyph::COLON, 0 });
	number(minute, true);
	if (twelveHour) {
		glyphs.push_back({ Glyph::THIN_SPACE, 0 });
		glyphs.push_back({ pm ? Glyph::PM : Glyph::AM, 0 });
	}
	glyphs.push_back({ Glyph::SPACE, 0 });
	return glyphs;
}

int GlyphAdvance(const StatusGlyph &g) {
	switch (g.glyph) {
	case Glyph::DIGIT: return g.digit == 1 ? 10 : (g.digit == 2 ? 11 : 12);
	case Glyph::SLASH: return 7;
	case Glyph::COLON: return 6;
	case Glyph::SPACE: return 5;
	case Glyph::THIN_SPACE: return 1;
	case Glyph::AM:
	case Glyph::PM: return 29;
	}
	return 0;
}

}  // namespace

int ImposeArt::BatteryLevel(int percent) {
	// impose.prx's thresholds.
	if (percent >= 60)
		return 0;
	if (percent >= 30)
		return 1;
	if (percent >= 12)
		return 2;
	return 3;
}

std::string ImposeArt::ClockText(int month, int day, int hour, int minute, bool dayFirst, bool twelveHour) {
	std::string text;
	for (const StatusGlyph &g : StatusBarGlyphs(month, day, hour, minute, dayFirst, twelveHour)) {
		switch (g.glyph) {
		case Glyph::DIGIT: text += (char)('0' + g.digit); break;
		case Glyph::SLASH: text += '/'; break;
		case Glyph::COLON: text += ':'; break;
		case Glyph::SPACE: text += ' '; break;
		case Glyph::THIN_SPACE: break;
		case Glyph::AM: text += " AM"; break;
		case Glyph::PM: text += " PM"; break;
		}
	}
	while (!text.empty() && text.back() == ' ') {
		text.pop_back();
	}
	return text;
}

void ImposeArt::DrawStatusBar(UIContext &dc, const Bounds &screen, int month, int day, int hour, int minute, bool dayFirst, bool twelveHour, int batteryPercent, uint32_t color) {
	const float sx = screen.w / PSP_W;
	const float sy = screen.h / PSP_H;
	auto at = [&](Item item, int x, int y) {
		const Sprite &s = sprites_[(int)item];
		Draw(dc, item, Bounds(screen.x + x * sx, screen.y + y * sy, s.w * sx, s.h * sy), color);
	};

	// Everything is at y 5. The battery sits at x 450, and the clock ends where it starts, or at
	// the screen's edge without one.
	const int y = 5;
	int right = PSP_W;
	if (batteryPercent >= 0) {
		right = 450;
		at((Item)((int)Item::BATTERY_FULL + BatteryLevel(batteryPercent)), 450, y);
	}

	const std::vector<StatusGlyph> glyphs = StatusBarGlyphs(month, day, hour, minute, dayFirst, twelveHour);
	int width = 0;
	for (const StatusGlyph &g : glyphs) {
		width += GlyphAdvance(g);
	}
	int x = right - width;
	for (const StatusGlyph &g : glyphs) {
		switch (g.glyph) {
		case Glyph::DIGIT: at((Item)((int)Item::DIGIT_0 + g.digit), x, y); break;
		case Glyph::SLASH: at(Item::SLASH, x, y); break;
		case Glyph::COLON: at(Item::COLON, x, y); break;
		case Glyph::AM: at(Item::AM, x, y); break;
		case Glyph::PM: at(Item::PM, x, y); break;
		default: break;
		}
		x += GlyphAdvance(g);
	}
}

void ImposeArt::DrawFrame(UIContext &dc, const Bounds &screen, int volume, float spread, uint32_t color) {
	const float sx = screen.w / PSP_W;
	const float sy = screen.h / PSP_H;
	auto at = [&](Item item, float x, float y) {
		const Sprite &s = sprites_[(int)item];
		Draw(dc, item, Bounds(screen.x + x * sx, screen.y + y * sy, s.w * sx, s.h * sy), color);
	};
	// At 96 and 175 once impose.prx's opening animation is done, moving apart from the middle.
	const float half = roundf(LINES_HALF_GAP * spread);
	at(Item::LINE, 80, LINES_MIDDLE - half);
	at(Item::LINE, 80, LINES_MIDDLE + half);
	if (volume >= 0) {
		DrawVolumeBar(dc, screen, volume, color);
	}
}

void ImposeArt::DrawVolumeBar(UIContext &dc, const Bounds &screen, int volume, uint32_t color, bool clear) {
	const float sx = screen.w / PSP_W;
	const float sy = screen.h / PSP_H;
	auto at = [&](Item item, int x, int y) {
		const Sprite &s = sprites_[(int)item];
		Draw(dc, item, Bounds(screen.x + x * sx, screen.y + y * sy, s.w * sx, s.h * sy), color);
	};
	at(clear ? Item::SPEAKER_CLEAR : Item::SPEAKER, 28, 223);
	for (int i = 0; i < 30; i++) {
		if (i < volume) {
			at(clear ? Item::VOLUME_SET_CLEAR : Item::VOLUME_SET, 62 + 12 * i, 223);
		} else {
			at(clear ? Item::VOLUME_UNSET_CLEAR : Item::VOLUME_UNSET, 62 + 12 * i, 223);
		}
	}
}

static uint32_t WithAlpha(uint32_t color, float alpha) {
	const uint32_t a = (uint32_t)(((color >> 24) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f) + 0.5f);
	return (color & 0x00FFFFFF) | (a << 24);
}

// The volume bar without the firmware's art: the speaker, then a bar per step set and a dot per
// step not, where impose puts them.
static void DrawPlainVolumeBar(UIContext &dc, const Bounds &screen, int volume, float alpha = 1.0f) {
	const float sx = screen.w / 480.0f;
	const float sy = screen.h / 272.0f;
	auto PSPRect = [&](float x, float y, float w, float h) { return Bounds(screen.x + x * sx, screen.y + y * sy, w * sx, h * sy); };
	const Bounds speaker = PSPRect(28, 223, 32, 28);
	dc.Draw()->DrawImage(ImageID(volume > 0 ? "I_SPEAKER" : "I_SPEAKER_OFF"), speaker.centerX(), speaker.y + 12.0f * sy, 22.0f * sy / 32.0f, WithAlpha(0xFFFFFFFF, alpha), ALIGN_CENTER);
	for (int i = 0; i < 30; i++) {
		if (i < volume) {
			dc.FillRect(UI::Drawable(WithAlpha(0xFFFFFFFF, alpha)), PSPRect(64.5f + 12 * i, 228, 3, 14));
		} else {
			dc.FillRect(UI::Drawable(WithAlpha(0xFFC5C5C5, alpha)), PSPRect(65 + 12 * i, 239, 2, 2));
		}
	}
}

// The HOME button's quit screen. On a PSP, impose draws it over a running game: the game keeps
// going but stops getting input, and Yes runs the game's exit callback (or just ends it) and goes
// back to the XMB, showing "Please wait..." meanwhile. Here it goes back to the XMB when the XMB
// launched the game, otherwise to our game browser, like Exit to menu. With a firmware dump
// installed it uses the firmware's own artwork (see ImposeArt), otherwise it's drawn the same
// way with our font.

// Whether HOME brings up the quit screen: in any game, but not in the XMB itself (where HOME does
// nothing on a PSP), the PS1 emulator (which has its own HOME menu) or a GE dump.
static bool Available() {
	return !__KernelIsRunningVSH() && !PopsBootActive() && PSP_CoreParameter().fileType != IdentifiedFileType::PPSSPP_GE_DUMP;
}

static uint32_t ConfirmButton() {
	// The system's confirm button, as impose reports it (see __ImposeInit).
	const bool circle = PSP_CoreParameter().compat.flags().ForceCircleButtonConfirm || g_Config.iButtonPreference == PSP_SYSTEMPARAM_BUTTON_CIRCLE;
	return circle ? CTRL_CIRCLE : CTRL_CROSS;
}

// How long impose shows the volume bar over a game after a volume key.
static constexpr double VOLUME_BAR_SECONDS = 2.0;

// impose.prx's animations, timed from a video of a PSP (at 30 frames a second). Opening, the
// screen fades in over the game in OPEN_TIME while the lines move apart from the middle, and the
// question, Yes / No and the button guide come after, all at once. The highlighted answer blinks,
// counting from the opening: highlighted for half of BLINK_PERIOD, then plain for most of the rest,
// with a step of its dimmer look on the way down and back up.
// Choosing Yes shows "Please wait..." at once and fades the rest out over QUIT_FADE_TIME. Choosing
// No is assumed to run the opening backwards (the video doesn't show it).
static constexpr double OPEN_TIME = 1.0 / 6.0;
static constexpr double QUIT_FADE_TIME = 1.0 / 6.0;
static constexpr double BLINK_PERIOD = 1.0;
static constexpr double BLINK_DIM_TIME = 1.0 / 15.0;

enum class Blink {
	ON,
	DIM,
	OFF,
};

static Blink BlinkAt(double sinceOpen) {
	const double t = fmod(sinceOpen, BLINK_PERIOD);
	const double half = BLINK_PERIOD / 2.0;
	if (t < half) {
		return Blink::ON;
	}
	if (t < half + BLINK_DIM_TIME || t >= BLINK_PERIOD - BLINK_DIM_TIME) {
		return Blink::DIM;
	}
	return Blink::OFF;
}

// Our game volume (0-100) as the PSP's volume steps (0-30), which the volume bar shows.
static int VolumeStep() {
	return (std::clamp(g_Config.iGameVolume, 0, VOLUMEHI_FULL) * 30 + VOLUMEHI_FULL / 2) / VOLUMEHI_FULL;
}

void ImposeHomeScreen::SetState(State state) {
	state_ = state;
	stateTime_ = time_now_d();
}

void ImposeHomeScreen::Open() {
	INFO_LOG(Log::System, "HOME: showing the quit screen");
	SetState(State::ASKING);
	openTime_ = stateTime_;
	yes_ = false;
	__CtrlSetGameInputBlocked(true);
}

void ImposeHomeScreen::Close() {
	if (state_ != State::CLOSED) {
		state_ = State::CLOSED;
		__CtrlSetGameInputBlocked(false);
	}
}

void ImposeHomeScreen::Choose(bool quit) {
	if (!quit && coreState == CORE_RUNNING_CPU) {
		// The game gets its input back once the screen has gone.
		SetState(State::CLOSING);
		return;
	}
	if (!quit || coreState != CORE_RUNNING_CPU) {
		Close();
		return;
	}
	// Like a PSP: the game's exit callback first, so it can clean up (and usually calls
	// sceKernelExitGame itself). Without one, the game is just ended.
	//
	// The game still shouldn't get input meanwhile (TODO: though nothing enforces this?)
	// and "Please wait..." stays up until it's gone.
	SetState(State::QUITTING);
	if (!__KernelIsExitCallbackPending() && __KernelInvokeRegisteredExitCallback(__KernelCanExitToVSH())) {
		INFO_LOG(Log::System, "HOME: quitting, running the game's exit callback");
		exitDeadline_ = time_now_d() + 4.0;
	} else {
		FinishQuit();
	}
}

void ImposeHomeScreen::FinishQuit() {
	// Back to the XMB if it launched the game, otherwise to the game browser.
	if (__KernelRequestExitToVSH()) {
		return;
	}
	INFO_LOG(Log::System, "HOME: quitting to the game browser");
	Close();
	pendingAction_ = Action::LEAVE_GAME;
}

ImposeHomeScreen::Action ImposeHomeScreen::Update(bool running, bool onTop) {
	// A quit chosen since the last update.
	if (pendingAction_ != Action::NONE) {
		return TakeAction();
	}
	if (!running) {
		// Paused in the debugger, say, or on the way out: keep showing "Please wait..." while a
		// quit is under way, otherwise let go.
		if (state_ == State::ASKING || state_ == State::CLOSING || !PSP_IsInited()) {
			Close();
		}
		lastButtons_ = 0;
		volumeBarUntil_ = -1.0;
		if (!PSP_IsInited()) {
			exitDeadline_ = -1.0;
		}
		return Action::NONE;
	}

	// A game whose exit callback never gets round to exiting: quit it anyway.
	if (exitDeadline_ >= 0.0 && time_now_d() > exitDeadline_) {
		exitDeadline_ = -1.0;
		WARN_LOG(Log::System, "HOME: the exit callback didn't end the game, quitting it");
		FinishQuit();
		return TakeAction();
	}

	const uint32_t buttons = __CtrlPeekButtons();
	const uint32_t pressed = buttons & ~lastButtons_;
	lastButtons_ = buttons;

	if (state_ == State::CLOSING && time_now_d() - stateTime_ >= OPEN_TIME) {
		Close();
	}

	if (!onTop) {
		// The pause menu or such is up. Leave the quit screen as it was.
		return Action::NONE;
	}

	// The volume keys move the volume a step, which is our game volume. In a game impose shows the
	// volume bar for a moment; on the quit screen, it's always there.
	if ((pressed & (CTRL_VOL_UP | CTRL_VOL_DOWN)) && state_ != State::QUITTING) {
		const int step = std::clamp(VolumeStep() + ((pressed & CTRL_VOL_UP) ? 1 : -1), 0, 30);
		g_Config.iGameVolume = (step * VOLUMEHI_FULL + 15) / 30;
		volumeBarUntil_ = time_now_d() + VOLUME_BAR_SECONDS;
	}

	switch (state_) {
	case State::CLOSED:
		if ((pressed & CTRL_HOME) && exitDeadline_ < 0.0 && Available()) {
			Open();
		}
		break;
	case State::ASKING:
	{
		if (time_now_d() - openTime_ < OPEN_TIME) {
			// Still opening: nothing to choose yet.
			break;
		}
		const uint32_t confirm = ConfirmButton();
		const uint32_t cancel = confirm == CTRL_CIRCLE ? CTRL_CROSS : CTRL_CIRCLE;
		if (pressed & (CTRL_LEFT | CTRL_RIGHT)) {
			yes_ = !yes_;
		}
		if (pressed & confirm) {
			Choose(yes_);
		} else if (pressed & (cancel | CTRL_HOME)) {
			Choose(false);
		}
		break;
	}
	case State::QUITTING:
	case State::CLOSING:
		break;
	}
	return TakeAction();
}

ImposeHomeScreen::Action ImposeHomeScreen::TakeAction() {
	Action action = pendingAction_;
	pendingAction_ = Action::NONE;
	return action;
}

bool ImposeHomeScreen::IsVisible() const {
	return IsOpen() || (g_Config.iVolumeBarStyle != (int)VolumeBarStyle::OFF && time_now_d() < volumeBarUntil_);
}

bool ImposeHomeScreen::UseFirmwareArt() {
	if (g_Config.iHomeScreenStyle != (int)HomeScreenStyle::FIRMWARE) {
		return false;
	}
	if (!art_) {
		art_ = std::make_unique<ImposeArt>();
	}
	return art_->Load(GetPSPLanguage());
}

void ImposeHomeScreen::Draw(UIContext &dc, const Bounds &screen) {
	using Item = ImposeArt::Item;

	if (state_ == State::CLOSED) {
		// Only the volume bar, over the game, on the gray impose draws behind it (unless that's
		// turned off).
		const float sx = screen.w / 480.0f;
		const float sy = screen.h / 272.0f;
		dc.Flush();
		dc.RebindTexture();
		const bool clear = g_Config.iVolumeBarStyle == (int)VolumeBarStyle::NO_BACKGROUND;
		if (!clear) {
			dc.FillRect(UI::Drawable(ImposeArt::BACKGROUND), Bounds(screen.x + 24 * sx, screen.y + 221 * sy, 400 * sx, 30 * sy));
		}
		if (UseFirmwareArt()) {
			art_->DrawVolumeBar(dc, screen, VolumeStep(), 0xFFFFFFFF, clear);
		} else {
			DrawPlainVolumeBar(dc, screen, VolumeStep());
		}
		dc.Flush();
		return;
	}

	const float sx = screen.w / 480.0f;
	const float sy = screen.h / 272.0f;
	// A rectangle on the PSP screen (480x272).
	auto PSPRect = [&](float x, float y, float w, float h) { return Bounds(screen.x + x * sx, screen.y + y * sy, w * sx, h * sy); };

	const bool circleConfirm = ConfirmButton() == CTRL_CIRCLE;

	// Where the animations are (see OPEN_TIME). background: how opaque the screen is over the
	// game. spread: how far the lines have moved apart. contents: how opaque the status bar, the
	// lines and the volume bar are. The question goes with the contents once the opening is done.
	const double now = time_now_d();
	const float sinceState = (float)((now - stateTime_) / OPEN_TIME);
	float background = 1.0f;
	float spread = 1.0f;
	float contents = 1.0f;
	bool question = true;
	bool pleaseWait = false;
	switch (state_) {
	case State::ASKING:
		background = std::clamp((float)((now - openTime_) / OPEN_TIME), 0.0f, 1.0f);
		spread = background;
		contents = background;
		question = background >= 1.0f;
		break;
	case State::CLOSING:
		background = 1.0f - std::clamp(sinceState, 0.0f, 1.0f);
		spread = background;
		contents = background;
		question = false;
		break;
	case State::QUITTING:
		contents = 1.0f - std::clamp((float)((now - stateTime_) / QUIT_FADE_TIME), 0.0f, 1.0f);
		question = contents > 0.0f;
		pleaseWait = true;
		break;
	case State::CLOSED:
		return;
	}
	const uint32_t contentColor = WithAlpha(0xFFFFFFFF, contents);
	const Blink blink = BlinkAt(now - openTime_);

	// impose covers the whole screen with this, which its text bitmaps are drawn on.
	dc.Flush();
	dc.RebindTexture();
	dc.FillRect(UI::Drawable(WithAlpha(ImposeArt::BACKGROUND, background)), screen);

	// The status bar: the emulated PSP's date and time, and the battery - this device's, since the
	// PSP's is always full. A device without one (a desktop) shows it full, like what games are told.
	ScePspDateTime nowPSP{};
	const bool haveTime = __RtcGetCurrentLocalTime(&nowPSP);
	const bool dayFirst = g_Config.iDateFormat == PSP_SYSTEMPARAM_DATE_FORMAT_DDMMYYYY;
	const bool twelveHour = g_Config.iTimeFormat == PSP_SYSTEMPARAM_TIME_FORMAT_12HR;
	int batteryPercent = 100;
	if (System_GetPropertyBool(SYSPROP_CAN_READ_BATTERY_PERCENTAGE)) {
		const int64_t percent = System_GetPropertyInt(SYSPROP_BATTERY_PERCENTAGE);
		if (percent >= 0 && percent <= 100) {
			batteryPercent = (int)percent;
		}
	}

	// The volume bar shows our game volume, in the PSP's 30 steps.
	const int volume = VolumeStep();

	if (UseFirmwareArt()) {
		ImposeArt &art = *art_;
		if (haveTime && contents > 0.0f) {
			art.DrawStatusBar(dc, screen, nowPSP.month, nowPSP.day, nowPSP.hour, nowPSP.minute, dayFirst, twelveHour, batteryPercent, contentColor);
		}
		yesBounds_ = art.ItemBounds(Item::YES, screen);
		noBounds_ = art.ItemBounds(Item::NO, screen);
		if (contents > 0.0f) {
			art.DrawFrame(dc, screen, volume, spread, contentColor);
		}
		if (question) {
			const Item highlighted = blink == Blink::ON ? Item::YES_SELECTED : (blink == Blink::DIM ? Item::YES_DIM : Item::YES);
			const Item highlightedNo = blink == Blink::ON ? Item::NO_SELECTED : (blink == Blink::DIM ? Item::NO_DIM : Item::NO);
			art.Draw(dc, Item::QUESTION, art.ItemBounds(Item::QUESTION, screen), contentColor);
			art.Draw(dc, yes_ ? highlighted : Item::YES, yesBounds_, contentColor);
			art.Draw(dc, yes_ ? Item::NO : highlightedNo, noBounds_, contentColor);
			// Cross on the left, circle on the right, the labels by their buttons.
			art.Draw(dc, Item::CROSS_ICON, art.IconBounds(false, screen), contentColor);
			art.Draw(dc, Item::CIRCLE_ICON, art.IconBounds(true, screen), contentColor);
			art.Draw(dc, Item::ENTER, art.LabelBounds(Item::ENTER, circleConfirm, screen), contentColor);
			art.Draw(dc, Item::BACK, art.LabelBounds(Item::BACK, !circleConfirm, screen), contentColor);
		}
		if (pleaseWait) {
			art.Draw(dc, Item::PLEASE_WAIT, art.ItemBounds(Item::PLEASE_WAIT, screen));
		}
		dc.Flush();
		return;
	}

	// No firmware: the same screen with our font, at the firmware's positions (English layout).
	auto di = GetI18NCategory(I18NCat::DIALOG);
	const UI::Theme &theme = dc.GetTheme();
	dc.SetFontStyle(theme.uiFont);
	auto FitText = [&](std::string_view text, const Bounds &bounds, uint32_t color, int align) {
		// Scale the font to the bitmap's height, then squeeze if the translation is longer.
		float w, h;
		dc.MeasureText(theme.uiFont, 1.0f, 1.0f, text, &w, &h);
		float scale = h > 0.0f ? bounds.h / h : 1.0f;
		if (w * scale > bounds.w && (align & ALIGN_HCENTER)) {
			scale = std::max(bounds.w / w, scale * 0.5f);
		}
		dc.SetFontScale(scale, scale);
		const float x = (align & ALIGN_HCENTER) ? bounds.centerX() : bounds.x;
		dc.DrawText(text, x, bounds.centerY(), color, (align & ALIGN_HCENTER) ? ALIGN_CENTER : (ALIGN_LEFT | ALIGN_VCENTER));
	};
	auto faded = [&](uint32_t color) { return WithAlpha(color, contents); };

	if (contents > 0.0f) {
		// Status bar: the clock right-aligned at the top, the battery as its level in quarters.
		float clockRight = 480.0f;
		if (batteryPercent >= 0) {
			clockRight = 450.0f;
			const Bounds battery = PSPRect(452, 6, 24, 15);
			const int level = 3 - ImposeArt::BatteryLevel(batteryPercent);  // Bars, 0-3.
			dc.FillRect(UI::Drawable(faded(0xFFFFFFFF)), Bounds(battery.x, battery.y, battery.w, battery.h));
			dc.FillRect(UI::Drawable(ImposeArt::BACKGROUND), Bounds(battery.x + sx, battery.y + sy, battery.w - 2 * sx, battery.h - 2 * sy));
			for (int i = 0; i < level; i++) {
				dc.FillRect(UI::Drawable(faded(0xFFFFFFFF)), PSPRect(455 + 6.5f * i, 9, 5, 9));
			}
		}
		if (haveTime) {
			const std::string clock = ImposeArt::ClockText(nowPSP.month, nowPSP.day, nowPSP.hour, nowPSP.minute, dayFirst, twelveHour);
			float w, h;
			dc.MeasureText(theme.uiFont, 1.0f, 1.0f, clock, &w, &h);
			const float scale = h > 0.0f ? 15.0f * sy / h : 1.0f;
			dc.SetFontScale(scale, scale);
			dc.DrawText(clock, screen.x + (clockRight - 6.0f) * sx, screen.y + 12.5f * sy, faded(0xFFFFFFFF), ALIGN_RIGHT | ALIGN_VCENTER);
		}

		// The lines above and below the question, moving apart as the screen opens.
		const float half = roundf(LINES_HALF_GAP * spread);
		dc.FillRect(UI::Drawable(faded(0xFF9C9C9C)), PSPRect(80, LINES_MIDDLE + 3.0f - half, 328, 1.5f));
		dc.FillRect(UI::Drawable(faded(0xFF9C9C9C)), PSPRect(80, LINES_MIDDLE + 3.0f + half, 328, 1.5f));

		DrawPlainVolumeBar(dc, screen, volume, contents);
	}

	yesBounds_ = PSPRect(190, 146, 32, 18);
	noBounds_ = PSPRect(258, 146, 32, 18);
	if (question) {
		FitText(di->T("Do you want to quit the game?"), PSPRect(40, 112, 400, 19), faded(0xFFFFFFFF), ALIGN_HCENTER);
		const Bounds &selected = yes_ ? yesBounds_ : noBounds_;
		if (blink != Blink::OFF) {
			dc.FillRect(UI::Drawable(faded(blink == Blink::ON ? 0xFFA4A4A4 : 0xFF6E6E6E)), selected);
		}
		FitText(di->T("Yes"), yesBounds_, faded(0xFFFFFFFF), ALIGN_HCENTER);
		FitText(di->T("No"), noBounds_, faded(0xFFFFFFFF), ALIGN_HCENTER);

		// Button guide, as the firmware has it: cross on the left, circle on the right, each
		// labelled with what it does.
		const float iconScale = 16.0f * sy / 32.0f;  // Atlas button icons are about 32 high.
		const Bounds crossIcon = PSPRect(188, 254, 16, 16);
		const Bounds circleIcon = PSPRect(266, 254, 16, 16);
		dc.Draw()->DrawImage(ImageID("I_CROSS"), crossIcon.centerX(), crossIcon.centerY(), iconScale, faded(0xFFFFFFFF), ALIGN_CENTER);
		FitText(circleConfirm ? di->T("Back") : di->T("Enter"), PSPRect(204, 254, 60, 14), faded(0xFFFFFFFF), ALIGN_LEFT);
		dc.Draw()->DrawImage(ImageID("I_CIRCLE"), circleIcon.centerX(), circleIcon.centerY(), iconScale, faded(0xFFFFFFFF), ALIGN_CENTER);
		FitText(circleConfirm ? di->T("Enter") : di->T("Back"), PSPRect(282, 254, 60, 14), faded(0xFFFFFFFF), ALIGN_LEFT);
	}
	if (pleaseWait) {
		FitText(di->T("Please wait..."), PSPRect(120, 128, 240, 16), 0xFFFFFFFF, ALIGN_HCENTER);
	}

	dc.SetFontScale(1.0f, 1.0f);
	dc.Flush();
}

ImposeHomeScreen::ImposeHomeScreen() {}

ImposeHomeScreen::~ImposeHomeScreen() {
	Close();
}

bool ImposeHomeScreen::FirmwareArtAvailable() {
	return !g_Config.nandRootDirectory.empty() && File::Exists(ImposeRSCPath());
}

void ImposeHomeScreen::DeviceLost() {
	if (art_) {
		art_->DeviceLost();
	}
}
