#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "SmfFile.h"

// The D-110's "ROM Play" demo songs, stored in the data half of the control ROM (IC12).
//
// Each song is laid out as:
//   16 bytes   name, space padded ("Macho Memory    ")
//   16 bytes   header: tick period (little-endian word, in 2 us Timer1 counts: 1816H = 12.332 ms),
//              reverb mode/time/level, then 11 bytes the firmware skips
//   256 bytes  rhythm setup for keys 24-87 (tone, level, pan, output assign per key)
//   events     delta time (1 byte in ticks; F8 adds 248 ticks), then a MIDI message with running
//              status; FC in place of a message ends the song
// Parts 1-8 play on MIDI channels 2-9 and the rhythm part on channel 10. The songs send no note-offs
// to the rhythm part: its tones ignore them and play their envelopes to the end.
//
// ROM Play does not use the timbre memory. A program change selects entry (program & 63) of a timbre
// bank in the ROM, whose entries with tone group 5 refer to nine tones that exist only for the demo
// songs. The firmware also holds each song's partial reserves.

// Timbre bank and demo tones shared by all songs.
struct RomPlayTimbres {
    // Group, number, key shift, fine tune, bender range, assign mode, output assign, dummy.
    std::array<std::array<uint8_t, 8>, 64> timbres{};
    struct DemoTone {
        std::array<uint8_t, 246> data{};  // Laid out like the tone temporary area (common + 4 partials)
        // A part cannot select group 5, so it is set to this preset first: the one with the same name, or
        // else the one with the most similar data. ("Syn Lead 1" is an edited copy of b31.)
        uint8_t group = 0xFF;
        uint8_t number = 0;
    };
    std::vector<DemoTone> tones;  // Group 5, by number
};

struct RomSong {
    std::string name;
    double tickSeconds = 0.012332;
    uint8_t reverb[3] = {};              // Mode (0-8), time (0-7), level (0-7)
    uint8_t partialReserve[9] = {};      // Parts 1-8 and rhythm
    bool hasPartialReserve = false;
    uint8_t rhythmSetup[64 * 4] = {};    // Keys 24-87
    std::vector<uint8_t> events;         // Raw event stream up to and including the FC end mark
    std::shared_ptr<const RomPlayTimbres> timbres;  // Null if the firmware tables were not found
    double beatTicks = 0.0;              // Length of a quarter note in ticks, measured from the song (0 = unknown)
    double downbeatTick = 0.0;           // A tick on which a bar starts
};

// Finds the songs in a control ROM image (empty for ROMs without ROM Play data, e.g. MT-32).
std::vector<RomSong> findRomSongs(const std::vector<uint8_t>& controlRom);
bool loadRomSongs(const std::filesystem::path& controlRomPath, std::vector<RomSong>& songs, std::string& error);

// Converts songs for playback: the setup ROM Play makes (system area, part levels and key ranges, rhythm
// setup, controller reset), then the song events, with each program change followed by the SysEx that
// sets the part to the ROM Play timbre. Several songs are chained with a pause in between ("Chain of Songs").
// The SysEx is addressed to `deviceId` (unit number less one; 10H = unit #17, the D-110's default).
std::unique_ptr<SmfFile> romSongsToSmf(const std::vector<RomSong>& songs, uint8_t deviceId = 0x10);

// Writes one song as a Standard MIDI File for other sequencers and real hardware: the tempo is the song's
// own, the song starts on a bar line after a lead-in, and the setup SysEx is spaced out so that a real
// D-110 can take it in.
bool saveRomSongSmf(const std::filesystem::path& path, const RomSong& song, uint8_t deviceId, std::string& error);

// The power-up message in the firmware (" D-110  ver1.10 " / "  Aug. 30, 1988 "), or empty if not found.
std::string findBootMessage(const std::vector<uint8_t>& controlRom);
