#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct RomEntry {
    std::filesystem::path path;
    std::string fileName;     // UTF-8
    std::string shortName;    // mt32emu ROM id, e.g. "ctrl_d110_1_10_2"
    std::string description;  // e.g. "D-110 Control v1.10"
    bool isControl = false;
};

// Identifies every complete Control/PCM ROM image in a folder by size and SHA1.
// Sorted with D-110 ROMs first.
std::vector<RomEntry> scanRomFolder(const std::filesystem::path& folder);

// Looks for a "roms" folder in each of the given directories and their parents.
std::filesystem::path findRomFolder(const std::vector<std::filesystem::path>& startDirs);

// Picks a matching Control/PCM pair, preferring D-110, then CM-32L, then MT-32 ROMs.
// Returns false when the folder has no usable pair.
bool pickDefaultRoms(const std::vector<RomEntry>& roms, int& controlIndex, int& pcmIndex);

// Machine family of a ROM from its short name: "d110", "cm32l" or "mt32".
std::string romFamily(const RomEntry& rom);
