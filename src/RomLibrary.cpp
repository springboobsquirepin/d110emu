#include "RomLibrary.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <set>
#include <system_error>

#include "mt32emu.h"

namespace {

// Sizes of all ROM images the library knows, so unrelated files are not hashed.
std::set<uintmax_t> knownRomSizes() {
    std::set<uintmax_t> sizes;
    const MT32Emu::ROMInfo** list = MT32Emu::ROMInfo::getROMInfoList(
        (1 << MT32Emu::ROMInfo::Control) | (1 << MT32Emu::ROMInfo::PCM), 1 << MT32Emu::ROMInfo::Full);
    for (const MT32Emu::ROMInfo** info = list; *info != nullptr; info++) sizes.insert((*info)->fileSize);
    MT32Emu::ROMInfo::freeROMInfoList(list);
    return sizes;
}

int familyRank(const std::string& family) {
    if (family == "d110") return 0;
    if (family == "cm32l") return 1;
    if (family == "mt32") return 2;
    return 3;
}

}  // namespace

std::string romFamily(const RomEntry& rom) {
    // Short names look like "ctrl_d110_1_10_2", "pcm_cm32l" or "ctrl_mt32_bluer".
    const std::string& name = rom.shortName;
    const size_t start = name.find('_');
    if (start == std::string::npos) return name;
    const size_t end = name.find('_', start + 1);
    return name.substr(start + 1, end == std::string::npos ? std::string::npos : end - start - 1);
}

std::vector<RomEntry> scanRomFolder(const std::filesystem::path& folder) {
    std::vector<RomEntry> roms;
    std::error_code ec;
    if (folder.empty() || !std::filesystem::is_directory(folder, ec)) return roms;

    const std::set<uintmax_t> sizes = knownRomSizes();
    for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fileError;
        if (!it->is_regular_file(fileError)) continue;
        const uintmax_t size = it->file_size(fileError);
        if (fileError || sizes.count(size) == 0) continue;

        std::ifstream in(it->path(), std::ios::binary);
        const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        MT32Emu::ArrayFile file(data.data(), data.size());
        const MT32Emu::ROMInfo* info = MT32Emu::ROMInfo::getROMInfo(&file);
        if (info == nullptr || info->pairType != MT32Emu::ROMInfo::Full || info->type == MT32Emu::ROMInfo::Reverb) continue;

        RomEntry rom;
        rom.path = it->path();
        rom.fileName = it->path().filename().u8string();
        rom.shortName = info->shortName;
        rom.description = info->description;
        rom.isControl = info->type == MT32Emu::ROMInfo::Control;
        roms.push_back(rom);
    }

    std::sort(roms.begin(), roms.end(), [](const RomEntry& a, const RomEntry& b) {
        const int rankA = familyRank(romFamily(a));
        const int rankB = familyRank(romFamily(b));
        if (rankA != rankB) return rankA < rankB;
        if (a.isControl != b.isControl) return a.isControl;
        if (a.description != b.description) return a.description < b.description;
        return a.fileName < b.fileName;
    });
    return roms;
}

std::filesystem::path findRomFolder(const std::vector<std::filesystem::path>& startDirs) {
    for (const std::filesystem::path& start : startDirs) {
        std::error_code ec;
        std::filesystem::path dir = std::filesystem::absolute(start, ec);
        for (int depth = 0; depth < 8 && !dir.empty(); depth++) {
            const std::filesystem::path candidate = dir / "roms";
            if (std::filesystem::is_directory(candidate, ec)) return candidate;
            const std::filesystem::path parent = dir.parent_path();
            if (parent == dir) break;
            dir = parent;
        }
    }
    return {};
}

bool pickDefaultRoms(const std::vector<RomEntry>& roms, int& controlIndex, int& pcmIndex) {
    controlIndex = -1;
    pcmIndex = -1;
    // The list is sorted by family preference, so the first Control ROM with a PCM ROM of the same family wins.
    for (size_t c = 0; c < roms.size(); c++) {
        if (!roms[c].isControl) continue;
        for (size_t p = 0; p < roms.size(); p++) {
            if (!roms[p].isControl && romFamily(roms[p]) == romFamily(roms[c])) {
                controlIndex = int(c);
                pcmIndex = int(p);
                return true;
            }
        }
    }
    return false;
}
