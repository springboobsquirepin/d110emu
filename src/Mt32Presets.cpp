#include "Mt32Presets.h"

#include <cstring>
#include <fstream>
#include <iterator>

#include "mt32emu.h"

namespace {

constexpr size_t kCommonSize = 14;
constexpr size_t kPartialSize = 58;
constexpr size_t kPartialMute = 12;  // Common byte: bit n set = partial n+1 on

// Where each control ROM keeps its timbre banks (mt32emu's ControlROMMaps).
struct BankLayout {
    const char* shortName;
    uint32_t mapA, offsetA;  // 64 little-endian pointers each, plus the offset
    uint32_t mapB, offsetB;
    bool compressed;         // A and B compressed (R always is)
    uint32_t mapR;
    int rhythmCount;
};
constexpr BankLayout kLayouts[] = {
    {"ctrl_mt32_1_04", 0x8000, 0x0000, 0xC000, 0x4000, false, 0x3200, 30},
    {"ctrl_mt32_1_05", 0x8000, 0x0000, 0xC000, 0x4000, false, 0x3200, 30},
    {"ctrl_mt32_1_06", 0x8000, 0x0000, 0xC000, 0x4000, false, 0x3200, 30},
    {"ctrl_mt32_1_07", 0x8000, 0x0000, 0xC000, 0x4000, false, 0x3200, 30},
    {"ctrl_mt32_bluer", 0x8000, 0x0000, 0xC000, 0x4000, false, 0x3200, 30},
    {"ctrl_mt32_2_04", 0x8000, 0x8000, 0x8080, 0x8000, true, 0x8500, 30},
    {"ctrl_cm32l_1_00", 0x8000, 0x8000, 0x8080, 0x8000, true, 0x8500, 64},
    {"ctrl_cm32l_1_02", 0x8000, 0x8000, 0x8080, 0x8000, true, 0x8500, 64},
};

// A timbre as the ROM holds it. Compressed timbres leave out muted partials (except the first), which take the data
// of the partial before them.
bool readTimbre(const uint8_t* rom, size_t size, uint32_t address, bool compressed, Mt32Presets::Timbre& out) {
    if (!compressed) {
        if (size_t(address) + out.size() > size) return false;
        std::memcpy(out.data(), rom + address, out.size());
        return true;
    }
    if (size_t(address) + kCommonSize > size) return false;
    std::memcpy(out.data(), rom + address, kCommonSize);
    size_t source = address + kCommonSize;
    for (size_t partial = 0; partial < 4; partial++) {
        if (partial != 0 && ((out[kPartialMute] >> partial) & 1) == 0) {
            source -= kPartialSize;
        } else if (source + kPartialSize > size) {
            return false;
        }
        std::memcpy(out.data() + kCommonSize + partial * kPartialSize, rom + source, kPartialSize);
        source += kPartialSize;
    }
    return true;
}

bool readBank(const uint8_t* rom, size_t size, uint32_t map, uint32_t offset, bool compressed, int count, Mt32Presets::Timbre* out) {
    if (size_t(map) + size_t(count) * 2 > size) return false;
    for (int i = 0; i < count; i++) {
        const uint32_t address = uint32_t(rom[map + 2 * i] | (rom[map + 2 * i + 1] << 8)) + offset;
        if (!readTimbre(rom, size, address, compressed, out[i])) return false;
    }
    return true;
}

}  // namespace

std::string Mt32Presets::name(const Timbre& timbre) {
    std::string text(reinterpret_cast<const char*>(timbre.data()), 10);
    for (char& c : text) {
        if (c < 32 || c > 126) c = ' ';
    }
    while (!text.empty() && text.back() == ' ') text.pop_back();
    return text;
}

bool decodeMt32Presets(const uint8_t* rom, size_t size, Mt32Presets& out, std::string& error) {
    MT32Emu::ArrayFile file(rom, size);
    const MT32Emu::ROMInfo* info = MT32Emu::ROMInfo::getROMInfo(&file);
    if (info == nullptr || info->type != MT32Emu::ROMInfo::Control) {
        error = "not a known control ROM";
        return false;
    }
    for (const BankLayout& layout : kLayouts) {
        if (std::strcmp(layout.shortName, info->shortName) != 0) continue;
        Mt32Presets presets;
        presets.description = info->description;
        presets.shortName = info->shortName;
        presets.rhythm.resize(size_t(layout.rhythmCount));
        if (!readBank(rom, size, layout.mapA, layout.offsetA, layout.compressed, 64, &presets.melodic[0]) ||
            !readBank(rom, size, layout.mapB, layout.offsetB, layout.compressed, 64, &presets.melodic[64]) ||
            !readBank(rom, size, layout.mapR, 0, true, layout.rhythmCount, presets.rhythm.data())) {
            error = "timbre bank outside the ROM";
            return false;
        }
        out = std::move(presets);
        return true;
    }
    error = std::string(info->description) + " is not an MT-32 or CM-32L control ROM";
    return false;
}

bool loadMt32Presets(const std::filesystem::path& path, Mt32Presets& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path.u8string();
        return false;
    }
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return decodeMt32Presets(data.data(), data.size(), out, error);
}
