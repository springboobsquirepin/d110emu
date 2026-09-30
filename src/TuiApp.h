#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "AppCore.h"
#include "TextScreen.h"
#include "TuiMenus.h"

// The emulator in a terminal (MainTui.cpp): AppCore with a text interface for headless machines and systems without a
// good graphics backend, drawn on a Tui::Screen from the terminal's keys; or with no interface at all (headless), for a
// service, where the log goes to the standard output.
//
// The status screen shows the unit: its LCD, reverb, tune and patch, the MIDI file player, the MIDI inputs, the parts
// (channel, timbre, level, pan, output, controllers, partials, a level meter, the notes), the output level, the
// partials and the log. Menus hold the configuration and the System tab's settings; the settings and the memory are the
// standalone's (the same files), so both front-ends can share them.
class TuiApp : public AppCore {
public:
    struct Options {
        bool headless = false;           // No interface: the log (display messages too) goes to the standard output
        Tui::Capabilities capabilities;  // What the terminal shows (which characters the meters use)
    };

    explicit TuiApp(const Options& options) : tuiOptions_(options) {}

    // Command-line choices, applied as the settings are read (before the synth starts) and kept like a menu's: the ROM
    // folder, the audio device and MIDI inputs (a whole name, or part of one).
    void setCommandLine(const std::string& romFolder, const std::string& audioDevice, const std::vector<std::string>& midiInputs);

    // Once per interface frame: follows the engine, the meters and the test notes, opens MIDI inputs that appear and
    // the audio device again when it stopped and the devices changed.
    void tick();
    void draw(Tui::Screen& screen);
    void onKey(const Tui::Key& key);
    bool quitRequested() const { return quit_; }
    void requestQuit() { quit_ = true; }
    // Ctrl+L: the whole screen should be drawn again (a terminal that lost its contents); taken by the caller.
    bool takeRedrawRequest();
    void log(const std::string& line) { addLog(line); }
    // Log lines to the standard error from now on (after the interface closed: what shutting down reports).
    void echoLog() { echo_ = true; }
    // What runs and where, for the start of the headless mode's output.
    std::string summary() const;

    // For tests: the time the display's timers and meters follow (seconds; negative: a steady clock).
    void setClock(double seconds) { clock_ = seconds; }

protected:
    double now() const override;
    void loadFrontEndSettings() override;
    void onLogAdded(const std::string& line) override;

private:
    using MenuItem = Tui::MenuItem;
    struct TestNote {
        uint8_t channel = 0;
        std::vector<uint8_t> keys;
        double offAt = 0.0;
    };

    // Styles (what the terminal's colours allow).
    Tui::Style style(int foreground, uint8_t attributes = 0) const;  // ANSI 0-15, -1 = the terminal's own
    void setUpLcdStyles();

    // The screens.
    void drawStatus(Tui::Screen& screen);
    void drawTitle(Tui::Screen& screen, const std::string& title);
    void drawLcd(Tui::Screen& screen, int x, int y);
    void drawInfo(Tui::Screen& screen, int x, int y, int width);
    int drawParts(Tui::Screen& screen, int y, int rows);
    void drawOutputMeter(Tui::Screen& screen, int y);
    void drawPartials(Tui::Screen& screen, int y);
    void drawLogLines(Tui::Screen& screen, int y, int rows);
    void drawLogView(Tui::Screen& screen);
    void drawHelp(Tui::Screen& screen);
    // A level from 0 to 1 as a bar `width` cells wide, with the characters the terminal has.
    void drawBar(Tui::Screen& screen, int x, int y, int width, float level, const Tui::Style& filled, const Tui::Style& empty);

    // Keys.
    void onStatusKey(const Tui::Key& key);
    void moveSelection(int step);
    void playTestNote();
    void toggleMute(bool solo);

    // Menus.
    void openOptions();
    void buildOptions(std::vector<MenuItem>& items);
    void openMidiInputs();
    void openRomPlay();
    void openFiles(const std::filesystem::path& folder);
    void openInitializeConfirm();

    // Settings changes, as the standalone's configuration window and System tab make them.
    void chooseAudioDevice(const std::string& name);
    void chooseRom(bool control, int index);
    void setRomFolder(const std::string& folder);
    std::string midiInputsText() const;

    Options tuiOptions_;
    double clock_ = -1.0;
    bool echo_ = false;
    bool quit_ = false;
    bool redraw_ = false;
    bool showLog_ = false;       // Tab: the log instead of the status
    bool showHelp_ = false;
    int logScroll_ = 0;          // Lines up from the newest
    Tui::Menus menus_;           // Open menus and the text input

    // Command line
    std::string commandRomFolder_;
    std::string commandAudioDevice_;
    std::vector<std::string> commandMidiInputs_;
    std::vector<std::string> pendingMidiInputs_;  // Of those, the ones not there yet

    // Meters and timers
    std::array<float, kMaxPartCount> partLevel_{};  // 0-1, falling after the notes
    float outputLeft_ = 0.0f;                       // Peak levels, falling
    float outputRight_ = 0.0f;
    double clipUntil_ = -1.0;                       // Full scale reached: "CLIP" shows until then
    double lastTick_ = -1.0;
    std::vector<std::string> audioDeviceList_;      // At the last try of the audio device
    double nextAudioCheck_ = 0.0;
    std::vector<TestNote> testNotes_;
    std::map<std::string, uint32_t> midiCounts_;    // Messages from each open MIDI input, for its activity light
    std::map<std::string, double> midiActivity_;    // When each last sent something
    uint64_t midiEvents_ = 0;                       // All MIDI received (other programs' connections too)
    double midiEventTime_ = -1.0;
    int partsScroll_ = 0;                           // First part row shown when not all fit

    // LCD colours (the lcd_scheme setting, as the standalone shows it)
    Tui::Style lcd_;
    Tui::Style lcdInverted_;
    Tui::Style lcdBezel_;
};
