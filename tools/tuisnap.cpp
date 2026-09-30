// The terminal interface (TuiApp) without a terminal: the unit runs with no audio or MIDI device, keys are typed at given
// times, and the screen is printed as text; --vt writes what a terminal would receive for the last frame (its escape
// sequences), to check with a terminal emulator. --translator: MT32Translator's terminal version (TranslatorTui) instead,
// its pipe running without MIDI ports.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "Platform.h"
#include "TextScreen.h"
#include "TranslatorTui.h"
#include "TuiApp.h"

namespace {

const char* const kUsage =
    "Usage: tuisnap [options] [out.txt]\n"
    "  --seconds S                  How long it runs (default 2)\n"
    "  --size WxH                   The terminal's size (default 80x24)\n"
    "  --key T:KEYS                 Types KEYS at T seconds (repeatable): characters, and {up} {down} {left} {right}\n"
    "                               {enter} {esc} {tab} {space} {bksp} {del} {home} {end} {pgup} {pgdn} {f1}-{f12}\n"
    "                               {ctrl-c} {ctrl-l}\n"
    "  --glyphs unicode|console|ascii, --colors none|16|256|true (default: unicode, true)\n"
    "  --midi FILE, --syx FILE (repeatable), --rom-song N, --mt32, --sixteen, --performance, --nice-panning\n"
    "  --memory FILE                The memory file: loaded, and saved at the end\n"
    "  --setting \"key = value\"      A line for the settings file (repeatable)\n"
    "  --vt FILE                    The last frame as the terminal receives it, drawn whole\n"
    "  --translator                 MT32Translator's terminal version instead (--syx files go through its pipe;\n"
    "                               --setting lines go to its settings file)\n";

struct KeyEvent {
    double time = 0.0;
    std::vector<Tui::Key> keys;
};

bool parseKeys(const std::string& text, std::vector<Tui::Key>& keys, std::string& error) {
    static const std::map<std::string, Tui::Key::Code> kNamed = {
        {"up", Tui::Key::Up},       {"down", Tui::Key::Down},     {"left", Tui::Key::Left},     {"right", Tui::Key::Right},
        {"enter", Tui::Key::Enter}, {"esc", Tui::Key::Escape},    {"tab", Tui::Key::Tab},       {"bksp", Tui::Key::Backspace},
        {"del", Tui::Key::Delete},  {"home", Tui::Key::Home},     {"end", Tui::Key::End},       {"pgup", Tui::Key::PageUp},
        {"pgdn", Tui::Key::PageDown}, {"f1", Tui::Key::F1},       {"f2", Tui::Key::F2},         {"f3", Tui::Key::F3},
        {"f4", Tui::Key::F4},       {"f5", Tui::Key::F5},         {"f6", Tui::Key::F6},         {"f7", Tui::Key::F7},
        {"f8", Tui::Key::F8},       {"f9", Tui::Key::F9},         {"f10", Tui::Key::F10},       {"f11", Tui::Key::F11},
        {"f12", Tui::Key::F12},
    };
    for (size_t i = 0; i < text.size();) {
        if (text[i] != '{') {
            keys.push_back(Tui::Key::character(Tui::decodeUtf8(text, i)));
            continue;
        }
        const size_t end = text.find('}', i);
        if (end == std::string::npos) {
            error = "no } in " + text;
            return false;
        }
        const std::string name = text.substr(i + 1, end - i - 1);
        i = end + 1;
        if (name == "space") {
            keys.push_back(Tui::Key::character(U' '));
        } else if (name.rfind("ctrl-", 0) == 0 && name.size() == 6) {
            Tui::Key key = Tui::Key::character(char32_t(name[5]));
            key.ctrl = true;
            keys.push_back(key);
        } else if (kNamed.count(name) != 0) {
            keys.push_back(Tui::Key::of(kNamed.at(name)));
        } else {
            error = "unknown key {" + name + "}";
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    double seconds = 2.0;
    int columns = 80;
    int rows = 24;
    std::vector<KeyEvent> keyEvents;
    Tui::Capabilities caps;
    caps.colors = Tui::Capabilities::Colors::TrueColor;
    std::vector<std::filesystem::path> sysexFiles;
    std::filesystem::path midiFile;
    std::filesystem::path memoryFile;
    std::filesystem::path vtFile;
    std::filesystem::path output;
    std::vector<std::string> settingLines;
    int romSong = -1;
    bool translator = false;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n%s", arg.c_str(), kUsage);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--seconds") {
            seconds = std::atof(next().c_str());
        } else if (arg == "--size") {
            const std::string size = next();
            if (std::sscanf(size.c_str(), "%dx%d", &columns, &rows) != 2) {
                std::fprintf(stderr, "--size wants WxH\n");
                return 2;
            }
        } else if (arg == "--key") {
            const std::string value = next();
            const size_t colon = value.find(':');
            KeyEvent event;
            std::string error;
            if (colon == std::string::npos || !parseKeys(value.substr(colon + 1), event.keys, error)) {
                std::fprintf(stderr, "--key %s: %s\n", value.c_str(), error.empty() ? "wants T:KEYS" : error.c_str());
                return 2;
            }
            event.time = std::atof(value.substr(0, colon).c_str());
            keyEvents.push_back(event);
        } else if (arg == "--glyphs") {
            const std::string value = next();
            caps.glyphs = value == "ascii" ? Tui::Capabilities::Glyphs::Ascii
                        : value == "console" ? Tui::Capabilities::Glyphs::Console
                                             : Tui::Capabilities::Glyphs::Unicode;
            caps.boldBright = value == "console";
        } else if (arg == "--colors") {
            const std::string value = next();
            caps.colors = value == "none" ? Tui::Capabilities::Colors::None
                        : value == "16"   ? Tui::Capabilities::Colors::Ansi16
                        : value == "256"  ? Tui::Capabilities::Colors::Ansi256
                                          : Tui::Capabilities::Colors::TrueColor;
        } else if (arg == "--midi") {
            midiFile = next();
        } else if (arg == "--syx") {
            sysexFiles.push_back(next());
        } else if (arg == "--rom-song") {
            romSong = std::atoi(next().c_str());
        } else if (arg == "--mt32") {
            settingLines.push_back("mt32_translation = true");
        } else if (arg == "--sixteen") {
            settingLines.push_back("sixteen_parts = true");
            settingLines.push_back("partials = 512");
        } else if (arg == "--performance") {
            settingLines.push_back("performance_mode = true");
        } else if (arg == "--nice-panning") {
            settingLines.push_back("nice_panning = true");
        } else if (arg == "--memory") {
            memoryFile = next();
        } else if (arg == "--setting") {
            settingLines.push_back(next());
        } else if (arg == "--vt") {
            vtFile = next();
        } else if (arg == "--translator") {
            translator = true;
        } else if (arg == "--help") {
            std::printf("%s", kUsage);
            return 0;
        } else if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "Unknown option %s\n%s", arg.c_str(), kUsage);
            return 2;
        } else {
            output = arg;
        }
    }

    auto writeScreen = [&](const Tui::Screen& screen) {
        const std::string text = screen.plainText();
        if (output.empty()) {
            std::printf("%s", text.c_str());
        } else {
            std::ofstream out(output, std::ios::binary);
            out << text;
        }
        if (!vtFile.empty()) {
            Tui::Encoder encoder(caps);
            std::ofstream out(vtFile, std::ios::binary);
            out << encoder.frame(screen);
        }
    };

    if (translator) {
        // MT32Translator: its pipe runs without ports; SysEx files go through it; a throwaway settings file.
        const std::filesystem::path settingsFile = std::filesystem::temp_directory_path() / "mt32translator-tuisnap.ini";
        std::filesystem::remove(settingsFile);
        {
            std::ofstream seed(settingsFile);
            for (const std::string& line : settingLines) seed << line << "\n";
        }
        TranslatorOptions options;
        options.settingsFile = settingsFile;
        options.romSearchDirs = {Platform::executableDirectory(), std::filesystem::current_path()};
        options.enableMidi = false;
        TranslatorTui::Options tuiOptions;
        tuiOptions.capabilities = caps;
        TranslatorTui app(tuiOptions);
        app.setClock(0.0);
        app.init(options);
        for (const std::filesystem::path& file : sysexFiles) app.openFile(file);
        Tui::Screen screen;
        screen.resize(columns, rows);
        std::vector<bool> typed(keyEvents.size(), false);
        MidiPipe::Clock::time_point virtualTime = MidiPipe::Clock::now() + std::chrono::hours(1);
        const int frames = std::max(2, int(seconds * 30.0));
        for (int frame = 0; frame < frames; frame++) {
            const double time = frame / 30.0;
            app.setClock(time);
            for (size_t k = 0; k < keyEvents.size(); k++) {
                if (typed[k] || keyEvents[k].time > time) continue;
                typed[k] = true;
                for (const Tui::Key& key : keyEvents[k].keys) app.onKey(key);
            }
            // Everything queued is sent at once, on a clock of its own (the pipe's thread then has nothing due).
            for (MidiPipe::Clock::time_point due = virtualTime; due != MidiPipe::Clock::time_point::max(); due = app.pipe().process(due)) {
                virtualTime = std::max(virtualTime, due);
            }
            app.tick();
            app.draw(screen);
            if (app.quitRequested()) break;
        }
        writeScreen(screen);
        const MidiPipe::Stats stats = app.pipe().stats();
        std::printf("pipe: received %llu, sent %llu (%llu SysEx bytes)\n", static_cast<unsigned long long>(stats.received),
                    static_cast<unsigned long long>(stats.sent), static_cast<unsigned long long>(stats.sysexBytesSent));
        if (app.quitRequested()) std::printf("quit requested\n");
        app.shutdown();
        std::ifstream saved(settingsFile);
        for (std::string line; std::getline(saved, line);) {
            if (line.rfind("target", 0) == 0 || line.rfind("unit_", 0) == 0 || line.rfind("preset_", 0) == 0 || line.rfind("memory_in_unit", 0) == 0 ||
                line.rfind("cache_", 0) == 0 || line.rfind("sysex_gap_ms", 0) == 0) {
                std::printf("saved: %s\n", line.c_str());
            }
        }
        std::filesystem::remove(settingsFile);
        return 0;
    }

    // A throwaway settings file, with the settings asked for.
    const std::filesystem::path settingsFile = std::filesystem::temp_directory_path() / "d110emu-tuisnap.ini";
    std::filesystem::remove(settingsFile);
    {
        std::ofstream seed(settingsFile);
        for (const std::string& line : settingLines) seed << line << "\n";
    }

    AppOptions options;
    options.settingsFile = settingsFile;
    options.memoryFile = memoryFile;
    options.romSearchDirs = {Platform::executableDirectory(), std::filesystem::current_path()};
    options.enableAudio = false;
    options.enableMidiInput = false;
    options.outputSampleRate = 48000;

    TuiApp::Options tuiOptions;
    tuiOptions.capabilities = caps;
    TuiApp app(tuiOptions);
    app.setClock(0.0);
    app.init(options);
    for (const std::filesystem::path& file : sysexFiles) app.openFile(file);
    if (!midiFile.empty()) app.openFile(midiFile);
    if (romSong >= 0) app.playRomSong(romSong - 1);

    // 30 frames a second, the synth advanced 1600 frames at 48 kHz before each.
    Tui::Screen screen;
    screen.resize(columns, rows);
    std::vector<float> audio(2 * 1600);
    std::vector<bool> typed(keyEvents.size(), false);
    const int frames = std::max(2, int(seconds * 30.0));
    for (int frame = 0; frame < frames; frame++) {
        const double time = frame / 30.0;
        app.engine().render(audio.data(), 1600);
        app.measurePeaks(audio.data(), 1600, false);
        app.setClock(time);
        for (size_t k = 0; k < keyEvents.size(); k++) {
            if (typed[k] || keyEvents[k].time > time) continue;
            typed[k] = true;
            for (const Tui::Key& key : keyEvents[k].keys) app.onKey(key);
        }
        app.tick();
        app.draw(screen);
        if (app.quitRequested()) break;
    }

    writeScreen(screen);
    const std::string lcd = app.lcdText();
    std::string plain;
    std::string inverted;
    for (unsigned char c : lcd) {
        plain += char(c & 0x7F);
        inverted += (c & 0x80) != 0 ? '^' : ' ';
    }
    std::printf("lcd: |%s|%s|\n", plain.substr(0, 16).c_str(), plain.substr(16).c_str());
    if (inverted.find('^') != std::string::npos) std::printf("inv: |%s|%s|\n", inverted.substr(0, 16).c_str(), inverted.substr(16).c_str());
    if (app.quitRequested()) std::printf("quit requested\n");
    app.shutdown();
    std::filesystem::remove(settingsFile);
    return 0;
}
