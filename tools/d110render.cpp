// Offline renderer: plays a MIDI file (or a built-in test pattern) through the emulation into a
// 16-bit stereo WAV file. Handy for checking emulation changes without audio hardware.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "Platform.h"
#include "ReverbKit.h"
#include "ReverbSettingsFile.h"
#include "RomLibrary.h"
#include "RomPlay.h"
#include "SynthEngine.h"

namespace {

void printUsage() {
    std::fprintf(stderr,
                 "Usage: d110render [options] [input.mid] output.wav\n"
                 "  --roms DIR       ROM folder (default: a \"roms\" folder near the program or working directory)\n"
                 "  --rate HZ        Output sample rate (default 48000)\n"
                 "  --analog MODE    auto | digital | coarse | accurate | oversampled (default auto)\n"
                 "  --tail SECONDS   Time rendered after the song ends, for releases and reverb (default 2)\n"
                 "  --partials N     Maximum partials (default 32, like the real unit)\n"
                 "  --syx FILE       Load a SysEx file (e.g. a bulk dump) before playing; may be repeated\n"
                 "  --mt32           MT-32 translation: set up the MT-32's power-on state and translate the SysEx\n"
                 "                   files and the song from the MT-32 (D-110 ROMs only)\n"
                 "  --dump-syx FILE  Save the synth's memory as SysEx after rendering\n"
                 "  --test           Play a built-in test pattern (a chord per part, then drums)\n"
                 "  --rom-song N     Play D-110 ROM Play song N (1-8), or 0 for the whole chain\n"
                 "  --d20-track      Play the D-20 rhythm track of the loaded SysEx files (--syx)\n"
                 "  --d20-pattern P  Play D-20 rhythm pattern P (P-11 to P-88) once\n"
                 "  --tempo BPM      Tempo for the D-20 rhythm track or pattern (default 120)\n"
                 "  --export-rom-songs DIR  Write the ROM Play songs as .mid files to DIR and exit\n"
                 "  --reverb-kit DIR Write the reverb recording kit (test MIDI files for a real D-110 and D-10/D-20,\n"
                 "                   the schedule, instructions) to DIR and exit\n"
                 "  --reverb-settings FILE  The D-series reverb's parameters (d110emu-reverb.ini)\n"
                 "  --save-reverb-settings FILE  Write the D-series reverb's parameters in use (the defaults, or\n"
                 "                   --reverb-settings) to FILE and exit\n"
                 "  --munt-reverb    D-110 ROMs: the MT-32 family's reverb models instead of the D-series one\n");
}

bool writeWav(const std::filesystem::path& path, const std::vector<int16_t>& samples, uint32_t rate) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    auto u32 = [&](uint32_t v) { const char b[4] = {char(v), char(v >> 8), char(v >> 16), char(v >> 24)}; out.write(b, 4); };
    auto u16 = [&](uint16_t v) { const char b[2] = {char(v), char(v >> 8)}; out.write(b, 2); };
    const uint32_t dataSize = uint32_t(samples.size() * sizeof(int16_t));
    out.write("RIFF", 4);
    u32(36 + dataSize);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(1);  // PCM
    u16(2);  // Stereo
    u32(rate);
    u32(rate * 4);
    u16(4);
    u16(16);
    out.write("data", 4);
    u32(dataSize);
    for (int16_t sample : samples) u16(uint16_t(sample));
    return bool(out);
}

// One chord per melodic part on its assigned channel, then a short drum pattern on the rhythm part.
std::unique_ptr<SmfFile> makeTestPattern(const EngineStatus& status) {
    std::unique_ptr<SmfFile> smf(new SmfFile);
    auto add = [&](double time, uint32_t status, uint32_t data1, uint32_t data2) {
        smf->events.push_back(SmfEvent{time, status | (data1 << 8) | (data2 << 16), 0, 0});
    };
    double time = 0.0;
    for (int part = 0; part < kRhythmPart; part++) {
        const uint8_t channel = status.parts[part].channel;
        if (channel >= 16) continue;
        for (uint32_t note : {60u, 64u, 67u}) {
            add(time, 0x90u | channel, note, 100);
            add(time + 0.7, 0x80u | channel, note, 0);
        }
        time += 0.8;
    }
    const uint8_t rhythm = status.parts[kRhythmPart].channel;
    if (rhythm < 16) {
        for (int beat = 0; beat < 8; beat++) {
            const uint32_t drum = beat % 2 ? 38 : 36;  // Snare / kick
            add(time, 0x90u | rhythm, drum, 110);
            add(time, 0x90u | rhythm, 42, 90);  // Closed hi-hat
            add(time + 0.1, 0x80u | rhythm, drum, 0);
            add(time + 0.1, 0x80u | rhythm, 42, 0);
            time += 0.25;
        }
    }
    std::stable_sort(smf->events.begin(), smf->events.end(), [](const SmfEvent& a, const SmfEvent& b) { return a.time < b.time; });
    smf->duration = time;
    return smf;
}

