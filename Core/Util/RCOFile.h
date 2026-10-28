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
#include <string_view>
#include <vector>

// Reader for the PSP firmware's RCO resource files (flash0:/vsh/resource/*.rco), which hold the
// art, strings, sounds and object layout of the system screens and the utility dialogs.
// Read-only and little-endian (PSP) only. See RCOFile.cpp for the format.

class RCOFile {
public:
	enum class Table : uint8_t {
		MAIN = 1,
		VSMX = 2,
		TEXT = 3,
		IMAGE = 4,
		MODEL = 5,
		SOUND = 6,
		FONT = 7,
		OBJECT = 8,
		ANIM = 9,
	};

	// RCO language ids, which aren't the same as the PSP's system language ids.
	enum Language : int {
		RCO_LANG_JAPANESE = 0,
		RCO_LANG_ENGLISH = 1,
		RCO_LANG_FRENCH = 2,
		RCO_LANG_SPANISH = 3,
		RCO_LANG_GERMAN = 4,
		RCO_LANG_ITALIAN = 5,
		RCO_LANG_DUTCH = 6,
		RCO_LANG_PORTUGUESE = 7,
		RCO_LANG_RUSSIAN = 8,
		RCO_LANG_KOREAN = 9,
		RCO_LANG_CHINESE_TRADITIONAL = 10,
		RCO_LANG_CHINESE_SIMPLIFIED = 11,
	};

	// Object types (Table::OBJECT entries).
	enum ObjectType : uint8_t {
		RCO_OBJ_PAGE = 0x01,
		RCO_OBJ_PLANE = 0x02,
		RCO_OBJ_BUTTON = 0x03,
		RCO_OBJ_TEXT = 0x0D,
		RCO_OBJ_GROUP = 0x12,
		RCO_OBJ_ICON = 0x19,
	};

	// Reference kinds used in object and animation attributes.
	enum RefType : uint32_t {
		RCO_REF_EVENT = 0x400,
		RCO_REF_TEXT = 0x401,
		RCO_REF_IMAGE = 0x402,
		RCO_REF_MODEL = 0x403,
		RCO_REF_FONT = 0x405,
		RCO_REF_OBJECT2 = 0x407,
		RCO_REF_ANIM = 0x408,
		RCO_REF_OBJECT = 0x409,
		RCO_REF_NONE = 0xFFFF,
	};

	struct Entry {
		Table table;
		uint8_t type;           // 0 for a table's root, 1 for most items, object/anim type otherwise.
		uint32_t offset;        // Where it is in the (decompressed) tree, which is what references point at.
		std::string label;
		int parent = -1;        // Index into Entries(), -1 for the main table.
		std::vector<int> children;
		// What follows the 0x28-byte entry header (object attributes, image info...).
		std::vector<uint8_t> extra;
	};

	bool Load(const std::vector<uint8_t> &data);
	bool IsLoaded() const { return !entries_.empty(); }

	const std::vector<Entry> &Entries() const { return entries_; }
	// -1 if there's no such entry in the table.
	int Find(Table table, std::string_view label) const;
	int FindByOffset(uint32_t offset) const;

	// Image data, decompressed (usually a GIM). Empty if missing.
	std::vector<uint8_t> ImageData(std::string_view label) const;
	std::vector<uint8_t> ImageData(int entryIndex) const;

	// A string from the text table in UTF-8, falling back to English when the language lacks it.
	// Empty if missing.
	std::string Text(int language, std::string_view label) const;
	bool HasLanguage(int language) const;
	// The labels of a language's strings.
	std::vector<std::string> TextLabels(int language) const;

	// Object attributes, by index in the type's attribute list (as rcomage's objattribdef-psp.ini
	// numbers them, counting a reference as one attribute). Reading by index rather than by byte
	// offset since references take two words.
	float ObjectFloat(int entryIndex, int attr) const;
	uint32_t ObjectInt(int entryIndex, int attr) const;
	// The reference's target entry (by offset), or -1.
	int ObjectRef(int entryIndex, int attr, uint32_t *refType = nullptr) const;

	// For debugging: one line per entry, indented by depth.
	std::string Dump() const;

private:
	bool ParseEntry(uint32_t pos, int parent, int depth, uint32_t *end);
	bool ReadTables(const std::vector<uint8_t> &file);
	const uint8_t *TreePtr(uint32_t offset, uint32_t size) const;
	std::string LabelAt(uint32_t offset) const;
	// Word offset of attribute attr in an object of this type, or -1.
	static int AttributeWordOffset(uint8_t objType, int attr);

	struct TextBlock {
		int language;
		std::vector<uint8_t> data;
	};

	// The file as read, for image and sound data, which live outside the tree.
	std::vector<uint8_t> file_;
	// The tree, laid out at the file offsets it would have uncompressed: [0, treeBase_) is the
	// header, then the (possibly decompressed) tables, labels, events and pointer tables.
	std::vector<uint8_t> tree_;
	std::vector<TextBlock> text_;
	std::vector<Entry> entries_;

	uint32_t labelsOffset_ = 0;
	uint32_t labelsLength_ = 0;
	uint32_t imageDataOffset_ = 0;
	uint32_t imageDataLength_ = 0;
	int compression_ = 0;
};
