#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "TextScreen.h"
#include "TranslatorCore.h"
#include "TuiMenus.h"

// MT32Translator in a terminal (MainTranslatorTui.cpp): TranslatorCore with a text interface, for a translation box
// that runs on a machine without a desktop, or with none at all (headless), as a service, where the log goes to the
// standard output. Its settings are the window's (mt32translator.ini), so both can set it up.
//
// The status screen shows the ports with their activity, the unit, the presets, the tone cache, the timing, the pipe's
// counters and the log; the options menu holds what the window's left column does, and the MT-32 presets' choices.
class TranslatorTui : public TranslatorCore {
public:
    struct Options {
        bool headless = false;           // No interface: the log goes to the standard output
        Tui::Capabilities capabilities;  // What the terminal shows
    };

    // Choices from the command line, applied as the settings are read and kept as the menus keep them: MIDI inputs and
    // outputs by a whole name or part of one (those not there yet are opened when they appear), the unit and its number.
    struct CommandLine {
        std::vector<std::string> inputs;
        std::string unitOutput;
        std::string replyOutput;
        std::string unitInput;
        std::string target;   // "d110" or "d20"
        int unitNumber = 0;   // 17-32; 0: as set
    };

    explicit TranslatorTui(const Options& options) : tuiOptions_(options) {}

    void setCommandLine(const CommandLine& line) { commandLine_ = line; }
    // Once an interface frame: the core's update, the activity lights, the ports named on the command line.
    void tick();
    void draw(Tui::Screen& screen);
    void onKey(const Tui::Key& key);
    // Ctrl+L: the whole screen should be drawn again; taken by the caller.
    bool takeRedrawRequest();
    // Log lines to the standard error from now on (after the interface closed: what shutting down reports).
    void echoLog() { echo_ = true; }
    // What runs and where, for the start of the headless mode's output.
    std::string summary() const;
    // A file named on the command line or chosen: SysEx files (.syx, .dat) go to the unit through the translation.
    void openFile(const std::filesystem::path& path);

    // For tests: the time the activity lights and the port checks follow (seconds; negative: a steady clock).
    void setClock(double seconds) { clock_ = seconds; }

protected:
    double now() const override;
    void loadFrontEndSettings() override;
    void onLogAdded(const std::string& line) override;

private:
    using MenuItem = Tui::MenuItem;

    // The screens.
    void drawStatus(Tui::Screen& screen);
    void drawTitle(Tui::Screen& screen, const std::string& title);
    int drawPorts(Tui::Screen& screen, int y);
    int drawSetup(Tui::Screen& screen, int y);
    void drawCounters(Tui::Screen& screen, int y);
    void drawLogLines(Tui::Screen& screen, int y, int rows);
    void drawLogView(Tui::Screen& screen);
    void drawHelp(Tui::Screen& screen);

    // Keys.
    void onStatusKey(const Tui::Key& key);

    // Menus.
    void openOptions();
    void buildOptions(std::vector<MenuItem>& items);
    void openInputs();
    void openOutputChoice(bool unit);
    void openUnitInputChoice();
    void openChannels();
    void openPresetList();
    void openPreset(int timbre);
    void openSendFile();
    void openTranslateFile();
    void openControlRom();
    void settingsChanged();  // Into the pipe, and saved

    // What the screens say.
    std::string presetName(int timbre) const;    // "Acou Piano 1": the ROM's, else the MT-32's list's
    std::string presetPlays(int timbre) const;   // "a11 AcouPiano1 +1 oct", "MT-32's own"
    std::string presetModeText() const;
    std::string inputsText() const;
    std::string outputText(const MidiOutputPort& port, const std::string& wanted) const;
    std::string unitInputText() const;
    std::filesystem::path startFolder() const;

    Options tuiOptions_;
    CommandLine commandLine_;
    std::vector<std::string> pendingInputs_;  // Named on the command line, not there yet
    std::string pendingUnitOutput_;
    std::string pendingReplyOutput_;
    std::string pendingUnitInput_;
    double clock_ = -1.0;
    bool echo_ = false;
    bool redraw_ = false;
    bool showLog_ = false;
    bool showHelp_ = false;
    int logScroll_ = 0;
    Tui::Menus menus_;
    std::filesystem::path lastFolder_;  // Where the file browser was last

    // Activity lights: when each port last carried something.
    std::map<std::string, uint32_t> inputCounts_;
    std::map<std::string, double> inputActivity_;
    uint64_t ownCount_ = 0;  // Messages that came to the program's own input (all received, less the inputs')
    uint64_t sentCount_ = 0;
    uint64_t replyCount_ = 0;
    uint32_t unitInputCount_ = 0;
    double ownActivity_ = -1.0;
    double sentActivity_ = -1.0;
    double replyActivity_ = -1.0;
    double unitInputActivity_ = -1.0;
};
