#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// One event of a Standard MIDI File, with its time already resolved through the tempo map.
struct SmfEvent {
    double time;            // Seconds from the start of the song
    uint32_t shortMessage;  // status | data1 << 8 | data2 << 16; 0 for SysEx events
    uint32_t sysexOffset;   // SysEx only: offset into SmfFile::sysexData
    uint32_t sysexLength;   // SysEx only: complete message length, F0 ... F7 included
};

// A Standard MIDI File flattened into a single time-ordered event list.
struct SmfFile {
    std::vector<SmfEvent> events;
    std::vector<uint8_t> sysexData;
    double duration = 0.0;
    int format = 0;
    int trackCount = 0;
    std::string title;  // Name of the first track, if present
};

// Parses SMF format 0/1/2, optionally wrapped in a RIFF RMID container.
bool parseSmf(const uint8_t* data, size_t size, SmfFile& out, std::string& error);
bool loadSmfFile(const std::filesystem::path& path, SmfFile& out, std::string& error);

// Writes a format 0 SMF with one tempo (microseconds per quarter note) and the given ticks per quarter note;
// events are placed on the nearest tick.
std::vector<uint8_t> writeSmf(const SmfFile& smf, uint16_t ticksPerQuarter, uint32_t microsPerQuarter = 500000);
bool saveSmfFile(const std::filesystem::path& path, const SmfFile& smf, uint16_t ticksPerQuarter, uint32_t microsPerQuarter,
                 std::string& error);
