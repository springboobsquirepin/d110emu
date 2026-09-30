#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// The preset timbres of an MT-32, CM-32L or LAPC-I control ROM, decoded as the unit does at power-on (mt32emu's
// initTimbres): groups A and B (A1-B64) and the rhythm timbres (R1-R30, R1-R64 on a CM-32L). Each is a timbre in the
// MT-32's SysEx format: common part and 4 partials, 246 bytes, as in timbre memory (08 00 00). The data is read from
// the user's ROM at run time; none of it is part of the source.
struct Mt32Presets {
    using Timbre = std::array<uint8_t, 246>;
    std::string description;            // e.g. "MT-32 Control v2.04"
    std::string shortName;              // mt32emu's ROM id, e.g. "ctrl_mt32_2_04"
    std::array<Timbre, 128> melodic{};  // A1-A64, B1-B64
    std::vector<Timbre> rhythm;         // R1-R30, or R1-R64

    bool valid() const { return !rhythm.empty(); }
    // "ElecPiano1": the timbre's name without trailing spaces.
    static std::string name(const Timbre& timbre);
};

// Decodes a control ROM image. Returns false, with a reason, for anything but an MT-32 or CM-32L control ROM.
bool decodeMt32Presets(const uint8_t* rom, size_t size, Mt32Presets& out, std::string& error);
bool loadMt32Presets(const std::filesystem::path& path, Mt32Presets& out, std::string& error);
