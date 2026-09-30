// Behaviour tests for the D-110 extensions, run against the real ROMs (no audio device needed).
// Usage: d110tests [--roms DIR]   Exit code 0 when all checks pass.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>
#include <mutex>
#include <iterator>
#include <functional>
#include <initializer_list>
#include <string>
#include <tuple>
#include <vector>

#include "AudioOutput.h"
#include "D20Rhythm.h"
#include "MidiPipe.h"
#include "PatternCapture.h"
#include "Mt32Presets.h"
#include "Mt32Translator.h"
#include "Platform.h"
#include "RomLibrary.h"
#include "ReverbKit.h"
#include "ReverbSettingsFile.h"
#include "RolandSysex.h"
#include "RomPlay.h"
#include "SynthEngine.h"
#include "ToneEditor.h"
#include "ToneEditorApp.h"
#include "ToneModel.h"
#include "miniaudio.h"
#include "MidiStreamParser.h"
#include "TextScreen.h"
#include "TranslatorTui.h"
#include "TuiApp.h"
#include "UnitLink.h"
#include "UnitSetup.h"

namespace {

int failures = 0;

void check(bool condition, const char* description) {
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", description);
    if (!condition) failures++;
}

struct Levels {
    double left = 0.0;
    double right = 0.0;
};

// Renders `seconds` of audio and returns the RMS of each channel.
Levels render(SynthEngine& engine, double seconds) {
    const uint32_t frames = uint32_t(seconds * 48000.0);
    std::vector<float> buffer(2 * 480);
    double left = 0.0;
    double right = 0.0;
    for (uint32_t done = 0; done < frames; done += 480) {
        engine.render(buffer.data(), 480);
        for (size_t i = 0; i < buffer.size(); i += 2) {
            left += double(buffer[i]) * buffer[i];
            right += double(buffer[i + 1]) * buffer[i + 1];
        }
    }
    return {std::sqrt(left / frames), std::sqrt(right / frames)};
}

void noteOn(SynthEngine& engine, int channel, int key, int velocity = 100) {
    engine.onMidiShortMessage(uint32_t(0x90 | channel) | (uint32_t(key) << 8) | (uint32_t(velocity) << 16));
}

void allOff(SynthEngine& engine) {
    engine.allNotesOff();
    render(engine, 1.5);  // Let releases and reverb die away
}

uint8_t readByte(SynthEngine& engine, uint32_t address) {
    uint8_t value = 0;
    engine.readMemory(address, 1, &value);
    return value;
}

// The 7-bit SysEx address `offset` bytes after `base` (both written like 0x030110).
uint32_t at(uint32_t base, uint32_t offset) {
    const uint32_t packed = ((base >> 16) << 14) + (((base >> 8) & 0x7F) << 7) + (base & 0x7F) + offset;
    return ((packed >> 14) << 16) | (((packed >> 7) & 0x7F) << 8) | (packed & 0x7F);
}

// Roland DT1 message for `device` (10H: the MT-32's unit #17), address as three 7-bit bytes (0x040000).
std::vector<uint8_t> dataSet(uint8_t device, uint32_t address, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> message = {0xF0, 0x41, device, 0x16, 0x12, uint8_t(address >> 16), uint8_t((address >> 8) & 0x7F), uint8_t(address & 0x7F)};
    message.insert(message.end(), data.begin(), data.end());
    unsigned sum = 0;
    for (size_t i = 5; i < message.size(); i++) sum += message[i];
    message.push_back(uint8_t((128 - sum % 128) % 128));
    message.push_back(0xF7);
    return message;
}

void sendSysex(SynthEngine& engine, const std::vector<uint8_t>& message) {
    engine.onMidiSysex(message.data(), message.size());
}

// An MT-32 timbre with one PCM partial (partial 1 of structure P+P; the others muted) at coarse pitch C4, sustained.
std::vector<uint8_t> mt32Timbre(const char* name, uint8_t wave, uint8_t waveform = 0) {
    static const uint8_t partial[58] = {
        36, 50, 11, 1, 0, 0, 0, 7,                                // WG: coarse C4, fine, key follow 1, bender on, waveform, wave, PW
        0, 0, 0, 0, 0, 0, 0, 50, 50, 50, 50, 50,                  // Pitch envelope (flat)
        0, 0, 0,                                                  // Pitch LFO off
        100, 0, 11, 0, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 100, 100, 100, 100,  // TVF (not used by PCM partials)
        100, 50, 0, 12, 0, 12, 0, 0, 0, 0, 0, 0, 0, 100, 100, 100, 100,   // TVA: full level, instant attack, sustained
    };
    std::vector<uint8_t> timbre(246, 0);
    std::memset(timbre.data(), ' ', 10);
    std::memcpy(timbre.data(), name, std::min<size_t>(std::strlen(name), 10));
    timbre[10] = 5;  // Structure 6 (PCM + PCM) for partials 1-2
    timbre[11] = 0;
    timbre[12] = 1;  // Only partial 1 plays
    for (size_t i = 0; i < 4; i++) std::memcpy(&timbre[14 + i * 58], partial, 58);
    timbre[14 + 4] = waveform;
    timbre[14 + 5] = wave;
    return timbre;
}

// Fundamental frequency (Hz) of `seconds` of rendered audio, measured after the attack, or 0 if none found.
double measurePitch(SynthEngine& engine, double seconds) {
    const uint32_t frames = uint32_t(seconds * 48000.0);
    std::vector<float> buffer(2 * 480);
    std::vector<double> mono;
    for (uint32_t done = 0; done < frames; done += 480) {
        engine.render(buffer.data(), 480);
        for (size_t i = 0; i < buffer.size(); i += 2) mono.push_back(double(buffer[i]) + buffer[i + 1]);
    }
    const size_t start = mono.size() / 3, window = 4096;
    const int minLag = 40, maxLag = 960;  // 50-1200 Hz
    if (mono.size() < start + window + size_t(maxLag) + 2) return 0.0;
    // YIN: cumulative mean normalised difference, first dip below 0.2 (else the deepest).
    std::vector<double> cmnd(size_t(maxLag) + 2, 1.0);
    double running = 0.0;
    for (int lag = 1; lag <= maxLag + 1; lag++) {
        double d = 0.0;
        for (size_t i = 0; i < window; i++) {
            const double diff = mono[start + i] - mono[start + i + size_t(lag)];
            d += diff * diff;
        }
        running += d;
        cmnd[size_t(lag)] = running > 0.0 ? d * lag / running : 1.0;
    }
    int best = -1;
    for (int lag = minLag; lag <= maxLag; lag++) {
        if (cmnd[size_t(lag)] < 0.2 && cmnd[size_t(lag)] <= cmnd[size_t(lag) + 1]) {
            best = lag;
            break;
        }
    }
    if (best < 0) {
        best = minLag;
        for (int lag = minLag; lag <= maxLag; lag++) {
            if (cmnd[size_t(lag)] < cmnd[size_t(best)]) best = lag;
        }
    }
    const double a = cmnd[size_t(best) - 1], b = cmnd[size_t(best)], c = cmnd[size_t(best) + 1];
    const double shift = (a - 2 * b + c) != 0.0 ? 0.5 * (a - c) / (a - 2 * b + c) : 0.0;
    return 48000.0 / (best + shift);
}

// The pitch (Hz) every 5 ms of `seconds` of rendered audio, from 20 ms windows, for a note of 200-800 Hz.
std::vector<double> pitchTrack(SynthEngine& engine, double seconds) {
    const uint32_t frames = uint32_t(seconds * 48000.0);
    std::vector<float> buffer(2 * 480);
    std::vector<double> mono;
    for (uint32_t done = 0; done < frames; done += 480) {
        engine.render(buffer.data(), 480);
        for (size_t i = 0; i < buffer.size(); i += 2) mono.push_back(double(buffer[i]) + buffer[i + 1]);
    }
    const size_t window = 960;
    const int minLag = 60, maxLag = 240;
    std::vector<double> track;
    std::vector<double> cmnd(size_t(maxLag) + 2, 1.0);
    for (size_t at = 0; at + window + size_t(maxLag) + 1 < mono.size(); at += 240) {
        // YIN, as measurePitch: the first dip below 0.2, as a period's multiples dip as deep; else the deepest.
        double running = 0.0;
        for (int lag = 1; lag <= maxLag + 1; lag++) {
            double sum = 0.0;
            for (size_t i = 0; i < window; i++) {
                const double diff = mono[at + i] - mono[at + i + size_t(lag)];
                sum += diff * diff;
            }
            running += sum;
            cmnd[size_t(lag)] = running > 0.0 ? sum * lag / running : 1.0;
        }
        int best = -1;
        for (int lag = minLag; lag <= maxLag; lag++) {
            if (cmnd[size_t(lag)] < 0.2 && cmnd[size_t(lag)] <= cmnd[size_t(lag) - 1] && cmnd[size_t(lag)] <= cmnd[size_t(lag) + 1]) {
                best = lag;
                break;
            }
        }
        if (best < 0) {
            best = minLag;
            for (int lag = minLag; lag <= maxLag; lag++) {
                if (cmnd[size_t(lag)] < cmnd[size_t(best)]) best = lag;
            }
        }
        const double a = cmnd[size_t(best) - 1], b = cmnd[size_t(best)], c = cmnd[size_t(best) + 1];
        const double shift = (a - 2 * b + c) != 0.0 ? 0.5 * (a - c) / (a - 2 * b + c) : 0.0;
        track.push_back(48000.0 / (best + shift));
    }
    return track;
}

// How far a pitch track goes, top to bottom, in cents.
double pitchSpreadCents(const std::vector<double>& track) {
    if (track.empty()) return 0.0;
    const auto range = std::minmax_element(track.begin(), track.end());
    return 1200.0 * std::log2(*range.second / *range.first);
}

// The rate (Hz) at which a pitch track (5 ms steps) goes up and down: the first peak of its autocorrelation.
double modulationRate(const std::vector<double>& track) {
    const size_t n = track.size();
    if (n < 8) return 0.0;
    double mean = 0.0;
    for (double f : track) mean += f;
    mean /= double(n);
    const auto r = [&](size_t lag) {
        double sum = 0.0;
        for (size_t i = 0; i + lag < n; i++) sum += (track[i] - mean) * (track[i + lag] - mean);
        return sum / double(n - lag);
    };
    size_t lag = 1;
    while (lag < n / 2 && r(lag) > 0.0) lag++;  // Past the first zero
    while (lag + 1 < n / 2 && !(r(lag) >= r(lag - 1) && r(lag) >= r(lag + 1))) lag++;
    return 1.0 / (double(lag) * 0.005);
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path romFolder;
    for (int i = 1; i + 1 < argc; i++) {
        if (std::string(argv[i]) == "--roms") romFolder = Platform::fromUtf8(argv[++i]);
    }
    if (romFolder.empty()) romFolder = findRomFolder({Platform::executableDirectory(), std::filesystem::current_path()});
    const std::vector<RomEntry> roms = scanRomFolder(romFolder);
    int control = -1;
    int pcm = -1;
    if (!pickDefaultRoms(roms, control, pcm) || romFamily(roms[control]) != "d110") {
        std::fprintf(stderr, "D-110 ROMs not found in \"%s\"\n", Platform::toUtf8(romFolder).c_str());
        return 2;
    }

    SynthEngine engine;
    EngineOptions options;
    options.midiTimestamping = false;
    options.controlChannel = 15;  // MIDI channel 16
    engine.setOptions(options);
    EngineConfig config;
    config.controlRom = roms[control].path;
    config.pcmRom = roms[pcm].path;
    std::string error;
    if (!engine.configure(config, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 2;
    }
    EngineStatus status;
    engine.getStatus(status);
    check(status.d110, "D-110 mode is active for the D-110 control ROM");
    bool reservesAsSet = true;
    for (uint32_t i = 0; i < status.partCount; i++) reservesAsSet = reservesAsSet && status.parts[i].reservedPartials == status.parts[i].reserve;
    check(reservesAsSet, "32 partials: every part reserves exactly its setting, as on the unit");
    check(status.parts[0].channel == 0 && status.parts[7].channel == 7 && status.parts[kRhythmPart].channel == 9,
          "power-up channels: parts 1-8 on 1-8, rhythm on 10");

    // Pan: D-110 panpot 0 is hard left.
    engine.setPartParameter(0, TimbreTemp::Panpot, 0);
    render(engine, 0.05);
    noteOn(engine, 0, 60);
    Levels levels = render(engine, 0.5);
    check(levels.left > 4.0 * levels.right, "panpot 0 plays on the left");
    allOff(engine);
    engine.setPartParameter(0, TimbreTemp::Panpot, 14);
    render(engine, 0.05);
    noteOn(engine, 0, 60);
    levels = render(engine, 0.5);
    check(levels.right > 4.0 * levels.left, "panpot 14 plays on the right");
    allOff(engine);
    engine.setPartParameter(0, TimbreTemp::Panpot, 7);

    // Mute and solo (the Play tab): a muted part's partials are left out of the mix at once, sounding notes too.
    engine.setPartParameter(0, TimbreTemp::OutputAssign, 0);  // Dry: no reverb tail to wait for
    render(engine, 0.05);
    noteOn(engine, 0, 60);
    const Levels heard = render(engine, 0.3);
    engine.setMutedParts(1u << 0);
    render(engine, 0.02);
    const Levels muted = render(engine, 0.2);
    engine.setMutedParts(1u << 1);  // Another part muted: part 1 is heard again
    render(engine, 0.02);
    const Levels unmuted = render(engine, 0.2);
    engine.setMutedParts(0);
    allOff(engine);
    engine.setPartParameter(0, TimbreTemp::OutputAssign, 1);
    std::printf("      mute: heard %.2e, muted %.2e, unmuted %.2e\n", heard.left + heard.right, muted.left + muted.right,
                unmuted.left + unmuted.right);
    check(heard.left + heard.right > 1e-3 && muted.left + muted.right < 1e-6 && unmuted.left + unmuted.right > 1e-4,
          "a muted part falls silent at once, with its sounding notes, and is heard again when unmuted");

    // Key range: notes outside the part's range are ignored.
    engine.setPartParameter(0, TimbreTemp::KeyRangeLower, 62);
    const Levels baseline = render(engine, 0.3);  // Whatever reverb tail is left
    noteOn(engine, 0, 60);
    const Levels outside = render(engine, 0.3);
    noteOn(engine, 0, 64);
    const Levels inside = render(engine, 0.3);
    std::printf("      key range levels: baseline %.2e, outside %.2e, inside %.2e\n", baseline.left + baseline.right,
                outside.left + outside.right, inside.left + inside.right);
    check(outside.left + outside.right < 0.01 * (inside.left + inside.right), "note below the key range is silent");
    check(inside.left + inside.right > 1e-3, "note inside the key range sounds");
    allOff(engine);
    engine.setPartParameter(0, TimbreTemp::KeyRangeLower, 0);

    // MIDI volume is separate from the output level on the D-110.
    const uint8_t levelBefore = readByte(engine, 0x030008);
    engine.onMidiShortMessage(0xB0 | (7 << 8) | (64 << 16));
    render(engine, 0.02);
    engine.getStatus(status);
    check(readByte(engine, 0x030008) == levelBefore && status.parts[0].midiVolume == 50,
          "CC 7 sets MIDI volume, not the output level");
    engine.onMidiShortMessage(0xB0 | (121 << 8));
    render(engine, 0.02);

    // Patch write (40 02 00) and recall, including by program change on the control channel.
    engine.setPartParameter(0, TimbreTemp::OutputLevel, 55);
    const uint8_t writePatch5[2] = {5, 0};
    engine.writeData(0x400200, writePatch5, 2);
    render(engine, 0.02);
    engine.setPartParameter(0, TimbreTemp::OutputLevel, 90);
    render(engine, 0.02);
    engine.onMidiShortMessage(0xCF | (5 << 8));  // Program change 6 on channel 16
    render(engine, 0.02);
    engine.getStatus(status);
    check(status.currentPatch == 5 && status.parts[0].temp[TimbreTemp::OutputLevel] == 55,
          "control-channel program change recalls the written patch");

    // Timbre write (40 01 00 + 2 * part) copies the part's timbre into timbre memory.
    engine.setPartParameter(1, TimbreTemp::KeyShift, 30);
    const uint8_t writeTimbre10[2] = {10, 0};
    engine.writeData(0x400102, writeTimbre10, 2);
    render(engine, 0.02);
    uint8_t timbre[8] = {};
    engine.readMemory(0x050000 + 10 * 8, 8, timbre);
    uint8_t part2[8] = {};
    engine.readMemory(0x030010, 8, part2);
    check(std::memcmp(timbre, part2, 7) == 0 && timbre[2] == 30, "timbre write stores the part's timbre");

    // Tone write (40 00 00 + 2 * part) stores the part's tone as internal tone i05.
    const uint8_t writeTone4[2] = {4, 0};
    engine.writeData(0x400000, writeTone4, 2);
    render(engine, 0.02);
    const std::vector<std::string> names = engine.toneNames();
    engine.getStatus(status);
    check(names.size() == 448 && names[128 + 4] == std::string(status.parts[0].name) && status.parts[0].tone == std::string("i15"),
          "tone write stores the tone in memory and points the part at it");

    // Reverb types: OFF and Delay 3 must not crash (mode indexes past the MT-32 reverb models).
    engine.setReverb(8, 4, 5);
    render(engine, 0.05);
    engine.setReverb(7, 4, 5);
    noteOn(engine, 0, 60);
    levels = render(engine, 0.3);
    engine.getStatus(status);
    check(status.reverbMode == 7 && levels.left > 0.0, "reverb types 7 (Delay 3) and 8 (off) are accepted");
    allOff(engine);

    // The D-series reverb (types 1-8, tunable in d110emu-reverb.ini): a noise burst with the reverb, less the same burst
    // dry, is the reverb alone. Its decay follows the RT60 set for the Reverb Time, its level the wet level set for the
    // Reverb Level, and a delay's taps arrive when set.
    {
        MT32Emu::DSeriesReverbSettings settings = MT32Emu::DSeriesReverbSettings::getDefaults();
        MT32Emu::DSeriesReverbType& hall = settings.types[2];  // Medium Hall, nearly undamped: the RT60 for all frequencies
        hall.dampingHz = 16000.0f;
        hall.bandwidthHz = 16000.0f;
        hall.rt60[3] = 1.0f;  // Reverb Time 4
        MT32Emu::DSeriesReverbType& delay2 = settings.types[6];
        delay2.delayLMs[3] = 120.0f;
        delay2.delayRMs[3] = 200.0f;
        delay2.feedback = 0.0f;
        SynthEngine reverb;
        reverb.setOptions(options);
        reverb.setDSeriesReverbSettings(settings);
        reverb.configure(config, error);
        std::vector<uint8_t> noise = mt32Timbre("Burst", 110);  // Noise (Loop), sustained until the note-off
        reverb.writeData(0x040000, noise.data(), noise.size());
        const uint8_t mixReverb = 1;
        reverb.writeData(0x030006, &mixReverb, 1);  // Part 1: output Mix + reverb
        auto capture = [&](double seconds) {
            std::vector<float> samples(size_t(seconds * 48000.0) * 2);
            for (size_t done = 0; done < samples.size(); done += 960) reverb.render(samples.data() + done, 480);
            return samples;
        };
        auto burst = [&](int type, int time, int level) {
            reverb.setReverb(type, time - 1, level);
            capture(0.2);
            noteOn(reverb, 0, 60, 127);
            std::vector<float> samples = capture(0.02);
            reverb.onMidiShortMessage(0x3C80);
            const std::vector<float> rest = capture(3.0);
            samples.insert(samples.end(), rest.begin(), rest.end());
            capture(3.0);  // The tail dies away
            return samples;
        };
        const std::vector<float> dry = burst(8, 1, 0);
        auto wetOf = [&](const std::vector<float>& mixed) {
            std::vector<float> wet(mixed.size());
            for (size_t i = 0; i < mixed.size(); i++) wet[i] = mixed[i] - dry[i];
            return wet;
        };
        auto energy = [](const std::vector<float>& x) {
            double sum = 0.0;
            for (float v : x) sum += double(v) * v;
            return sum;
        };
        // RT60 from the Schroeder curve's -5 to -25 dB.
        auto rt60 = [](const std::vector<float>& wet) {
            std::vector<double> edc(wet.size() / 2 + 1, 0.0);
            for (size_t i = wet.size() / 2; i-- > 0;) edc[i] = edc[i + 1] + double(wet[2 * i]) * wet[2 * i] + double(wet[2 * i + 1]) * wet[2 * i + 1];
            double t5 = -1.0, t25 = -1.0;
            for (size_t i = 0; i < edc.size() && edc[0] > 0.0; i++) {
                const double db = 10.0 * std::log10(edc[i] / edc[0] + 1e-30);
                if (t5 < 0.0 && db <= -5.0) t5 = double(i) / 48000.0;
                if (t25 < 0.0 && db <= -25.0) t25 = double(i) / 48000.0;
            }
            return t5 >= 0.0 && t25 > t5 ? (t25 - t5) * 3.0 : 0.0;
        };
        const std::vector<float> hallWet = wetOf(burst(2, 4, 7));
        const double hallRt60 = rt60(hallWet);
        const std::vector<float> quietWet = wetOf(burst(2, 4, 1));
        const double levelStep = 10.0 * std::log10(energy(hallWet) / energy(quietWet));
        // Delay 2 at Reverb Time 4: the first echo left at 120 ms, right at 200 ms.
        const std::vector<float> delayWet = wetOf(burst(6, 4, 7));
        auto firstArrival = [&](int channel) {
            double loudest = 0.0;
            for (size_t i = size_t(channel); i < delayWet.size(); i += 2) loudest = std::max(loudest, double(std::fabs(delayWet[i])));
            for (size_t i = size_t(channel); i < delayWet.size(); i += 2) {
                if (std::fabs(delayWet[i]) > loudest * 0.3) return double(i / 2) * 1000.0 / 48000.0;
            }
            return -1.0;
        };
        const double delayL = firstArrival(0), delayR = firstArrival(1);
        // The MT-32 chip model (mt32emu's) instead: its Hall rings far longer.
        EngineOptions muntOptions = options;
        muntOptions.dSeriesReverb = false;
        reverb.setOptions(muntOptions);
        const double muntRt60 = rt60(wetOf(burst(2, 4, 7)));
        reverb.setOptions(options);
        std::printf("      D-series reverb: Medium Hall RT60 %.2f s (set 1.00), Level 7 against 1 %.1f dB (the D-20's 24.2), Delay 2 taps "
                    "%.1f/%.1f ms (set 120/200); MT-32 chip Hall %.2f s\n", hallRt60, levelStep, delayL, delayR, muntRt60);
        check(std::fabs(hallRt60 - 1.0) < 0.15 && std::fabs(levelStep - 24.2) < 0.5 && std::fabs(delayL - 120.0) < 3.0 &&
                  std::fabs(delayR - 200.0) < 3.0 && muntRt60 > hallRt60 * 1.5,
              "D-series reverb: the RT60 set for the Reverb Time, the wet level for the Reverb Level, a delay's taps; the "
              "MT-32 chip model is still there");

        // The settings file, and the recording kit's two files in step.
        const std::filesystem::path settingsFile = std::filesystem::temp_directory_path() / "d110tests-reverb.ini";
        MT32Emu::DSeriesReverbSettings loaded;
        std::string fileError;
        const bool fileRoundTrip = saveReverbSettings(settingsFile, settings, fileError) && loadReverbSettings(settingsFile, loaded, fileError) &&
                                   loaded.types[2].rt60[3] == 1.0f && loaded.types[2].dampingHz == 16000.0f &&
                                   loaded.types[6].delayRMs[3] == 200.0f && loaded.types[0].wetDb[7] == settings.types[0].wetDb[7] &&
                                   std::fabs(loaded.types[4].combMs[3] - settings.types[4].combMs[3]) < 0.001f;
        std::filesystem::remove(settingsFile);
        std::vector<ReverbKitSegment> d110Schedule, d20Schedule;
        const std::unique_ptr<SmfFile> d110Kit = reverbKitSmf(ReverbKitTarget::D110, d110Schedule);
        const std::unique_ptr<SmfFile> d20Kit = reverbKitSmf(ReverbKitTarget::D20, d20Schedule);
        auto noteTimes = [](const SmfFile& smf) {
            std::vector<double> times;
            for (const SmfEvent& event : smf.events) {
                if ((event.shortMessage & 0xF0u) == 0x90u) times.push_back(event.time);
            }
            return times;
        };
        int bursts = 0;
        for (const ReverbKitSegment& segment : d110Schedule) bursts += segment.kind == "burst" ? 1 : 0;
        const bool kit = d110Schedule.size() == 3 + 64 + 8 + 8 && bursts == 72 && noteTimes(*d110Kit) == noteTimes(*d20Kit) &&
                         d110Kit->duration < 490.0;
        check(fileRoundTrip && kit, "D-series reverb: settings file round trip; the recording kit's D-110 and D-20 files keep the same time");
    }

    // Master tune (system area 00, the System tab's slider): A4 = 440 Hz x 2^((value - 64) / 1536), 440.0 Hz (40H) by default
    // as on the D-110 and D-10/D-20 (the MT-32's 442.0 Hz is 4AH). A steady C4 (the Slap Bass loop at coarse 48, 261.6 Hz at
    // 440 Hz) moves by +7.8 cents at 4AH, -50.0 at 0 and +49.2 at 127.
    auto tuneTestEngine = [&](SynthEngine& engine, const EngineOptions& engineOptions) {
        engine.setOptions(engineOptions);
        engine.configure(config, error);
        std::vector<uint8_t> tone = mt32Timbre("Tune test", 81);
        tone[14] = 48;
        engine.writeData(0x040000, tone.data(), tone.size());
    };
    auto measureC4 = [&](SynthEngine& engine) {
        render(engine, 0.02);
        noteOn(engine, 0, 60);
        const double hz = measurePitch(engine, 0.6);
        engine.allNotesOff();
        render(engine, 0.3);
        return hz;
    };
    {
        SynthEngine tuned;
        tuneTestEngine(tuned, options);
        EngineStatus tunedStatus;
        tuned.getStatus(tunedStatus);
        const bool defaultTune = tunedStatus.masterTune == 0x40;
        const double powerOn = measureC4(tuned);
        auto pitchAt = [&](int value) {
            tuned.setMasterTune(value);
            return measureC4(tuned);
        };
        const double normal = pitchAt(0x40), mt32 = pitchAt(0x4A), low = pitchAt(0), high = pitchAt(127);
        tuned.getStatus(tunedStatus);
        std::printf("      master tune: C4 %.2f Hz at power-on, %.2f Hz at 64, %.2f Hz at 74 (ratio %.4f), %.2f Hz at 0 (ratio %.4f), "
                    "%.2f Hz at 127 (ratio %.4f)\n", powerOn, normal, mt32, mt32 / normal, low, low / normal, high, high / normal);
        check(defaultTune && tunedStatus.masterTune == 127 && std::fabs(powerOn / 261.6 - 1.0) < 0.01 &&
                  std::fabs(normal / powerOn - 1.0) < 0.001 && std::fabs(mt32 / normal - std::exp2(10.0 / 1536.0)) < 0.002 &&
                  std::fabs(low / normal - std::exp2(-64.0 / 1536.0)) < 0.002 && std::fabs(high / normal - std::exp2(63.0 / 1536.0)) < 0.002,
              "master tune: the system area's byte, 440 Hz (40H) by default as on the D-110 and D-10/D-20, tunes the unit from "
              "427.5 to 452.7 Hz");
    }

    // GS master tune (MIDI extensions): 41 10 42 12 40 00 00 and four nibbles, 0018H-07E8H = -100.0..+100.0 cents, on top
    // of the unit's own master tune (the SC-88Pro's MIDI implementation: 442.0 Hz is 00 04 04 0F, +7.9 cents). GS, GM and
    // XG resets end it, as Reset MIDI and switching the extensions off do; the memory keeps the unit's own tune.
    {
        auto gsMasterTune = [](int value, bool damaged = false) {
            std::vector<uint8_t> message = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x00, uint8_t((value >> 12) & 15),
                                            uint8_t((value >> 8) & 15), uint8_t((value >> 4) & 15), uint8_t(value & 15)};
            message.push_back(uint8_t(RolandSysex::checksum(message.data() + 5, 7) ^ (damaged ? 1 : 0)));
            message.push_back(0xF7);
            return message;
        };
        const std::vector<uint8_t> gsReset = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7};
        const std::vector<uint8_t> gmOn = {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7};
        SynthEngine gs;
        tuneTestEngine(gs, options);
        const double base = measureC4(gs);
        gs.applySysex(gsMasterTune(0x44F));
        const double at442 = measureC4(gs);
        EngineStatus gsStatus;
        gs.getStatus(gsStatus);
        const bool reported = gsStatus.gsMasterTune == 79 && gsStatus.masterTune == 0x40;
        gs.applySysex(gsMasterTune(0x7E8));
        const double up = measureC4(gs);
        gs.applySysex(gsMasterTune(0x7FF));  // Beyond the range: +100.0 cents
        const double clamped = measureC4(gs);
        gs.applySysex(gsMasterTune(0x018));
        const double down = measureC4(gs);
        gs.applySysex(gsReset);
        const double afterGsReset = measureC4(gs);
        gs.applySysex(gsMasterTune(0x44F, true));  // Damaged checksum: ignored
        const double damaged = measureC4(gs);
        gs.applySysex(gsMasterTune(0x44F));
        gs.applySysex(gmOn);
        const double afterGmOn = measureC4(gs);
        gs.applySysex(gsMasterTune(0x44F));
        gs.resetMidiChannels();
        const double afterResetMidi = measureC4(gs);
        gs.applySysex(gsMasterTune(0x44F));
        EngineOptions noExtensions = options;
        noExtensions.midiExtensions = false;
        gs.setOptions(noExtensions);
        const double extensionsOff = measureC4(gs);
        gs.applySysex(gsMasterTune(0x44F));
        const double ignoredWithout = measureC4(gs);
        std::printf("      GS master tune: C4 %.2f Hz, %.2f at +7.9 cents (ratio %.5f), %.2f at +100 (%.4f), %.2f beyond (%.4f), "
                    "%.2f at -100 (%.4f)\n", base, at442, at442 / base, up, up / base, clamped, clamped / base, down, down / base);
        auto same = [&](double hz) { return std::fabs(hz / base - 1.0) < 0.0005; };
        check(reported && std::fabs(at442 / base - std::exp2(79.0 / 12000.0)) < 0.0015 &&
                  std::fabs(up / base - std::exp2(1.0 / 12.0)) < 0.002 && std::fabs(clamped / up - 1.0) < 0.0005 &&
                  std::fabs(down / base - std::exp2(-1.0 / 12.0)) < 0.002,
              "GS master tune (MIDI extensions): 442.0 Hz as the SC-88Pro's table sends it, -100 to +100 cents, on top of the "
              "unit's master tune (which stays 440.0 Hz in memory)");
        check(same(afterGsReset) && same(damaged) && same(afterGmOn) && same(afterResetMidi) && same(extensionsOff) && same(ignoredWithout),
              "GS master tune: ended by GS Reset, GM System On, Reset MIDI and switching the extensions off; ignored when damaged "
              "or without the extensions");
    }

