// Command-line MT-32 file translation, as MT32Translator's "Translate file": a SysEx file (.syx, or a game's .dat
// with handshake packets) becomes one that loads into a D-110, D-10 or D-20's memory; a MIDI file becomes one that
// plays on the unit, with the MT-32's power-on setup first.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "MidiPipe.h"
#include "Mt32Presets.h"
#include "Platform.h"
#include "SmfFile.h"

namespace {

void printUsage() {
    std::fprintf(stderr,
                 "Usage: mt32translate [options] input output\n"
                 "  input .syx/.dat -> output .syx for the unit's memory (load it with the unit's Memory Protect off)\n"
                 "  input .mid      -> output .mid that plays on the unit, the MT-32's power-on setup first\n"
                 "Options:\n"
                 "  --target d110|d20  the unit (default d20: D-10/D-20 in multi-timbral mode)\n"
                 "  --unit N           its unit number, 17-32 (default 17: device ID 10H)\n"
                 "  --channels LIST    D-20 part channels 1-8 and rhythm, e.g. 1,2,3,4,5,6,7,8,10 (default)\n"
                 "  --keep-memory      MIDI files: program changes send the part's timbre and tone (D-20 default)\n"
                 "  --memory-in-unit   MIDI files: the MT-32's memories go into the unit's (D-110 default)\n"
                 "  --rom ROM          MT-32 or CM-32L control ROM: the MT-32's own presets where none fits the unit\n"
                 "  --exact ROM        ...and for every preset\n"
                 "  --stand-ins        the unit's closest presets only, even with --rom\n"
                 "  --roomy-toms       MT-32 toms on the TomTom1 set instead of TomTom2\n"
                 "  --gap MS           pause after each SysEx of the setup (default 20)\n");
}

bool hasExtension(const std::string& path, const char* extension) {
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    const size_t length = std::strlen(extension);
    return lower.size() >= length && lower.compare(lower.size() - length, length, extension) == 0;
}

}  // namespace

int main(int argc, char** argv) {
    PipeSettings settings;
    bool memorySet = false;
    bool standIns = false;
    std::string romPath;
    std::string input;
    std::string output;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--target" && hasValue) {
            const std::string value = argv[++i];
            if (value == "d110") {
                settings.target = Mt32Translator::Target::D110;
            } else if (value == "d20" || value == "d10") {
                settings.target = Mt32Translator::Target::D20;
            } else {
                printUsage();
                return 2;
            }
        } else if (arg == "--unit" && hasValue) {
            settings.unitNumber = std::clamp(std::atoi(argv[++i]), 17, 32);
        } else if (arg == "--channels" && hasValue) {
            int part = 0;
            for (const char* p = argv[++i]; *p != '\0' && part < 9;) {
                const int channel = std::atoi(p);
                settings.unitChannels[part++] = uint8_t(channel >= 1 && channel <= 16 ? channel - 1 : 16);
                while (*p != '\0' && *p != ',') p++;
                if (*p == ',') p++;
            }
        } else if (arg == "--keep-memory") {
            settings.memoryInUnit = false;
            memorySet = true;
        } else if (arg == "--memory-in-unit") {
            settings.memoryInUnit = true;
            memorySet = true;
        } else if ((arg == "--exact" || arg == "--rom") && hasValue) {
            romPath = argv[++i];
            settings.presetMode = arg == "--exact" ? Mt32Translator::PresetMode::Exact : Mt32Translator::PresetMode::Hybrid;
        } else if (arg == "--stand-ins") {
            standIns = true;
        } else if (arg == "--roomy-toms") {
            settings.roomyToms = true;
        } else if (arg == "--gap" && hasValue) {
            settings.sysexGapMs = std::clamp(std::atoi(argv[++i]), 0, 500);
        } else if (arg[0] != '-' && input.empty()) {
            input = arg;
        } else if (arg[0] != '-' && output.empty()) {
            output = arg;
        } else {
            printUsage();
            return 2;
        }
    }
    if (input.empty() || output.empty()) {
        printUsage();
        return 2;
    }
    if (!memorySet) settings.memoryInUnit = settings.target == Mt32Translator::Target::D110;
    if (standIns) settings.presetMode = Mt32Translator::PresetMode::StandIns;

    std::shared_ptr<Mt32Presets> presets;
    if (!romPath.empty()) {
        presets = std::make_shared<Mt32Presets>();
        std::string error;
        if (!loadMt32Presets(Platform::fromUtf8(romPath), *presets, error)) {
            std::fprintf(stderr, "%s: %s\n", romPath.c_str(), error.c_str());
            return 1;
        }
        std::printf("MT-32 presets from %s\n", presets->description.c_str());
    }

    if (hasExtension(input, ".mid") || hasExtension(input, ".midi")) {
        SmfFile in;
        std::string error;
        if (!loadSmfFile(Platform::fromUtf8(input), in, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        SmfFile out;
        const double setup = translateMidiFile(in, out, settings, presets);
        if (!saveSmfFile(Platform::fromUtf8(output), out, 480, 500000, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        std::printf("%s: %zu events -> %zu events (setup %.2f s)\n", output.c_str(), in.events.size(), out.events.size(), setup);
        return 0;
    }

    std::ifstream in(Platform::fromUtf8(input), std::ios::binary);
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.empty()) {
        std::fprintf(stderr, "Cannot read %s\n", input.c_str());
        return 1;
    }
    const std::vector<uint8_t> translated = translateSysexFile(data, settings, presets);
    std::ofstream out(Platform::fromUtf8(output), std::ios::binary);
    out.write(reinterpret_cast<const char*>(translated.data()), std::streamsize(translated.size()));
    if (!out) {
        std::fprintf(stderr, "Cannot write %s\n", output.c_str());
        return 1;
    }
    std::printf("%s: %zu bytes -> %zu bytes\n", output.c_str(), data.size(), translated.size());
    return 0;
}
