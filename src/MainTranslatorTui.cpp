// MT32Translator in a terminal (mt32translator-tui; MT32TranslatorTUI.exe on Windows): the translation box with a text
// interface (TranslatorTui), or with none (--headless), for a machine that sits between an MT-32 program and a real
// D-110, D-10 or D-20 as a service. It keeps its settings in the files the window uses.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "MidiInput.h"
#include "MidiOutput.h"
#include "Platform.h"
#include "Terminal.h"
#include "TranslatorTui.h"
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
    "Usage: mt32translator-tui [options] [file.syx ...]\n"
    "\n"
    "MT32Translator in a terminal: MIDI for an MT-32 in, MIDI for a real D-110, D-10 or D-20 out. SysEx files named\n"
    "here (.syx, or a game's .dat) go to the unit through the translation at the start.\n"
    "\n"
    "Options:\n"
    "  --headless          No interface: runs until stopped (Ctrl+C, or SIGTERM from a service manager), printing\n"
    "                      the log\n"
    "  --config DIR        The folder of mt32translator.ini and mt32translator-cache.syx (default:\n"
    "                      " D110EMU_DATA_FOLDER ", which the window uses too; a roms folder there\n"
    "                      is searched for an MT-32 control ROM)\n"
    "  --in NAME           Translate what this MIDI input receives (a whole name or part of one); repeatable\n"
    "  --out NAME          The MIDI output to the unit\n"
    "  --replies NAME      The MIDI output back to the program, for handshake transfers (\"none\": no replies)\n"
    "  --unit-in NAME      The MIDI input from the unit's MIDI OUT (\"none\": none)\n"
    "  --unit d110|d20     The unit: a D-110, or a D-10 or D-20 in multi-timbral mode\n"
    "  --unit-number N     The unit number set on the unit (17-32)\n"
    "                      (the options above are remembered, as the options menu keeps them; ports that are not\n"
    "                      there yet are opened when they appear)\n"
    "  --list              List the MIDI inputs and outputs, then quit\n"
    "  --ascii             Plain ASCII: no line, block or symbol characters\n"
    "  --no-color          No colours (so does the NO_COLOR environment variable)\n"
    "  --version           What this is built from\n"
    "  --help              This text\n";

struct CommandLine {
    bool headless = false;
    bool list = false;
    bool ascii = false;
    bool noColor = false;
    bool help = false;
    bool version = false;
    std::string configFolder;
    TranslatorTui::CommandLine ports;
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
        } else if (arg == "--help" || arg == "-h") {
            line.help = true;
        } else if (arg == "--version") {
            line.version = true;
        } else if (arg == "--config") {
            value(line.configFolder);
        } else if (arg == "--in") {
            std::string name;
            value(name);
            if (!name.empty()) line.ports.inputs.push_back(name);
        } else if (arg == "--out") {
            value(line.ports.unitOutput);
        } else if (arg == "--replies") {
            value(line.ports.replyOutput);
        } else if (arg == "--unit-in") {
            value(line.ports.unitInput);
        } else if (arg == "--unit") {
            value(line.ports.target);
            if (line.error.empty() && line.ports.target != "d110" && line.ports.target != "d20") line.error = "--unit wants d110 or d20";
        } else if (arg == "--unit-number") {
            std::string number;
            value(number);
            line.ports.unitNumber = std::atoi(number.c_str());
            if (line.error.empty() && (line.ports.unitNumber < 17 || line.ports.unitNumber > 32)) line.error = "--unit-number wants 17-32";
        } else if (arg.size() > 1 && arg[0] == '-') {
            line.error = "Unknown option " + arg;
        } else {
            line.files.push_back(arg);
        }
        if (!line.error.empty()) break;
    }
    return line;
}

// Where the settings and the tone cache are: --config's folder, else the user's D110Emu folder
// (Platform::appDataDirectory), which the window uses too.
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

int listPorts() {
    setMidiProgramName("MT32Translator");
    ListSink sink;
    MidiInputManager midi(sink, true);
    std::printf("MIDI inputs:\n");
    if (!midi.systemError().empty()) std::printf("  (%s)\n", midi.systemError().c_str());
    const std::vector<std::string> inputs = midi.listPorts();
    if (inputs.empty() && midi.systemError().empty()) std::printf("  (none)\n");
    for (const std::string& name : inputs) std::printf("  %s\n", name.c_str());
    std::printf("MIDI outputs:\n");
    if (!MidiOutputPort::systemError().empty()) std::printf("  (%s)\n", MidiOutputPort::systemError().c_str());
    const std::vector<std::string> outputs = MidiOutputPort::listPorts();
    if (outputs.empty() && MidiOutputPort::systemError().empty()) std::printf("  (none)\n");
    for (const std::string& name : outputs) std::printf("  %s\n", name.c_str());
    if (!midi.ownPortName().empty()) std::printf("Programs can send MIDI to %s while MT32Translator runs.\n", midi.ownPortName().c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const CommandLine line = parseCommandLine(Tui::commandLineArguments(argc, argv));
    if (!line.error.empty()) {
        std::fprintf(stderr, "mt32translator-tui: %s\n\n%s", line.error.c_str(), kUsage);
        return 2;
    }
    if (line.help) {
        std::printf("%s", kUsage);
        return 0;
    }
    if (line.version) {
        std::printf("MT32Translator - MT-32 MIDI for the D-110, D-10 and D-20, terminal interface\n");
        std::printf("MT-32 presets from its control ROM: Munt mt32emu's ROM identification (LGPL 2.1+)\n");
        return 0;
    }
    if (line.list) return listPorts();

    Tui::catchQuitSignals();

    const std::filesystem::path folder = configFolder(line.configFolder);
    if (line.configFolder.empty()) {
        // Earlier versions (Windows) kept the files next to the program.
        Platform::adoptOldFiles(folder, {Platform::programDirectory()}, "mt32translator.ini", {"mt32translator-cache.syx"});
    }
    TranslatorOptions options;
    options.settingsFile = folder / "mt32translator.ini";
    options.cacheFile = folder / "mt32translator-cache.syx";
    std::error_code ec;
    options.romSearchDirs = {Platform::executableDirectory(), std::filesystem::current_path(ec), folder};

    TranslatorTui::Options tuiOptions;
    tuiOptions.headless = line.headless;
    Tui::Terminal terminal;
    if (!line.headless) {
        std::string error;
        if (!terminal.open(error)) {
            std::fprintf(stderr, "mt32translator-tui: %s. Without a terminal, run it with --headless.\n", error.c_str());
            return 1;
        }
        tuiOptions.capabilities = terminal.capabilities();
        if (line.ascii) tuiOptions.capabilities.glyphs = Tui::Capabilities::Glyphs::Ascii;
        if (line.noColor) tuiOptions.capabilities.colors = Tui::Capabilities::Colors::None;
    }

    TranslatorTui app(tuiOptions);
    app.setCommandLine(line.ports);
    Tui::ErrorCapture errors;
    Tui::Encoder encoder(tuiOptions.capabilities);
    Tui::Screen screen;
    if (!line.headless) errors.start();
    app.init(options);
    if (line.headless) std::printf("MT32Translator, headless\n%s\n", app.summary().c_str());
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