    // Display: 32 characters, then display reset.
    const char hello[] = "Hello D-110";
    engine.writeData(0x200000, reinterpret_cast<const uint8_t*>(hello), sizeof(hello) - 1);
    render(engine, 0.02);
    engine.getStatus(status);
    check(status.lcdMessageShown && status.lcdMessage.compare(0, 11, "Hello D-110") == 0, "display write shows text");
    const uint8_t zero = 0;
    engine.writeData(0x200100, &zero, 1);
    render(engine, 0.02);
    engine.getStatus(status);
    check(!status.lcdMessageShown, "display reset returns to the normal display");

    // The MT-32 reset address does nothing on the D-110 (it would also wipe the user's memory).
    const uint8_t channels[kBasePartCount] = {3, 3, 3, 3, 3, 3, 3, 3, 9};
    engine.writeData(0x10000D, channels, kBasePartCount);
    engine.writeData(0x7F0000, &zero, 1);
    render(engine, 0.02);
    engine.getStatus(status);
    check(status.parts[0].channel == 3, "MT-32 reset (7F 00 00) is ignored");

    // RPN 0 (pitch bend range) and, as an extension, RPN 2 (coarse tune -> key shift).
    engine.setPartChannels({0, 1, 2, 3, 4, 5, 6, 7, 9, 8, 10, 11, 12, 13, 14, 15});
    auto controlChange = [&](int channel, int controller, int value) {
        engine.onMidiShortMessage(uint32_t(0xB0 | channel) | (uint32_t(controller) << 8) | (uint32_t(value) << 16));
    };
    const uint8_t timbreBender = readByte(engine, 0x030004);
    controlChange(0, 101, 0);
    controlChange(0, 100, 0);
    controlChange(0, 6, 7);
    controlChange(0, 100, 2);
    controlChange(0, 6, 64 + 5);
    render(engine, 0.02);
    engine.getStatus(status);
    check(timbreBender != 7 && status.parts[0].bendRangeCents == 700 && readByte(engine, 0x030004) == timbreBender &&
              readByte(engine, 0x030002) == 24 + 5,
          "RPN 0 sets the channel's pitch bend range (MIDI extensions: the timbre's stays), RPN 2 the key shift");
    controlChange(0, 6, 64);
    controlChange(0, 101, 127);
    controlChange(0, 100, 127);

    // CC 74 (brightness) moves the filter cutoff of a synth tone. The LA32 filter also attenuates as it closes,
    // so a closed filter is much quieter than an open one.
    engine.setPartParameter(0, TimbreTemp::ToneGroup, 0);
    engine.setPartParameter(0, TimbreTemp::ToneNumber, 50);  // a73, a synth tone
    render(engine, 0.05);
    auto levelAt = [&](int value) {
        controlChange(0, 74, value);
        noteOn(engine, 0, 48);
        render(engine, 0.2);
        const Levels sounding = render(engine, 0.3);
        allOff(engine);
        return sounding.left + sounding.right;
    };
    const double closed = levelAt(10);
    const double open = levelAt(120);
    std::printf("      level with CC 74 = 10: %.2e, CC 74 = 120: %.2e\n", closed, open);
    check(open > 3.0 * closed, "CC 74 opens and closes the filter of a synth tone");
    controlChange(0, 74, 64);

    // GM System On resets the channels (here: MIDI volume), and so does the reset button.
    controlChange(0, 7, 64);
    render(engine, 0.02);
    const uint8_t gmOn[] = {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7};
    engine.onMidiSysex(gmOn, sizeof(gmOn));
    render(engine, 0.02);
    engine.getStatus(status);
    const bool gmReset = status.parts[0].midiVolume == 100;
    controlChange(0, 7, 64);
    render(engine, 0.02);
    engine.resetMidiChannels();
    engine.getStatus(status);
    check(gmReset && status.parts[0].midiVolume == 100, "GM System On and Reset MIDI restore the channels' controllers");

    // With the extensions off, the unit's own behaviour: CC 74 and GM System On are ignored.
    EngineOptions strict = engine.options();
    strict.midiExtensions = false;
    engine.setOptions(strict);
    controlChange(0, 7, 64);
    render(engine, 0.02);
    engine.onMidiSysex(gmOn, sizeof(gmOn));
    render(engine, 0.02);
    engine.getStatus(status);
    check(status.parts[0].midiVolume == 50, "without MIDI extensions a GM reset is ignored");
    strict.midiExtensions = true;
    engine.setOptions(strict);
    controlChange(0, 121, 0);
    engine.setPartParameter(0, TimbreTemp::ToneGroup, 0);
    engine.setPartParameter(0, TimbreTemp::ToneNumber, 0);
    render(engine, 0.02);