const char* analogName(AnalogMode mode) {
    static const char* const names[] = {"digital only", "coarse", "accurate", "oversampled"};
    return mode == AnalogMode::Auto ? "auto" : names[int(mode)];
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path romFolder;
    std::vector<std::filesystem::path> positional;
    uint32_t rate = 48000;
    AnalogMode analog = AnalogMode::Auto;
    double tail = 2.0;
    bool test = false;
    uint32_t partials = kDefaultPartials;
    int romSong = -1;
    std::filesystem::path exportDir;
    std::filesystem::path reverbKitDir;
    std::filesystem::path reverbSettingsFile;
    std::filesystem::path saveReverbSettingsFile;
    bool muntReverb = false;
    std::vector<std::filesystem::path> sysexFiles;
    std::filesystem::path dumpFile;
    bool mt32 = false;
    bool d20Track = false;
    int d20Pattern = -1;
    double tempo = 120.0;

    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--roms" && hasValue) {
            romFolder = Platform::fromUtf8(argv[++i]);
        } else if (arg == "--rate" && hasValue) {
            rate = uint32_t(std::max(8000, std::atoi(argv[++i])));
        } else if (arg == "--tail" && hasValue) {
            tail = std::max(0.0, std::atof(argv[++i]));
        } else if (arg == "--analog" && hasValue) {
            const std::string mode = argv[++i];
            if (mode == "digital") analog = AnalogMode::DigitalOnly;
            else if (mode == "coarse") analog = AnalogMode::Coarse;
            else if (mode == "accurate") analog = AnalogMode::Accurate;
            else if (mode == "oversampled") analog = AnalogMode::Oversampled;
            else if (mode != "auto") {
                printUsage();
                return 2;
            }
        } else if (arg == "--partials" && hasValue) {
            partials = uint32_t(std::clamp(std::atoi(argv[++i]), 8, kMaxPartials));
        } else if (arg == "--syx" && hasValue) {
            sysexFiles.push_back(Platform::fromUtf8(argv[++i]));
        } else if (arg == "--dump-syx" && hasValue) {
            dumpFile = Platform::fromUtf8(argv[++i]);
        } else if (arg == "--rom-song" && hasValue) {
            romSong = std::clamp(std::atoi(argv[++i]), 0, 99);
        } else if (arg == "--export-rom-songs" && hasValue) {
            exportDir = Platform::fromUtf8(argv[++i]);
        } else if (arg == "--reverb-kit" && hasValue) {
            reverbKitDir = Platform::fromUtf8(argv[++i]);
        } else if (arg == "--reverb-settings" && hasValue) {
            reverbSettingsFile = Platform::fromUtf8(argv[++i]);
        } else if (arg == "--save-reverb-settings" && hasValue) {
            saveReverbSettingsFile = Platform::fromUtf8(argv[++i]);
        } else if (arg == "--munt-reverb") {
            muntReverb = true;
        } else if (arg == "--test") {
            test = true;
        } else if (arg == "--mt32") {
            mt32 = true;
        } else if (arg == "--d20-track") {
            d20Track = true;
        } else if (arg == "--d20-pattern" && hasValue) {
            const std::string name = argv[++i];  // "P-51" or "51"
            const std::string digits = name.size() >= 2 ? name.substr(name.size() - 2) : name;
            if (digits.size() != 2 || digits[0] < '1' || digits[0] > '8' || digits[1] < '1' || digits[1] > '8') {
                printUsage();
                return 2;
            }
            d20Pattern = (digits[0] - '1') * 8 + (digits[1] - '1');
        } else if (arg == "--tempo" && hasValue) {
            tempo = std::clamp(std::atof(argv[++i]), 20.0, 300.0);
        } else if (arg == "--help" || arg == "-h" || (arg.size() > 1 && arg[0] == '-')) {
            printUsage();
            return arg == "--help" || arg == "-h" ? 0 : 2;
        } else {
            positional.push_back(Platform::fromUtf8(arg));
        }
    }
    if (!saveReverbSettingsFile.empty()) {
        MT32Emu::DSeriesReverbSettings reverbSettings = MT32Emu::DSeriesReverbSettings::getDefaults();
        std::string error;
        if ((!reverbSettingsFile.empty() && !loadReverbSettings(reverbSettingsFile, reverbSettings, error)) ||
            !saveReverbSettings(saveReverbSettingsFile, reverbSettings, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        std::printf("Wrote   %s\n", Platform::toUtf8(saveReverbSettingsFile).c_str());
        return 0;
    }
    if (!reverbKitDir.empty()) {
        std::string error;
        if (!writeReverbKit(reverbKitDir, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        std::printf("Wrote   the reverb recording kit to %s\n", Platform::toUtf8(reverbKitDir).c_str());
        return 0;
    }
    const bool noInputFile = test || romSong >= 0 || d20Track || d20Pattern >= 0;
    if (exportDir.empty() && positional.size() != (noInputFile ? 1u : 2u)) {
        printUsage();
        return 2;
    }

    if (romFolder.empty()) romFolder = findRomFolder({Platform::executableDirectory(), std::filesystem::current_path()});
    const std::vector<RomEntry> roms = scanRomFolder(romFolder);
    int control = -1;
    int pcm = -1;
    if (!pickDefaultRoms(roms, control, pcm)) {
        std::fprintf(stderr, "No usable Control/PCM ROM pair found in \"%s\"\n", Platform::toUtf8(romFolder).c_str());
        return 1;
    }
    std::printf("ROMs:    %s (%s) + %s (%s)\n", roms[control].description.c_str(), roms[control].fileName.c_str(),
                roms[pcm].description.c_str(), roms[pcm].fileName.c_str());

    std::vector<RomSong> romSongs;
    std::string romError;
    if ((romSong >= 0 || !exportDir.empty()) && !loadRomSongs(roms[control].path, romSongs, romError)) {
        std::fprintf(stderr, "%s\n", romError.c_str());
        return 1;
    }
    if (!exportDir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(exportDir, ec);
        for (size_t i = 0; i < romSongs.size(); i++) {
            char fileName[64];
            std::snprintf(fileName, sizeof(fileName), "%02d %s.mid", int(i + 1), romSongs[i].name.c_str());
            std::string error;
            if (!saveRomSongSmf(exportDir / Platform::fromUtf8(fileName), romSongs[i], 0x10, error)) {
                std::fprintf(stderr, "%s\n", error.c_str());
                return 1;
            }
            const RomSong& song = romSongs[i];
            const double beat = song.beatTicks > 0.0 ? song.beatTicks : 0.5 / song.tickSeconds;
            std::printf("Wrote   %s (%.1f BPM)\n", fileName, 60.0 / (beat * song.tickSeconds));
        }
        return 0;
    }
    if (romSong > int(romSongs.size())) {
        std::fprintf(stderr, "The ROM has %zu songs\n", romSongs.size());
        return 1;
    }
    const std::filesystem::path output = positional.back();

    SynthEngine engine;
    EngineOptions engineOptions = engine.options();
    engineOptions.dSeriesReverb = !muntReverb;
    engine.setOptions(engineOptions);
    if (!reverbSettingsFile.empty()) {
        MT32Emu::DSeriesReverbSettings reverbSettings;
        std::string settingsError;
        if (!loadReverbSettings(reverbSettingsFile, reverbSettings, settingsError)) {
            std::fprintf(stderr, "%s\n", settingsError.c_str());
            return 1;
        }
        engine.setDSeriesReverbSettings(reverbSettings);
    }
    EngineConfig config;
    config.controlRom = roms[control].path;
    config.pcmRom = roms[pcm].path;
    config.analogMode = analog;
    config.outputSampleRate = rate;
    config.partialCount = partials;
    std::string error;
    if (!engine.configure(config, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    if (mt32) {
        engine.setMt32Translation(true);
        engine.mt32PowerOn();
    }
    for (const std::filesystem::path& file : sysexFiles) {
        if (!engine.loadSysexFile(file, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
    }

    EngineStatus status;
    engine.getStatus(status);
    std::printf("Output:  %u Hz, analog mode %s (requested %s)%s\n", rate, analogName(status.analogMode), analogName(analog),
                status.mt32Translation ? ", MT-32 translation" : "");
    std::printf("Parts:  ");
    for (int i = 0; i < int(status.partCount); i++) {
        const PartStatus& part = status.parts[i];
        const std::string label = i == kRhythmPart ? "R" : std::to_string(i < kRhythmPart ? i + 1 : i);
        std::printf(" %s=ch%s %s \"%s\" lvl%d pan%d%s", label.c_str(), part.channel < 16 ? std::to_string(part.channel + 1).c_str() : "off",
                    part.tone, part.name, part.temp[TimbreTemp::OutputLevel], part.temp[TimbreTemp::Panpot],
                    i + 1 < int(status.partCount) ? "," : "\n");
    }

    if (romSong > 0) {
        engine.loadMidi(romSongsToSmf({romSongs[size_t(romSong - 1)]}), romSongs[size_t(romSong - 1)].name, MidiFileKind::UnitSong);
    } else if (romSong == 0) {
        engine.loadMidi(romSongsToSmf(romSongs), "Chain of Songs", MidiFileKind::UnitSong);
    } else if (d20Track || d20Pattern >= 0) {
        const D20Rhythm rhythm = engine.d20Rhythm();
        const uint8_t channel = status.parts[kRhythmPart].channel < 16 ? status.parts[kRhythmPart].channel : 9;
        std::unique_ptr<SmfFile> smf = d20Track ? d20TrackToSmf(rhythm, channel, tempo) : d20PatternToSmf(rhythm, d20Pattern, channel, tempo);
        std::printf("D-20:    %s, %zu notes, %.2f s at %.0f BPM\n", d20Track ? ("rhythm track, " + std::to_string(rhythm.track.size()) + " bars").c_str()
                    : (d20PatternName(d20Pattern) + ", " + std::to_string(rhythm.patterns[size_t(d20Pattern)].beats) + "/4").c_str(),
                    smf->events.size() / 2, smf->duration, tempo);
        engine.loadMidi(std::move(smf), d20Track ? "D-20 rhythm track" : "D-20 pattern", MidiFileKind::UnitRhythm);
    } else if (test) {
        engine.loadMidi(makeTestPattern(status), "test pattern");
    } else if (!engine.loadMidiFile(positional[0], error)) {
        std::fprintf(stderr, "%s: %s\n", Platform::toUtf8(positional[0]).c_str(), error.c_str());
        return 1;
    }
    engine.playerPlay();

    const uint32_t blockFrames = 512;
    std::vector<float> block(2 * blockFrames);
    std::vector<int16_t> samples;
    double peak = 0.0;
    double sumSquares = 0.0;
    double tailRendered = 0.0;
    double songLength = 0.0;
    const double blockSeconds = double(blockFrames) / rate;
    const size_t maxSamples = size_t(rate) * 2 * 3600;  // One hour
    while (samples.size() < maxSamples) {
        engine.render(block.data(), blockFrames);
        for (float sample : block) {
            peak = std::max(peak, double(std::fabs(sample)));
            sumSquares += double(sample) * sample;
            samples.push_back(int16_t(std::lround(std::clamp(sample, -1.0f, 1.0f) * 32767.0f)));
        }
        engine.getStatus(status);
        if (status.player.state == MidiPlayer::State::Playing) {
            songLength = status.player.position;
        } else if ((tailRendered += blockSeconds) >= tail) {
            break;
        }
    }
    for (const std::string& line : engine.takeLog()) std::fprintf(stderr, "synth: %s\n", line.c_str());
    static const char* const mt32ReverbModes[] = {"room", "hall", "plate", "tap delay"};
    static const char* const d110ReverbModes[] = {"small room", "medium room", "medium hall", "large hall", "plate", "delay 1", "delay 2", "delay 3", "off"};
    std::printf("State:   master volume %d, reverb %s time %d level %d\n", status.masterVolume,
                status.d110 ? d110ReverbModes[std::min<int>(status.reverbMode, 8)] : mt32ReverbModes[std::min<int>(status.reverbMode, 3)],
                status.reverbTime + 1, status.reverbLevel);
    if (!dumpFile.empty()) {
        if (!engine.saveSysexFile(dumpFile, DumpEverything, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        std::printf("Dumped:  %s\n", Platform::toUtf8(dumpFile).c_str());
    }

    if (!writeWav(output, samples, rate)) {
        std::fprintf(stderr, "Cannot write %s\n", Platform::toUtf8(output).c_str());
        return 1;
    }
    const double rms = std::sqrt(sumSquares / std::max<size_t>(samples.size(), 1));
    auto dbfs = [](double v) { return v > 0.0 ? 20.0 * std::log10(v) : -INFINITY; };
    std::printf("Wrote:   %s, %.2f s (song %.2f s), peak %.1f dBFS, RMS %.1f dBFS, MIDI queue overflows %llu\n",
                Platform::toUtf8(output).c_str(), double(samples.size()) / 2.0 / rate, songLength, dbfs(peak), dbfs(rms),
                static_cast<unsigned long long>(status.queueOverflows));
    return 0;
}
