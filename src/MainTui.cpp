// D110Emu in a terminal (d110emu-tui; D110EmuTUI.exe on Windows): the emulator with a text interface (TuiApp), or with
// none (--headless) for a service. It keeps its settings and memory in the files the standalone uses.

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "AudioOutput.h"
#include "MidiInput.h"
#include "Platform.h"
#include "Terminal.h"
#include "TuiApp.h"
#include "TuiMain.h"

namespace {

// The user's D110Emu folder (Platform::appDataDirectory), for the usage.
#if defined(_WIN32)
#define D110EMU_DATA_FOLDER "%APPDATA%\\D110Emu"
#elif defined(__APPLE__)
#define D110EMU_DATA_FOLDER "~/Library/Application Support/D110Emu"
#else
#define D110EMU_DATA_FOLDER "$XDG_DATA_HOME/D110Emu, else ~/.local/share/D110Emu"
#endif

const char* const kUsage =
    "Usage: d110emu-tui [options] [file.mid | file.syx ...]\n"
    "\n"
    "D110Emu, the Roland D-110 emulator, in a terminal. MIDI files play and SysEx files load at the start.\n"
    "\n"
    "Options:\n"
    "  --headless           No interface: runs until stopped (Ctrl+C, or SIGTERM from a service manager),\n"
    "                       printing the log and the display's SysEx messages\n"
    "  --config DIR         The folder of d110emu.ini, d110emu-memory.syx and d110emu-reverb.ini (default:\n"
    "                       " D110EMU_DATA_FOLDER ", which the window uses too; a roms folder there is found)\n"
    "  --roms DIR           The ROM folder\n"
    "  --audio-device NAME  The audio output (a whole name or part of one; \"default\": the system's)\n"
    "  --midi-in NAME       Play from this MIDI input (a whole name or part of one); repeatable\n"
    "                       (--roms, --audio-device and --midi-in are remembered, as the options menu keeps them)\n"
    "  --list               List the audio devices and MIDI inputs, then quit\n"
    "  --ascii              Plain ASCII: no line, block or symbol characters\n"
    "  --no-color           No colours (so does the NO_COLOR environment variable)\n"
    "  --no-audio           No sound: time passes, nothing is heard (tests)\n"
    "  --version            What this is built from\n"
    "  --help               This text\n";

struct CommandLine {
    bool headless = false;
    bool list = false;
    bool ascii = false;
    bool noColor = false;
    bool noAudio = false;
    bool help = false;
    bool version = false;
    std::string configFolder;
    std::string romFolder;
    std::string audioDevice;
    std::vector<std::string> midiInputs;
    std::vector<std::string> files;
    std::string error;
};

CommandLine parseCommandLine(const std::vector<std::string>& args) {
    CommandLine line;
    for (size_t i = 0; i < args.size(); i++) {
        const std::string& arg = args[i];
        auto value = [&](std::string& target) {
            if (i + 1 >= args.size()) {
                line.error = arg + " needs a value";
                return;
            }
            target = args[++i];
        };
        if (arg == "--headless") {
            line.headless = true;
        } else if (arg == "--list") {
            line.list = true;
        } else if (arg == "--ascii") {
            line.ascii = true;
        } else if (arg == "--no-color" || arg == "--no-colour") {
            line.noColor = true;
        } else if (arg == "--no-audio") {
            line.noAudio = true;
        } else if (arg == "--help" || arg == "-h") {
            line.help = true;
        } else if (arg == "--version") {
            line.version = true;
        } else if (arg == "--config") {
            value(line.configFolder);
        } else if (arg == "--roms") {
            value(line.romFolder);
        } else if (arg == "--audio-device") {
            value(line.audioDevice);
        } else if (arg == "--midi-in") {
            std::string name;
            value(name);
            if (!name.empty()) line.midiInputs.push_back(name);
        } else if (arg.size() > 1 && arg[0] == '-') {
            line.error = "Unknown option " + arg;
        } else {
            line.files.push_back(arg);
        }
        if (!line.error.empty()) break;
    }
    return line;
}

// Where the settings, the memory and the reverb tuning are: --config's folder, else the user's D110Emu folder
// (Platform::appDataDirectory), which the standalone uses too.
std::filesystem::path configFolder(const std::string& chosen) {
    if (chosen.empty()) return Platform::appDataDirectory("D110Emu");
    std::error_code ec;
    const std::filesystem::path folder = Platform::fromUtf8(chosen);
    std::filesystem::create_directories(folder, ec);
    return folder;
}

class ListSink : public MidiInputSink {
public:
    void onMidiShortMessage(uint32_t /*message*/) override {}
    void onMidiSysex(const uint8_t* /*data*/, size_t /*length*/) override {}
};

int listDevices() {
    AudioOutput audio;
    std::printf("Audio devices (%s):\n", audio.backendName().c_str());
    std::printf("  default (the system's)\n");
    for (const std::string& name : audio.listDevices()) std::printf("  %s\n", name.c_str());
    ListSink sink;
    MidiInputManager midi(sink);
    std::printf("MIDI inputs:\n");
    if (!midi.systemError().empty()) std::printf("  (%s)\n", midi.systemError().c_str());
    const std::vector<std::string> ports = midi.listPorts();
    if (ports.empty() && midi.systemError().empty()) std::printf("  (none)\n");
    for (const std::string& name : ports) std::printf("  %s\n", name.c_str());
    if (!midi.ownPortName().empty()) std::printf("Programs can send MIDI to %s while D110Emu runs.\n", midi.ownPortName().c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const CommandLine line = parseCommandLine(Tui::commandLineArguments(argc, argv));
    if (!line.error.empty()) {
        std::fprintf(stderr, "d110emu-tui: %s\n\n%s", line.error.c_str(), kUsage);
        return 2;
    }
    if (line.help) {
        std::printf("%s", kUsage);
        return 0;
    }
    if (line.version) {
        std::printf("D110Emu - Roland D-110 emulator, terminal interface\n");
        std::printf("Emulation: Munt mt32emu %s (LGPL 2.1+) with D-110 ROM support\n", MT32Emu::Synth::getLibraryVersionString());
        std::printf("Audio: miniaudio (public domain / MIT-0)\n");
        return 0;
    }
    if (line.list) return listDevices();

    Tui::catchQuitSignals();

    AppOptions options;
    useStandaloneFolder(options, configFolder(line.configFolder), line.configFolder.empty());
    options.nullAudio = line.noAudio;

    TuiApp::Options tuiOptions;
    tuiOptions.headless = line.headless;
    Tui::Terminal terminal;
    if (!line.headless) {
        std::string error;
        if (!terminal.open(error)) {
            std::fprintf(stderr, "d110emu-tui: %s. Without a terminal, run it with --headless.\n", error.c_str());
            return 1;
        }
        tuiOptions.capabilities = terminal.capabilities();
        if (line.ascii) tuiOptions.capabilities.glyphs = Tui::Capabilities::Glyphs::Ascii;
        if (line.noColor) tuiOptions.capabilities.colors = Tui::Capabilities::Colors::None;
    }

    TuiApp app(tuiOptions);
    app.setCommandLine(line.romFolder, line.audioDevice, line.midiInputs);
    Tui::ErrorCapture errors;
    Tui::Encoder encoder(tuiOptions.capabilities);
    Tui::Screen screen;
    if (!line.headless) {
        // Loading the ROMs takes a moment.
        int columns = 80;
        int rows = 24;
        terminal.size(columns, rows);
        screen.resize(columns, rows);
        screen.text(2, 1, "D110Emu is starting...", Tui::Style());
        terminal.write(encoder.frame(screen));
        errors.start();
    }
    app.init(options);
    if (line.headless) std::printf("D110Emu, headless\n%s\n", app.summary().c_str());
    for (const std::string& file : line.files) app.openFile(Platform::fromUtf8(file));

    if (line.headless) {
        while (!Tui::quitSignalled()) {
            app.tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::printf("Stopping\n");
        app.shutdown();
        return 0;
    }

    std::vector<Tui::Key> keys;
    while (!app.quitRequested() && !Tui::quitSignalled() && !terminal.lost()) {
        keys.clear();
        terminal.readKeys(33, keys);  // About 30 frames a second
        for (const Tui::Key& key : keys) app.onKey(key);
        for (const std::string& error : errors.take()) app.log(error);
        app.tick();
        int columns = 80;
        int rows = 24;
        terminal.size(columns, rows);
        if (columns != screen.columns() || rows != screen.rows()) screen.resize(columns, rows);
        if (app.takeRedrawRequest()) encoder.reset();
        app.draw(screen);
        terminal.write(encoder.frame(screen));
    }
    errors.stop();
    terminal.close();
    app.echoLog();  // What shutting down reports reaches the terminal
    app.shutdown();
    return 0;
}