    // Reset MIDI, and GM and GS resets with the extensions, put every part's level at 100 and its pan at the centre (the
    // fine pan dropped), as GM and GS modules reset them; the rhythm keys keep their own. Without the extensions a GM
    // reset leaves them.
    {
        const uint8_t gsReset[] = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7};
        const uint32_t kickPan = at(0x030110, (36 - 24) * 4 + 2);  // The bass drum's pan in the rhythm setup
        const uint8_t kickPanBefore = readByte(engine, kickPan);
        auto moveMix = [&] {
            engine.setPartParameter(0, TimbreTemp::OutputLevel, 55);
            engine.setPartParameter(0, TimbreTemp::Panpot, 2);
            controlChange(2, 10, 100);  // Part 3: its pan and fine pan
            engine.setPartParameter(kRhythmPart, TimbreTemp::OutputLevel, 60);
            render(engine, 0.02);
            engine.getStatus(status);
            return status.parts[0].temp[TimbreTemp::OutputLevel] == 55 && status.parts[0].temp[TimbreTemp::Panpot] == 2 &&
                   status.parts[2].finePanSet && status.parts[kRhythmPart].temp[TimbreTemp::OutputLevel] == 60;
        };
        auto mixReset = [&] {
            engine.getStatus(status);
            bool reset = readByte(engine, kickPan) == kickPanBefore;
            for (uint32_t i = 0; i < status.partCount; i++) {
                reset = reset && status.parts[i].temp[TimbreTemp::OutputLevel] == 100 && status.parts[i].temp[TimbreTemp::Panpot] == 7 &&
                        !status.parts[i].finePanSet;
            }
            return reset;
        };
        const bool moved = moveMix();
        engine.resetMidiChannels();
        const bool button = mixReset();
        moveMix();
        engine.onMidiSysex(gmOn, sizeof(gmOn));
        render(engine, 0.02);
        const bool gm = mixReset();
        moveMix();
        engine.onMidiSysex(gsReset, sizeof(gsReset));
        render(engine, 0.02);
        const bool gs = mixReset();
        strict.midiExtensions = false;
        engine.setOptions(strict);
        moveMix();
        engine.onMidiSysex(gmOn, sizeof(gmOn));
        render(engine, 0.02);
        engine.getStatus(status);
        const bool kept = status.parts[0].temp[TimbreTemp::OutputLevel] == 55 && status.parts[0].temp[TimbreTemp::Panpot] == 2 &&
                          status.parts[2].finePanSet;
        strict.midiExtensions = true;
        engine.setOptions(strict);
        engine.resetMidiChannels();
        std::printf("      levels and pans reset: Reset MIDI %d, GM System On %d, GS Reset %d; kept without the extensions %d\n", button,
                    gm, gs, kept);
        check(moved && button && gm && gs && kept,
              "Reset MIDI and GM and GS resets put every part's level at 100 and pan at the centre (fine pans dropped, the rhythm "
              "keys' own kept); without the extensions a GM reset leaves them");
    }

    // The MIDI extensions as General MIDI modules have them: a GM or GS stream keeps the part channels, the pitch bend
    // range is the MIDI channel's, and GS NRPNs and GM2 sound controllers move a part's envelope times and vibrato.
    {
        SynthEngine gm;
        gm.setOptions(options);
        std::string gmError;
        const bool started = gm.configure(config, gmError);
        check(started, "a second synth starts for the MIDI extensions' tests");
        EngineStatus st;
        auto cc = [&](int controller, int value) { gm.onMidiShortMessage(0xB0u | (uint32_t(controller) << 8) | (uint32_t(value) << 16)); };
        auto rpn = [&](int number, int msb, int lsb) {  // lsb < 0: none
            cc(101, 0);
            cc(100, number);
            cc(6, msb);
            if (lsb >= 0) cc(38, lsb);
            cc(101, 127);
            cc(100, 127);
        };
        auto nrpn = [&](int msb, int lsb, int value) {
            cc(99, msb);
            cc(98, lsb);
            cc(6, value);
            cc(101, 127);
            cc(100, 127);
        };
        auto channels = [&]() {
            render(gm, 0.02);
            gm.getStatus(st);
            std::vector<int> list;
            for (int part = 0; part < kBasePartCount; part++) list.push_back(st.parts[part].channel);
            return list;
        };
        const uint8_t gmOn[] = {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7};
        const std::vector<uint8_t> gsReset = {0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7};
        const std::vector<int> start = channels();
        const std::vector<int> unitChannels = {0, 1, 2, 3, 4, 5, 6, 7, 9};

        // As thunderforce5.mid (made for the SC-88) does: GM System On and GS Reset, then SysEx for an MT-32 beside the
        // SC-88 that turns its parts 1-8 and rhythm off, one channel at a time. Played from a file.
        auto smfOf = [](const std::vector<std::vector<uint8_t>>& messages) {
            std::unique_ptr<SmfFile> smf(new SmfFile);
            double time = 0.0;
            for (const std::vector<uint8_t>& message : messages) {
                smf->events.push_back({time, 0, uint32_t(smf->sysexData.size()), uint32_t(message.size())});
                smf->sysexData.insert(smf->sysexData.end(), message.begin(), message.end());
                time += 0.02;
            }
            smf->duration = time;
            return smf;
        };
        std::vector<std::vector<uint8_t>> gsFile = {std::vector<uint8_t>(gmOn, gmOn + sizeof(gmOn)), gsReset};
        for (uint32_t part = 0; part < uint32_t(kBasePartCount); part++) gsFile.push_back(dataSet(0x10, 0x10000D + part, {0x10}));
        gm.takeLog();
        gm.loadMidi(smfOf(gsFile), "GS file");
        gm.playerPlay();
        render(gm, 0.4);
        const std::vector<std::string> gsLog = gm.takeLog();
        const std::vector<int> afterGsFile = channels();
        const bool fileKept = afterGsFile == unitChannels;
        const long logged = std::count_if(gsLog.begin(), gsLog.end(), [](const std::string& line) { return line.find("Kept the part channels") != std::string::npos; });
        // The next file (no GM reset) is the unit's music again, and live MIDI is a stream of its own.
        gm.loadMidi(smfOf({dataSet(0x10, 0x10000D, {0x10})}), "D-110 file");
        gm.playerPlay();
        render(gm, 0.2);
        const bool nextFileSets = channels()[0] == kChannelOff;
        sendSysex(gm, dataSet(0x10, 0x10000D, {0}));
        sendSysex(gm, dataSet(0x10, 0x10000E, {0x10}));
        const bool liveSets = channels()[0] == 0 && channels()[1] == kChannelOff;
        sendSysex(gm, dataSet(0x10, 0x10000E, {1}));
        // Live: after GS Reset, the channels in a longer message are left out and the rest (the patch name) is taken.
        sendSysex(gm, gsReset);
        sendSysex(gm, dataSet(0x10, 0x100013, {0x10, 0x10, 0x10, 100, 'G', 'S', ' ', 't', 'e', 's', 't', ' ', ' ', ' '}));
        const bool liveKept = channels() == unitChannels && std::string(st.patchName) == "GS test   ";
        // Without the extensions, as on the unit.
        EngineOptions unitOptions = options;
        unitOptions.midiExtensions = false;
        gm.setOptions(unitOptions);
        gm.onMidiSysex(gmOn, sizeof(gmOn));
        sendSysex(gm, dataSet(0x10, 0x10000D, {0x10}));
        const bool unitSets = channels()[0] == kChannelOff;
        gm.setOptions(options);
        gm.setPartChannels({0, 1, 2, 3, 4, 5, 6, 7, 9, 8, 10, 11, 12, 13, 14, 15});
        std::printf("      channels at start %d %d ... %d, after the GS file %d %d ... %d; the log said so %ld time(s)\n", start[0], start[1],
                    start[8], afterGsFile[0], afterGsFile[1], afterGsFile[8], logged);
        check(start == unitChannels && fileKept && logged == 1 && liveKept,
              "MIDI extensions: after a GM or GS reset, MT-32 SysEx (for an MT-32 beside a GS module) keeps the part channels, "
              "from a file or live; the rest of such a message is taken, and the log says so once");
        check(nextFileSets && liveSets && unitSets && channels() == unitChannels,
              "MIDI extensions: SysEx sets the part channels as on the unit in the next file, in live MIDI without a GM reset, and "
              "with the extensions off");

        // The pitch bend range. A synth tone alone (bender on, instant attack, full sustain, T5 50, no vibrato) in tone
        // memory i01; timbres A11 and A12 play it dry with bender ranges 12 and 5.
        auto testTone = [](int lfoRate, int lfoDepth, int t2, int sustain) {
            std::vector<uint8_t> tone(246, 0);
            std::memcpy(tone.data(), "GM test   ", 10);
            tone[12] = 1;  // Partial 1 alone (structure 1: synth + synth)
            static const uint8_t partial[58] = {
                36, 50, 11, 1, 0, 0, 0, 0,                                       // WG: C4, key follow 1, bender on, square
                0, 0, 0, 0, 0, 0, 0, 50, 50, 50, 50, 50,                         // Pitch envelope (flat)
                60, 0, 0,                                                        // Pitch LFO
                40, 0, 11, 0, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 100, 100, 100, 100,  // TVF: cutoff 40, no envelope
                100, 0, 0, 12, 0, 12, 0, 0, 0, 0, 0, 0, 50, 100, 100, 100, 100,  // TVA: instant attack, sustained, T5 50
            };
            for (size_t i = 0; i < 4; i++) std::memcpy(&tone[14 + 58 * i], partial, 58);
            tone[14 + 20] = uint8_t(lfoRate);
            tone[14 + 21] = uint8_t(lfoDepth);
            tone[14 + 50] = uint8_t(t2);                                          // TVA T2
            tone[14 + 55] = tone[14 + 56] = tone[14 + 57] = uint8_t(sustain);     // TVA L2, L3 and sustain
            return tone;
        };
        const std::vector<uint8_t> plainTone = testTone(60, 0, 0, 100);
        gm.writeData(0x080000, plainTone.data(), plainTone.size());
        const uint8_t timbres[16] = {2, 0, 24, 50, 12, 2, 0, 0, 2, 0, 24, 50, 5, 2, 0, 0};
        gm.writeData(0x050000, timbres, sizeof(timbres));
        render(gm, 0.02);
        gm.onMidiShortMessage(0xC0);  // A11: bender range 12
        render(gm, 0.02);
        gm.getStatus(st);
        const bool twoAtStart = st.midiExtensions && st.parts[0].bendRangeCents == 200 && st.parts[0].temp[TimbreTemp::BenderRange] == 12;
        auto bentPitch = [&](int bend) {  // 0-16383
            gm.onMidiShortMessage(0xE0u | (uint32_t(bend & 0x7F) << 8) | (uint32_t(bend >> 7) << 16));
            noteOn(gm, 0, 60);
            const double hz = measurePitch(gm, 0.6);
            gm.onMidiShortMessage(0x3C80);
            render(gm, 0.3);
            return hz;
        };
        const double unbent = bentPitch(8192);
        const double bentTwo = bentPitch(16383);
        rpn(0, 12, -1);
        const double bentTwelve = bentPitch(16383);
        gm.onMidiShortMessage(0x01C0);  // A12: bender range 5
        cc(121, 0);
        const double stillTwelve = bentPitch(16383);
        gm.getStatus(st);
        const bool kept = st.parts[0].bendRangeCents == 1200 && st.parts[0].temp[TimbreTemp::BenderRange] == 5;
        rpn(0, 1, 50);
        const double bentOneAndHalf = bentPitch(16383);
        gm.getStatus(st);
        const bool cents = st.parts[0].bendRangeCents == 150 && st.parts[0].temp[TimbreTemp::BenderRange] == 5;
        gm.onMidiSysex(gmOn, sizeof(gmOn));
        render(gm, 0.02);
        gm.getStatus(st);
        const bool afterGmOn = st.parts[0].bendRangeCents == 200;
        rpn(0, 7, -1);
        render(gm, 0.02);
        gm.resetMidiChannels();
        gm.getStatus(st);
        const bool afterResetMidi = st.parts[0].bendRangeCents == 200;
        // Without the extensions: the timbre's range, which RPN 0 changes until the next program change.
        gm.setOptions(unitOptions);
        gm.getStatus(st);
        const bool timbreRange = !st.midiExtensions && st.parts[0].bendRangeCents == 500;
        rpn(0, 7, -1);
        const double unitSeven = bentPitch(16383);
        gm.getStatus(st);
        const bool rpnWritesTimbre = st.parts[0].bendRangeCents == 700 && st.parts[0].temp[TimbreTemp::BenderRange] == 7;
        gm.onMidiShortMessage(0xC0);  // A11
        render(gm, 0.02);
        gm.getStatus(st);
        const bool programTakesTimbre = st.parts[0].bendRangeCents == 1200;
        gm.onMidiShortMessage(0x4000E0);  // Pitch bend back to the centre
        gm.setOptions(options);
        gm.getStatus(st);
        const bool backToChannel = st.midiExtensions && st.parts[0].bendRangeCents == 200;
        auto isBent = [&](double hz, double semitones) { return unbent > 0.0 && std::fabs(hz / unbent / std::exp2(semitones / 12.0) - 1.0) < 0.003; };
        std::printf("      C4 %.2f Hz; bent up: %.2f (channel's 2), %.2f (12), %.2f (12 after A12 and CC 121), %.2f (1.5), %.2f (timbre's 7, "
                    "extensions off)\n", unbent, bentTwo, bentTwelve, stillTwelve, bentOneAndHalf, unitSeven);
        check(twoAtStart && isBent(bentTwo, 2.0) && isBent(bentTwelve, 12.0) && isBent(bentOneAndHalf, 1.5) && cents,
              "MIDI extensions: a channel bends by 2 semitones from the start (its timbre says 12), RPN 0 sets the channel's "
              "range (12; 1.5 with CC 38's cents) and leaves the timbre's alone (measured)");
        check(kept && isBent(stillTwelve, 12.0) && afterGmOn && afterResetMidi,
              "MIDI extensions: the channel's bend range outlives program changes and Reset All Controllers; GM System On and "
              "Reset MIDI make it 2 semitones again");
        check(timbreRange && rpnWritesTimbre && isBent(unitSeven, 7.0) && programTakesTimbre && backToChannel,
              "without the MIDI extensions a part bends by its timbre's range, which RPN 0 changes until the next program change, "
              "as on the unit");

        // Envelope times: GS NRPNs 01H 63H (attack), 64H (decay) and 66H (release), and CC 73, 75 and 72.
        auto setTone = [&](const std::vector<uint8_t>& tone) {
            gm.writeData(0x040000, tone.data(), tone.size());  // Part 1's tone temp
            render(gm, 0.02);
        };
        auto sum = [](const Levels& levels) { return levels.left + levels.right; };
        auto attackShare = [&]() {  // The first 40 ms against the sustained level
            noteOn(gm, 0, 60);
            const Levels early = render(gm, 0.04);
            render(gm, 1.5);
            const Levels late = render(gm, 0.2);
            gm.onMidiShortMessage(0x3C80);
            render(gm, 0.5);
            return sum(early) / sum(late);
        };
        auto releaseShare = [&]() {  // 0.1-0.3 s after the note off against the level before it
            noteOn(gm, 0, 60);
            render(gm, 0.3);
            const Levels held = render(gm, 0.1);
            gm.onMidiShortMessage(0x3C80);
            render(gm, 0.1);
            const Levels after = render(gm, 0.2);
            render(gm, 4.0);
            return sum(after) / sum(held);
        };
        auto decayShare = [&]() {  // 0.3-0.5 s after the note on against the first 20 ms
            noteOn(gm, 0, 60);
            const Levels first = render(gm, 0.02);
            render(gm, 0.28);
            const Levels later = render(gm, 0.2);
            gm.onMidiShortMessage(0x3C80);
            render(gm, 0.5);
            return sum(later) / sum(first);
        };
        setTone(plainTone);
        const double attackPlain = attackShare();
        nrpn(1, 0x63, 127);
        const double attackSlow = attackShare();
        cc(121, 0);
        const double attackAfterReset = attackShare();
        nrpn(1, 0x63, 64);
        cc(73, 127);
        const double attackByCC = attackShare();
        cc(73, 64);
        const double releasePlain = releaseShare();
        nrpn(1, 0x66, 64 + 32);
        const double releaseLong = releaseShare();
        cc(72, 0);
        const double releaseShort = releaseShare();
        cc(72, 64);
        setTone(testTone(60, 0, 30, 30));  // T2 30, down to 30
        const double decayPlain = decayShare();
        nrpn(1, 0x64, 64 + 40);
        const double decaySlow = decayShare();
        gm.onMidiSysex(gmOn, sizeof(gmOn));
        const double decayAfterGmOn = decayShare();
        std::printf("      attack (first 40 ms / sustain): %.2f, +63 %.3f, after CC 121 %.3f, CC 73 = 127 %.3f; release (0.1-0.3 s after / "
                    "before): %.4f, +32 %.3f, CC 72 = 0 %.5f; decay (0.3-0.5 s / start): %.3f, +40 %.3f, after GM System On %.3f\n",
                    attackPlain, attackSlow, attackAfterReset, attackByCC, releasePlain, releaseLong, releaseShort, decayPlain, decaySlow,
                    decayAfterGmOn);
        check(attackPlain > 0.8 && attackSlow < 0.2 && attackAfterReset < 0.2 && attackByCC < 0.2,
              "MIDI extensions: NRPN 01H 63H and CC 73 slow a part's attack, and Reset All Controllers keeps it");
        check(releaseLong > 20.0 * releasePlain && releaseShort < 0.5 * releasePlain,
              "MIDI extensions: NRPN 01H 66H and CC 72 lengthen and shorten a part's release");
        check(decaySlow > 2.0 * decayPlain && std::fabs(decayAfterGmOn / decayPlain - 1.0) < 0.05,
              "MIDI extensions: NRPN 01H 64H slows a part's decay, until GM System On");

        // Vibrato: GS NRPNs 01H 09H (depth) and 08H (rate), and CC 77 and 76.
        auto vibrato = [&]() {  // The pitch of a held note after 0.1 s
            noteOn(gm, 0, 60);
            std::vector<double> track = pitchTrack(gm, 2.1);
            gm.onMidiShortMessage(0x3C80);
            render(gm, 0.5);
            track.erase(track.begin(), track.begin() + std::min<size_t>(20, track.size()));
            return track;
        };
        setTone(plainTone);  // LFO rate 60, depth 0
        const double spreadPlain = pitchSpreadCents(vibrato());
        nrpn(1, 0x09, 127);
        const std::vector<double> deep = vibrato();
        cc(121, 0);
        const double spreadAfterReset = pitchSpreadCents(vibrato());
        nrpn(1, 0x09, 64);
        cc(77, 127);
        const double spreadByCC = pitchSpreadCents(vibrato());
        nrpn(1, 0x08, 64 + 16);
        const std::vector<double> fast = vibrato();
        cc(76, 64 - 16);
        const std::vector<double> slow = vibrato();
        gm.onMidiSysex(gmOn, sizeof(gmOn));
        const double spreadAfterGmOn = pitchSpreadCents(vibrato());
        gm.setOptions(unitOptions);
        nrpn(1, 0x09, 127);
        const double spreadWithoutExtensions = pitchSpreadCents(vibrato());
        gm.setOptions(options);
        const double rateDeep = modulationRate(deep), rateFast = modulationRate(fast), rateSlow = modulationRate(slow);
        std::printf("      vibrato (cents top to bottom): tone's %.1f, NRPN depth +63 %.1f at %.2f Hz, after CC 121 %.1f, CC 77 = 127 %.1f; "
                    "rate +16 %.2f Hz, CC 76 -16 %.2f Hz; after GM System On %.1f, without the extensions %.1f\n", spreadPlain,
                    pitchSpreadCents(deep), rateDeep, spreadAfterReset, spreadByCC, rateFast, rateSlow, spreadAfterGmOn, spreadWithoutExtensions);
        check(spreadPlain < 3.0 && pitchSpreadCents(deep) > 40.0 && spreadAfterReset > 40.0 && spreadByCC > 40.0 && spreadAfterGmOn < 3.0 &&
                  spreadWithoutExtensions < 3.0,
              "MIDI extensions: NRPN 01H 09H and CC 77 give a part vibrato (a tone without any here), kept by Reset All "
              "Controllers, ended by GM System On, ignored without the extensions");
        check(rateDeep > 3.0 && rateDeep < 4.5 && rateFast / rateDeep > 1.7 && rateFast / rateDeep < 2.3 && rateDeep / rateSlow > 1.7 &&
                  rateDeep / rateSlow < 2.3,
              "MIDI extensions: NRPN 01H 08H and CC 76 speed the vibrato up and slow it down (+16 twice as fast, -16 half)");

        // Portamento: CC 65 switches it on, CC 5 sets its time (312.5 / 2^(time / 16) semitones a second: an octave in
        // 0.614 s at 64, 0.154 s at 32), CC 84 names the key the next note glides from, or moves a note sounding there.
        setTone(plainTone);
        const auto semitonesFromC4 = [](double hz) { return 12.0 * std::log2(hz / 261.63); };
        // How long after the note-on the pitch first comes within 10 cents of `target` (semitones from C4).
        const auto arrival = [&](const std::vector<double>& track, double target) {
            for (size_t i = 0; i < track.size(); i++) {
                if (std::fabs(semitonesFromC4(track[i]) - target) < 0.1) return 0.01 + 0.005 * double(i);  // Windows' middles
            }
            return -1.0;
        };
        const auto afterC4 = [&](int key) {  // C4, then `key`: the second note's pitch
            noteOn(gm, 0, 60);
            render(gm, 0.3);
            gm.onMidiShortMessage(0x3C80);
            render(gm, 0.3);
            noteOn(gm, 0, key);
            const std::vector<double> track = pitchTrack(gm, 1.0);
            gm.onMidiShortMessage(0x80u | (uint32_t(key) << 8));
            render(gm, 0.5);
            return track;
        };
        cc(65, 127);
        cc(5, 64);
        const std::vector<double> slowGlide = afterC4(72);
        cc(5, 32);
        const std::vector<double> fastGlide = afterC4(72);
        cc(65, 0);
        const std::vector<double> noGlide = afterC4(72);
        cc(5, 64);
        cc(84, 60);
        noteOn(gm, 0, 72);
        const std::vector<double> controlGlide = pitchTrack(gm, 1.0);
        gm.onMidiShortMessage(0x4880);
        render(gm, 0.5);
        noteOn(gm, 0, 67);
        const std::vector<double> afterControl = pitchTrack(gm, 0.3);
        gm.onMidiShortMessage(0x4380);
        render(gm, 0.5);
        bool monotonic = !slowGlide.empty();
        for (size_t i = 1; i < slowGlide.size(); i++) monotonic = monotonic && slowGlide[i] >= slowGlide[i - 1] * 0.999;
        std::printf("      portamento C4 -> C5: starts %.2f semitones up, at C5 after %.3f s (time 64) and %.3f s (32), %.3f s off; "
                    "CC 84 from C4 %.3f s, the next note %.3f s\n", semitonesFromC4(slowGlide.front()), arrival(slowGlide, 12.0),
                    arrival(fastGlide, 12.0), arrival(noGlide, 12.0), arrival(controlGlide, 12.0), arrival(afterControl, 7.0));
        check(semitonesFromC4(slowGlide.front()) < 0.6 && monotonic && std::fabs(arrival(slowGlide, 12.0) - 0.614) < 0.04 &&
                  std::fabs(arrival(fastGlide, 12.0) - 0.154) < 0.03 && arrival(noGlide, 12.0) >= 0.0 && arrival(noGlide, 12.0) < 0.02,
              "MIDI extensions: with portamento on (CC 65) a note glides from the last one, as fast as CC 5 says (an octave in "
              "0.61 s at 64, 0.15 s at 32); off, it does not");
        check(std::fabs(arrival(controlGlide, 12.0) - 0.614) < 0.04 && arrival(afterControl, 7.0) >= 0.0 && arrival(afterControl, 7.0) < 0.02,
              "MIDI extensions: CC 84 makes the next note glide from its key, with portamento off, and only that note");

        // CC 84 on a key that still sounds: that note moves to the new key (legato); the old key's note-off does nothing.
        noteOn(gm, 0, 60);
        render(gm, 0.3);
        cc(5, 48);  // 39 semitones a second: C4 to E4 in 0.1 s
        cc(84, 60);
        noteOn(gm, 0, 64);
        const std::vector<double> legato = pitchTrack(gm, 0.4);
        gm.getStatus(st);
        const bool movedNote = st.parts[0].noteCount == 1 && st.parts[0].keys[0] == 64;
        gm.onMidiShortMessage(0x3C80);
        render(gm, 0.1);
        gm.getStatus(st);
        const bool keptByOldKey = st.parts[0].noteCount == 1;
        gm.onMidiShortMessage(0x4080);
        render(gm, 0.5);
        gm.getStatus(st);
        const bool endedByNewKey = st.parts[0].noteCount == 0;
        std::printf("      legato C4 -> E4: starts %.2f semitones up, at E4 after %.3f s; %d note(s)\n", semitonesFromC4(legato.front()),
                    arrival(legato, 4.0), movedNote ? 1 : 0);
        check(movedNote && semitonesFromC4(legato.front()) < 1.0 && std::fabs(arrival(legato, 4.0) - 0.103) < 0.03 && keptByOldKey &&
                  endedByNewKey,
              "MIDI extensions: CC 84 on a key that sounds moves that note to the next key, gliding (legato): no new note, the "
              "old key's note-off ignored, the new key's ending it");

        // A partial that does not follow the key (key follow 0) does not glide either.
        std::vector<uint8_t> fixedTone(plainTone);
        for (size_t partial = 0; partial < 4; partial++) fixedTone[14 + 58 * partial + 2] = 3;  // Pitch key follow 0
        setTone(fixedTone);
        cc(65, 127);
        cc(5, 64);
        const double fixedSpread = pitchSpreadCents(afterC4(72));
        setTone(plainTone);
        // Reset All Controllers switches portamento off; GM System On resets it all; without the extensions it is ignored.
        cc(121, 0);
        const double glideAfterReset = arrival(afterC4(72), 12.0);
        cc(65, 127);
        gm.onMidiSysex(gmOn, sizeof(gmOn));
        const double glideAfterGmOn = arrival(afterC4(72), 12.0);
        gm.setOptions(unitOptions);
        cc(65, 127);
        cc(5, 64);
        const double glideWithoutExtensions = arrival(afterC4(72), 12.0);
        gm.setOptions(options);
        std::printf("      no key follow: %.1f cents; at C5 after %.3f s (after CC 121), %.3f s (GM System On), %.3f s (no extensions)\n",
                    fixedSpread, glideAfterReset, glideAfterGmOn, glideWithoutExtensions);
        check(fixedSpread < 5.0 && glideAfterReset >= 0.0 && glideAfterReset < 0.02 && glideAfterGmOn >= 0.0 && glideAfterGmOn < 0.02 && glideWithoutExtensions >= 0.0 &&
                  glideWithoutExtensions < 0.02,
              "MIDI extensions: partials without key follow do not glide; Reset All Controllers and GM System On switch "
              "portamento off; without the extensions it is ignored");
    }

    // A burst of notes starts at once; with the MIDI cable emulation the notes follow each other.
    auto burst = [&](bool cableSpeed) {
        EngineOptions timing = engine.options();
        timing.midiCableSpeed = cableSpeed;
        engine.setOptions(timing);
        for (int key = 60; key < 68; key++) noteOn(engine, 0, key);
        std::vector<float> oneMillisecond(2 * 48);
        engine.render(oneMillisecond.data(), 48);
        engine.getStatus(status);
        const uint32_t sounding = status.parts[0].noteCount;
        allOff(engine);
        return sounding;
    };
    const uint32_t immediate = burst(false);
    const uint32_t cable = burst(true);
    burst(false);
    std::printf("      notes sounding 1 ms after an 8-note burst: %u, with MIDI cable speed: %u\n", immediate, cable);
    check(immediate == 8 && cable < 4, "a burst of notes plays at once; MIDI cable speed spaces it out");

    // Hi-hats: a closed hi-hat (key 42, r01) cuts off Open High Hat-1 (key 46, r03) but not Open High Hat-2 (key 44, r04).
    auto hatTail = [&](int openKey, bool closeIt) {
        noteOn(engine, 9, openKey);
        render(engine, 0.1);
        if (closeIt) noteOn(engine, 9, 42);
        render(engine, 0.15);
        const Levels tail = render(engine, 0.4);
        allOff(engine);
        return tail.left + tail.right;
    };
    const double closedAlone = [&] {
        noteOn(engine, 9, 42);
        render(engine, 0.25);
        const Levels tail = render(engine, 0.4);
        allOff(engine);
        return tail.left + tail.right;
    }();
    const double open1 = hatTail(46, false);
    const double open1Closed = hatTail(46, true);
    const double open2 = hatTail(44, false);
    const double open2Closed = hatTail(44, true);
    std::printf("      hi-hat tails: closed %.2e, open-1 %.2e -> %.2e with closed, open-2 %.2e -> %.2e with closed\n",
                closedAlone, open1, open1Closed, open2, open2Closed);
    check(open1Closed < 0.5 * open1 && open2Closed > 0.8 * open2, "a closed hi-hat cuts off Open High Hat-1 only");

    // Fine pan (nice panning): parts and rhythm keys pan in 129 steps; a panpot write drops the fine pan.
    {
        engine.setPartParameter(0, TimbreTemp::ToneGroup, 0);
        engine.setPartParameter(0, TimbreTemp::ToneNumber, 0);  // a11: structures without stereo pairs
        engine.setPartFinePan(0, -32);
        render(engine, 0.02);
        engine.getStatus(status);
        const bool coarseFollows = status.parts[0].finePanSet && status.parts[0].finePan == -32 && status.parts[0].temp[TimbreTemp::Panpot] == 3;
        EngineOptions nice = options;
        nice.nicePanning = true;
        nice.reverbEnabled = false;  // The reverb returns on both sides
        engine.setOptions(nice);
        noteOn(engine, 0, 60);
        const Levels fine = render(engine, 0.3);  // Left 96/128, right 32/128
        allOff(engine);
        engine.onMidiShortMessage(0x600AB0);  // CC 10 = 96: +33
        render(engine, 0.02);
        noteOn(engine, 0, 60);
        const Levels controller = render(engine, 0.3);  // Left 31/128, right 97/128
        allOff(engine);
        engine.getStatus(status);
        const bool controllerFine = status.parts[0].finePan == 33 && status.parts[0].temp[TimbreTemp::Panpot] == 11;
        engine.setPartParameter(0, TimbreTemp::Panpot, 7);  // Drops the fine pan
        engine.setRhythmFinePan(38, 64);                      // Snare hard right
        render(engine, 0.02);
        engine.getStatus(status);
        const bool dropped = !status.parts[0].finePanSet && status.parts[0].finePan == 0;
        noteOn(engine, 9, 38);
        const Levels snare = render(engine, 0.3);
        allOff(engine);
        engine.setOptions(options);
        std::printf("      fine pan -32: L/R %.2f; CC 10 = 96: R/L %.2f; rhythm key +64: L/R %.3f\n", fine.left / fine.right,
                    controller.right / controller.left, snare.left / snare.right);
        check(coarseFollows && controllerFine && dropped && std::fabs(fine.left / fine.right - 3.0) < 0.1 &&
                  std::fabs(controller.right / controller.left - 97.0 / 31.0) < 0.1 && snare.left < 0.01 * snare.right,
              "fine pan: -64..+64 for parts and rhythm keys with nice panning, CC 10 at full resolution, panpot writes drop it");
        engine.setPartFinePan(1, 20);  // Kept for the memory dump below
    }

    // A memory dump reloads to the same state.
    const std::vector<uint8_t> dump = engine.dumpSysex(DumpEverything);
    engine.applySysex(dump);
    check(engine.dumpSysex(DumpEverything) == dump, "memory dump round-trips");

    // ROM Play songs are found in the control ROM, with the firmware's tick and the songs' own timbres.
    std::vector<RomSong> songs;
    check(loadRomSongs(roms[control].path, songs, error) && songs.size() == 8 && songs[0].name == "Macho Memory",
          "8 ROM Play songs found");
    if (songs.size() == 8) {
        const RomSong& macho = songs[0];
        check(std::fabs(macho.tickSeconds - 0.012332) < 1e-9 && macho.timbres && macho.timbres->tones.size() == 9 &&
                  macho.hasPartialReserve && macho.partialReserve[0] == 4 && macho.partialReserve[8] == 6,
              "ROM Play tick (12.332 ms), timbre bank, demo tones and partial reserves read from the firmware");

        // A song ends at its end mark, at most one long wait (248 ticks) after its last message: Bumble Dee's data has
        // six there (18.8 s of silence after its last note-off); the others end as their data does (Folk 344 ticks after).
        auto endingSeconds = [](const RomSong& song) {
            const std::unique_ptr<SmfFile> smf = romSongsToSmf({song});
            double last = 0.0;
            for (const SmfEvent& event : smf->events) {
                if (event.shortMessage != 0) last = std::max(last, event.time);
            }
            return smf->duration - last;
        };
        const double bumbleDee = endingSeconds(songs[5]), folk = endingSeconds(songs[4]);
        std::printf("      ROM Play endings: Bumble Dee %.2f s after its last message, Folk %.2f s\n", bumbleDee, folk);
        check(songs[5].name == "Bumble Dee" && std::fabs(bumbleDee - 283 * songs[5].tickSeconds) < 0.001 &&
                  std::fabs(folk - 344 * songs[4].tickSeconds) < 0.001,
              "ROM Play: Bumble Dee ends one long wait after its last note, not six (18.8 s of silence); Folk as its data has it");

        // Byte 07H of the user's timbres (d110emu's card and tone bank flags) must not reach the song's parts.
        std::vector<uint8_t> timbres(128 * 8);
        engine.readMemory(0x050000, uint32_t(timbres.size()), timbres.data());
        for (size_t t = 0; t < 128; t++) timbres[t * 8 + 7] = MT32Emu::PART_ALT_TONES;
        engine.writeData(0x050000, timbres.data(), timbres.size());
        engine.loadMidi(romSongsToSmf({macho}), macho.name, MidiFileKind::UnitSong);
        engine.playerPlay();
        render(engine, 1.0);
        engine.getStatus(status);
        bool flagsCleared = true;
        for (int part = 0; part < 8; part++) flagsCleared = flagsCleared && status.parts[part].temp[7] == 0;
        check(flagsCleared, "ROM Play: the song's timbres clear the flags the user's timbres carry (byte 07H)");
        for (size_t t = 0; t < 128; t++) timbres[t * 8 + 7] = 0;
        engine.writeData(0x050000, timbres.data(), timbres.size());
        uint8_t reserve[9] = {};
        engine.readMemory(0x100004, 9, reserve);
        check(status.parts[0].channel == 1 && status.parts[7].channel == 8 && status.parts[kRhythmPart].channel == 9 &&
                  std::string(status.parts[0].tone) == "b45" && std::string(status.parts[5].name) == "Syn Lead 1" &&
                  std::memcmp(reserve, macho.partialReserve, 9) == 0,
              "Macho Memory: parts on channels 2-10, program 27 -> b45, A34 -> demo tone Syn Lead 1, song's partial reserves");
        engine.playerStop();

        // ROM Play (the unit's own songs) plays as the unit does: with the MIDI extensions off, so its parts bend by the
        // song's timbres' ranges (the extensions give each channel a range of its own).
        engine.loadMidi(romSongsToSmf({macho}), macho.name, MidiFileKind::UnitSong);
        engine.playerPlay();
        render(engine, 1.0);
        engine.getStatus(status);
        bool timbreRanges = engine.options().midiExtensions && !status.midiExtensions;
        std::printf("      Macho Memory's bend ranges (semitones, parts 1-8):");
        for (int part = 0; part < 8; part++) {
            std::printf(" %u", status.parts[part].bendRangeCents / 100);
            timbreRanges = timbreRanges && status.parts[part].bendRangeCents == status.parts[part].temp[TimbreTemp::BenderRange] * 100u;
        }
        std::printf("\n");
        engine.playerPause();
        render(engine, 0.02);
        engine.getStatus(status);
        const bool offWhilePaused = !status.midiExtensions;
        engine.playerStop();
        render(engine, 0.02);
        engine.getStatus(status);
        check(timbreRanges && offWhilePaused && status.midiExtensions,
              "ROM Play plays with the MIDI extensions off, as the unit does (its parts bend by the song's timbres' ranges), also "
              "while paused; they are back when it stops");

        // The exported file keeps the ROM timing (first to last note) at the song's own tempo.
        const std::filesystem::path exported = std::filesystem::temp_directory_path() / "d110tests-rom-song.mid";
        SmfFile smf;
        const bool saved = saveRomSongSmf(exported, macho, 0x10, error) && loadSmfFile(exported, smf, error);
        std::filesystem::remove(exported);
        double first = -1.0;
        double last = 0.0;
        for (const SmfEvent& event : smf.events) {
            if ((event.shortMessage & 0xF0) == 0x90 && (event.shortMessage >> 16) != 0) {
                if (first < 0.0) first = event.time;
                last = event.time;
            }
        }
        // Macho Memory's first note is on tick 27, its last on tick 6012.
        std::printf("      export: first note %.3f s, last %.3f s\n", first, last);
        check(saved && std::fabs((last - first) - (6012 - 27) * 0.012332) < 0.002, "exported song keeps the ROM timing");
    }

    // D-20 performance mode: a split performance (lower a11 below C4, upper b31 from C4) on channel 1.
    {
        uint8_t performance[38] = {2, 24, 0, 0, 1, 16, 24, 24, 50, 50, 2, 2, 0, 0, 1, 1, 2, 4, 5, 50, 100};
        std::memcpy(&performance[0x15], "Test Split      ", 16);
        engine.writeData(0x070000 + 0x26, performance, sizeof(performance));  // Patch A12
        engine.setPerformanceMode(true, 0);
        engine.onMidiShortMessage(0xC0 | (1 << 8));  // Program change 2 on channel 1: A12
        render(engine, 0.02);
        engine.getStatus(status);
        const bool setUp = status.performanceMode && status.currentPerformance == 1 && status.parts[0].channel == 0 &&
                           status.parts[1].channel == 0 && status.parts[2].channel == kChannelOff &&
                           std::string(status.parts[0].tone) == "b31" && std::string(status.parts[1].tone) == "a11" &&
                           status.parts[0].temp[TimbreTemp::KeyRangeLower] == 60 && status.parts[1].temp[TimbreTemp::KeyRangeUpper] == 59;
        noteOn(engine, 0, 72);
        render(engine, 0.05);
        engine.getStatus(status);
        const bool upperOnly = status.parts[0].noteCount == 1 && status.parts[1].noteCount == 0;
        allOff(engine);
        noteOn(engine, 0, 48);
        render(engine, 0.05);
        engine.getStatus(status);
        const bool lowerOnly = status.parts[0].noteCount == 0 && status.parts[1].noteCount == 1;
        allOff(engine);
        check(setUp && upperOnly && lowerOnly, "performance mode: program change selects a split performance on parts 1 and 2");
        engine.setPerformanceMode(false, 0);
    }

    // Reset MIDI in performance mode: the patch sets parts 1 and 2 again (level 80, balance 70: upper 80, lower 48).
    {
        uint8_t performance[38] = {1, 24, 0, 0, 1, 16, 24, 24, 50, 50, 2, 2, 0, 0, 1, 1, 2, 4, 5, 70, 80};
        std::memcpy(&performance[0x15], "Test Dual       ", 16);
        engine.writeData(0x070000 + 2 * 0x26, performance, sizeof(performance));  // Patch A13
        engine.setPerformanceMode(true, 0);
        engine.onMidiShortMessage(0xC0 | (2 << 8));
        render(engine, 0.02);
        engine.resetMidiChannels();
        engine.getStatus(status);
        const bool patchLevels = status.parts[0].temp[TimbreTemp::OutputLevel] == 80 && status.parts[1].temp[TimbreTemp::OutputLevel] == 48 &&
                                 status.parts[0].temp[TimbreTemp::Panpot] == 7 && status.parts[1].temp[TimbreTemp::Panpot] == 7 &&
                                 status.parts[3].temp[TimbreTemp::OutputLevel] == 100;
        engine.setPerformanceMode(false, 0);
        check(patchLevels, "Reset MIDI in performance mode: the patch sets parts 1 and 2 again (their levels by the balance)");
    }

    // The current performance patch: the D-20's patch write makes the written patch current, as the D-110's patch write
    // does its patch, and a restart that keeps the memory keeps both numbers (the display shows them).
    {
        SynthEngine numbers;
        numbers.setOptions(options);
        numbers.configure(config, error);
        numbers.setPerformanceMode(true, 0);
        numbers.recallPerformance(9);  // A22
        render(numbers, 0.02);
        const uint8_t writeA35[2] = {20, 0};  // 40 03 00: patch A35, internal
        numbers.writeData(0x400300, writeA35, 2);
        render(numbers, 0.02);
        EngineStatus ns;
        numbers.getStatus(ns);
        uint8_t written[38], patchTemp[38];
        numbers.readMemory(at(0x070000, 20 * 38), sizeof(written), written);
        numbers.readMemory(0x030400, sizeof(patchTemp), patchTemp);
        const bool writeCurrent = ns.currentPerformance == 20 && std::memcmp(written, patchTemp, sizeof(written)) == 0;
        numbers.setCurrentPatchNumber(12);
        numbers.configure(config, error);
        numbers.getStatus(ns);
        const bool restartKept = ns.currentPatch == 12 && ns.currentPerformance == 20;
        std::printf("      current performance after the patch write %u; after a restart: patch %u, performance %u\n",
                    unsigned(writeCurrent ? 20 : 0), unsigned(ns.currentPatch), unsigned(ns.currentPerformance));
        check(writeCurrent && restartKept,
              "the D-20's patch write makes the written performance patch current; a restart keeps the current patch and "
              "performance numbers");
    }

    // The D-20's performance patches start as a factory reset leaves them (D20Initial.syx: the user's D-20 dumped then),
    // with A11 current; a patch of zeros, from memory saved before d110emu had them, is the initial patch again.
    {
        SynthEngine initial;
        initial.setOptions(options);
        initial.configure(config, error);
        auto initialPatch = [](uint32_t number) {
            std::vector<uint8_t> patch = {2, 24, 1, 0, 0, 0, 24, 24, 50, 50, 12, 12, 0, 0, 1, 1, 2, 5, 3, 50, 100};
            const char* name = "<Initial Patch> ";
            patch.insert(patch.end(), name, name + 16);
            patch.push_back(4);
            const bool bankB = number >= 64;
            patch[2] = bankB ? 0 : 1;  // The lower tone: b01-b64 in bank A, a01-a64 in bank B
            patch[4] = bankB ? 1 : 0;  // The upper tone: the other group
            patch[3] = patch[5] = uint8_t(number % 64);
            return patch;
        };
        std::vector<uint8_t> expected;
        for (uint32_t n = 0; n < 128; n++) {
            const std::vector<uint8_t> patch = initialPatch(n);
            expected.insert(expected.end(), patch.begin(), patch.end());
        }
        std::vector<uint8_t> patches(128 * 38), track(502);
        initial.readMemory(0x070000, uint32_t(patches.size()), patches.data());
        initial.readMemory(0x0C0000, uint32_t(track.size()), track.data());
        std::vector<uint8_t> file;
        for (const char* path : {"../D20Initial.syx", "../../D20Initial.syx", "D20Initial.syx"}) {
            std::ifstream in(path, std::ios::binary);
            if (in) file.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (!file.empty()) break;
        }
        bool asDumped = true;
        if (!file.empty()) {
            // Its patches (07 xx xx) and rhythm track (0C xx xx), from the DT1 messages.
            std::vector<uint8_t> dumpedPatches(patches.size(), 0xFF), dumpedTrack(track.size(), 0xFF);
            for (size_t i = 0; i < file.size();) {
                const size_t end = size_t(std::find(file.begin() + long(i), file.end(), uint8_t(0xF7)) - file.begin());
                if (end >= file.size()) break;
                if (end - i > 10 && file[i] == 0xF0 && file[i + 1] == 0x41 && file[i + 3] == 0x16 && file[i + 4] == 0x12) {
                    const size_t offset = (size_t(file[i + 6]) << 7) | file[i + 7];
                    std::vector<uint8_t>& area = file[i + 5] == 0x07 ? dumpedPatches : dumpedTrack;
                    if (file[i + 5] == 0x07 || file[i + 5] == 0x0C) {
                        for (size_t k = 0; i + 8 + k < end - 1 && offset + k < area.size(); k++) area[offset + k] = file[i + 8 + k];
                    }
                }
                i = end + 1;
            }
            asDumped = dumpedPatches == patches && dumpedTrack == track;
            std::printf("      D20Initial.syx: patches %s, rhythm track %s\n", dumpedPatches == patches ? "equal" : "DIFFERENT",
                        dumpedTrack == track ? "equal" : "DIFFERENT");
        } else {
            std::printf("      (D20Initial.syx not found: the patches checked against their bytes as written here)\n");
        }
        EngineStatus initialStatus;
        initial.setPerformanceMode(true, 0);
        render(initial, 0.02);
        initial.getStatus(initialStatus);
        const bool a11Plays = initialStatus.currentPerformance == 0 && std::string(initialStatus.parts[0].tone) == "a11" &&
                              std::string(initialStatus.parts[1].tone) == "b11" && initialStatus.parts[0].temp[TimbreTemp::KeyRangeLower] == 60 &&
                              initialStatus.parts[1].temp[TimbreTemp::KeyRangeUpper] == 59;
        initial.onMidiShortMessage(0xC0 | (127 << 8));  // B88
        render(initial, 0.02);
        initial.getStatus(initialStatus);
        const bool b88Plays = initialStatus.currentPerformance == 127 && std::string(initialStatus.parts[0].tone) == "b88" &&
                              std::string(initialStatus.parts[1].tone) == "a88";
        const std::vector<uint8_t> zeros(38, 0);
        initial.writeData(at(0x070000, 2 * 38), zeros.data(), uint32_t(zeros.size()));  // A13
        render(initial, 0.02);
        std::vector<uint8_t> a13(38);
        initial.readMemory(at(0x070000, 2 * 38), uint32_t(a13.size()), a13.data());
        const bool zerosInitial = a13 == initialPatch(2);
        std::printf("      initial patches %d, as dumped %d, A11 plays %d, B88 plays %d, zeros become the initial A13 %d\n",
                    patches == expected, asDumped, a11Plays, b88Plays, zerosInitial);
        check(patches == expected && asDumped && a11Plays && b88Plays && zerosInitial,
              "D-20 performance patches start as the D-20's initial patches (split at C4, a/b tones in order; the factory "
              "track as dumped too), A11 first; a patch of zeros becomes its initial patch");
    }

    // Part outputs (the plugins'): a part in the mask plays all its notes out of its own stereo output, with its pan,
    // instead of the mix (the rhythm part with all its keys). So the mix is the mix with those parts muted, and a part's
    // output the mix of that part alone, sample for sample: at 48 kHz (no resampling) and at 44.1 kHz (each stream
    // through a resampler of its own, in step with the mix's).
    {
        struct Take {
            std::vector<float> mix, part1, rhythm;
            std::vector<float> multi[SynthEngine::kMultiPairs];  // MULTI 1 and 2, 3 and 4, 5 and 6
        };
        struct Setup {
            bool partOutputs = true;
            bool multiOutputs = false;
            uint32_t mask = 0;
            uint32_t muted = 0;
            uint32_t rate = 48000;
            uint8_t pan = 7;           // Part 1's panpot
            uint8_t outputAssign = 2;  // Part 1's: MULTI 1
            uint8_t drumAssign = 3;    // The bass drum's (key 36): MULTI 2
            bool multiPairs = false;   // MULTI 1+2, 3+4 and 5+6 as stereo pairs
            bool part1Only = false;
        };
        const auto take = [&](const Setup& setup) {
            SynthEngine partEngine;
            EngineConfig partConfig = config;
            partConfig.partOutputs = setup.partOutputs;
            partConfig.multiOutputs = setup.multiOutputs;
            partConfig.outputSampleRate = setup.rate;
            partEngine.setOptions(options);
            partEngine.configure(partConfig, error);
            partEngine.setPartOutputMask(setup.mask);
            partEngine.setMutedParts(setup.muted);
            partEngine.setMultiPairsStereo(setup.multiPairs);
            partEngine.writeData(0x030006, &setup.outputAssign, 1);
            partEngine.writeData(0x030009, &setup.pan, 1);
            partEngine.writeData(0x030143, &setup.drumAssign, 1);  // Rhythm key 36 (bass drum)
            std::srand(1);
            Take result;
            std::vector<float> mix(2 * 512), part1(2 * 512), rhythm(2 * 512), pairs[SynthEngine::kMultiPairs];
            float* parts[SynthEngine::kPartOutputs] = {};
            float* multi[SynthEngine::kMultiPairs] = {};
            parts[0] = part1.data();
            parts[8] = rhythm.data();
            for (int pair = 0; pair < SynthEngine::kMultiPairs; pair++) {
                pairs[pair].resize(2 * 512);
                multi[pair] = pairs[pair].data();
            }
            for (uint32_t done = 0; done < setup.rate; done += 512) {
                if (done == 1024) {
                    noteOn(partEngine, 0, 60);
                    if (!setup.part1Only) {
                        noteOn(partEngine, 1, 64);
                        noteOn(partEngine, 9, 36);
                    }
                }
                partEngine.renderBuses(mix.data(), parts, multi, 512);
                result.mix.insert(result.mix.end(), mix.begin(), mix.end());
                result.part1.insert(result.part1.end(), part1.begin(), part1.end());
                result.rhythm.insert(result.rhythm.end(), rhythm.begin(), rhythm.end());
                for (int pair = 0; pair < SynthEngine::kMultiPairs; pair++) {
                    result.multi[pair].insert(result.multi[pair].end(), pairs[pair].begin(), pairs[pair].end());
                }
            }
            return result;
        };
        const auto level = [](const std::vector<float>& samples, int channel) {  // RMS of one channel (0, 1) or both (-1)
            double sum = 0.0;
            size_t count = 0;
            for (size_t i = 0; i < samples.size(); i++) {
                if (channel >= 0 && int(i % 2) != channel) continue;
                sum += double(samples[i]) * samples[i];
                count++;
            }
            return count > 0 ? std::sqrt(sum / double(count)) : 0.0;
        };
        bool exact = true;
        for (uint32_t rate : {48000u, 44100u}) {
            Setup routedSetup;
            routedSetup.mask = 1u | 1u << 8;
            routedSetup.rate = rate;
            const Take routed = take(routedSetup);
            Setup othersSetup = routedSetup;
            othersSetup.partOutputs = false;
            othersSetup.mask = 0;
            othersSetup.muted = 1u | 1u << 8;  // Part 1 and the rhythm part muted
            const Take others = take(othersSetup);
            Setup aloneSetup = othersSetup;
            aloneSetup.muted = 0xFFFFu & ~1u;  // All but part 1
            const Take alone = take(aloneSetup);
            const bool mixEqual = routed.mix == others.mix, partEqual = routed.part1 == alone.mix;
            std::printf("      part outputs at %u Hz: mix as with parts 1 and R muted %d, part 1's output as part 1 alone %d "
                        "(level %.4f), rhythm output %.4f, mix %.4f\n",
                        rate, mixEqual, partEqual, level(routed.part1, -1), level(routed.rhythm, -1), level(routed.mix, -1));
            exact = exact && mixEqual && partEqual && level(routed.part1, -1) > 0.001 && level(routed.rhythm, -1) > 0.001 &&
                    level(routed.mix, -1) > 0.001;
        }
        // The pan stays on a part's own output: part 1 hard left has nothing on the right.
        Setup leftSetup;
        leftSetup.mask = 1;
        leftSetup.pan = 0;
        leftSetup.part1Only = true;
        const Take left = take(leftSetup);
        const bool panKept = level(left.part1, 0) > 0.001 && level(left.part1, 1) == 0.0 && level(left.mix, -1) < 1e-6;
        // Outside the mask, MULTI notes stay in the mix (centred, as without part outputs) and the part's output is silent.
        Setup unroutedSetup;
        const Take unrouted = take(unroutedSetup);
        Setup plainSetup;
        plainSetup.partOutputs = false;
        const bool unroutedAsPlain = unrouted.mix == take(plainSetup).mix && level(unrouted.part1, -1) == 0.0 && level(unrouted.rhythm, -1) == 0.0;
        // MULTI 5 while reverb is on: lost on the unit (and in the mix), heard on the part's own output.
        Setup multi5Setup;
        multi5Setup.outputAssign = 6;
        multi5Setup.part1Only = true;
        const double lost = level(take(multi5Setup).mix, -1);
        multi5Setup.mask = 1;
        const double heard = level(take(multi5Setup).part1, -1);
        // Whatever the output assign: MIX goes out of the part's own output too, and MIX + reverb still feeds the reverb,
        // whose return stays in the mix: the own output and the mix add up to the part in the plain mix.
        Setup mixSetup;
        mixSetup.mask = 1;
        mixSetup.outputAssign = 0;
        mixSetup.part1Only = true;
        const Take mixRouted = take(mixSetup);
        const bool anyAssign = level(mixRouted.part1, -1) > 0.001 && level(mixRouted.mix, -1) < 1e-6;
        Setup reverbSetup = mixSetup;
        reverbSetup.outputAssign = 1;
        const Take reverbRouted = take(reverbSetup);
        reverbSetup.partOutputs = false;
        reverbSetup.mask = 0;
        const Take reverbPlain = take(reverbSetup);
        double worst = 0.0;
        for (size_t i = 0; i < reverbPlain.mix.size(); i++) {
            worst = std::max(worst, std::fabs(double(reverbRouted.part1[i]) + reverbRouted.mix[i] - reverbPlain.mix[i]));
        }
        const bool reverbReturn = level(reverbRouted.part1, -1) > 0.001 && level(reverbRouted.mix, -1) > 0.0005 && worst < 1e-3;
        std::printf("      pan kept %d, outside the mask as without part outputs %d, MULTI 5 with reverb: mix %.4f, own output %.4f; "
                    "MIX routed too %d; MIX + reverb: own output %.4f, reverb in the mix %.4f, their sum off the plain mix by %.6f\n",
                    panKept, unroutedAsPlain, lost, heard, anyAssign, level(reverbRouted.part1, -1), level(reverbRouted.mix, -1), worst);
        check(exact && panKept && unroutedAsPlain && lost < 1e-6 && heard > 0.001 && anyAssign && reverbReturn,
              "part outputs: parts in the mask play out of their own output (panned; the rhythm part with all its keys) "
              "whatever their output assign, MIX + reverb feeding the reverb in the mix; the mix and each part's output "
              "sample for sample as with the other parts muted, at 48 and 44.1 kHz");

        // MULTI outputs of their own (the plugins; the standalone's 7.1 surround): notes on MULTI 1-6 leave the mix for
        // their output, mono (the centred partial's left and right: its full level) and dry. So the mix is the mix with
        // those notes muted, and MULTI 1 (part 1) and MULTI 2 (the bass drum) the left and right of that part alone in
        // the mix, to a few 16-bit steps of rounding (the renderer's): at 48 and 44.1 kHz.
        bool multiExact = true;
        for (uint32_t rate : {48000u, 44100u}) {
            Setup multiSetup;
            multiSetup.partOutputs = false;  // As the standalone: the MULTI pairs without the parts' outputs
            multiSetup.multiOutputs = true;
            multiSetup.rate = rate;
            const Take multi = take(multiSetup);
            Setup othersSetup = multiSetup;
            othersSetup.multiOutputs = false;
            othersSetup.muted = 1u | 1u << 8;  // Part 1 (MULTI 1) and the rhythm part (only the bass drum, on MULTI 2)
            const Take others = take(othersSetup);
            Setup aloneSetup = othersSetup;
            aloneSetup.muted = 0xFFFFu & ~1u;
            const Take alone = take(aloneSetup);
            Setup drumSetup = othersSetup;
            drumSetup.muted = 0xFFFFu & ~(1u << 8);
            const Take drum = take(drumSetup);
            double worst1 = 0.0, worst2 = 0.0;
            for (size_t i = 0; i + 1 < multi.mix.size(); i += 2) {
                worst1 = std::max(worst1, std::fabs(double(multi.multi[0][i]) - (double(alone.mix[i]) + alone.mix[i + 1])));
                worst2 = std::max(worst2, std::fabs(double(multi.multi[0][i + 1]) - (double(drum.mix[i]) + drum.mix[i + 1])));
            }
            const bool mixEqual = multi.mix == others.mix;
            // Silent: at 44.1 kHz srctools' resampler leaves about 1e-22 (against denormals), in the mix too.
            const bool restSilent = level(multi.multi[1], -1) < 1e-12 && level(multi.multi[2], -1) < 1e-12;
            std::printf("      MULTI outputs at %u Hz: mix as with those notes muted %d, MULTI 1 %.4f (off part 1's left + right "
                        "by %.6f), MULTI 2 %.4f (off the bass drum's by %.6f), MULTI 3-6 silent %d, parts' outputs silent %d\n",
                        rate, mixEqual, level(multi.multi[0], 0), worst1, level(multi.multi[0], 1), worst2, restSilent,
                        level(multi.part1, -1) == 0.0 && level(multi.rhythm, -1) == 0.0);
            multiExact = multiExact && mixEqual && restSilent && worst1 < 4.0 / 32768 && worst2 < 4.0 / 32768 &&
                         level(multi.multi[0], 0) > 0.001 && level(multi.multi[0], 1) > 0.001 && level(multi.part1, -1) == 0.0;
        }
        // MULTI 5 while reverb is on: silent on the unit, but played where it is an output of its own.
        Setup multi5Out;
        multi5Out.partOutputs = false;
        multi5Out.multiOutputs = true;
        multi5Out.outputAssign = 6;
        multi5Out.part1Only = true;
        const Take multi5 = take(multi5Out);
        const bool multi5Heard = level(multi5.multi[2], 0) > 0.001 && level(multi5.multi[2], 1) == 0.0 && level(multi5.mix, -1) == 0.0;
        // An own output comes first: a part on its own output takes its MULTI notes there.
        Setup ownFirst = multi5Out;
        ownFirst.partOutputs = true;
        ownFirst.mask = 1;
        const Take own = take(ownFirst);
        const bool ownBeforeMulti = level(own.part1, -1) > 0.001 && level(own.multi[2], -1) == 0.0;
        std::printf("      MULTI 5 with reverb on: its output %.4f, the mix %.4f; a part on its own output: own %.4f, MULTI 5 %.4f\n",
                    level(multi5.multi[2], 0), level(multi5.mix, -1), level(own.part1, -1), level(own.multi[2], -1));
        check(multiExact && multi5Heard && ownBeforeMulti,
              "MULTI outputs: notes on MULTI 1-6 play out of their own (mono) output instead of the mix, at the full level "
              "of their left and right in the mix (at 48 and 44.1 kHz); MULTI 5 and 6 even with reverb on; a part's own "
              "output comes first");

        // MULTI as stereo pairs (setMultiPairsStereo): notes on either output of a pair play out of both its channels with
        // their pan, dry. Part 1 on MULTI 2 panned hard left and the bass drum on MULTI 2 make the pair 1+2 the mix of those
        // two notes alone on Mix (dry), sample for sample, at 48 and 44.1 kHz; part 1 alone is on the left only (as a mono
        // output, MULTI 2 is the right channel).
        bool pairsExact = true;
        for (uint32_t rate : {48000u, 44100u}) {
            Setup pairsSetup;
            pairsSetup.partOutputs = false;
            pairsSetup.multiOutputs = true;
            pairsSetup.multiPairs = true;
            pairsSetup.rate = rate;
            pairsSetup.outputAssign = 3;  // MULTI 2
            pairsSetup.pan = 0;           // Hard left
            const Take pairs = take(pairsSetup);
            Setup othersSetup = pairsSetup;
            othersSetup.multiOutputs = false;
            othersSetup.muted = 1u | 1u << 8;
            const Take others = take(othersSetup);
            Setup aloneSetup = othersSetup;
            aloneSetup.outputAssign = 0;  // Mix (dry)
            aloneSetup.drumAssign = 0;
            aloneSetup.muted = 0xFFFFu & ~(1u | 1u << 8);
            const Take alone = take(aloneSetup);
            Setup leftSetup = pairsSetup;
            leftSetup.part1Only = true;
            const Take left = take(leftSetup);
            const bool mixEqual = pairs.mix == others.mix, pairEqual = pairs.multi[0] == alone.mix;
            const bool restSilent = level(pairs.multi[1], -1) < 1e-12 && level(pairs.multi[2], -1) < 1e-12;
            const bool onTheLeft = level(left.multi[0], 0) > 0.001 && level(left.multi[0], 1) < 1e-12;  // (srctools' 1e-22 at 44.1 kHz)
            std::printf("      MULTI pairs at %u Hz: mix as with those notes muted %d, Multi 1+2 as the two alone on Mix %d "
                        "(left %.4f, right %.4f), part 1 alone on the left %d, Multi 3+4 and 5+6 silent %d\n",
                        rate, mixEqual, pairEqual, level(pairs.multi[0], 0), level(pairs.multi[0], 1), onTheLeft, restSilent);
            pairsExact = pairsExact && mixEqual && pairEqual && restSilent && onTheLeft && level(pairs.multi[0], 1) > 0.001;
        }
        check(pairsExact,
              "MULTI as stereo pairs: notes on either output of a pair (MULTI 2 here) play out of both its channels with "
              "their pan and dry, sample for sample as on Mix alone (at 48 and 44.1 kHz)");
    }

    // 7.1 surround (the standalone): MULTI 1 and 2 at the front left and right with the mix, 3 and 4 at the back (rear),
    // 5 and 6 at the sides, in AudioOutput's order (WASAPI's: FL FR FC LFE BL BR SL SR); centre and LFE silent. Each
    // MULTI output alone, with reverb on (the power-on setup).
    {
        const int speakerOf[6] = {0, 1, 4, 5, 6, 7};
        const char* const speakerNames[8] = {"FL", "FR", "FC", "LFE", "BL", "BR", "SL", "SR"};
        const auto surroundTake = [&](bool multiOutputs, uint8_t outputAssign, uint8_t pan, std::array<double, 8>& energy,
                                      bool pairs = false) {
            SynthEngine surroundEngine;
            EngineConfig surroundConfig = config;
            surroundConfig.multiOutputs = multiOutputs;
            surroundEngine.setOptions(options);
            surroundEngine.setMultiPairsStereo(pairs);
            surroundEngine.configure(surroundConfig, error);
            surroundEngine.writeData(0x030006, &outputAssign, 1);
            surroundEngine.writeData(0x030009, &pan, 1);
            std::srand(1);
            std::vector<float> frames(size_t(SynthEngine::kSurroundChannels) * 512);
            energy.fill(0.0);
            for (uint32_t done = 0; done < 24000; done += 512) {
                if (done == 1024) noteOn(surroundEngine, 0, 60);
                surroundEngine.renderSurround(frames.data(), 512);
                for (size_t i = 0; i < frames.size(); i++) energy[i % 8] += double(frames[i]) * frames[i];
            }
        };
        bool routed = true;
        std::string heardOn;
        for (int n = 0; n < 6; n++) {
            std::array<double, 8> energy{};
            surroundTake(true, uint8_t(2 + n), 7, energy);
            std::string speakers;
            for (int c = 0; c < 8; c++) {
                if (energy[size_t(c)] > 0.0) speakers += std::string(speakers.empty() ? "" : "+") + speakerNames[c];
                if (c == speakerOf[n] ? energy[size_t(c)] < 1.0 : energy[size_t(c)] != 0.0) routed = false;
            }
            heardOn += " MULTI " + std::to_string(n + 1) + ": " + (speakers.empty() ? "none" : speakers) + ";";
        }
        // The mix at the front: part 1 on Mix, panned hard right, only on the front right; the same without MULTI outputs.
        std::array<double, 8> mixEnergy{}, stereoEnergy{};
        surroundTake(true, 0, 14, mixEnergy);
        surroundTake(false, 0, 14, stereoEnergy);
        const auto onlyOn = [](const std::array<double, 8>& energy, int speaker) {
            for (int c = 0; c < 8; c++) {
                if (c == speaker ? energy[size_t(c)] < 1.0 : energy[size_t(c)] > energy[size_t(speaker)] * 1e-6) return false;
            }
            return true;
        };
        const bool mixInFront = onlyOn(mixEnergy, 1) && onlyOn(stereoEnergy, 1) && mixEnergy == stereoEnergy;
        std::printf("     %s the mix (part 1 hard right) FR only %d, as without MULTI outputs %d\n", heardOn.c_str(),
                    onlyOn(mixEnergy, 1), mixEnergy == stereoEnergy);
        check(routed && mixInFront,
              "7.1 surround: each MULTI output on its own speaker (1 and 2 front left and right, 3 and 4 back, 5 and 6 "
              "side; centre and LFE silent), MULTI 5 and 6 with reverb on; the mix at the front");

        // MULTI as stereo pairs of speakers (front, rear, side): part 1 on MULTI 4 panned hard left plays at the rear left
        // only (a mono MULTI 4 is the rear right), centred on both rear speakers; on MULTI 5, centred, on both sides.
        std::array<double, 8> rearLeft{}, rearBoth{}, sides{};
        surroundTake(true, 5, 0, rearLeft, true);
        surroundTake(true, 5, 7, rearBoth, true);
        surroundTake(true, 6, 7, sides, true);
        const auto only = [](const std::array<double, 8>& energy, std::initializer_list<int> speakers) {
            for (int c = 0; c < 8; c++) {
                const bool wanted = std::find(speakers.begin(), speakers.end(), c) != speakers.end();
                if (wanted ? energy[size_t(c)] < 1.0 : energy[size_t(c)] != 0.0) return false;
            }
            return true;
        };
        // (Centred is 8:6 without nice panning, as in the mix: the LA32 pans in even steps.)
        const bool pairedSpeakers = only(rearLeft, {4}) && only(rearBoth, {4, 5}) && only(sides, {6, 7});
        std::printf("      7.1 as stereo pairs: part 1 on MULTI 4 panned left BL only %d, centred BL and BR %d (%.0f, %.0f); on MULTI 5 "
                    "centred SL and SR %d (%.0f, %.0f)\n",
                    only(rearLeft, {4}), only(rearBoth, {4, 5}), rearBoth[4], rearBoth[5], only(sides, {6, 7}), sides[6], sides[7]);
        check(pairedSpeakers,
              "7.1 surround with MULTI as stereo pairs: a note on either output of a pair plays on that pair of speakers with "
              "its pan (rear, side)");

        // The audio device: miniaudio (its null backend here, WASAPI on Windows) opens 8 channels in that order and pulls
        // the engine's surround frames.
        SynthEngine deviceEngine;
        EngineConfig deviceConfig = config;
        deviceConfig.multiOutputs = true;
        deviceEngine.setOptions(options);
        deviceEngine.configure(deviceConfig, error);
        struct Pulled {
            SynthEngine* engine;
            std::atomic<uint64_t> frames{0};
        } pulled{&deviceEngine};
        AudioOutput device(true);
        std::string audioError;
        const bool opened = device.open("", 48000, 480, AudioOutput::Layout::Surround71,
                                        [](void* user, float* interleaved, uint32_t frames) {
                                            Pulled* target = static_cast<Pulled*>(user);
                                            target->engine->renderSurround(interleaved, frames);
                                            target->frames += frames;
                                        },
                                        &pulled, audioError);
        const bool started = opened && device.start(audioError);
        for (int wait = 0; wait < 100 && pulled.frames < 4800; wait++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const std::string ours = device.speakers(), theirs = device.deviceSpeakers();
        const uint32_t channels = device.channels();
        device.close();
        std::printf("      device: opened %d, started %d (%s), %u channels \"%s\", the device's \"%s\", %llu frames pulled\n", opened,
                    started, audioError.c_str(), channels, ours.c_str(), theirs.c_str(), (unsigned long long)pulled.frames.load());
        check(started && channels == 8 && ours == "FL FR FC LFE BL BR SL SR" && theirs == ours && pulled.frames >= 4800,
              "7.1 surround device: 8 channels by speaker (FL FR FC LFE BL BR SL SR, WASAPI's 7.1 order), the engine's "
              "surround frames pulled");

        // What miniaudio makes of them on a device with other speakers (mixing by position, as it does for a device):
        // each MULTI output on its own speaker of a 7.1 device in either order (WASAPI's, ALSA's), and still heard on
        // 5.1 and stereo devices.
        const ma_channel ourMap[8] = {MA_CHANNEL_FRONT_LEFT, MA_CHANNEL_FRONT_RIGHT, MA_CHANNEL_FRONT_CENTER, MA_CHANNEL_LFE,
                                      MA_CHANNEL_BACK_LEFT,  MA_CHANNEL_BACK_RIGHT,  MA_CHANNEL_SIDE_LEFT,    MA_CHANNEL_SIDE_RIGHT};
        struct DeviceLayout {
            const char* name;
            std::vector<ma_channel> map;
        };
        const DeviceLayout layouts[] = {
            {"7.1 (WASAPI)", {MA_CHANNEL_FRONT_LEFT, MA_CHANNEL_FRONT_RIGHT, MA_CHANNEL_FRONT_CENTER, MA_CHANNEL_LFE,
                              MA_CHANNEL_BACK_LEFT, MA_CHANNEL_BACK_RIGHT, MA_CHANNEL_SIDE_LEFT, MA_CHANNEL_SIDE_RIGHT}},
            {"7.1 (ALSA)", {MA_CHANNEL_FRONT_LEFT, MA_CHANNEL_FRONT_RIGHT, MA_CHANNEL_BACK_LEFT, MA_CHANNEL_BACK_RIGHT,
                            MA_CHANNEL_FRONT_CENTER, MA_CHANNEL_LFE, MA_CHANNEL_SIDE_LEFT, MA_CHANNEL_SIDE_RIGHT}},
            {"5.1 (side)", {MA_CHANNEL_FRONT_LEFT, MA_CHANNEL_FRONT_RIGHT, MA_CHANNEL_FRONT_CENTER, MA_CHANNEL_LFE,
                            MA_CHANNEL_SIDE_LEFT, MA_CHANNEL_SIDE_RIGHT}},
            {"5.1 (back)", {MA_CHANNEL_FRONT_LEFT, MA_CHANNEL_FRONT_RIGHT, MA_CHANNEL_FRONT_CENTER, MA_CHANNEL_LFE,
                            MA_CHANNEL_BACK_LEFT, MA_CHANNEL_BACK_RIGHT}},
            {"stereo", {MA_CHANNEL_FRONT_LEFT, MA_CHANNEL_FRONT_RIGHT}},
        };
        const auto positionName = [&](ma_channel position) {
            for (int c = 0; c < 8; c++) {
                if (ourMap[c] == position) return speakerNames[c];
            }
            return "?";
        };
        bool mapped = true;
        for (const DeviceLayout& layout : layouts) {
            const ma_uint32 outChannels = ma_uint32(layout.map.size());
            const ma_channel_converter_config converterConfig = ma_channel_converter_config_init(
                ma_format_f32, 8, ourMap, outChannels, layout.map.data(), ma_channel_mix_mode_default);
            ma_channel_converter converter;
            if (ma_channel_converter_init(&converterConfig, nullptr, &converter) != MA_SUCCESS) {
                mapped = false;
                continue;
            }
            std::string report;
            for (int n = 0; n < 6; n++) {
                float in[8] = {};
                in[speakerOf[n]] = 1.0f;
                float out[8] = {};
                ma_channel_converter_process_pcm_frames(&converter, out, in, 1);
                std::string gains;
                double total = 0.0;
                for (ma_uint32 c = 0; c < outChannels; c++) {
                    total += std::fabs(out[c]);
                    // On a 7.1 device, exactly the speaker of the same name.
                    const bool same = layout.map[c] == ourMap[speakerOf[n]];
                    if (outChannels == 8 && (same ? out[c] != 1.0f : out[c] != 0.0f)) mapped = false;
                    if (out[c] == 0.0f) continue;
                    char gain[48];
                    std::snprintf(gain, sizeof(gain), "%s%s %.2f", gains.empty() ? "" : "+", positionName(layout.map[c]), out[c]);
                    gains += gain;
                }
                if (total < 0.25) mapped = false;  // Every MULTI output is heard, at -12 dB or louder
                report += " M" + std::to_string(n + 1) + ": " + (gains.empty() ? "none" : gains) + ";";
            }
            ma_channel_converter_uninit(&converter, nullptr);
            std::printf("      %s device:%s\n", layout.name, report.c_str());
        }
        check(mapped, "7.1 surround on the device: miniaudio puts each MULTI output on the speaker of its name on a 7.1 "
                      "device (in WASAPI's or ALSA's order); 5.1 and stereo devices still get them all (-12 dB or louder)");
    }

    // Memory card: write the part's tone to card tone c12 and its timbre to C-A21; a part given C-A21 plays card tones.
    {
        engine.setPartParameter(2, TimbreTemp::ToneGroup, 1);
        engine.setPartParameter(2, TimbreTemp::ToneNumber, 16);  // b31 Syn Lead 1
        render(engine, 0.02);
        const uint8_t notReady[2] = {1, 1};
        engine.writeData(0x400004, notReady, 2);  // Tone write to card without a card: rejected
        render(engine, 0.02);
        const bool withoutCard = engine.toneNames()[256 + 1].empty();
        engine.insertCard(std::filesystem::path(), error);
        const uint8_t toneToCard[2] = {1, 1};      // Part 3's tone -> card tone c12
        engine.writeData(0x400004, toneToCard, 2);
        const uint8_t timbreToCard[2] = {8, 1};    // Part 3's timbre -> C-A21
        engine.writeData(0x400104, timbreToCard, 2);
        render(engine, 0.02);
        engine.getStatus(status);
        const bool toneWritten = engine.toneNames()[256 + 1] == "Syn Lead 1" && std::string(status.parts[2].tone) == "c12";
        engine.setPartProgram(3, 128 + 8);  // Part 4: card timbre C-A21
        render(engine, 0.02);
        engine.getStatus(status);
        const bool cardTimbre = status.cardInserted && status.parts[3].programFromCard && status.parts[3].program == 8 &&
                                std::string(status.parts[3].tone) == "c12" && std::string(status.parts[3].name) == "Syn Lead 1";
        const std::filesystem::path cardFile = std::filesystem::temp_directory_path() / "d110tests-card.syx";
        const bool saved = engine.saveCard(cardFile, error);
        engine.ejectCard();
        const bool ejected = engine.toneNames()[256 + 1].empty();
        const bool reinserted = engine.insertCard(cardFile, error) && engine.toneNames()[256 + 1] == "Syn Lead 1";
        std::filesystem::remove(cardFile);
        engine.ejectCard();
        std::printf("      card: without %d, tone written %d, card timbre %d, saved %d, ejected %d, reinserted %d\n", withoutCard, toneWritten,
                    cardTimbre, saved, ejected, reinserted);
        check(withoutCard && toneWritten && cardTimbre && saved && ejected && reinserted,
              "memory card: tone and timbre writes to the card, card timbres, save, eject and insert");
    }

    // 16-part mode: parts 9-15 on channels 9 and 11-16, with their own temporary areas at 13 00 00 / 14 00 00.
    {
        SynthEngine sixteen;
        sixteen.setOptions(options);
        EngineConfig sixteenConfig = config;
        sixteenConfig.sixteenParts = true;
        sixteenConfig.partialCount = 512;
        EngineStatus st;
        const bool opened = sixteen.configure(sixteenConfig, error);
        sixteen.getStatus(st);
        check(opened && st.partCount == 16 && st.parts[9].channel == 8 && st.parts[10].channel == 10 && st.parts[15].channel == 15 &&
                  st.parts[kRhythmPart].channel == 9,
              "16-part mode: 15 parts and rhythm, parts 9-15 on channels 9 and 11-16, rhythm on 10");

        // Partial reserves: with 512 partials every part keeps at least 4, the others share the rest in proportion.
        const uint8_t reserves[9] = {0, 10, 6, 4, 3, 0, 0, 0, 9};
        const uint8_t extraReserves[7] = {};
        sixteen.writeData(0x100004, reserves, 9);
        sixteen.writeData(0x110000, extraReserves, 7);
        render(sixteen, 0.02);
        sixteen.getStatus(st);
        uint32_t reservedTotal = 0;
        bool floored = true;
        for (uint32_t i = 0; i < st.partCount; i++) {
            reservedTotal += st.parts[i].reservedPartials;
            if (st.parts[i].reserve == 0 && st.parts[i].reservedPartials != 4) floored = false;
        }
        std::printf("      512 partials: part 1 %u, part 2 %u, rhythm %u, total %u\n", st.parts[0].reservedPartials,
                    st.parts[1].reservedPartials, st.parts[kRhythmPart].reservedPartials, reservedTotal);
        check(floored && st.parts[1].reservedPartials == 146 && st.parts[kRhythmPart].reservedPartials == 131 && reservedTotal <= 512,
              "512 partials: parts with reserve 0 keep 4 partials, the others share the rest in proportion");

        noteOn(sixteen, 11, 60);  // Channel 12: part 11
        render(sixteen, 0.05);
        sixteen.getStatus(st);
        check(st.parts[11].noteCount == 1 && st.parts[11].activePartials > 0, "16-part mode: a note on channel 12 plays part 11");
        allOff(sixteen);

        const uint8_t level = 55;
        sixteen.writeData(0x130008, &level, 1);  // Part 9's output level
        const char toneName[] = "Part9 Tone";
        sixteen.writeData(0x140000, reinterpret_cast<const uint8_t*>(toneName), 10);  // Part 9's tone name
        render(sixteen, 0.02);
        sixteen.getStatus(st);
        check(st.parts[9].temp[TimbreTemp::OutputLevel] == 55 && std::string(st.parts[9].name) == "Part9 Tone",
              "16-part mode: SysEx at 13 00 00 and 14 00 00 reaches part 9");

        const std::vector<uint8_t> dump16 = sixteen.dumpSysex(DumpEverything);
        SynthEngine copy;
        copy.setOptions(options);
        copy.configure(sixteenConfig, error);
        copy.applySysex(dump16);
        check(copy.dumpSysex(DumpEverything) == dump16, "16-part mode: memory dump with parts 9-15 round-trips");
    }

    // MT-32 translation: MT-32 SysEx and controllers converted for the D-110.
    {
        SynthEngine mt32;
        EngineOptions mt32Options = options;
        mt32Options.unitNumber = 20;  // Device ID 13H: MT-32 SysEx (10H) must still arrive
        mt32.setOptions(mt32Options);
        mt32.configure(config, error);
        mt32.setMt32Translation(true);
        mt32.mt32PowerOn();
        render(mt32, 0.02);
        EngineStatus st;
        mt32.getStatus(st);
        check(st.mt32Translation && st.parts[0].channel == 1 && st.parts[7].channel == 8 && st.parts[kRhythmPart].channel == 9 &&
                  std::string(st.parts[0].tone) == "b45" && std::string(st.parts[7].tone) == "b81" && st.parts[0].program == 68 &&
                  st.parts[7].program == 122 && st.parts[4].temp[TimbreTemp::Panpot] == 11 &&
                  st.parts[6].temp[TimbreTemp::Panpot] == 14 && st.parts[0].temp[TimbreTemp::KeyRangeUpper] == 127,
              "MT-32 power-on: parts on channels 2-10 with Slap Bass 1 ... Orche Hit (b45 ... b81), the MT-32's pans");
        check(readByte(mt32, at(0x030110, (38 - 24) * 4)) == 64 + 18 && readByte(mt32, at(0x030110, (41 - 24) * 4 + 2)) == 3 &&
                  readByte(mt32, at(0x030110, (74 - 24) * 4)) == 127 && readByte(mt32, 0x050001) == 0 && readByte(mt32, at(0x050001, 8 * 68)) == 28,
              "MT-32 power-on: rhythm keys 35-75 as on the MT-32 (38 -> r19 SnareDrum1), patch memory A1-B64 -> D-110 presets");

        // Patch temp pan and a dummy key range: MT-32 panpot 0 is hard right.
        sendSysex(mt32, dataSet(0x10, 0x030000, {1, 26, 24, 50, 12, 0, 1, 0, 100, 0, 0, 0, 0, 0, 0, 0}));  // B27 Trombone 1
        render(mt32, 0.05);
        mt32.getStatus(st);
        noteOn(mt32, 1, 60);
        Levels levels = render(mt32, 0.4);
        check(std::string(st.parts[0].tone) == "a63" && st.parts[0].temp[TimbreTemp::Panpot] == 14 &&
                  st.parts[0].temp[TimbreTemp::KeyRangeLower] == 0 && st.parts[0].temp[TimbreTemp::KeyRangeUpper] == 127 &&
                  levels.right > 4.0 * levels.left,
              "MT-32 patch temp (device 10H): Trombone 1 -> a63, panpot 0 plays right, dummy key range -> 0-127");
        check(mt32.options().midiExtensions && !st.midiExtensions && st.parts[0].bendRangeCents == 1200,
              "MT-32 translation plays with the MIDI extensions off, as the MT-32 does: parts bend by their timbres' ranges (here 12)");
        allOff(mt32);
        mt32.onMidiShortMessage(0x0A0AB1);  // CC 10 = 10 on channel 2
        mt32.onMidiShortMessage(0x44C1);    // Program 68 (Slap Bass 1)
        render(mt32, 0.05);
        mt32.getStatus(st);
        check(st.parts[0].temp[TimbreTemp::Panpot] == 13 && std::string(st.parts[0].tone) == "b45",
              "MT-32 CC 10 is mirrored; program changes select the translated MT-32 patches");
        // A stand-in an octave away gets a key shift: the D-110's Syn Bass 3 is the MT-32's an octave down.
        sendSysex(mt32, dataSet(0x10, 0x030000, {0, 30, 24, 50, 12, 0, 1, 0, 100, 7, 0, 0, 0, 0, 0, 0}));  // A31 Syn Bass 3
        render(mt32, 0.05);
        mt32.getStatus(st);
        check(std::string(st.parts[0].tone) == "b37" && st.parts[0].temp[TimbreTemp::KeyShift] == 36 &&
                  readByte(mt32, at(0x050002, 8 * 30)) == 36 && readByte(mt32, at(0x050000, 8 * 27)) == 1 &&
                  readByte(mt32, at(0x050001, 8 * 27)) == 14 && readByte(mt32, at(0x050002, 8 * 27)) == 24,
              "MT-32 stand-ins: A31 Syn Bass 3 -> b37 an octave up (key shift +12), also in patch memory; A28 Syn Brass4 -> b27 Steam Pad");
        mt32.onMidiShortMessage(0x30C1);  // Program 48 (Str Sect 1 -> Strings 1), without a key shift
        render(mt32, 0.05);

        // A timbre upload to the part's tone: MT-32 wave 108 (Slap Bass loop) -> D-110 Slap Bass (Loop), tuned an octave up.
        const std::vector<uint8_t> bass = mt32Timbre("MT BassLp", 108);
        sendSysex(mt32, dataSet(0x10, 0x040000, bass));
        render(mt32, 0.05);
        const bool bassTone = readByte(mt32, 0x04000E) == 48 && readByte(mt32, 0x040012) == 0 && readByte(mt32, 0x040013) == 81;
        noteOn(mt32, 1, 60);
        const double bassPitch = measurePitch(mt32, 0.6);
        allOff(mt32);
        const uint8_t uncorrected = 36;  // Control: the same D-110 wave at the MT-32's coarse pitch sounds an octave lower
        mt32.writeData(0x04000E, &uncorrected, 1);
        render(mt32, 0.02);
        noteOn(mt32, 1, 60);
        const double bassUncorrected = measurePitch(mt32, 0.6);
        allOff(mt32);
        // Into tone memory M1 in two parts, then through the patch memory and a program change: the MT-32's violin
        // loop. The D-110 has the same sample (Loop-63 repeats it); the MT-32 plays it at B3 + 7 cents at coarse 36,
        // 12.93 semitones below the D-110's Violin-1 (the two ROMs' pitch words), and so must the D-110.
        const std::vector<uint8_t> strings = mt32Timbre("MT VlnLp", 115);
        sendSysex(mt32, dataSet(0x10, 0x080000, std::vector<uint8_t>(strings.begin(), strings.begin() + 128)));
        sendSysex(mt32, dataSet(0x10, 0x080100, std::vector<uint8_t>(strings.begin() + 128, strings.end())));
        sendSysex(mt32, dataSet(0x10, 0x050000 + 5 * 8, {2, 0, 24, 50, 12, 0, 1, 0}));  // Patch 6 -> M1
        mt32.onMidiShortMessage(0x05C1);
        render(mt32, 0.05);
        mt32.getStatus(st);
        const bool stringTone = readByte(mt32, 0x08000E) == 23 && readByte(mt32, 0x08000F) == 94 && readByte(mt32, 0x080012) == 2 &&
                                readByte(mt32, 0x080013) == 92 && std::string(st.parts[0].tone) == "i11";
        noteOn(mt32, 1, 60);
        const double stringPitch = measurePitch(mt32, 0.6);
        allOff(mt32);
        const double violinPitch = 442.0 * std::pow(2.0, (72.0 - 12.93 - 69.0) / 12.0);  // Violin-1 sounds C5 at 20608
        std::printf("      MT-32 bass loop %.1f Hz (uncorrected %.1f Hz), violin loop %.1f Hz (MT-32: %.1f Hz); C4 = 262.8 Hz at A = 442 Hz\n",
                    bassPitch, bassUncorrected, stringPitch, violinPitch);
        check(bassTone && stringTone && std::fabs(bassPitch / 262.8 - 1.0) < 0.02 && std::fabs(stringPitch / violinPitch - 1.0) < 0.01 &&
                  std::fabs(bassUncorrected / 131.4 - 1.0) < 0.02,
              "MT-32 timbres: PCM waves become the D-110's, pitch corrected to sound as on the MT-32 (C4 at key 60)");

        // One byte: a fixed-pitch drum for M1's partial 1 moves it to bank 2 and drops the octave correction.
        sendSysex(mt32, dataSet(0x10, 0x080013, {55}));  // Ac. Snare Drum #
        render(mt32, 0.02);
        const bool drum = readByte(mt32, 0x08000E) == 36 && readByte(mt32, 0x080012) == 2 && readByte(mt32, 0x080013) == 3;
        // A part playing a preset: an MT-32 wave written alone becomes a bank-1 stand-in.
        mt32.onMidiShortMessage(0x00C2);  // Part 2: A1 AcouPiano1
        render(mt32, 0.02);
        const uint8_t coarseBefore = readByte(mt32, at(0x040000, 246 + 14));
        sendSysex(mt32, dataSet(0x10, at(0x040000, 246 + 19), {58}));  // Part 2, partial 1 wave: Closed Hihat #
        render(mt32, 0.02);
        const bool standIn = readByte(mt32, at(0x040000, 246 + 19)) == 9 && readByte(mt32, at(0x040000, 246 + 14)) == coarseBefore;
        check(drum && standIn, "MT-32 wave writes: bank 2 for fixed-pitch drums, bank-1 stand-ins for tones not written in full");

        // Rhythm setup, reverb, master volume and the display.
        sendSysex(mt32, dataSet(0x10, at(0x030110, (40 - 24) * 4), {64 + 5, 90, 0, 1}));  // Key 40: R6 Elec SD, hard right
        sendSysex(mt32, dataSet(0x10, 0x100001, {1, 4, 5}));                          // Reverb Hall
        sendSysex(mt32, dataSet(0x10, 0x200000, {'H', 'e', 'l', 'l', 'o', ' ', 'M', 'T', '-', '3', '2'}));
        render(mt32, 0.02);
        mt32.getStatus(st);
        check(readByte(mt32, at(0x030110, (40 - 24) * 4)) == 64 + 19 && readByte(mt32, at(0x030110, (40 - 24) * 4 + 2)) == 14 && st.reverbMode == 2 &&
                  st.lcdMessage == std::string("Hello MT-32") + std::string(21, ' '),
              "MT-32 rhythm setup (R6 -> r20), reverb Hall -> Hall 1, display text padded to 32 characters");
        mt32.onMidiShortMessage(0x00C1);
        render(mt32, 0.05);
        noteOn(mt32, 1, 60);
        const Levels loud = render(mt32, 0.4);
        allOff(mt32);
        sendSysex(mt32, dataSet(0x10, 0x100016, {30}));
        render(mt32, 0.02);
        noteOn(mt32, 1, 60);
        const Levels quiet = render(mt32, 0.4);
        allOff(mt32);
        mt32.getStatus(st);
        const bool volume = st.masterVolume == 30 && quiet.left + quiet.right < 0.5 * (loud.left + loud.right);
        mt32.setMt32Translation(false);
        mt32.getStatus(st);
        check(volume && st.masterVolume == 100, "MT-32 master volume applies while translating, 100 again afterwards");

        // Without translation: CC 10 is not mirrored. With it: a reset brings back the power-on setup, D-series SysEx passes.
        mt32.onMidiShortMessage(0x0A0AB1);
        render(mt32, 0.02);
        mt32.getStatus(st);
        const bool untranslated = st.parts[0].temp[TimbreTemp::Panpot] == 1;
        mt32.setMt32Translation(true);
        const std::array<uint8_t, kMaxPartCount> channels = {0, 1, 2, 3, 4, 5, 6, 7, 9, 8, 10, 11, 12, 13, 14, 15};
        mt32.setPartChannels(channels);
        sendSysex(mt32, dataSet(0x10, 0x7F0000, {0}));
        const char patchName[] = "D-110 Only";
        sendSysex(mt32, dataSet(0x10, 0x060000 + 0x00, std::vector<uint8_t>(patchName, patchName + 10)));
        render(mt32, 0.02);
        mt32.getStatus(st);
        char name[11] = {};
        mt32.readMemory(0x060000, 10, reinterpret_cast<uint8_t*>(name));
        const std::vector<uint8_t> d110Dump = mt32.dumpSysex(DumpEverything);
        const std::vector<uint8_t> mt32File = dataSet(0x10, 0x100001, {0, 5, 3});
        check(untranslated && st.parts[0].channel == 1 && st.parts[7].channel == 8 && std::string(name) == patchName &&
                  Mt32Translator::isDSeriesData(d110Dump.data(), d110Dump.size()) && !Mt32Translator::isDSeriesData(mt32File.data(), mt32File.size()),
              "MT-32 translation off leaves CC 10 alone; MT-32 reset -> power-on setup; D-110-only SysEx is not translated");

        // The wave table: every MT-32 wave has a D-110 wave, bank-1 stand-ins are in bank 1, corrections stay within
        // 5 octaves. The MT-32's Tubular Bells loop plays its sample 57.53 semitones below the D-110's Loop-43, which
        // fits from coarse 58 up; lower, the D-110's Wind Bell (the same sample, not looped) takes over at -36.34.
        bool tableOk = true;
        for (int wave = 0; wave < 256; wave++) {
            const Mt32Translator::WaveMapping& m = Mt32Translator::waveMapping(wave);
            if (m.bank1Wave >= 128 || m.coarse < -60 || m.coarse > 36 || m.fine < -50 || m.fine > 50) tableOk = false;
        }
        uint8_t pcmWave = 0, pcmCoarse = 0, pcmFine = 0;
        Mt32Translator::translatePcm(106, 72, 50, pcmWave, pcmCoarse, pcmFine);
        const bool looped = pcmWave == 200 && pcmCoarse == 14 && pcmFine == 97;  // Loop-43: 72 - 57.53 = 14 + 47 cents
        Mt32Translator::translatePcm(106, 48, 50, pcmWave, pcmCoarse, pcmFine);
        const bool fallback = pcmWave == 51 && pcmCoarse == 12 && pcmFine == 16;  // Wind Bell: 48 - 36.34 = 12 - 34 cents
        Mt32Translator::translatePcm(28, 36, 82, pcmWave, pcmCoarse, pcmFine);    // Shakuhachi +32 cents, +43 more
        const bool carried = pcmWave == 44 && pcmCoarse == 37 && pcmFine == 25;   // Breath: 1 semitone and 25 cents up
        std::printf("      Tubular Bells loop: coarse 72 on Loop-43 %s, coarse 48 on Wind Bell %s; fine pitch carried %s\n", looped ? "yes" : "no",
                    fallback ? "yes" : "no", carried ? "yes" : "no");
        if (!looped || !fallback || !carried) tableOk = false;
        for (int timbre = 0; timbre < 128; timbre++) {
            if (Mt32Translator::presetTone(timbre) >= 256 || (Mt32Translator::presetTone(timbre) >= 128 && Mt32Translator::presetTone(timbre) < 192)) tableOk = false;
        }
        check(tableOk, "MT-32 wave, preset and rhythm tables are within the D-110's ranges");

        // The user's corrections: Timpani on Timpani Attack (+12, as the D-110's own Timpani tunes both samples alike),
        // wave 53 on Spectrum-7 (the same waveform: -18.18 for the pitch words, +0.43 for a 2.5% stretch), the breath
        // noise loop at the recorded rate as on the MT-32; and their stand-in choices, now the defaults.
        const Mt32Translator::WaveMapping& timpani = Mt32Translator::waveMapping(46);
        const Mt32Translator::WaveMapping& spectrum = Mt32Translator::waveMapping(53);
        const Mt32Translator::WaveMapping& breath = Mt32Translator::waveMapping(24);
        // The user's choices by ear: Slap Bass -> Thump Bass, the Picked Bass loop -> Electric Piano-2 (Loop) +24, Steel Gt ->
        // Acoustic Piano Low +12, the El. Snare sample (2, 76, and 56 as tones use it, e.g. VOICE_MT.DAT's E-TOM) ->
        // Snare Drum-1 +4 while the MT-32's rhythm timbres keep the drum kit's Snare Drum-2*, and Spectrum-7 keeps the
        // MT-32's fine tune (a loop).
        auto maps = [](int mt32Wave, int wave, int coarse, int fine) {
            const Mt32Translator::WaveMapping& mapping = Mt32Translator::waveMapping(mt32Wave);
            return mapping.wave == wave && mapping.coarse == coarse && mapping.fine == fine;
        };
        const bool waves = timpani.wave == 30 && timpani.coarse == 12 && timpani.fine == 0 && spectrum.wave == 106 &&
                           spectrum.coarse == -18 && spectrum.fine == 0 && breath.wave == 110 && breath.coarse == 0 && breath.fine == 0 &&
                           maps(36, 60, 12, 0) && maps(37, 92, 24, 0) && maps(40, 33, 12, 0) && maps(2, 3, 4, 0) && maps(76, 160, 4, 0) &&
                           maps(56, 131, 4, 0) && Mt32Translator::waveMapping(56, true).wave == 132 &&
                           Mt32Translator::waveMapping(56, true).coarse == 0 && Mt32Translator::waveMapping(2, true).wave == 3 &&
                           Mt32Translator::rhythmTone(5) == 19;
        // An E-TOM-like tone (El. Snare # at C4) as a tone and as a rhythm timbre: bank 2, wave 3 (Snare Drum-1*) 4 up, or
        // wave 4 (Snare Drum-2*) unshifted.
        uint8_t eTom[246], eTomKit[246];
        const std::vector<uint8_t> eTomMt32 = mt32Timbre("E-TOM", 56);
        Mt32Translator().convertTone(eTomMt32.data(), eTom);
        Mt32Translator().convertTone(eTomMt32.data(), eTomKit, true);
        const bool snares = eTom[14 + 5] == 3 && (eTom[14 + 4] & 2) != 0 && eTom[14] == 40 && eTomKit[14 + 5] == 4 &&
                            (eTomKit[14 + 4] & 2) != 0 && eTomKit[14] == 36;
        using Choice = Mt32Translator::PresetChoice;
        const struct { int timbre; Choice choice; } userChoices[] = {
            {10, {8, 0, true}}, {14, {14, 12, false}}, {29, {17, 0, false}}, {50, {32, 0, false}},
            {86, {57, 12, false}}, {107, {48, -12, true}}, {112, {116, 12, false}},
        };
        bool defaults = true;
        for (const auto& user : userChoices) {
            if (!(Mt32Translator::defaultPresetChoice(user.timbre) == user.choice)) defaults = false;
        }
        const PipeSettings pipeDefaults;
        for (int timbre = 0; timbre < 128; timbre++) {
            if (!(pipeDefaults.presetChoices[size_t(timbre)] == Mt32Translator::defaultPresetChoice(timbre))) defaults = false;
        }
        check(waves && snares && defaults,
              "MT-32 waves as the user chose them (Timpani Attack, Spectrum-7 -18 keeping the fine tune, Thump Bass, Electric Piano-2 "
              "loop, Acoustic Piano Low, El. Snare and El. Snare # as Snare Drum-1 +4 outside the drum kit); preset defaults");
    }

    // Reset MIDI while translating: every part's level at 100 and pan centred, through the translation, which keeps them
    // when it sends the parts again (the roomy toms switched on); the MT-32's power-on setup keeps its own.
    {
        SynthEngine translated;
        translated.setOptions(options);
        translated.configure(config, error);
        translated.setMt32Translation(true);
        translated.mt32PowerOn();
        render(translated, 0.02);
        EngineStatus st;
        auto allReset = [&] {
            translated.getStatus(st);
            bool reset = true;
            for (uint32_t i = 0; i < st.partCount; i++) {
                reset = reset && st.parts[i].temp[TimbreTemp::OutputLevel] == 100 && st.parts[i].temp[TimbreTemp::Panpot] == 7;
            }
            return reset;
        };
        translated.getStatus(st);
        const bool powerOn = st.parts[0].temp[TimbreTemp::OutputLevel] == 80 && st.parts[4].temp[TimbreTemp::Panpot] == 11;
        translated.resetMidiChannels();
        const bool reset = allReset();
        translated.setMt32RoomyToms(true);
        render(translated, 0.02);
        const bool kept = allReset();
        check(powerOn && reset && kept,
              "Reset MIDI while translating: levels 100 and pans centred, kept when the translation sends the parts again");
    }

    // Exact MT-32 presets, from an MT-32 or CM-32L control ROM in the ROM folder.
    {
        int presetRom = -1;
        for (size_t i = 0; i < roms.size(); i++) {
            const std::string family = romFamily(roms[i]);
            if (presetRom < 0 && roms[i].isControl && (family == "mt32" || family == "cm32l")) presetRom = int(i);
        }
        std::shared_ptr<Mt32Presets> presets = std::make_shared<Mt32Presets>();
        std::string presetError;
        if (presetRom < 0 || !loadMt32Presets(roms[size_t(presetRom)].path, *presets, presetError)) {
            std::printf("SKIP  exact MT-32 presets: no MT-32 or CM-32L control ROM in the ROM folder\n");
        } else {
            const bool decoded = Mt32Presets::name(presets->melodic[0]) == "AcouPiano1" && Mt32Presets::name(presets->melodic[127]) == "JungleTune" &&
                                 Mt32Presets::name(presets->rhythm[0]) == "Acou BD" && presets->melodic[0][14 + 3 * 58 + 5] == 20;
            std::printf("      presets from %s\n", presets->description.c_str());
            check(decoded, "MT-32 presets decoded from the control ROM: A1 AcouPiano1 ... B64 JungleTune, R1 Acou BD");

            // d110emu: the D-110's preset banks hold the translated MT-32 presets while translating.
            SynthEngine exact;
            exact.setOptions(options);
            exact.configure(config, error);
            const std::vector<std::string> d110Names = exact.toneNames();
            exact.setMt32Presets(presets);
            exact.setMt32Translation(true);
            exact.mt32PowerOn();
            render(exact, 0.05);
            EngineStatus st;
            exact.getStatus(st);
            uint8_t expected[246];
            Mt32Translator().convertTone(presets->melodic[68].data(), expected);
            uint8_t temp[246];
            exact.readMemory(0x040000, sizeof(temp), temp);
            auto startsWith = [](const char* text, const char* prefix) { return std::strncmp(text, prefix, std::strlen(prefix)) == 0; };
            check(st.mt32PresetBanks && std::string(st.parts[0].tone) == "b15" && startsWith(st.parts[0].name, "Slap Bass1") &&
                      std::memcmp(temp, expected, sizeof(temp)) == 0 && readByte(exact, at(0x030110, (38 - 24) * 4)) == 64 + 1 &&
                      startsWith(exact.toneNames()[192 + 1].c_str(), "Acou SD"),
                  "exact MT-32 presets: part 1 plays the MT-32's B5 Slap Bass1 (as b15), key 38 its R2 Acou SD (as r02)");
            noteOn(exact, 9, 38);
            const Levels drum = render(exact, 0.2);
            allOff(exact);
            // The drum kit keeps its El. Snare # (R6 Elec SD on Snare Drum-2*): r06 holds R6 translated as a rhythm timbre,
            // which differs from R6 translated as a tone. Part 8 plays r06 to show it.
            uint8_t kit[246], kitAsTone[246];
            Mt32Translator().convertTone(presets->rhythm[5].data(), kit, true);
            Mt32Translator().convertTone(presets->rhythm[5].data(), kitAsTone);
            const uint8_t r06[2] = {3, 5};
            exact.writePartTemp(7, TimbreTemp::ToneGroup, r06, 2);
            render(exact, 0.02);
            exact.readMemory(at(0x040000, 7 * 246), sizeof(temp), temp);
            const bool kitBank = std::memcmp(temp, kit, sizeof(temp)) == 0 && std::memcmp(kit, kitAsTone, sizeof(kit)) != 0;
            // On a real unit, a part that selects R6 gets the drum kit's version too.
            Mt32Translator kitPart;
            kitPart.setPresets(presets);
            kitPart.setPresetMode(Mt32Translator::PresetMode::Exact);
            std::vector<uint8_t> kitOut;
            std::vector<uint32_t> kitShort;
            const std::vector<uint8_t> selectR6 = dataSet(0x10, 0x030000, {3, 5});
            kitPart.translateSysex(selectR6.data(), selectR6.size(), 0x10, kitOut, kitShort);
            const bool kitSent = kitOut.size() == 13 + 256 && std::memcmp(&kitOut[13 + 8], kit, sizeof(kit)) == 0;
            check(kitBank && kitSent, "the MT-32's rhythm timbres keep the drum kit's waves: R6 Elec SD's El. Snare # on Snare Drum-2*, "
                                      "in the r06 bank slot and when a part selects it");
            exact.onMidiShortMessage(0x5AC1);  // Program 90: B27 Trombone 1
            render(exact, 0.05);
            exact.getStatus(st);
            const bool programmed = std::string(st.parts[0].tone) == "b43" && startsWith(st.parts[0].name, "Trombone 1");
            exact.setMt32PresetMode(Mt32Translator::PresetMode::StandIns);  // D-110 stand-ins, sent again: B27 -> a63
            render(exact, 0.05);
            exact.getStatus(st);
            const bool standIns = !st.mt32PresetBanks && std::string(st.parts[0].tone) == "a63" && exact.toneNames()[0] == d110Names[0] &&
                                  readByte(exact, at(0x030110, (38 - 24) * 4)) == 64 + 18;
            exact.setMt32PresetMode(Mt32Translator::PresetMode::Exact);
            render(exact, 0.05);
            exact.getStatus(st);
            const bool again = st.mt32PresetBanks && std::string(st.parts[0].tone) == "b43";
            exact.setMt32Translation(false);
            const std::vector<std::string> namesAfter = exact.toneNames();
            const bool restored = std::equal(d110Names.begin(), d110Names.begin() + 256, namesAfter.begin());
            std::printf("      drum %.4f, program change %d, stand-ins %d, exact again %d, D-110 presets back %d\n", drum.left + drum.right,
                        programmed, standIns, again, restored);
            check(drum.left + drum.right > 0.001 && programmed && standIns && again && restored,
                  "exact MT-32 presets: rhythm sounds, program changes; switching them off brings stand-ins, ending translation the D-110's presets");

            // Real units: the preset goes to the part's tone temp after each selection (Exact mode).
            Mt32Translator translator;
            translator.setPresets(presets);
            translator.setPresetMode(Mt32Translator::PresetMode::Exact);
            std::vector<uint8_t> after;
            std::vector<uint32_t> shortOut;
            translator.translateShort(0x00C1, 0x10, shortOut, after);  // Channel 2 (part 1): A1 AcouPiano1
            Mt32Translator().convertTone(presets->melodic[0].data(), expected);
            const bool programTone = shortOut == std::vector<uint32_t>{0x00C1} && after.size() == 246 + 10 && after[4] == 0x12 &&
                                     after[5] == 0x04 && after[6] == 0 && after[7] == 0 && std::memcmp(&after[8], expected, 246) == 0;
            std::vector<uint8_t> out;
            std::vector<uint32_t> programs;
            const std::vector<uint8_t> select = dataSet(0x10, 0x030010, {0, 1});  // Part 2: A2 AcouPiano2
            translator.translateSysex(select.data(), select.size(), 0x11, out, programs);
            Mt32Translator().convertTone(presets->melodic[1].data(), expected);
            // The patch temp message (group, number and key shift), then the tone.
            const bool selectTone = out.size() == 13 + 256 && out[2] == 0x11 && out[5] == 0x03 && out[7] == 0x10 && out[13 + 5] == 0x04 &&
                                    out[13 + 6] == 0x01 && out[13 + 7] == 0x76 && std::memcmp(&out[13 + 8], expected, 246) == 0;
            // Stand-ins: the first edit of a preset part's tone sends it whole, as the unit holds another tone.
            Mt32Translator standIn;
            standIn.setPresets(presets);
            after.clear();
            standIn.translateShort(0x00C1, 0x10, shortOut, after);
            out.clear();
            const std::vector<uint8_t> edit = dataSet(0x10, at(0x040000, 14 + 41), {80});  // Partial 1 TVA level
            standIn.translateSysex(edit.data(), edit.size(), 0x10, out, programs);
            Mt32Translator().convertTone(presets->melodic[0].data(), expected);
            expected[14 + 41] = 80;
            const bool upgraded = after.empty() && out.size() == 256 && std::memcmp(&out[8], expected, 246) == 0;
            check(programTone && selectTone && upgraded,
                  "MT-32 presets on real units: program changes and timbre selections send the preset's tone; stand-ins get it whole on the first edit");

            // d110emu's tone banks d and e hold the MT-32's presets beside the D-110's; the timbres P-D11-P-E88 play them.
            {
                SynthEngine banks;
                banks.setOptions(options);
                banks.configure(config, error);
                const std::vector<std::string> romNames = banks.toneNames();
                banks.setMt32Presets(presets);
                render(banks, 0.02);
                EngineStatus bs;
                banks.getStatus(bs);
                std::vector<std::string> names = banks.toneNames();
                const bool listed = bs.altTones && !bs.mt32PresetBanks && names.size() == 448 && startsWith(names[320].c_str(), "AcouPiano1") &&
                                    startsWith(names[384 + 4].c_str(), "Slap Bass1") &&
                                    std::equal(romNames.begin(), romNames.begin() + 256, names.begin());
                banks.setPartProgram(0, 256 + 4);  // P-D15: tone d15, the MT-32's A5
                render(banks, 0.02);
                banks.getStatus(bs);
                Mt32Translator().convertTone(presets->melodic[4].data(), expected);
                banks.readMemory(0x040000, sizeof(temp), temp);
                const bool played = std::string(bs.parts[0].tone) == "d15" && bs.parts[0].programFromAlt && bs.parts[0].program == 4 &&
                                    bs.parts[0].temp[7] == MT32Emu::PART_ALT_TONES && bs.parts[0].temp[TimbreTemp::KeyShift] == 24 &&
                                    std::memcmp(temp, expected, sizeof(temp)) == 0;
                noteOn(banks, 0, 60);
                const Levels altLevel = render(banks, 0.3);
                allOff(banks);
                // A patch keeps the flag: write I-22, load another tone, recall I-22 on the control channel.
                const uint8_t writePatch[2] = {9, 0};
                banks.writeData(0x400200, writePatch, 2);
                render(banks, 0.02);  // SysEx waits for the next render; setPartProgram() does not
                banks.setPartProgram(0, 0);
                render(banks, 0.02);
                banks.onMidiShortMessage(0xCF | (9 << 8));
                render(banks, 0.02);
                banks.getStatus(bs);
                const bool recalled = std::string(bs.parts[0].tone) == "d15";
                // So does a timbre written from the part: program change 21 (I-A36) then plays d15 again.
                const uint8_t writeTimbre[2] = {20, 0};
                banks.writeData(0x400100, writeTimbre, 2);
                render(banks, 0.02);
                banks.setPartProgram(0, 0);
                render(banks, 0.02);
                banks.onMidiShortMessage(0xC0 | (20 << 8));
                render(banks, 0.02);
                banks.getStatus(bs);
                const bool stored = std::string(bs.parts[0].tone) == "d15" && !bs.parts[0].programFromAlt && bs.parts[0].program == 20 &&
                                    readByte(banks, at(0x050000, 20 * 8 + 7)) == MT32Emu::PART_ALT_TONES;
                // Without the MT-32's presets, the part plays a15 of the same number.
                banks.setMt32Presets(nullptr);
                render(banks, 0.02);
                banks.getStatus(bs);
                const bool fallback = !bs.altTones && std::string(bs.parts[0].tone) == "a15";
                std::printf("      tone banks: listed %d, played %d (%.4f), patch %d, timbre %d, fallback %d\n", listed, played,
                            altLevel.left + altLevel.right, recalled, stored, fallback);
                check(listed && played && altLevel.left + altLevel.right > 1e-3 && recalled && stored && fallback,
                      "tone banks d and e: the MT-32's presets beside the D-110's, P-D15 plays d15; patches and timbres keep them; a and b without them");
            }

            // Translating with presets chosen one by one: a, b and r hold what each MT-32 preset and rhythm sound plays (the
            // MT-32's own, or a copy of its D-110 stand-in, whose key shift the part gets), and d and e the D-110's a and b.
            {
                SynthEngine hybrid;
                hybrid.setOptions(options);
                hybrid.configure(config, error);
                const std::vector<std::string> romNames = hybrid.toneNames();
                hybrid.setMt32PresetMode(Mt32Translator::PresetMode::Hybrid);
                hybrid.setMt32Presets(presets);
                hybrid.setMt32Translation(true);
                hybrid.mt32PowerOn();
                render(hybrid, 0.05);
                EngineStatus hs;
                hybrid.getStatus(hs);
                std::vector<std::string> names = hybrid.toneNames();
                const Mt32Translator::PresetChoice slap = Mt32Translator::defaultPresetChoice(68);  // B5 Slap Bass 1, part 1
                const int snare = Mt32Translator::rhythmTone(1);                                      // R2 Acou SD's stand-in
                const bool standIn = hs.mt32PresetBanks && std::string(hs.parts[0].tone) == "b15" && !slap.exact && slap.keyShift == 12 &&
                                     names[68] == romNames[slap.tone] && hs.parts[0].temp[TimbreTemp::KeyShift] == 24 + 12 &&
                                     startsWith(names[62].c_str(), "Elec Gtr 2") &&  // A63: the MT-32's own is preferred
                                     std::equal(romNames.begin(), romNames.begin() + 128, names.begin() + 320) &&
                                     readByte(hybrid, at(0x030110, (38 - 24) * 4)) == 64 + 1 && names[192 + 1] == romNames[size_t(192 + snare)];
                Mt32Translator::PresetChoices choices;
                Mt32Translator::RhythmChoices rhythmChoices;
                Mt32Translator::parsePresetChoices("", choices);
                Mt32Translator::parseRhythmChoices("", rhythmChoices);
                choices[68].exact = true;       // Slap Bass 1: the MT-32's own
                rhythmChoices[1].exact = true;  // Acou SD: the MT-32's own
                rhythmChoices[0].tone = 15;     // Acou BD: r16 instead of the built-in stand-in
                hybrid.setMt32PresetChoices(choices, rhythmChoices);
                render(hybrid, 0.05);
                hybrid.getStatus(hs);
                names = hybrid.toneNames();
                const bool picked = std::string(hs.parts[0].tone) == "b15" && startsWith(hs.parts[0].name, "Slap Bass1") &&
                                    hs.parts[0].temp[TimbreTemp::KeyShift] == 24 && startsWith(names[192 + 1].c_str(), "Acou SD") &&
                                    names[192] == romNames[192 + 15];
                noteOn(hybrid, 9, 38);
                const Levels drum = render(hybrid, 0.2);
                allOff(hybrid);
                // The MT-32's rhythm "OFF" (R31 on an MT-32) has no stand-in: the key is off on the D-110 too.
                Mt32Translator bank;
                bank.setPresets(presets);
                bank.setPresetMode(Mt32Translator::PresetMode::Hybrid);
                bank.setPresetBanks(true);
                std::vector<uint8_t> offOut;
                std::vector<uint32_t> offShort;
                const std::vector<uint8_t> offKey = dataSet(0x10, 0x030110, {94, 100, 7, 1});
                bank.translateSysex(offKey.data(), offKey.size(), 0x10, offOut, offShort);
                const bool off = offOut.size() > 8 && offOut[8] == 127;
                hybrid.setMt32Translation(false);
                names = hybrid.toneNames();
                const bool swappedBack = std::equal(romNames.begin(), romNames.begin() + 256, names.begin()) && startsWith(names[320].c_str(), "AcouPiano1");
                std::printf("      chosen presets: stand-in %d, picked %d, drum %.4f, off %d, back %d\n", standIn, picked, drum.left + drum.right,
                            off, swappedBack);
                check(standIn && picked && drum.left + drum.right > 1e-3 && off && swappedBack,
                      "presets chosen one by one: stand-ins (with their shift) or the MT-32's own in a, b and r, the D-110's in d and e; swapped back after translating");
            }
        }
    }

    // The user's choices by ear on the D-20: Schooldaze plays Space Horn (b18), Triangle (the MT-32's own) an octave up,
    // Syn Brass 3 Syn Lead 2 (b32). Choices are kept as text; the user's ini line is now the built-in default, so
    // nothing is left to write.
    {
        const Mt32Translator::PresetChoice schooldaze = Mt32Translator::defaultPresetChoice(45);
        const Mt32Translator::PresetChoice triangle = Mt32Translator::defaultPresetChoice(121);
        const Mt32Translator::PresetChoice synBrass3 = Mt32Translator::defaultPresetChoice(26);
        Mt32Translator::PresetChoices choices;
        Mt32Translator::parsePresetChoices("45:71:0:0;121:117:12:1", choices);
        const bool defaults = schooldaze.tone == 71 && schooldaze.keyShift == 0 && !schooldaze.exact && triangle.tone == 117 &&
                              triangle.keyShift == 12 && triangle.exact && synBrass3.tone == 81 && synBrass3.keyShift == 0 &&
                              !synBrass3.exact && Mt32Translator::presetChoicesText(choices).empty();
        Mt32Translator::parsePresetChoices("3:16:-12:1;200:1:0:0;7:150:0:0", choices);  // The last two are invalid
        const bool presetText = Mt32Translator::presetChoicesText(choices) == "3:16:-12:1";
        Mt32Translator::RhythmChoices rhythm;
        Mt32Translator::parseRhythmChoices("0:15:0;1:-1:1;29:63:0;30:80:0", rhythm);  // R1 on r16, R2 the MT-32's, R30 off
        const bool rhythmText = Mt32Translator::rhythmChoicesText(rhythm) == "0:15:0;1:-1:1;29:63:0";
        Mt32Translator translator;
        for (int t = 0; t < 64; t++) translator.setRhythmChoice(t, rhythm[size_t(t)]);
        const bool standIns = translator.rhythmStandIn(0) == 15 && translator.rhythmStandIn(1) == Mt32Translator::rhythmTone(1) &&
                              translator.rhythmStandIn(29) == Mt32Translator::kRhythmOff;
        check(defaults && presetText && rhythmText && standIns,
              "preset choices: Schooldaze on Space Horn, Triangle +12, Syn Brass 3 on Syn Lead 2 built in; preset and rhythm choices as text and back");
    }

    // The D-10/D-20 target: panel channels, memories kept by the translator, master volume as CC 7.
    {
        Mt32Translator d20;
        d20.setTarget(Mt32Translator::Target::D20);
        d20.setMemoryInUnit(false);
        d20.setMasterVolumeAsVolume(true);
        std::vector<uint8_t> out;
        std::vector<uint32_t> shortOut;
        auto translate = [&](const std::vector<uint8_t>& message) {
            out.clear();
            shortOut.clear();
            d20.translateSysex(message.data(), message.size(), 0x10, out, shortOut);
        };
        // A timbre into memory M1 as handshake data (DAT), and patch 6 pointing at it: kept, nothing sent.
        std::vector<uint8_t> timbre = mt32Timbre("MT BassLp", 108);
        timbre.resize(256, 0);
        std::vector<uint8_t> dat = dataSet(0x10, 0x080000, timbre);
        dat[4] = 0x42;
        translate(dat);
        const bool memoryKept = out.empty();
        translate(dataSet(0x10, 0x050000 + 5 * 8, {2, 0, 24, 50, 12, 0, 1, 0}));
        const bool patchKept = out.empty();
        // Program 5 on channel 2 (part 1): the part's timbre temp and its tone, no program change for the unit.
        out.clear();
        shortOut.clear();
        d20.translateShort(0x05C1, 0x10, shortOut, out);
        uint8_t expected[246];
        Mt32Translator().convertTone(timbre.data(), expected);
        const bool program = shortOut.empty() && out.size() == 17 + 256 && out[5] == 0x03 && out[6] == 0 && out[7] == 0 && out[8] == 2 &&
                             out[9] == 0 && out[17 + 5] == 0x04 && std::memcmp(&out[17 + 8], expected, 246) == 0;
        // Notes move to the unit's channels (parts 1-8 on 1-8, rhythm on 10), also after the MT-32 side moves part 1
        // to channel 5, which part 4 has too.
        shortOut.clear();
        d20.translateShort(0x7F3C91, 0x10, shortOut, out);
        const bool routed = shortOut == std::vector<uint32_t>{0x7F3C90};
        translate(dataSet(0x10, 0x10000D, {4}));  // Part 1 to channel 5
        const bool channelKept = out.empty();
        shortOut.clear();
        d20.translateShort(0x7F3C94, 0x10, shortOut, out);
        const bool rerouted = shortOut == std::vector<uint32_t>{0x7F3C90, 0x7F3C93};
        // One partial reserve: the D-20 gets all 9.
        translate(dataSet(0x10, 0x100004, {1}));
        const bool reserves = out.size() == 19 && out[7] == 0x04 && out[8] == 1 && out[9] == 10 && out[16] == 6;
        // Master volume 50: no SysEx byte, CC 7 at half on every part's channel; CC 7 and Reset All Controllers follow it.
        translate(dataSet(0x10, 0x100016, {50}));
        const bool master = out.empty() && shortOut.size() == 9 && shortOut[0] == (0xB0u | (7u << 8) | (64u << 16));
        shortOut.clear();
        d20.translateShort(0x6407B2, 0x10, shortOut, out);  // CC 7 = 100 on channel 3 (part 2)
        const bool volume = shortOut == std::vector<uint32_t>{0x3207B1};
        shortOut.clear();
        d20.translateShort(0x0079B2, 0x10, shortOut, out);  // Reset All Controllers
        const bool restored = shortOut == std::vector<uint32_t>{0x0079B1, 0x3207B1};
        std::printf("      memory kept %d/%d, program %d, routed %d/%d/%d, reserves %d, master %d, volume %d/%d\n", memoryKept, patchKept,
                    program, routed, channelKept, rerouted, reserves, master, volume, restored);
        check(memoryKept && patchKept && program && routed && channelKept && rerouted && reserves && master && volume && restored,
              "D-20 target: memories kept, program changes as timbre and tone, panel channels, reserve package, master volume as CC 7");
    }

    // Round two: the tone cache (checked in an emulated D-110, which takes write requests like the real units), roomy
    // toms, per-preset choices, key shifts alone, and thinning controllers.
    {
        auto messagesOf = [](const std::vector<uint8_t>& data) {
            std::vector<std::vector<uint8_t>> list;
            for (size_t i = 0; i < data.size();) {
                const size_t end = size_t(std::find(data.begin() + long(i), data.end(), uint8_t(0xF7)) - data.begin());
                if (end >= data.size()) break;
                list.emplace_back(data.begin() + long(i), data.begin() + long(end) + 1);
                i = end + 1;
            }
            return list;
        };
        Mt32Translator cached;
        cached.setMemoryInUnit(false);
        cached.setToneCache(32, 63);  // i51-i88
        SynthEngine unit;
        unit.setOptions(options);
        unit.configure(config, error);
        std::vector<uint8_t> out;
        std::vector<uint32_t> shortOut;
        auto send = [&](const std::vector<uint8_t>& message) {
            out.clear();
            shortOut.clear();
            cached.translateSysex(message.data(), message.size(), 0x10, out, shortOut);
            unit.applySysex(out);
            render(unit, 0.02);
        };
        // The unit starts from the MT-32's power-on state, as the translator assumes.
        out.clear();
        cached.powerOn(0x10, out, shortOut);
        unit.applySysex(out);
        render(unit, 0.02);
        // Two memory timbres, then part 1 selects M2: its tone goes to the part, and to slot i51 (write request).
        const std::vector<uint8_t> m1 = mt32Timbre("MT One", 22), m2 = mt32Timbre("MT Two", 25);
        send(dataSet(0x10, 0x080000, m1));
        send(dataSet(0x10, at(0x080000, 256), m2));
        const bool nothingYet = out.empty();
        send(dataSet(0x10, 0x030000, {2, 1}));  // Part 1: M2
        std::vector<std::vector<uint8_t>> sent = messagesOf(out);
        uint8_t expected[246], slotTone[246], temp[246];
        cached.convertTone(m2.data(), expected);
        unit.readMemory(at(0x080000, 32 * 256), 246, slotTone);
        const bool first = sent.size() == 3 && sent[0][5] == 0x03 && sent[0][8] == 2 && sent[0][9] == 32 && sent[1][5] == 0x04 &&
                           sent[2][5] == 0x40 && sent[2][8] == 32 && std::memcmp(slotTone, expected, 246) == 0;
        // Part 2 selects M2 too: only its timbre temp, pointing at the slot; the unit loads the tone from there.
        send(dataSet(0x10, 0x030010, {2, 1}));
        sent = messagesOf(out);
        unit.readMemory(at(0x040000, 246), 246, temp);
        EngineStatus st;
        unit.getStatus(st);
        const bool hit = sent.size() == 1 && sent[0].size() == 13 && sent[0][9] == 32 && std::memcmp(temp, expected, 246) == 0 &&
                         std::string(st.parts[1].tone) == "i51";
        // A rhythm key on M1: the tone goes through part 8's tone temp into the next slot, and part 8 gets its own back.
        uint8_t part8Before[246];
        unit.readMemory(at(0x040000, 7 * 246), 246, part8Before);
        send(dataSet(0x10, at(0x030110, (40 - 24) * 4), {0, 100, 7, 1}));  // Key 40: M1
        cached.convertTone(m1.data(), expected);
        unit.readMemory(at(0x080000, 33 * 256), 246, slotTone);
        uint8_t part8After[246];
        unit.readMemory(at(0x040000, 7 * 246), 246, part8After);
        const bool rhythm = readByte(unit, at(0x030110, (40 - 24) * 4)) == 33 && std::memcmp(slotTone, expected, 246) == 0 &&
                            std::memcmp(part8Before, part8After, 246) == 0;
        if (!rhythm) {
            std::printf("      rhythm key %d, slot tone %s, part 8 %s\n", readByte(unit, at(0x030110, (40 - 24) * 4)),
                        std::memcmp(slotTone, expected, 246) == 0 ? "ok" : "differs", std::memcmp(part8Before, part8After, 246) == 0 ? "kept" : "changed");
        }
        const Mt32Translator::CacheStats stats = cached.toneCacheStats();
        // The unit refuses a write (memory protect): the cache goes off and the parts get their tones directly.
        out.clear();
        const bool refused = !cached.writeRequestResult(2, 0x10, out) && cached.toneCacheStats().failed;
        sent = messagesOf(out);
        size_t toneMessages = 0;
        for (const std::vector<uint8_t>& m : sent) toneMessages += m[5] == 0x04 ? 1 : 0;
        const bool recovered = refused && toneMessages == 2;
        // Kept between sessions.
        Mt32Translator next;
        next.setMemoryInUnit(false);
        next.setToneCache(32, 63);
        cached.setToneCache(32, 63);
        cached.restoreToneCache(std::vector<std::vector<uint8_t>>(1, std::vector<uint8_t>(expected, expected + 246)));
        next.restoreToneCache(cached.toneCacheContents());
        const bool kept = next.toneCacheStats().used == 1;
        std::printf("      cache: first %d, hit %d, rhythm %d (%d stored, %llu found), refused %d, kept %d\n", first, hit, rhythm, stats.used,
                    static_cast<unsigned long long>(stats.hits), recovered, kept);
        check(nothingYet && first && hit && rhythm && recovered && kept,
              "tone cache: a tone is stored once (write request), then selected by number; rhythm keys too; a refused write turns it off");

        // Roomy toms: the TomTom1 set for rhythm keys and Tom Tom-1 (2.4 semitones up) in the MT-32's timbres.
        Mt32Translator toms;
        uint8_t wave = 0, coarse = 0, fine = 0;
        Mt32Translator::translatePcm(57, 36, 50, wave, coarse, fine, false);
        const bool tomTom2 = toms.rhythmToneFor(2) == 30 && wave == 136 && coarse == 36 && fine == 50;
        toms.setRoomyToms(true);
        Mt32Translator::translatePcm(57, 36, 50, wave, coarse, fine, true);
        const bool tomTom1 = toms.rhythmToneFor(2) == 27 && toms.rhythmToneFor(4) == 29 && wave == 135 && coarse == 38 && fine == 90;
        check(tomTom2 && tomTom1, "MT-32 toms: TomTom2 (the same sample) by default, TomTom1 (roomier) as an option");

        // Hybrid: presets without a counterpart play the MT-32's own timbre; key shifts written alone keep the tone.
        std::shared_ptr<Mt32Presets> romPresets = std::make_shared<Mt32Presets>();
        std::string romError;
        bool hybridOk = true, shiftOk = true;
        for (const RomEntry& rom : roms) {
            if (rom.isControl && romFamily(rom) == "mt32" && loadMt32Presets(rom.path, *romPresets, romError)) {
                Mt32Translator hybrid;
                hybrid.setPresets(romPresets);
                hybrid.setPresetMode(Mt32Translator::PresetMode::Hybrid);
                std::vector<uint8_t> after;
                shortOut.clear();
                hybrid.translateShort(0x3EC1, 0x10, shortOut, after);  // Program 62: A63 Elec Gtr 2 (no counterpart)
                const bool exactTone = messagesOf(after).size() == 1;
                after.clear();
                hybrid.translateShort(0x00C1, 0x10, shortOut, after);  // A1 AcouPiano1: the unit's own
                hybridOk = exactTone && after.empty() && hybrid.presetChoice(62).exact && !hybrid.presetChoice(0).exact;
                out.clear();
                std::vector<uint32_t> unused;
                const std::vector<uint8_t> shift = dataSet(0x10, 0x030002, {36});  // Part 1: key shift alone
                hybrid.translateSysex(shift.data(), shift.size(), 0x10, out, unused);
                shiftOk = out.size() == 11 && out[7] == 0x02 && out[8] == 36;
                break;
            }
        }
        check(hybridOk && shiftOk, "hybrid presets: Elec Gtr 2 plays the MT-32's own, AcouPiano1 the unit's; a key shift alone keeps the tone");

        // Thinning: a repeated controller value is left out; controllers waiting behind a SysEx are merged.
        struct Recorder : MidiSender {
            std::vector<uint32_t> shortMessages;
            size_t sysexCount = 0;
            bool sendShort(uint32_t message) override {
                shortMessages.push_back(message);
                return true;
            }
            bool sendSysex(const uint8_t*, size_t) override {
                sysexCount++;
                return true;
            }
        } thinned;
        MidiPipe thin;
        PipeSettings thinSettings;
        thinSettings.target = Mt32Translator::Target::D110;
        thin.configure(thinSettings, nullptr);
        thin.setOutputs(&thinned, nullptr);
        const std::vector<uint8_t> text = dataSet(0x10, 0x200000, {'A'});
        thin.onMidiSysex(text.data(), text.size());
        for (uint32_t bend = 0; bend < 10; bend++) thin.onMidiShortMessage(0xE1 | ((bend * 100 & 0x7F) << 8) | (((bend * 100) >> 7) << 16));
        thin.onMidiShortMessage(0x6407B1);
        thin.onMidiShortMessage(0x6407B1);  // Same volume again
        for (MidiPipe::Clock::time_point t = MidiPipe::Clock::now(); t != MidiPipe::Clock::time_point::max();) t = thin.process(t);
        const bool thinOk = thinned.sysexCount == 1 && thinned.shortMessages.size() == 2 &&
                            thinned.shortMessages[0] == (0xE1u | ((900u & 0x7F) << 8) | ((900u >> 7) << 16)) && thin.stats().thinned == 10;
        std::printf("      thinning: %zu short messages sent, %llu left out\n", thinned.shortMessages.size(),
                    static_cast<unsigned long long>(thin.stats().thinned));
        check(thinOk, "reduce MIDI load: repeated values left out, bends waiting behind a SysEx merged into the newest");
    }

    // MT32Translator's pipe: handshake replies, pacing, and the unit's data (VOICE_MT.DAT, the X68000 Akumajo Dracula's
    // timbres, loaded by handshake), also through an emulated D-110.
    {
        struct Recorder : MidiSender {
            std::vector<uint32_t> shortMessages;
            std::vector<std::vector<uint8_t>> sysex;
            bool sendShort(uint32_t message) override {
                shortMessages.push_back(message);
                return true;
            }
            bool sendSysex(const uint8_t* data, size_t length) override {
                sysex.emplace_back(data, data + length);
                return true;
            }
        };
        std::vector<uint8_t> voice;
        for (const char* path : {"../VOICE_MT.DAT", "../../VOICE_MT.DAT", "VOICE_MT.DAT"}) {
            std::ifstream in(path, std::ios::binary);
            if (in) voice.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (!voice.empty()) break;
        }
        auto forEach = [](const std::vector<uint8_t>& data, const std::function<void(const uint8_t*, size_t)>& f) {
            for (size_t i = 0; i < data.size();) {
                const size_t end = size_t(std::find(data.begin() + long(i), data.end(), uint8_t(0xF7)) - data.begin());
                if (end >= data.size()) break;
                if (data[i] == 0xF0) f(&data[i], end + 1 - i);
                i = end + 1;
            }
        };
        // A handshake transfer like VOICE_MT.DAT's when the file is not here: WSD, 64 DAT packets, EOD.
        if (voice.empty()) {
            voice = {0xF0, 0x41, 0x00, 0x16, 0x40, 0x08, 0x00, 0x00, 0x01, 0x00, 0x00, 0x77, 0xF7};
            for (uint32_t n = 0; n < 64; n++) {
                std::vector<uint8_t> timbre = mt32Timbre("Timbre", uint8_t(22 + n % 20));
                timbre.resize(256, 0);
                std::vector<uint8_t> dat = dataSet(0x10, at(0x080000, n * 256), timbre);
                dat[4] = 0x42;
                voice.insert(voice.end(), dat.begin(), dat.end());
            }
            const uint8_t eod[] = {0xF0, 0x41, 0x10, 0x16, 0x45, 0xF7};
            voice.insert(voice.end(), eod, eod + sizeof(eod));
            std::printf("      (VOICE_MT.DAT not found: a made-up transfer of the same shape)\n");
        }

        Recorder unit, reply;
        MidiPipe pipe;
        PipeSettings pipeSettings;
        pipeSettings.target = Mt32Translator::Target::D110;
        pipeSettings.memoryInUnit = true;
        pipe.configure(pipeSettings, nullptr);
        pipe.setOutputs(&unit, &reply);
        size_t packets = 0;
        forEach(voice, [&](const uint8_t* message, size_t length) {
            pipe.onMidiSysex(message, length);
            packets++;
        });
        MidiPipe::Clock::time_point now = MidiPipe::Clock::now();
        for (int step = 0; step < 1000; step++) {
            const MidiPipe::Clock::time_point next = pipe.process(now);
            if (next == MidiPipe::Clock::time_point::max()) break;
            now = next;
        }
        const std::vector<uint8_t> ack = {0xF0, 0x41, 0x10, 0x16, 0x43, 0xF7};
        const bool acks = reply.sysex.size() == packets && std::all_of(reply.sysex.begin(), reply.sysex.end(), [&](const std::vector<uint8_t>& m) {
            return m == ack || (m.size() == 6 && m[4] == 0x43);
        });
        const std::vector<uint8_t> fileTranslation = translateSysexFile(voice, pipeSettings, nullptr);
        std::vector<uint8_t> sent;
        for (const std::vector<uint8_t>& m : unit.sysex) sent.insert(sent.end(), m.begin(), m.end());
        const bool data = !unit.sysex.empty() && sent == fileTranslation && unit.sysex[0][5] == 0x08 && pipe.stats().handshakes == 1;
        std::printf("      %zu packets, %zu replies, %zu messages to the unit (%zu bytes)\n", packets, reply.sysex.size(), unit.sysex.size(), sent.size());

        // A damaged packet: ERR, and nothing for the unit.
        std::vector<uint8_t> damaged = dataSet(0x10, 0x080000, {1, 2, 3});
        damaged[4] = 0x42;
        damaged[damaged.size() - 2] ^= 1;
        const size_t unitBefore = unit.sysex.size();
        pipe.onMidiSysex(damaged.data(), damaged.size());
        pipe.process(now + std::chrono::seconds(1));
        const bool err = reply.sysex.back().size() == 6 && reply.sysex.back()[4] == 0x4E && unit.sysex.size() == unitBefore;
        check(acks && data && err, "MT32Translator pipe: the program's handshake answered (WSD, 64 DAT, EOD: ACK; damaged: ERR), its timbres sent as DT1");

        // Pacing: a note behind a SysEx waits for its transmission and the pause.
        Recorder paced;
        MidiPipe timing;
        PipeSettings timingSettings;
        timingSettings.sysexGapMs = 20;
        timing.configure(timingSettings, nullptr);
        timing.setOutputs(&paced, nullptr);
        const std::vector<uint8_t> display = dataSet(0x10, 0x200000, std::vector<uint8_t>(20, 'A'));
        timing.onMidiSysex(display.data(), display.size());
        timing.onMidiShortMessage(0x7F3C91);
        const MidiPipe::Clock::time_point t0 = MidiPipe::Clock::now();
        const MidiPipe::Clock::time_point due = timing.process(t0);
        const double wait = std::chrono::duration<double, std::milli>(due - t0).count();
        const bool held = paced.sysex.size() == 1 && paced.shortMessages.empty();
        timing.process(due);
        // 0.32 ms per byte, and the pause scaled to the message's size (20 ms for 256 bytes).
        const bool pacedOk = held && paced.shortMessages == std::vector<uint32_t>{0x7F3C90} && std::fabs(wait - (0.32 * 42 + 20.0 * 42 / 256)) < 0.01;
        std::printf("      note held %.2f ms behind a 42-byte SysEx\n", wait);

        // D-20: the memories stay in the translator; a timbre selection sends the part's timbre and tone.
        Recorder d20Unit, d20Reply;
        MidiPipe d20Pipe;
        PipeSettings d20Settings;
        d20Pipe.configure(d20Settings, nullptr);
        d20Pipe.setOutputs(&d20Unit, &d20Reply);
        forEach(voice, [&](const uint8_t* message, size_t length) { d20Pipe.onMidiSysex(message, length); });
        d20Pipe.process(MidiPipe::Clock::now() + std::chrono::hours(1));
        const size_t afterLoad = d20Unit.sysex.size();
        const std::vector<uint8_t> select = dataSet(0x10, 0x030000, {2, 19});  // Part 1: M20
        d20Pipe.onMidiSysex(select.data(), select.size());
        for (MidiPipe::Clock::time_point t = MidiPipe::Clock::now() + std::chrono::hours(2); t != MidiPipe::Clock::time_point::max();) {
            t = d20Pipe.process(t);  // The tone follows the timbre temp after its pause
        }
        const bool d20Ok = afterLoad == 0 && d20Reply.sysex.size() == packets && d20Unit.sysex.size() == 2 && d20Unit.sysex[0][5] == 0x03 &&
                           d20Unit.sysex[1][5] == 0x04 && d20Unit.sysex[1].size() == 256;
        std::printf("      D-20: %zu after the load, %zu replies, %zu messages after the selection", afterLoad, d20Reply.sysex.size(), d20Unit.sysex.size());
        for (const std::vector<uint8_t>& m : d20Unit.sysex) std::printf(" [%02X %02X %02X, %zu]", m[5], m[6], m[7], m.size());
        std::printf("\n");
        check(pacedOk && d20Ok, "MT32Translator pipe: SysEx paced (transmission + pause); D-20: memories kept, a selection sends timbre and tone");

        // The sender thread delivers in order, then stops.
        struct LockedRecorder : MidiSender {
            std::mutex mutex;
            std::vector<uint32_t> shortMessages;
            size_t sysexCount = 0;
            bool sendShort(uint32_t message) override {
                std::lock_guard<std::mutex> lock(mutex);
                shortMessages.push_back(message);
                return true;
            }
            bool sendSysex(const uint8_t*, size_t) override {
                std::lock_guard<std::mutex> lock(mutex);
                sysexCount++;
                return true;
            }
        } threaded;
        bool delivered = false;
        {
            MidiPipe live;
            PipeSettings liveSettings;
            liveSettings.sysexGapMs = 1;
            live.configure(liveSettings, nullptr);
            live.setOutputs(&threaded, nullptr);
            live.start();
            live.onMidiSysex(display.data(), display.size());
            for (uint32_t key = 60; key < 64; key++) live.onMidiShortMessage(0x7F0091 | (key << 8));
            for (int wait = 0; wait < 200 && !delivered; wait++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                std::lock_guard<std::mutex> lock(threaded.mutex);
                delivered = threaded.sysexCount == 1 && threaded.shortMessages.size() == 4;
            }
            live.stop();
        }
        check(delivered && threaded.shortMessages.front() == 0x7F3C90 && threaded.shortMessages.back() == 0x7F3F90,
              "MT32Translator pipe: the sender thread delivers in order and stops");

        // Loopback: the pipe's D-110 data in an emulated D-110 equals d110emu's own MT-32 translation of the file.
        SynthEngine viaPipe;
        viaPipe.setOptions(options);
        viaPipe.configure(config, error);
        viaPipe.applySysex(sent);
        SynthEngine viaEmu;
        viaEmu.setOptions(options);
        viaEmu.configure(config, error);
        viaEmu.setMt32Translation(true);
        forEach(voice, [&](const uint8_t* message, size_t length) { viaEmu.onMidiSysex(message, length); });
        render(viaEmu, 0.5);
        std::vector<uint8_t> a(64 * 256), b(64 * 256);
        viaPipe.readMemory(0x080000, uint32_t(a.size()), a.data());
        viaEmu.readMemory(0x080000, uint32_t(b.size()), b.data());
        std::string firstName(reinterpret_cast<const char*>(a.data()), 10);
        std::printf("      i11 %s\n", firstName.c_str());
        check(a == b && a[0] != 0, "MT32Translator pipe and d110emu's MT-32 translation load the same D-110 tones");
    }

    // D-20 rhythm patterns and rhythm track: stored from D-20 dumps, parsed and played on the rhythm part.
    {
        SynthEngine d20;
        d20.setOptions(options);
        d20.configure(config, error);
        // A fresh memory holds the D-20's factory rhythm track: its presets P-11-P-48 in order, each twice.
        std::vector<int> factoryTrack(64);
        for (int bar = 0; bar < 64; bar++) factoryTrack[size_t(bar)] = bar / 2;
        const bool factoryAtStart = d20.d20Rhythm().track == factoryTrack;
        // P-51: 4/4 with four notes and an erased one (key 128); sent as nibbles, low first.
        std::vector<uint8_t> bytes(kD20PatternSize / 2, 0);
        bytes[0] = 3;  // 4/4
        const uint8_t notes[5][3] = {{0, 36, 100}, {24, 38, 90}, {36, 128, 0}, {48, 36, 100}, {72, 38, 90}};
        bytes[1] = 5;
        for (size_t i = 0; i < 5; i++) std::memcpy(&bytes[3 + 3 * i], notes[i], 3);
        bytes[3 + 3 * 96] = 0xFF;  // End mark
        std::vector<uint8_t> nibbles;
        for (uint8_t b : bytes) {
            nibbles.push_back(b & 0x0F);
            nibbles.push_back(b >> 4);
        }
        std::vector<uint8_t> dump;
        for (size_t offset = 0; offset < nibbles.size(); offset += 256) {
            const std::vector<uint8_t> chunk(nibbles.begin() + long(offset), nibbles.begin() + long(std::min(offset + 256, nibbles.size())));
            const std::vector<uint8_t> message = dataSet(0x10, at(0x0A0000, uint32_t(offset)), chunk);
            dump.insert(dump.end(), message.begin(), message.end());
        }
        const std::vector<uint8_t> track = dataSet(0x10, 0x0C0000, {3, 0, 32, 64 + 1, 32});  // P-51, blank 2/4, P-51
        dump.insert(dump.end(), track.begin(), track.end());
        d20.applySysex(dump);
        const D20Rhythm rhythm = d20.d20Rhythm();
        const D20Pattern& p51 = rhythm.patterns[32];
        const bool parsed = p51.present && p51.beats == 4 && p51.notes.size() == 4 && p51.notes[1].step == 24 && p51.notes[1].key == 38 &&
                            !rhythm.patterns[0].present && rhythm.track == std::vector<int>{32, 65, 32};
        std::vector<double> bars;
        const std::unique_ptr<SmfFile> smf = d20TrackToSmf(rhythm, 9, 120.0, &bars);
        const bool timed = bars == std::vector<double>{0.0, 2.0, 3.0} && std::fabs(smf->duration - 5.0) < 1e-9 && smf->events.size() == 16 &&
                           smf->events[0].shortMessage == (0x99u | (36u << 8) | (100u << 16)) && std::fabs(smf->events[2].time - 0.5) < 1e-9;
        check(parsed && timed, "D-20 rhythm: patterns (erased notes skipped) and track parsed, bars timed by their time signatures");

        // Played on the rhythm part, twice as fast: a pattern bar of 2 s passes in 1 s.
        d20.loadMidi(d20PatternToSmf(rhythm, 32, 9, 120.0), "P-51", MidiFileKind::UnitRhythm);
        d20.playerSetSpeed(2.0);
        d20.playerPlay();
        const Levels drums = render(d20, 0.1);  // The kick at the start
        EngineStatus st;
        const bool sounding = drums.left + drums.right > 1e-3;
        render(d20, 0.4);
        d20.getStatus(st);
        const bool fast = std::fabs(st.player.position - 1.0) < 0.05;
        std::printf("      D-20 pattern: rhythm part sounding %d, position after 0.5 s at double speed %.3f s\n", sounding, st.player.position);
        d20.playerStop();
        check(sounding && fast, "D-20 rhythm: a pattern plays on the rhythm part; player speed sets the tempo");

        // Another pattern chosen while one repeats (the Patterns tab queues it) takes over on the next bar's first beat:
        // P-51's 2 s bar (64000 samples) ends at 65000, where a 2/4 P-52 starts, then repeats every 32000 samples.
        D20Rhythm other = rhythm;
        other.patterns[33].present = true;
        other.patterns[33].beats = 2;
        other.patterns[33].notes = {{0, 40, 110}};
        MidiPlayer player;
        player.load(d20PatternToSmf(other, 32, 9, 120.0), "P-51");
        player.setLoop(true);
        player.play(1000);
        std::vector<std::pair<uint32_t, uint32_t>> played;  // Timestamp, message
        auto record = [&](uint32_t message, const uint8_t*, uint32_t, uint32_t timestamp) {
            played.push_back({timestamp, message});
            return true;
        };
        player.pump(1000 + 16000, record);
        player.queueNext(d20PatternToSmf(other, 33, 9, 120.0), "P-52");
        const bool waiting = player.queuedName() == "P-52" && player.name() == "P-51";
        for (uint32_t end = 1000 + 16000; end < 1000 + 64000 + 32000 + 16000; end += 512) player.pump(end, record);
        const uint32_t p52Note = 0x99u | (40u << 8) | (110u << 16);
        std::vector<uint32_t> p52Starts;
        bool p51After = false;
        for (const auto& event : played) {
            if (event.second == p52Note) p52Starts.push_back(event.first);
            if ((event.second & 0xF0u) == 0x90u && event.second != p52Note && event.first >= 65000) p51After = true;
        }
        const bool switched = waiting && p52Starts == std::vector<uint32_t>{65000, 97000} && !p51After && player.name() == "P-52" &&
                              player.queuedName().empty();
        player.queueNext(d20PatternToSmf(other, 32, 9, 120.0), "P-51");
        player.stop(nullptr);
        const bool cleared = player.queuedName().empty();
        // Through the engine: queued, then playing after the bar, as the app sees it.
        d20.loadMidi(d20PatternToSmf(other, 32, 9, 120.0), "D-20 P-51", MidiFileKind::UnitRhythm);
        d20.playerPlay();
        d20.playerQueueNext(d20PatternToSmf(other, 33, 9, 120.0), "D-20 P-52");
        d20.getStatus(st);
        const bool engineWaiting = st.player.queuedName == "D-20 P-52" && st.player.name == "D-20 P-51";
        render(d20, 2.1);
        d20.getStatus(st);
        const bool engineSwitched = st.player.name == "D-20 P-52" && st.player.queuedName.empty() && st.player.position < 0.2;
        d20.playerStop();
        std::printf("      queued pattern: P-52 starts at %s, name %s; engine %d/%d\n",
                    p52Starts.empty() ? "-" : std::to_string(p52Starts[0]).c_str(), player.name().c_str(), engineWaiting, engineSwitched);
        check(switched && cleared && engineWaiting && engineSwitched,
              "D-20 rhythm: a pattern queued while another repeats starts exactly on the next bar's first beat, then repeats");

        // The presets P-11-P-48 from a dump's P-51-P-88; everything survives a memory dump.
        const std::filesystem::path file = std::filesystem::temp_directory_path() / "d110tests-d20.syx";
        {
            std::ofstream out(file, std::ios::binary);
            out.write(reinterpret_cast<const char*>(dump.data()), std::streamsize(dump.size()));
        }
        const bool presets = d20.loadD20PresetPatterns(file, error) && d20.d20Rhythm().patterns[0].notes.size() == 4;
        std::filesystem::remove(file);
        const std::vector<uint8_t> memory = d20.dumpSysex(DumpEverything);
        SynthEngine copy;
        copy.setOptions(options);
        copy.configure(config, error);
        copy.applySysex(memory);
        const D20Rhythm copied = copy.d20Rhythm();
        check(presets && copied.patterns[0].notes.size() == 4 && copied.patterns[32].notes.size() == 4 && copied.track == rhythm.track,
              "D-20 rhythm: presets load from a dump's P-51-P-88; patterns and track travel in the memory file");

        // PatternCapture: patterns from what a D-20 plays on its MIDI OUT in Pattern Play. The made-up D-20 sends Start
        // (or Continue), 24 clocks per quarter, each step's notes on channel 10 up to 3 ms before or after its clock with
        // note-offs 10 ms later, a note on another channel, and Stop; the messages arrive in time order.
        auto makePattern = [](int beats, std::initializer_list<std::array<int, 3>> notes) {
            D20Pattern pattern;
            pattern.beats = beats;
            pattern.present = true;
            for (const auto& n : notes) pattern.notes.push_back({n[0], n[1], n[2]});
            return pattern;
        };
        auto play = [](PatternCapture& capture, const D20Pattern& pattern, int steps, double bpm, uint8_t start) {
            const double clock = 60.0 / bpm / kD20StepsPerQuarter;
            const int length = pattern.beats * kD20StepsPerQuarter;
            std::vector<std::pair<double, uint32_t>> events = {{0.0, start}};
            int jitter = 0;
            for (int step = 0; step < steps; step++) {
                const double at = 0.005 + step * clock;  // After Start, which a D-20 sends before the first step
                events.emplace_back(at, 0xF8u);
                for (const D20Pattern::Note& note : pattern.notes) {
                    if (note.step != step % length) continue;
                    const double time = at + double(jitter++ % 7 - 3) * 0.001;
                    events.emplace_back(time, 0x99u | uint32_t(note.key) << 8 | uint32_t(note.velocity) << 16);
                    events.emplace_back(time + 0.010, 0x89u | uint32_t(note.key) << 8 | 0x400000u);
                }
                if (step % 48 == 5) events.emplace_back(at + 0.002, 0x90u | 60u << 8 | 100u << 16);  // Channel 1: not the rhythm part
            }
            events.emplace_back(0.005 + steps * clock, 0xFCu);
            std::stable_sort(events.begin(), events.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            for (const auto& event : events) capture.onMessage(event.second, 100.0 + event.first);
        };
        auto sameNotes = [](std::vector<D20Pattern::Note> a, std::vector<D20Pattern::Note> b) {
            auto order = [](const D20Pattern::Note& x, const D20Pattern::Note& y) {
                return std::tie(x.step, x.key, x.velocity) < std::tie(y.step, y.key, y.velocity);
            };
            std::sort(a.begin(), a.end(), order);
            std::sort(b.begin(), b.end(), order);
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const D20Pattern::Note& x, const D20Pattern::Note& y) {
                       return x.step == y.step && x.key == y.key && x.velocity == y.velocity;
                   });
        };
        // A plain 4/4 beat repeats every half bar too, and a flam at the end of the bar (step 95) needs its clock.
        const D20Pattern beat = makePattern(4, {{0, 36, 120}, {0, 42, 100}, {12, 42, 70}, {24, 38, 110}, {24, 42, 100}, {36, 42, 70},
                                                 {48, 36, 120}, {48, 42, 100}, {60, 42, 70}, {72, 38, 110}, {72, 42, 100}, {84, 42, 70},
                                                 {95, 38, 40}});
        const D20Pattern halves = makePattern(4, {{0, 36, 120}, {12, 42, 70}, {24, 38, 110}, {36, 42, 70}, {48, 36, 120}, {60, 42, 70},
                                                   {72, 38, 110}, {84, 42, 70}});
        const D20Pattern waltz = makePattern(3, {{0, 36, 110}, {0, 51, 100}, {24, 51, 80}, {24, 42, 90}, {48, 51, 80}, {48, 42, 90}, {66, 38, 50}});
        PatternCapture capture;
        play(capture, beat, 4 * 96, 120.0, 0xFA);
        const bool finished = capture.takeFinished() && !capture.takeFinished() && !capture.playing();
        PatternCapture::Take take = capture.take();
        const bool beatOk = take.aligned && take.clocks == 4 * 96 && std::fabs(take.bpm - 120.0) < 0.5 && PatternCapture::suggestedBeats(take) == 4 &&
                            sameNotes(PatternCapture::pattern(take, 4).notes, beat.notes) && PatternCapture::pattern(take, 4).beats == 4;
        play(capture, halves, 4 * 96, 90.0, 0xFA);
        take = capture.take();
        const std::vector<int> halvesRepeat = PatternCapture::repeatingBeats(take);
        const bool halvesOk = halvesRepeat == std::vector<int>{2, 4, 6, 8} && PatternCapture::suggestedBeats(take) == 4 &&
                              sameNotes(PatternCapture::pattern(take, 4).notes, halves.notes);
        play(capture, waltz, 4 * 72, 140.0, 0xFA);
        take = capture.take();
        const bool waltzOk = PatternCapture::repeatingBeats(take) == std::vector<int>{3, 6} && PatternCapture::suggestedBeats(take) == 3 &&
                             sameNotes(PatternCapture::pattern(take, 3).notes, waltz.notes);
        play(capture, beat, 96 + 40, 120.0, 0xFB);  // START alone, and too short to compare bars
        take = capture.take();
        const bool continued = !take.aligned && PatternCapture::repeatingBeats(take).empty() && PatternCapture::suggestedBeats(take) == 4 &&
                               PatternCapture::wholeBars(take, 4) == 1;
        std::printf("      capture: 4/4 %d, halves repeat at %zu lengths, waltz %d, Continue %d\n", beatOk, halvesRepeat.size(), waltzOk, continued);
        check(finished && beatOk && halvesOk && waltzOk && continued,
              "PatternCapture: notes on their clocks despite jitter, 4/4 preferred, a waltz found as 3/4, Continue and short takes flagged");

        // The D-20's own format: the unused events and the end mark as it sends them; a dump in P-51-P-88 that reads
        // back, and that d110emu's Load presets puts in P-11-P-48.
        const std::vector<uint8_t> encoded = encodeD20Pattern(waltz);
        auto byteAt = [&](size_t i) { return encoded[2 * i] | encoded[2 * i + 1] << 4; };
        const bool format = encoded.size() == kD20PatternSize && byteAt(0) == 2 && byteAt(1) == 7 && byteAt(2) == 0 && byteAt(3 + 3 * 7) == 0xFF &&
                            byteAt(4 + 3 * 7) == 0x80 && byteAt(5 + 3 * 7) == 0 && byteAt(291) == 0xFF && byteAt(292) == 0 && byteAt(293) == 0;
        std::array<D20Pattern, kD20PresetPatterns> captured{};
        captured[0] = beat;
        captured[23] = waltz;  // P-38 Jazz Waltz
        const std::vector<uint8_t> patternDump = d20PatternDump(captured);
        bool messagesOk = true;
        size_t messages = 0;
        for (size_t i = 0; i < patternDump.size();) {
            const size_t end = size_t(std::find(patternDump.begin() + long(i), patternDump.end(), uint8_t(0xF7)) - patternDump.begin());
            unsigned sum = 0;
            for (size_t k = i + 5; k < end; k++) sum += patternDump[k];
            messagesOk = messagesOk && end < patternDump.size() && end - i - 9 <= 256 && patternDump[i + 2] == 0x10 && sum % 128 == 0;
            messages++;
            i = end + 1;
        }
        int found = 0;
        const std::array<D20Pattern, kD20PresetPatterns> readBack = readD20PatternDump(patternDump.data(), patternDump.size(), &found);
        const bool dumpOk = messagesOk && messages == 6 && patternDump[5] == 0x0A && patternDump[6] == 0 && patternDump[7] == 0 && found == 2 &&
                            readBack[0].beats == 4 && sameNotes(readBack[0].notes, beat.notes) && readBack[23].beats == 3 &&
                            sameNotes(readBack[23].notes, waltz.notes) && !readBack[1].present;
        const std::filesystem::path captureFile = std::filesystem::temp_directory_path() / "d110tests-capture.syx";
        {
            std::ofstream out(captureFile, std::ios::binary);
            out.write(reinterpret_cast<const char*>(patternDump.data()), std::streamsize(patternDump.size()));
        }
        const bool loaded = d20.loadD20PresetPatterns(captureFile, error);
        std::filesystem::remove(captureFile);
        const D20Rhythm withPresets = d20.d20Rhythm();
        const bool inEmulator = loaded && withPresets.patterns[0].beats == 4 && sameNotes(withPresets.patterns[0].notes, beat.notes) &&
                                withPresets.patterns[23].beats == 3 && sameNotes(withPresets.patterns[23].notes, waltz.notes);
        check(format && dumpOk && inEmulator && std::string(kD20PresetPatternNames[23]) == "Jazz Waltz",
              "PatternCapture: patterns in the D-20's format (unused events FF 80 00, end mark), a dump in P-51-P-88 that reads back "
              "and loads as d110emu's presets P-11-P-48");

        // The D-20's presets in the ROM folder (recorded from the user's D-20 with PatternCapture), which d110emu loads at
        // every start: all 32, the Jazz Waltz in 3/4 and the Bossanovas in 8/4.
        const std::filesystem::path presetFile = romFolder / "D-20 preset patterns.syx";
        std::error_code missingFile;
        if (!std::filesystem::exists(presetFile, missingFile)) {
            std::printf("SKIP  the D-20's preset patterns: no \"D-20 preset patterns.syx\" in the ROM folder\n");
        } else {
            const bool presetsLoaded = d20.loadD20PresetPatterns(presetFile, error);
            const D20Rhythm rom = d20.d20Rhythm();
            int withNotes = 0;
            for (int p = 0; p < kD20PresetPatterns; p++) {
                if (rom.patterns[size_t(p)].present && !rom.patterns[size_t(p)].notes.empty()) withNotes++;
            }
            std::printf("      ROM folder presets: %d of 32, P-11 %d/4 with %zu notes\n", withNotes, rom.patterns[0].beats, rom.patterns[0].notes.size());
            check(presetsLoaded && withNotes == kD20PresetPatterns && rom.patterns[23].beats == 3 && rom.patterns[27].beats == 8 &&
                      rom.patterns[28].beats == 8 && rom.patterns[0].beats == 4,
                  "the D-20's preset patterns P-11-P-48 load from the ROM folder: all 32, Jazz Waltz 3/4, Bossanova 1 and 2 in 8/4");
        }

        // A D-20 dumped after a factory reset (the user's, after a power cycle) in the ROM folder: its P-51-P-88 go where
        // the memory has never held a pattern; one loaded before stays.
        const std::filesystem::path initialFile = romFolder / "D-20 initial memory.syx";
        if (!std::filesystem::exists(initialFile, missingFile)) {
            std::printf("SKIP  the D-20's initial patterns: no \"D-20 initial memory.syx\" in the ROM folder\n");
        } else {
            SynthEngine fresh;
            fresh.setOptions(options);
            fresh.configure(config, error);
            std::array<D20Pattern, 32> own;
            own[1].beats = 3;
            own[1].notes = {{0, 36, 100}, {24, 38, 90}};
            own[1].present = true;
            fresh.applySysex(d20PatternDump(own));  // P-52
            int before = 0;
            for (int p = kD20PresetPatterns; p < kD20PatternCount; p++) before += fresh.d20Rhythm().patterns[size_t(p)].present ? 1 : 0;
            const bool loaded = fresh.loadD20InitialPatterns(initialFile, error);
            const D20Rhythm after = fresh.d20Rhythm();
            int initialOnes = 0;
            for (int p = kD20PresetPatterns; p < kD20PatternCount; p++) {
                const D20Pattern& pattern = after.patterns[size_t(p)];
                if (p != kD20PresetPatterns + 1 && pattern.present && pattern.beats == 4 && pattern.notes.size() == 13) initialOnes++;
            }
            const D20Pattern& kept = after.patterns[size_t(kD20PresetPatterns + 1)];
            const bool ownKept = kept.present && kept.beats == 3 && kept.notes.size() == 2;
            // A dump of the memory now holds them, so the next start (the memory file) keeps them.
            SynthEngine copy;
            copy.setOptions(options);
            copy.configure(config, error);
            copy.applySysex(fresh.dumpSysex(DumpD20Rhythm));
            const bool dumped = copy.d20Rhythm().patterns[size_t(kD20PresetPatterns)].notes.size() == 13;
            std::printf("      initial patterns: before %d present, then %d of the D-20's and P-52 kept %d, in a dump %d\n", before,
                        initialOnes, ownKept, dumped);
            check(loaded && before == 1 && initialOnes == 31 && ownKept && dumped,
                  "P-51-P-88 start with the D-20's initial patterns (4/4, 13 notes) from the ROM folder where the memory held none");
        }
        const bool customTrack = d20.d20Rhythm().track == std::vector<int>{32, 65, 32};
        d20.resetD20RhythmTrack();
        check(factoryAtStart && customTrack && d20.d20Rhythm().track == factoryTrack,
              "D-20 rhythm: the memory starts with the D-20's factory track (P-11-P-48 in order, each twice); a dump's own "
              "track replaces it, and Factory track brings it back");
    }


    // The tone editors: the model against the D-110's own table, the editor's messages (solo, Compare, undo, audition),
    // UnitLink's pacing, merging and requests, and ToneEditor on a unit played by d110emu (edits, write, reading back).
    {
        using namespace Tone;
        const std::vector<uint8_t> controlRom = [&] {
            std::ifstream in(roms[control].path, std::ios::binary);
            return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        }();
        bool maxima = controlRom.size() > 0x4A79 + 72;
        for (int i = 0; maxima && i < kCommonSize + kPartialSize; i++) maxima = controlRom[size_t(0x4A79 + i)] == maxValue(i);
        const Param* coarse = findParam(Partial::PitchCoarse);
        const Param* bias = findParam(Partial::TvfBiasPoint);
        const Param* keyfollow = findParam(Partial::PitchKeyfollow);
        const Param* wave = findParam(Partial::PcmWave);
        const bool shown = partialParams().size() == size_t(kPartialSize) && formatValue(Model::D110, *coarse, 36) == "C4" &&
                           formatValue(Model::D110, *bias, 0) == "<A1" && formatValue(Model::D110, *bias, 127) == ">C7" &&
                           formatValue(Model::D110, *keyfollow, 16) == "s2" && formatValue(Model::D110, *wave, 42, 0) == "1-043 Steamer" &&
                           formatValue(Model::MT32, *wave, 46, 0) == "1-047 Timpani";
        // The D-20 plays all of the tone (the same engine as the D-110); its panel only leaves five parameters out.
        int hidden = 0;
        for (const Param& param : partialParams()) hidden += hiddenOnPanel(Model::D20, param.offset) ? 1 : 0;
        const bool d20 = hidden == 5 && hiddenOnPanel(Model::D20, 0x12) && hiddenOnPanel(Model::D20, 0x38) && !hiddenOnPanel(Model::D110, 0x38) &&
                         paramLabel(Model::D20, *findParam(0x35)) == "Time 5 (release)" &&
                         envelopePoints(Model::D20, Envelope::Tva).size() == envelopePoints(Model::D110, Envelope::Tva).size();
        const uint8_t partialStruct[13] = {0, 0, 2, 2, 1, 3, 3, 0, 3, 0, 2, 1, 3};  // munt's table: bit 1 = first partial PCM
        bool structures = true;
        for (int value = 0; value < 13; value++) {
            structures = structures && structure(value).firstPcm == bool(partialStruct[value] & 2) &&
                         structure(value).secondPcm == bool(partialStruct[value] & 1);
        }
        check(maxima && shown && d20 && structures, "tone model: ranges equal the D-110 ROM's table; C4, <A1, >C7, s2 and wave names; "
                                                    "the D-20 edits every parameter; structures pair partials as the firmware does");

        struct ToneRecorder : ToneEditor::Host {
            std::vector<std::pair<int, std::vector<uint8_t>>> writes;
            std::vector<uint32_t> notes;
            int restrikes = 0;
            void writeTone(int offset, const uint8_t* data, int length) override { writes.push_back({offset, std::vector<uint8_t>(data, data + length)}); }
            void noteOn(int key, int velocity) override { notes.push_back(0x90u | uint32_t(key) << 8 | uint32_t(velocity) << 16); }
            void noteOff(int key) override { notes.push_back(0x80u | uint32_t(key) << 8); }
            void restrike() override { restrikes++; }
        };
        ToneEditor editor;
        ToneRecorder host;
        Data original = initialTone();
        original[Common::PartialMute] = 0x0F;
        editor.loadTone(original);
        const int cutoff = partialBase(1) + Partial::TvfCutoff;
        auto last = [&](int offset, std::vector<uint8_t> bytes) { return !host.writes.empty() && host.writes.back().first == offset && host.writes.back().second == bytes; };
        editor.setByte(host, cutoff, 60);
        const bool single = host.writes.size() == 1 && last(cutoff, {60});
        editor.setByte(host, cutoff + 1, 99);  // Resonance: 0-30
        const bool clamped = last(cutoff + 1, {30}) && editor.tone()[size_t(cutoff + 1)] == 30;
        editor.solo(host, 2);
        const bool soloSent = last(Common::PartialMute, {0x04}) && editor.tone()[Common::PartialMute] == 0x0F;
        const size_t writesBefore = host.writes.size();
        editor.setByte(host, Common::PartialMute, 0x07);  // The edit's own switches change; the unit keeps the solo
        const bool soloKept = host.writes.size() == writesBefore;
        editor.settle(host);
        const bool settled = last(Common::PartialMute, {0x07});
        editor.compare(host, true);
        const bool compared = host.writes.size() == writesBefore + 3 &&
                              host.writes[writesBefore + 1] == std::make_pair(int(Common::PartialMute), std::vector<uint8_t>{0x0F}) &&
                              host.writes[writesBefore + 2] == std::make_pair(cutoff, std::vector<uint8_t>{100, 0});
        editor.compare(host, false);
        const bool back = host.writes.size() == writesBefore + 5 && host.writes[writesBefore + 4] == std::make_pair(cutoff, std::vector<uint8_t>{60, 30});
        editor.undo(host);  // The partial switches
        editor.undo(host);  // The resonance
        const bool undone = last(cutoff + 1, {0}) && editor.tone()[Common::PartialMute] == 0x0F;
        editor.redo(host);
        const bool redone = last(cutoff + 1, {30});
        // Bytes far apart go as separate messages; a whole tone as one.
        Data far = editor.tone();
        far[size_t(partialBase(0) + Partial::TvaLevel)] = 11;
        far[size_t(partialBase(3) + Partial::TvaLevel)] = 22;
        editor.replaceTone(host, far);
        const bool whole = last(0, std::vector<uint8_t>(far.begin(), far.end()));
        const size_t beforeUndo = host.writes.size();
        editor.undo(host);
        const bool split = host.writes.size() == beforeUndo + 2 && host.writes[beforeUndo].first == partialBase(0) + Partial::TvaLevel &&
                           host.writes[beforeUndo + 1].first == partialBase(3) + Partial::TvaLevel;
        // The unit's panel changed a byte: taken in, nothing sent back; undo sends the old value.
        const size_t beforeExternal = host.writes.size();
        const uint8_t panel = 77;
        editor.takeExternal(cutoff, &panel, 1);
        const bool external = host.writes.size() == beforeExternal && editor.tone()[size_t(cutoff)] == 77 && editor.sentTone()[size_t(cutoff)] == 77;
        editor.undo(host);
        const bool externalUndone = last(cutoff, {60});
        std::printf("      editor: single %d clamped %d solo %d/%d settled %d compare %d/%d undo %d/%d whole %d split %d external %d/%d\n", single,
                    clamped, soloSent, soloKept, settled, compared, back, undone, redone, whole, split, external, externalUndone);
        check(single && clamped && soloSent && soloKept && settled && compared && back && undone && redone && whole && split && external &&
                  externalUndone,
              "tone editor: a change sends its byte (clamped); solo, Compare and undo send only what differs; panel changes are taken in");

        // Audition: Repeat plays the note with its length, Hold holds it, Restrike plays held notes again after a change.
        host.notes.clear();
        ToneEditor::Audition& audition = editor.audition();
        audition.key = 60;
        audition.velocity = 90;
        audition.lengthMs = 500;
        audition.repeatMs = 1000;
        audition.repeat = true;
        editor.update(host, 10.0);
        editor.update(host, 10.6);
        editor.update(host, 11.05);
        audition.repeat = false;
        editor.update(host, 11.6);
        const bool repeated = host.notes == std::vector<uint32_t>{0x5A3C90, 0x3C80, 0x5A3C90, 0x3C80};
        host.notes.clear();
        const int restrikesBefore = host.restrikes;
        audition.hold = true;
        editor.update(host, 12.0);
        editor.setByte(host, cutoff, 61);
        editor.update(host, 12.02);  // A change after a pause: at once
        const bool atOnce = host.notes == std::vector<uint32_t>{0x5A3C90, 0x3C80, 0x5A3C90} && host.restrikes == restrikesBefore + 1;
        editor.setByte(host, cutoff, 62);
        editor.update(host, 12.1);   // Another change right after: waits for it to settle
        const bool waited = host.notes.size() == 3;
        editor.update(host, 12.2);
        const bool restruck = atOnce && host.notes.size() == 5 && host.notes[4] == 0x5A3C90 && host.restrikes == restrikesBefore + 2;
        audition.hold = false;
        editor.update(host, 12.3);
        const bool released = host.notes.back() == 0x3C80;
        check(repeated && waited && restruck && released, "tone editor audition: Repeat plays the note, Hold holds it, Restrike plays it again after a change");

        struct Recorder : MidiSender {
            std::vector<uint32_t> shortMessages;
            std::vector<std::vector<uint8_t>> sysex;
            bool sendShort(uint32_t message) override {
                shortMessages.push_back(message);
                return true;
            }
            bool sendSysex(const uint8_t* data, size_t length) override {
                sysex.emplace_back(data, data + length);
                return true;
            }
        };
        UnitLink link;
        Recorder out;
        link.setOutput(&out);
        link.setDevice(0x10);
        link.setPause(20);
        const uint32_t level = toneTempAddress(0) + uint32_t(partialBase(0) + Partial::TvaLevel);
        for (const uint8_t value : {uint8_t(10), uint8_t(20), uint8_t(30)}) link.sendData(level, &value, 1, true);
        link.sendShort(0x7F3C90);
        const UnitLink::Clock::time_point t0 = UnitLink::Clock::now();
        const UnitLink::Clock::time_point due = link.process(t0);
        const double waitMs = std::chrono::duration<double, std::milli>(due - t0).count();
        const bool merged = out.sysex.size() == 1 && out.sysex[0].size() == 11 && out.sysex[0][8] == 30 && link.stats().merged == 2 &&
                            out.shortMessages.empty() && waitMs > 3.0 && waitMs < 8.0;
        link.process(due);
        const bool noteAfter = out.shortMessages.size() == 1;
        // A parameter change never overtakes a whole tone that was queued after an earlier change of it.
        const std::vector<uint8_t> tone246(246, 5);
        const uint8_t a = 1, b = 2;
        link.sendData(level, &a, 1, true);
        link.sendData(toneTempAddress(0), tone246.data(), tone246.size(), false);
        link.sendData(level, &b, 1, true);
        UnitLink::Clock::time_point now = due;
        for (int step = 0; step < 20; step++) {
            const UnitLink::Clock::time_point next = link.process(now);
            if (next == UnitLink::Clock::time_point::max()) break;
            now = next;
        }
        const bool ordered = out.sysex.size() == 4 && out.sysex[1][8] == 1 && out.sysex[2].size() == 256 && out.sysex[3][8] == 2;
        // Requests go one at a time: the next once the unit answered, or after the timeout.
        link.request(toneTempAddress(0), 246);
        link.request(toneTempAddress(1), 246);
        link.process(now + std::chrono::milliseconds(100));
        const std::vector<uint8_t> rq1 = {0xF0, 0x41, 0x10, 0x16, 0x11, 0x04, 0x00, 0x00, 0x00, 0x01, 0x76, 0x05, 0xF7};
        const bool firstRequest = out.sysex.size() == 5 && out.sysex[4] == rq1;
        link.process(now + std::chrono::milliseconds(300));
        const bool oneAtATime = out.sysex.size() == 5;
        const std::vector<uint8_t> answer = RolandSysex::dataSet(0x10, toneTempAddress(0), tone246.data(), 246);
        link.onMidiSysex(answer.data(), answer.size());
        link.process(now + std::chrono::milliseconds(400));
        // The D-110 manual's example (p.118): part 2's tone from the temporary area, unit number 17.
        const std::vector<uint8_t> manualExample = {0xF0, 0x41, 0x10, 0x16, 0x11, 0x04, 0x01, 0x76, 0x00, 0x01, 0x76, 0x0E, 0xF7};
        const bool secondRequest = out.sysex.size() == 6 && out.sysex[5] == manualExample;
        link.process(now + std::chrono::milliseconds(400 + UnitLink::kRequestTimeoutMs + 10));
        const std::vector<UnitLink::Received> received = link.takeReceived();
        const UnitLink::Stats linkStats = link.stats();
        const bool timedOut = linkStats.unanswered == 1 && linkStats.answered == 1 && !link.takeLog().empty() && received.size() == 1 &&
                              received[0].address == toneTempAddress(0) && received[0].data.size() == 246;
        std::printf("      link: merged %d (next after %.1f ms) note %d ordered %d requests %d/%d/%d timeout %d\n", merged, waitMs, noteAfter,
                    ordered, firstRequest, oneAtATime, secondRequest, timedOut);
        check(merged && noteAfter && ordered && firstRequest && oneAtATime && secondRequest && timedOut,
              "unit link: parameter changes merge while they wait and are paced; requests go one at a time and time out");

        // ToneEditor on a unit played by d110emu, which answers requests from its memory as a real D-110 does.
        SynthEngine unitEngine;
        unitEngine.setOptions(options);
        const bool unitOpen = unitEngine.configure(config, error);
        struct EngineUnit : MidiSender {
            SynthEngine& engine;
            UnitLink& link;
            EngineUnit(SynthEngine& e, UnitLink& l) : engine(e), link(l) {}
            bool sendShort(uint32_t message) override {
                engine.onMidiShortMessage(message);
                return true;
            }
            bool sendSysex(const uint8_t* data, size_t length) override {
                if (length == 13 && data[4] == 0x11) {
                    // A unit takes messages in order: what came before the request is in memory when it answers.
                    std::vector<float> audio(2 * 32);
                    engine.render(audio.data(), 32);
                    const uint32_t address = uint32_t(data[5]) << 14 | uint32_t(data[6]) << 7 | data[7];
                    const uint32_t size = uint32_t(data[8]) << 14 | uint32_t(data[9]) << 7 | data[10];
                    std::vector<uint8_t> bytes(size);
                    engine.readMemory(RolandSysex::unpack(address), size, bytes.data());
                    const std::vector<uint8_t> reply = RolandSysex::dataSet(data[2], address, bytes.data(), size);
                    link.onMidiSysex(reply.data(), reply.size());
                } else {
                    engine.onMidiSysex(data, length);
                }
                return true;
            }
        };
        ToneEditorApp app;
        app.setModel(Model::D110);
        EngineUnit unitSide(unitEngine, app.link());
        app.link().setOutput(&unitSide);
        auto drain = [&](UnitLink::Clock::time_point& clock) {
            for (int step = 0; step < 5000; step++) {
                const UnitLink::Clock::time_point next = app.link().process(clock);
                if (next == UnitLink::Clock::time_point::max()) break;
                clock = next;
            }
            render(unitEngine, 0.05);  // The unit takes the data in
            app.processReceived();
        };
        UnitLink::Clock::time_point clock = UnitLink::Clock::now();
        app.requestPartTone();
        drain(clock);
        Data unitTone;
        unitEngine.readMemory(RolandSysex::unpack(toneTempAddress(0)), kSize, unitTone.data());
        EngineStatus unitStatus;
        unitEngine.getStatus(unitStatus);
        std::string partName = unitStatus.parts[0].name;
        partName.erase(partName.find_last_not_of(' ') + 1);
        const bool fetched = unitOpen && app.editor().tone() == unitTone && !partName.empty() && name(unitTone) == partName;
        // Realtime edits: a drag of the TVA level (merged on the way), a new name, a partial switched on.
        for (int value = 20; value <= 90; value += 5) app.edit(partialBase(0) + Partial::TvaLevel, value);
        for (int i = 0; i < 10; i++) app.edit(i, "Edited 1  "[i]);
        app.edit(Common::PartialMute, app.editor().tone()[Common::PartialMute] | 0x08);
        drain(clock);
        unitEngine.readMemory(RolandSysex::unpack(toneTempAddress(0)), kSize, unitTone.data());
        const bool edited = unitTone == app.editor().sentTone() && unitTone[size_t(partialBase(0) + Partial::TvaLevel)] == 90 && name(unitTone) == "Edited 1";
        // The unit's panel sends a parameter: the editor follows.
        const uint8_t panelValue = 33;
        const std::vector<uint8_t> panelEdit = RolandSysex::dataSet(0x10, toneTempAddress(0) + uint32_t(partialBase(2) + Partial::TvfCutoff), &panelValue, 1);
        unitEngine.onMidiSysex(panelEdit.data(), panelEdit.size());  // Edited on the unit, which sends it out
        render(unitEngine, 0.05);
        app.link().onMidiSysex(panelEdit.data(), panelEdit.size());
        app.processReceived();
        const bool followed = app.editor().tone()[size_t(partialBase(2) + Partial::TvfCutoff)] == 33;
        // Written to i85 with the unit's tone write, then read back with the whole tone memory.
        app.writeToMemory(60);
        drain(clock);
        Data stored;
        unitEngine.readMemory(RolandSysex::unpack(toneMemoryAddress(60)), kSize, stored.data());
        unitEngine.getStatus(unitStatus);
        const bool written = stored == app.editor().tone() && std::string(unitStatus.parts[0].tone) == "i85";
        app.requestLibrary();
        drain(clock);
        int known = 0;
        for (int slot = 0; slot < 64; slot++) known += app.libraryTone(slot) != nullptr ? 1 : 0;
        const bool library = known == 64 && *app.libraryTone(60) == stored && app.link().stats().unanswered == 0;
        std::printf("      unit: fetched %d edited %d followed %d written %d library %d (%d tones), %llu messages, %llu merged\n", fetched, edited,
                    followed, written, library, known, static_cast<unsigned long long>(app.link().stats().sent),
                    static_cast<unsigned long long>(app.link().stats().merged));
        check(fetched && edited && followed && written && library,
              "ToneEditor on an emulated D-110: the part's tone read, realtime edits applied, panel edits followed, tone write, tone memory read back");

        // The unit's setup (ToneEditor's Parts, Rhythm and System tabs): read with RQ1, then changed as the tabs do.
        struct LinkHost : UnitSetup::Host {
            UnitLink& link;
            int sends = 0;
            explicit LinkHost(UnitLink& l) : link(l) {}
            void sendData(uint32_t address, const uint8_t* data, size_t length, bool merge) override {
                link.sendData(address, data, length, merge);
                sends++;
            }
            void request(uint32_t address, uint32_t size) override { link.request(address, size); }
            void sendShort(uint32_t message) override { link.sendShort(message); }
            std::string memoryToneName(int) override { return std::string(); }
        };
        LinkHost linkHost(app.link());
        UnitSetup& setup = app.setup();
        app.requestSetup();
        drain(clock);
        uint8_t part2Temp[16], key38[4], systemArea[0x21];
        unitEngine.readMemory(0x030010, 16, part2Temp);
        unitEngine.readMemory(at(0x030110, (38 - 24) * 4), 4, key38);
        unitEngine.readMemory(0x100000, 0x21, systemArea);
        bool setupRead = true;
        for (int i = 0; i < 16; i++) setupRead = setupRead && setup.partValue(1, i) == part2Temp[i];
        for (int i = 0; i < 4; i++) setupRead = setupRead && setup.rhythmValue(38, i) == key38[i];
        for (int i = 0; i < 0x21; i++) setupRead = setupRead && setup.systemValue(i) == systemArea[i];
        for (int part = 0; part < UnitSetup::kParts; part++) setupRead = setupRead && setup.channels()[size_t(part)] == systemArea[0x0D + part];
        setup.setPartValue(linkHost, 1, UnitSetup::Level, 55);
        setup.setPartValue(linkHost, 1, UnitSetup::Panpot, 3);
        setup.setPartTone(linkHost, 1, 1, 4);  // b15
        setup.setPartValue(linkHost, UnitSetup::kRhythmPart, UnitSetup::Level, 70);
        setup.setRhythmValue(linkHost, 38, UnitSetup::RhythmLevel, 77);
        setup.setSystemValue(linkHost, UnitSetup::ReverbMode, 2);
        setup.setReserve(linkHost, 7, 0);  // Room for part 1 in the package of nine (at most 32 in all)
        setup.setReserve(linkHost, 0, 30);  // Clamped to what is free
        setup.setChannel(linkHost, 2, 11);
        drain(clock);
        EngineStatus changedStatus;
        unitEngine.getStatus(changedStatus);
        int reservedTotal = 0;
        for (int part = 0; part < kBasePartCount; part++) reservedTotal += changedStatus.parts[part].reserve;
        const bool setupChanged = changedStatus.parts[1].temp[TimbreTemp::OutputLevel] == 55 && changedStatus.parts[1].temp[TimbreTemp::Panpot] == 3 &&
                                  std::string(changedStatus.parts[1].tone) == "b15" && changedStatus.parts[kRhythmPart].temp[TimbreTemp::OutputLevel] == 70 &&
                                  readByte(unitEngine, at(0x030110, (38 - 24) * 4 + 1)) == 77 && changedStatus.reverbMode == 2 &&
                                  changedStatus.parts[7].reserve == 0 && changedStatus.parts[0].reserve == systemArea[4] + systemArea[4 + 7] &&
                                  reservedTotal == 32 && changedStatus.parts[2].channel == 11;
        // A D-10/D-20 answers with dummies where a D-110 keeps its part channels (the same value for all nine): not taken.
        UnitSetup dummies;
        dummies.setModel(Model::D110);
        std::array<uint8_t, 0x21> dummyArea{};
        dummies.take(UnitSetup::kSystemAddress, dummyArea.data(), dummyArea.size());
        std::array<uint8_t, UnitSetup::kParts> dummyChannels{};
        const bool dummiesIgnored = dummies.channelsMissing() && !dummies.takeChannelsFromUnit(dummyChannels) && dummies.channels()[0] == 0 &&
                                    dummies.channels()[1] == 1 && dummies.channels()[UnitSetup::kRhythmPart] == 9;
        // The Rhythm tab's Play: a key on the rhythm part's channel (10 here), released half a second later.
        render(unitEngine, 1.0);
        const bool played = setup.playRhythmKey(linkHost, 38, 100.0);
        drain(clock);
        EngineStatus drumStatus;
        unitEngine.getStatus(drumStatus);
        const bool drumSounds = played && drumStatus.parts[kRhythmPart].noteCount > 0;
        setup.update(linkHost, 100.6);
        drain(clock);
        // Timbre memory (the Timbres tab): read back, played on part 1 and edited there and in memory at once, the unit's
        // timbre write, and a paste.
        uint8_t timbreMemory[128 * 8];
        unitEngine.readMemory(0x050000, sizeof(timbreMemory), timbreMemory);
        bool timbresRead = true;
        for (int number = 0; number < UnitSetup::kTimbres; number++) {
            for (int offset = 0; offset < UnitSetup::kTimbreSize; offset++) {
                timbresRead = timbresRead && setup.timbreValue(number, offset) == timbreMemory[number * 8 + offset];
            }
        }
        const bool selected = setup.selectTimbre(linkHost, 0, 5);  // A16 on part 1
        drain(clock);
        setup.setTimbreValue(linkHost, 5, UnitSetup::KeyShift, 24 + 7, 0);
        const bool toneMoved = setup.setTimbreTone(linkHost, 5, 1, 10, 0);  // b23: part 1's tone changes
        drain(clock);
        EngineStatus timbreStatus;
        unitEngine.getStatus(timbreStatus);
        const bool timbreEdited = selected && toneMoved && setup.partTimbre(0) == 5 && readByte(unitEngine, at(0x050000, 5 * 8 + 2)) == 31 &&
                                  readByte(unitEngine, at(0x050000, 5 * 8)) == 1 && readByte(unitEngine, at(0x050000, 5 * 8 + 1)) == 10 &&
                                  timbreStatus.parts[0].temp[TimbreTemp::KeyShift] == 31 && std::string(timbreStatus.parts[0].tone) == "b23";
        setup.setPartValue(linkHost, 0, UnitSetup::FineTune, 60);  // Edited on the part, then written as B88
        setup.writeTimbre(linkHost, 0, 127);
        drain(clock);
        uint8_t storedTimbre[8], partTemp[8];
        unitEngine.readMemory(at(0x050000, 127 * 8), 8, storedTimbre);
        unitEngine.readMemory(0x030000, 8, partTemp);
        const bool timbreWritten = std::memcmp(storedTimbre, partTemp, 7) == 0 && storedTimbre[3] == 60 && setup.timbreValue(127, UnitSetup::FineTune) == 60 &&
                                   setup.partTimbre(0) == 127;
        const uint8_t pasted[7] = {0, 3, 24 + 12, 50, 2, 2, 1};
        setup.pasteTimbre(linkHost, 10, pasted, 0);
        drain(clock);
        uint8_t pastedBack[7];
        unitEngine.readMemory(at(0x050000, 10 * 8), 7, pastedBack);
        const bool timbrePasted = std::memcmp(pastedBack, pasted, 7) == 0;
        std::printf("      timbres: read %d, edited in memory and on part 1 %d, timbre write %d, paste %d\n", timbresRead, timbreEdited,
                    timbreWritten, timbrePasted);
        check(timbresRead && timbreEdited && timbreWritten && timbrePasted,
              "ToneEditor's timbres: timbre memory read back; a timbre played on part 1 changes there and in memory at once; the unit's timbre write; paste");

        // The D-10/D-20 performance patch (the Performance tab), on the emulated unit in performance mode on channel 1.
        unitEngine.setPerformanceMode(true, 0);
        render(unitEngine, 0.1);
        app.setModel(Model::D20);  // The Performance tab is shown for the D-10/D-20 only
        UnitSetup& performance = app.setup();
        performance.performanceChannel() = 0;
        performance.requestPatch(linkHost);
        drain(clock);
        EngineStatus perfStatus;
        unitEngine.getStatus(perfStatus);
        bool patchRead = true;
        for (int offset = 0; offset < UnitSetup::kPatchSize; offset++) {
            patchRead = patchRead && performance.patchValue(offset) == perfStatus.performanceTemp[offset];
        }
        performance.setPatchValue(linkHost, UnitSetup::KeyMode, 2);      // Split
        performance.setPatchValue(linkHost, UnitSetup::SplitPoint, 24);  // At C4
        performance.setPatchTone(linkHost, true, 0, 10);                 // Upper a23
        performance.setPatchTone(linkHost, false, 1, 4);                 // Lower b15
        performance.setPatchValue(linkHost, UnitSetup::PatchLevel, 90);
        performance.setPatchName(linkHost, "Split Test");
        drain(clock);
        unitEngine.getStatus(perfStatus);
        const bool patchEdited = perfStatus.performanceTemp[0] == 2 && perfStatus.performanceTemp[1] == 24 && perfStatus.performanceTemp[0x14] == 90 &&
                                 std::string(reinterpret_cast<const char*>(&perfStatus.performanceTemp[0x15]), 10) == "Split Test" &&
                                 std::string(perfStatus.parts[0].tone) == "a23" && std::string(perfStatus.parts[1].tone) == "b15" &&
                                 perfStatus.parts[0].temp[TimbreTemp::KeyRangeLower] == 60 && perfStatus.parts[1].temp[TimbreTemp::KeyRangeUpper] == 59;
        performance.writePatch(linkHost, 9);  // A22, with the unit's patch write
        drain(clock);
        uint8_t storedPatch[UnitSetup::kPatchSize];
        unitEngine.readMemory(at(0x070000, 9 * 38), UnitSetup::kPatchSize, storedPatch);
        const bool patchWritten = std::memcmp(storedPatch, perfStatus.performanceTemp, UnitSetup::kPatchSize - 1) == 0 && performance.patchName(9) == "Split Test";
        performance.selectPatch(linkHost, 0);  // Another patch, then A22 again: program changes on the performance channel
        drain(clock);
        const bool otherPatch = performance.patchValue(UnitSetup::PatchLevel) != 90 || performance.patchValue(UnitSetup::KeyMode) != 2;
        performance.selectPatch(linkHost, 9);
        drain(clock);
        const bool patchSelected = otherPatch && performance.currentPatch() == 9 && performance.patchValue(UnitSetup::PatchLevel) == 90 &&
                                   performance.patchValue(UnitSetup::KeyMode) == 2;
        // The Tone tab in performance mode: Lower edits part 2's tone, and notes go to the performance channel.
        performance.performanceChannel() = 3;
        app.setPerformanceMode(true);
        app.selectPart(1);
        app.requestPartTone();
        drain(clock);
        Data lowerTone;
        unitEngine.readMemory(RolandSysex::unpack(toneTempAddress(1)), kSize, lowerTone.data());
        const bool lowerTones = app.performanceTones() && app.partChannel() == 3 && app.editor().tone() == lowerTone &&
                                performance.patchValue(UnitSetup::LowerToneGroup) == 1 && performance.patchValue(UnitSetup::LowerToneNumber) == 4;
        app.setPerformanceMode(false);
        const bool partsAgain = !app.performanceTones() && app.partChannel() == performance.channels()[1];
        performance.performanceChannel() = 0;
        unitEngine.setPerformanceMode(false, 0);
        app.setModel(Model::D110);
        std::printf("      performance: read %d, edited %d, written %d, selected %d, lower tone %d, parts again %d\n", patchRead, patchEdited,
                    patchWritten, patchSelected, lowerTones, partsAgain);
        check(patchRead && patchEdited && patchWritten && patchSelected && lowerTones && partsAgain,
              "ToneEditor's performance patch (D-10/D-20): read back; split, tones, level and name changed on the unit at once; the unit's "
              "patch write; patches selected; the lower tone edited with notes on the performance channel");

        // A D-20's part channels are set on its panel: changing one sends nothing.
        UnitSetup d20Setup;
        d20Setup.setModel(Model::D20);
        const int sendsBefore = linkHost.sends;
        d20Setup.setChannel(linkHost, 0, 5);
        const bool d20Channels = linkHost.sends == sendsBefore && d20Setup.channels()[0] == 5 && UnitSetup::systemSize(Model::D20) == 0x32;
        std::printf("      setup: read %d changed %d (part 1 reserve %u, total %d), D-20 channels local %d, dummy channels ignored %d, "
                    "rhythm Play %d\n", setupRead, setupChanged, changedStatus.parts[0].reserve, reservedTotal, d20Channels, dummiesIgnored,
                    drumSounds);
        check(setupRead && setupChanged && d20Channels && dummiesIgnored && drumSounds,
              "ToneEditor's unit setup: parts, rhythm setup and system read back and changed on the unit; a D-20's dummy channels "
              "ignored; Play sounds a rhythm key");

        // Tones in SysEx files: a bulk dump, and MT-32 timbres (VOICE_MT.DAT's handshake packets) with their waves translated.
        const std::vector<uint8_t> toneDump = unitEngine.dumpSysex(DumpTones);
        const std::vector<FoundTone> found = findTones(toneDump.data(), toneDump.size());
        const std::vector<std::string> toneNames = unitEngine.toneNames();
        const bool dumpFound = found.size() == 64 && found[60].where == "i85" && found[60].data == stored && name(found[3].data) == [&] {
            std::string n = toneNames[128 + 3];
            n.erase(n.find_last_not_of(' ') + 1);
            return n;
        }();
        std::vector<uint8_t> voiceFile;
        for (const char* path : {"../VOICE_MT.DAT", "../../VOICE_MT.DAT", "VOICE_MT.DAT"}) {
            std::ifstream in(path, std::ios::binary);
            if (in) voiceFile.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (!voiceFile.empty()) break;
        }
        bool voiceFound = true;
        if (!voiceFile.empty()) {
            const std::vector<FoundTone> timbres = findTones(voiceFile.data(), voiceFile.size());
            voiceFound = timbres.size() == 64 && timbres[0].where == "i11" && name(timbres[0].data) == "Power Hit";
            std::printf("      VOICE_MT.DAT: %zu timbres, first \"%s\"\n", timbres.size(), timbres.empty() ? "" : name(timbres[0].data).c_str());
        }
        check(dumpFound && voiceFound, "tones found in SysEx files: a bulk dump's 64 tones, VOICE_MT.DAT's 64 MT-32 timbres");
    }

    // The terminal interface: MIDI bytes into messages (the ALSA input), keys from a terminal's bytes, frames as escape
    // sequences, and the application's core with its keys (MT-32 translation, ROM Play and the memory kept).
    {
        struct Recorder : MidiInputSink {
            std::vector<uint32_t> shorts;
            std::vector<std::vector<uint8_t>> sysex;
            std::vector<uint8_t> realTime;
            void onMidiShortMessage(uint32_t message) override { shorts.push_back(message); }
            void onMidiSysex(const uint8_t* data, size_t length) override { sysex.emplace_back(data, data + length); }
            void onMidiRealTime(uint8_t status) override { realTime.push_back(status); }
        } sink;
        MidiStreamParser parser;
        // Running status with a clock between a note's data bytes; a program change; SysEx in three pieces with a clock in
        // it; a tune request; a SysEx cut short by a note-off; active sensing and reset, dropped.
        const uint8_t a[] = {0x90, 60, 100, 62, 0xF8, 90, 0xC1, 5};
        const uint8_t b[] = {0xF0, 0x41, 0x10};
        const uint8_t c[] = {0x16, 0xF8, 0x12};
        const uint8_t d[] = {0x20, 0xF7, 0xF6, 0xF0, 1, 2, 0x80, 60, 0, 0xFE, 0xFF};
        const uint32_t counted = parser.feed(sink, a, sizeof(a)) + parser.feed(sink, b, sizeof(b)) + parser.feed(sink, c, sizeof(c)) +
                                 parser.feed(sink, d, sizeof(d));
        const bool shortsRight = sink.shorts == std::vector<uint32_t>{0x643C90u, 0x5A3E90u, 0x05C1u, 0xF6u, 0x003C80u};
        const bool sysexRight = sink.sysex.size() == 1 && sink.sysex[0] == std::vector<uint8_t>{0xF0, 0x41, 0x10, 0x16, 0x12, 0x20, 0xF7};
        const bool realTimeRight = sink.realTime == std::vector<uint8_t>{0xF8, 0xF8};
        check(shortsRight && sysexRight && realTimeRight && counted == 6,
              "MIDI bytes into messages: running status, SysEx in pieces, real-time messages anywhere, a SysEx cut short dropped");

        auto keysOf = [](const std::string& bytes, bool flush) {
            Tui::KeyParser keyParser;
            std::vector<Tui::Key> keys;
            keyParser.feed(bytes.data(), bytes.size(), keys);
            if (flush) keyParser.flush(keys);
            return keys;
        };
        const std::vector<Tui::Key> arrows = keysOf("\x1b[A\x1bOB\x1b[1;5C\x1b[5~\x1b[[A\x1bOP\x1b[24~", false);
        const bool arrowsRight = arrows.size() == 7 && arrows[0].code == Tui::Key::Up && arrows[1].code == Tui::Key::Down &&
                                 arrows[2].code == Tui::Key::Right && arrows[2].ctrl && arrows[3].code == Tui::Key::PageUp &&
                                 arrows[4].code == Tui::Key::F1 && arrows[5].code == Tui::Key::F1 && arrows[6].code == Tui::Key::F12;
        const std::vector<Tui::Key> text = keysOf("q\x03\xc3\xa9\x1bx\r\n\x7f", false);
        const bool textRight = text.size() == 6 && text[0].is(U'q') && text[1].ctrl && text[1].ch == U'c' && text[2].ch == U'\u00e9' &&
                               text[3].alt && text[3].ch == U'x' && text[4].code == Tui::Key::Enter && text[5].code == Tui::Key::Backspace;
        Tui::KeyParser split;
        std::vector<Tui::Key> splitKeys;
        split.feed("\x1b", 1, splitKeys);
        const bool waited = splitKeys.empty() && split.pending();
        split.feed("[B", 2, splitKeys);
        const bool joined = splitKeys.size() == 1 && splitKeys[0].code == Tui::Key::Down;
        const std::vector<Tui::Key> escape = keysOf("\x1b", true);
        check(arrowsRight && textRight && waited && joined && escape.size() == 1 && escape[0].code == Tui::Key::Escape,
              "terminal keys: cursor, page and function keys (xterm's and the Linux console's), modifiers, UTF-8, Alt, Ctrl, a "
              "sequence split between reads, Escape alone");

        Tui::Capabilities ascii;
        ascii.glyphs = Tui::Capabilities::Glyphs::Ascii;
        ascii.colors = Tui::Capabilities::Colors::None;
        Tui::Encoder encoder(ascii);
        Tui::Screen screen;
        screen.resize(10, 3);
        screen.box(0, 0, 10, 3, Tui::Style());
        const std::string whole = encoder.frame(screen);
        screen.set(4, 1, U'█', Tui::Style());
        const std::string changed = encoder.frame(screen);
        const std::string unchanged = encoder.frame(screen);
        const bool asciiBox = whole.find("+--------+") != std::string::npos && whole.find('|') != std::string::npos &&
                              whole.find("\xe2") == std::string::npos;
        const bool onlyChanged = changed == "\x1b[2;5H\x1b[0m#" && unchanged.empty();
        Tui::Style lcd;
        lcd.fg = Tui::Color::rgb(24, 36, 12, 0);
        lcd.bg = Tui::Color::rgb(79, 139, 21, 2);
        auto colours = [&](Tui::Capabilities::Colors depth, bool boldBright, const Tui::Style& cellStyle) {
            Tui::Capabilities caps;
            caps.colors = depth;
            caps.boldBright = boldBright;
            Tui::Encoder colourEncoder(caps);
            Tui::Screen cell;
            cell.resize(1, 1);
            cell.set(0, 0, U'A', cellStyle);
            return colourEncoder.frame(cell);
        };
        Tui::Style bright;
        bright.fg = Tui::Color::ansiColor(10);
        const bool trueColour = colours(Tui::Capabilities::Colors::TrueColor, false, lcd).find("38;2;24;36;12;48;2;79;139;21m") != std::string::npos;
        const bool colours256 = colours(Tui::Capabilities::Colors::Ansi256, false, lcd).find(";38;5;234;48;5;64m") != std::string::npos;
        const bool colours16 = colours(Tui::Capabilities::Colors::Ansi16, false, lcd).find("\x1b[0;30;42mA") != std::string::npos;
        const bool console = colours(Tui::Capabilities::Colors::Ansi16, true, bright).find("\x1b[0;1;32mA") != std::string::npos &&
                             colours(Tui::Capabilities::Colors::Ansi16, false, bright).find("\x1b[0;92mA") != std::string::npos;
        check(asciiBox && onlyChanged && trueColour && colours256 && colours16 && console,
              "terminal frames: ASCII lines and blocks where the terminal lacks them, only the cells that changed, colours as "
              "the terminal has them (24-bit, 256, 16, the Linux console's bold brights)");
        // The LCD's yen sign: itself in UTF-8 (the Linux console's fonts have it, as Latin-1), Y where only ASCII shows.
        auto yenFrame = [](Tui::Capabilities::Glyphs glyphs) {
            Tui::Capabilities caps;
            caps.glyphs = glyphs;
            caps.colors = Tui::Capabilities::Colors::None;
            Tui::Encoder yenEncoder(caps);
            Tui::Screen cell;
            cell.resize(1, 1);
            cell.set(0, 0, U'¥', Tui::Style());
            return yenEncoder.frame(cell);
        };
        const std::string yenAscii = yenFrame(Tui::Capabilities::Glyphs::Ascii);
        check(yenFrame(Tui::Capabilities::Glyphs::Unicode).find("\xc2\xa5") != std::string::npos &&
                  yenFrame(Tui::Capabilities::Glyphs::Console).find("\xc2\xa5") != std::string::npos &&
                  yenAscii.find('Y') != std::string::npos && yenAscii.find("\xc2") == std::string::npos,
              "terminal frames: the yen sign itself where the terminal has it (the Linux console too), Y in ASCII");

        // The core with the terminal's keys, on settings and a memory file of its own.
        const std::filesystem::path folder = std::filesystem::temp_directory_path() / "d110tests-tui";
        std::error_code ec;
        std::filesystem::remove_all(folder, ec);
        std::filesystem::create_directories(folder, ec);
        {
            std::ofstream seed(folder / "d110emu.ini");
            seed << "rom_folder = " << Platform::toUtf8(std::filesystem::absolute(romFolder)) << "\n";
        }
        AppOptions appOptions;
        appOptions.settingsFile = folder / "d110emu.ini";
        appOptions.memoryFile = folder / "d110emu-memory.syx";
        appOptions.enableAudio = false;
        appOptions.enableMidiInput = false;
        appOptions.outputSampleRate = 48000;
        std::vector<float> audio(2 * 4800);
        auto run = [&](TuiApp& app, double& clock, int blocks) {
            for (int i = 0; i < blocks; i++) {
                app.engine().render(audio.data(), 4800);
                clock += 0.1;
                app.setClock(clock);
                app.tick();
            }
        };
        auto partChannel = [](TuiApp& app, int part) {
            EngineStatus appStatus;
            app.engine().getStatus(appStatus);
            return int(appStatus.parts[part].channel);
        };
        bool mt32Keys = false;
        bool romPlayChannels = false;
        {
            TuiApp app{TuiApp::Options()};
            double clock = 0.0;
            app.setClock(clock);
            app.init(appOptions);
            run(app, clock, 3);
            const int before = partChannel(app, 0);
            app.onKey(Tui::Key::character(U't'));
            run(app, clock, 3);
            const int translating = partChannel(app, 0);
            app.onKey(Tui::Key::character(U'T'));
            run(app, clock, 3);
            mt32Keys = before == 0 && translating == 1 && partChannel(app, 0) == 0;
            app.playRomSong(0);
            run(app, clock, 10);
            romPlayChannels = partChannel(app, 0) == 1;  // ROM Play's parts are on channels 2-9
            app.shutdown();  // During ROM Play
        }
        bool memoryKept = false;
        bool screenRight = false;
        bool menusRight = false;
        {
            TuiApp app{TuiApp::Options()};
            double clock = 0.0;
            app.setClock(clock);
            app.init(appOptions);
            run(app, clock, 3);
            memoryKept = partChannel(app, 0) == 0 && partChannel(app, kRhythmPart) == 9;
            Tui::Screen appScreen;
            appScreen.resize(80, 24);
            run(app, clock, 30);  // Past the boot message
            app.draw(appScreen);
            const std::string shown = appScreen.plainText();
            screenRight = shown.find("D110Emu") != std::string::npos && shown.find("12345678R  Part1") != std::string::npos &&
                          shown.find("Timbre") != std::string::npos && shown.find("Partials") != std::string::npos;
            app.onKey(Tui::Key::character(U'o'));
            app.draw(appScreen);
            const bool options = appScreen.plainText().find("Options") != std::string::npos;
            app.onKey(Tui::Key::of(Tui::Key::Escape));
            app.onKey(Tui::Key::character(U'?'));
            app.draw(appScreen);
            const bool help = appScreen.plainText().find("Keys") != std::string::npos;
            app.onKey(Tui::Key::character(U' '));  // Closes the help
            app.onKey(Tui::Key::character(U'q'));
            menusRight = options && help && app.quitRequested();
            app.shutdown();
        }
        // Restart synth from the interface: part 2's level at 100 and pan centred, its timbre kept; a restart for a setting
        // (a plugin host's sample rate) keeps the level and pan; X (Reset MIDI) resets them too.
        bool restartMix = false;
        bool settingRestart = false;
        bool resetKey = false;
        {
            TuiApp app{TuiApp::Options()};
            double clock = 0.0;
            app.setClock(clock);
            app.init(appOptions);
            run(app, clock, 3);
            EngineStatus appStatus;
            auto moveMix = [&] {
                app.engine().setPartParameter(1, TimbreTemp::OutputLevel, 40);
                app.engine().setPartParameter(1, TimbreTemp::Panpot, 12);
                run(app, clock, 1);
            };
            auto mixIs = [&](int level, int pan) {
                app.engine().getStatus(appStatus);
                return appStatus.parts[1].temp[TimbreTemp::OutputLevel] == level && appStatus.parts[1].temp[TimbreTemp::Panpot] == pan;
            };
            app.engine().setPartParameter(1, TimbreTemp::ToneNumber, 20);
            moveMix();
            const bool moved = mixIs(40, 12);
            app.resetSynth();
            run(app, clock, 3);
            restartMix = moved && mixIs(100, 7) && appStatus.parts[1].temp[TimbreTemp::ToneNumber] == 20;
            moveMix();
            app.setOutputSampleRate(44100);
            run(app, clock, 3);
            settingRestart = mixIs(40, 12);
            app.onKey(Tui::Key::character(U'x'));
            run(app, clock, 1);
            resetKey = mixIs(100, 7);
            app.shutdown();
        }
        // The LCD's character set: a display message's backslash (5CH) shows as the yen sign, as on the units.
        bool yenOnLcd = false;
        {
            TuiApp app{TuiApp::Options()};
            double clock = 0.0;
            app.setClock(clock);
            app.init(appOptions);
            run(app, clock, 30);  // Past the boot message
            std::vector<uint8_t> text(32, ' ');
            const char message[] = "C:\\D110EMU";
            std::memcpy(text.data(), message, sizeof(message) - 1);
            const std::vector<uint8_t> display = dataSet(0x10, 0x200000, text);
            app.engine().onMidiSysex(display.data(), display.size());
            run(app, clock, 2);
            Tui::Screen appScreen;
            appScreen.resize(80, 24);
            app.draw(appScreen);
            yenOnLcd = appScreen.plainText().find("C:\xc2\xa5" "D110EMU") != std::string::npos && app.lcdText().find("C:\\D110EMU") == 0;
            app.shutdown();
        }
        // Performance mode at the next start: the patch last recalled is still the current one (its number on the LCD).
        bool performanceKept = false;
        {
            {
                std::ofstream seed(folder / "d110emu.ini", std::ios::app);
                seed << "performance_mode = true\n";
            }
            EngineStatus appStatus;
            {
                TuiApp app{TuiApp::Options()};
                double clock = 0.0;
                app.setClock(clock);
                app.init(appOptions);
                run(app, clock, 3);
                app.engine().recallPerformance(9);  // A22
                run(app, clock, 2);
                app.shutdown();
            }
            TuiApp app{TuiApp::Options()};
            double clock = 0.0;
            app.setClock(clock);
            app.init(appOptions);
            run(app, clock, 30);  // Past the boot message
            app.engine().getStatus(appStatus);
            performanceKept = appStatus.performanceMode && appStatus.currentPerformance == 9 && app.lcdText().find("I-A22") == 0;
            std::printf("      after a new start: performance mode %d, current performance %u, LCD \"%s\"\n", appStatus.performanceMode,
                        unsigned(appStatus.currentPerformance), app.lcdText().substr(0, 16).c_str());
            app.shutdown();
        }
        std::filesystem::remove_all(folder, ec);
        std::printf("      MT-32 keys %d, ROM Play channels %d, memory kept %d, screen %d, menus %d\n", mt32Keys, romPlayChannels, memoryKept,
                    screenRight, menusRight);
        check(mt32Keys && romPlayChannels && memoryKept && screenRight && menusRight,
              "the terminal interface: T switches MT-32 translation on and off; quitting during ROM Play keeps the user's parts in "
              "the memory, not the song's; the status screen, the options, the help and Q");
        std::printf("      Restart synth %d, a restart for a setting %d, X %d\n", restartMix, settingRestart, resetKey);
        check(restartMix && settingRestart && resetKey,
              "Restart synth from the interface puts every part's level at 100 and pan centred (the timbre kept), as X (Reset MIDI) "
              "does; a restart for a setting keeps them");
        check(yenOnLcd, "the terminal version's LCD shows a display message's backslash (5CH) as the yen sign, as the units do");
        check(performanceKept, "performance mode at the next start: the performance patch last recalled is current again (I-A22 on the LCD)");
    }

    // MT32Translator's terminal version, without MIDI ports: its keys, its settings file, a translation.
    {
        const std::filesystem::path folder = std::filesystem::temp_directory_path() / "d110tests-translator";
        std::error_code ec;
        std::filesystem::remove_all(folder, ec);
        std::filesystem::create_directories(folder, ec);
        TranslatorOptions translatorOptions;
        translatorOptions.settingsFile = folder / "mt32translator.ini";
        translatorOptions.romSearchDirs = {std::filesystem::absolute(romFolder).parent_path()};
        translatorOptions.enableMidi = false;
        // Its protected parts, for the test.
        struct Translator : TranslatorTui {
            Translator() : TranslatorTui(TranslatorTui::Options()) {}
            using TranslatorCore::translateFileTo;
            using TranslatorCore::translatedName;
            const PipeSettings& settings() const { return pipeSettings_; }
            std::shared_ptr<const Mt32Presets> presets() const { return presets_; }
        };
        auto key = [](Translator& app, const std::vector<Tui::Key>& keys) {
            for (const Tui::Key& k : keys) app.onKey(k);
            app.tick();
        };
        const Tui::Key down = Tui::Key::of(Tui::Key::Down), left = Tui::Key::of(Tui::Key::Left), right = Tui::Key::of(Tui::Key::Right);
        bool unitKept = false;
        bool screenRight = false;
        bool translated = false;
        bool menusRight = false;
        {
            Translator app;
            app.setClock(0.0);
            app.init(translatorOptions);
            // Options: the fifth item is the unit (a D-10/D-20 at first); left makes it a D-110, and the unit number goes up.
            key(app, {Tui::Key::character(U'o'), down, down, down, down, left, down, right, right, Tui::Key::of(Tui::Key::Escape)});
            unitKept = app.settings().target == Mt32Translator::Target::D110 && app.settings().memoryInUnit && app.settings().unitNumber == 19;
            Tui::Screen translatorScreen;
            translatorScreen.resize(80, 24);
            app.draw(translatorScreen);
            const std::string shown = translatorScreen.plainText();
            screenRight = shown.find("MT32Translator") != std::string::npos && shown.find("D-110, unit number 19 (device 12H)") != std::string::npos &&
                          shown.find("Presets") != std::string::npos && shown.find("Received 0") != std::string::npos;
            // A MIDI file, as the terminal's T writes it: the same as the translation itself.
            SmfFile song;
            song.format = 0;
            song.trackCount = 1;
            song.events.push_back(SmfEvent{0.0, 0x000AC1, 0, 0});  // Program change on the MT-32's part 1
            song.events.push_back(SmfEvent{0.5, 0x7F3C91, 0, 0});
            song.events.push_back(SmfEvent{1.0, 0x003C81, 0, 0});
            song.duration = 1.5;
            const std::filesystem::path source = folder / "song.mid";
            std::string smfError;
            saveSmfFile(source, song, 480, 500000, smfError);
            const std::filesystem::path target = folder / app.translatedName(source);
            SmfFile loaded;
            SmfFile expected;
            loadSmfFile(source, loaded, smfError);
            translateMidiFile(loaded, expected, app.settings(), app.presets());
            const std::filesystem::path expectedFile = folder / "expected.mid";
            saveSmfFile(expectedFile, expected, 480, 500000, smfError);
            auto bytes = [](const std::filesystem::path& path) {
                std::ifstream in(path, std::ios::binary);
                return std::vector<char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            };
            translated = app.translateFileTo(source, target) && Platform::toUtf8(target.filename()) == "song (D-110).mid" &&
                         !bytes(target).empty() && bytes(target) == bytes(expectedFile);
            key(app, {Tui::Key::character(U'?')});
            app.draw(translatorScreen);
            const bool help = translatorScreen.plainText().find("Keys") != std::string::npos;
            key(app, {Tui::Key::character(U' '), Tui::Key::character(U'q')});
            menusRight = help && app.quitRequested();
            app.shutdown();
        }
        bool reloaded = false;
        {
            Translator app;
            app.setClock(0.0);
            app.init(translatorOptions);
            reloaded = app.settings().target == Mt32Translator::Target::D110 && app.settings().unitNumber == 19;
            app.shutdown();
        }
        std::filesystem::remove_all(folder, ec);
        std::printf("      unit %d, screen %d, translated %d, menus %d, kept %d\n", unitKept, screenRight, translated, menusRight, reloaded);
        check(unitKept && screenRight && translated && menusRight && reloaded,
              "MT32Translator's terminal version: the options' keys change the unit and its number, kept in its settings file; the "
              "status screen; a MIDI file translated as the translation itself does it; the help and Q");
    }

    std::printf("%s: %d failure(s)\n", failures == 0 ? "OK" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
