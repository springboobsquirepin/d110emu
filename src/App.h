#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "AppCore.h"
#include "Lcd.h"
#include "Platform.h"
#include "ToneEditor.h"
#include "ToneModel.h"

// The emulator's Dear ImGui interface on AppCore (the engine, audio output, MIDI inputs, settings and modes): the
// standalone's window and the plugins' editor. The platform layer (MainWin32.cpp, the plugins' editor) creates the
// window and calls frame().
class App : public AppCore {
public:
    App();
    ~App() override;

    // Colours and spacing shared by all front-ends; call before scaling the style for DPI.
    static void applyStyle();

    void init(const AppOptions& options) override;
    void frame();  // Builds the UI; call between ImGui::NewFrame() and ImGui::Render()
    bool quitRequested() const { return quitRequested_; }

    // Computer-keyboard piano input by physical key (PC scancode set 1, 0x01-0xFF; extended keys are ignored),
    // so that the note layout is the same on QWERTY, QWERTZ and AZERTY keyboards. The window layer calls this
    // for every key press and release, and clearKeys() when the window loses the keyboard focus.
    void onKey(int scancode, bool down);
    void clearKeys();

    // View > Window size (the standalone's): the window and its text in percent, on top of the display's scaling. The
    // window host follows it (HostedApp::uiZoom); the plugins' editors have their own.
    int uiZoom() const { return windowZoom_; }

    // For tools and tests: switch tabs, open windows.
    void selectTab(const std::string& name);
    void showReverbTuning(bool on) { showReverbTuning_ = on; }
    void showMt32Presets(bool on) { showMt32Presets_ = on; }
    void showConfiguration(bool on) { showConfig_ = on; }
    // The Custom LCD colours window (opening it selects the user's colours, which it edits).
    void showLcdColourEditor(bool on);

protected:
    double now() const override;  // Dear ImGui's time, which the tools advance frame by frame
    void loadFrontEndSettings() override;
    void saveFrontEndSettings() override;
    void onLogAdded(const std::string& line) override;

private:
    void browseMidiFile();
    void browseSysexFile();
    void saveMemoryAs();
    // The file dialogs: `then` runs with the path chosen, at once where the dialog blocks (Windows, the SDL window), or
    // in a later frame where it runs beside the App (the Linux plugin's zenity or kdialog, so that the host's windows
    // keep running meanwhile); drawDialogWait() waits for it, blocking the App's window as a modal dialog would.
    using PathAction = std::function<void(const std::filesystem::path&)>;
    void openFileThen(Platform::FileKind kind, PathAction then);
    void saveFileThen(Platform::FileKind kind, const std::string& defaultName, PathAction then);
    void pickFolderThen(const std::filesystem::path& initialFolder, PathAction then);
    void drawDialogWait();

    void drawMenuBar();
    void drawStatusBar();
    // The configuration window (File > Configuration...): ROMs, audio output and MIDI input.
    void drawConfigWindow();
    void drawRomPanel();
    void drawAudioPanel();
    void drawAudioDeviceSettings(float comboWidth, bool& restartAudio);  // Device, sample rate and buffer (not the plugin's)
    void drawMultiOutputChoice(float comboWidth);  // MULTI 1-6 as six mono outputs or three stereo pairs
    // An output assign (0-7) to choose, offering the MULTI pairs instead of Multi 1-6 while they are in use.
    bool outputAssignCombo(const char* id, int& assign, bool compact);
    void drawMidiPanel();
    void drawMidiPorts();  // The MIDI input ports with their activity lights (not the plugin's)
    // The System tab: sound, MIDI (extensions, MT-32 translation) and system settings.
    void drawSystemTab();
    void drawSoundPanel();
    void drawMidiOptionsPanel();
    void drawSystemPanel();
    void drawPlayerPanel();
    void drawPartsPanel();
    // A part's Output menu: the unit's output assign and, in the plugins, the part's own output (the rhythm part:
    // its keys' outputs or its own). `compact`: the Play tab's short names.
    void drawOutputCombo(int part, const char* id, float width, bool compact);
    void setOwnOutput(int part, bool own);
    // Where MULTI 1-6 play (the plugins' outputs, 7.1 surround's speakers, or the mix), for the Output menus' tooltips.
    std::string multiOutputsText() const;
    std::string multiChoicesText() const;  // "Multi 1-6", or the pairs while they are in use
    // Whether the host has switched off the plugin output of MULTI 1-6 (`output` 2-7 = output assign), or of `part`'s own.
    bool hostOutputOff(int output) const;
    bool hostPartOutputOff(int part) const;
    void drawPartialsPanel();
    // The keyboard: its channel, velocity and program change, and the keys, `pianoHeight` tall.
    void drawKeyboardPanel(float pianoHeight);
    // A tab's content, in a region of its own under the tab bar that scrolls when the content is taller, so the LCD and
    // the tabs stay in view; with the keyboard docked under it where `keyboard` is set and View > Keyboard is on.
    void drawTabContent(const char* id, bool keyboard, const std::function<void()>& content);
    void drawLogPanel();
    void drawReverbTuningPanel();
    void playReverbTest();
    void drawAboutPopup();
    void handleComputerKeyboard();

    // D-110 panel: LCD, part/patch/rhythm editing, ROM Play
    void drawLcdPanel();
    // The LCD's colours: a preset (Lcd::schemeName) or the user's own, which the Custom LCD colours window edits.
    Lcd::Colors lcdColors() const;
    void drawLcdColourItems();  // The LCD colours menu's items (View menu, the display's right-click menu)
    void useCustomLcdColours();  // Selects the user's colours, the first time as a copy of the preset in use
    void drawLcdColourEditor();
    void drawPartsEditor();
    void drawPatchesTab();
    void drawPerformanceTab();
    void drawRhythmTab();
    // D-20 rhythm patterns and rhythm track.
    void drawPatternsTab();
    void exportD20(int what);
    void loadD20Presets();
    // Groups 0-3 are a, b, i and r; group 4 the memory card's tones (c11-c88), offered when withCard is set and a card is
    // inserted; groups 5 and 6 the tone banks d and e (d11-e88), offered when withAlt is set and the engine has them.
    bool drawTonePicker(const char* id, uint8_t& group, uint8_t& number, const char* currentName = nullptr, bool withCard = false,
                        bool withAlt = false);
    // Memory card (a SysEx file of the card areas), saved on eject, restart and exit like a battery-backed card.
    void newCard();
    void insertCard();
    void ejectCard();
    void drawMt32PresetsWindow();
    void exportRomSongs(int song);

    // Tone editor (Tone tab): edits the tone temporary area of the part the Parts tab edits, in real time.
    void drawToneTab();
    void syncToneEditor();
    void writeEditedTone();
    void loadToneFile();
    void loadToneFile(const std::filesystem::path& path);
    void saveToneFile();
    void drawToneFilePicker();
    // Timbre memory editor (Timbres tab).
    void drawTimbresTab();
    void writeTimbreBytes(int timbre, int offset, const uint8_t* data, int length);
    // The channel the computer keyboard plays: the edited part's while the Tone tab is shown, the performance channel while
    // the Performance tab is (in performance mode), else the keyboard's.
    uint8_t pianoChannel() const;
    // What the tone editor plays and sends through: the engine, on the edited part's channel.
    struct ToneHost : ToneEditor::Host {
        explicit ToneHost(App& owner) : app(owner) {}
        void writeTone(int offset, const uint8_t* data, int length) override;
        void noteOn(int key, int velocity) override;
        void noteOff(int key) override;
        void restrike() override;
        void soundingNotes(std::array<bool, 128>& notes) override;
        App& app;
        uint8_t channel = kChannelOff;  // The edited part's channel
    };

    bool showMt32Presets_ = false;             // The MT-32 presets window
    int writePerformanceTarget_ = 0;
    std::string performanceNameEdit_;
    bool performanceNameActive_ = false;

    // MIDI input activity
    std::map<std::string, uint32_t> midiCounts_;
    std::map<std::string, double> midiActivity_;  // Time of the last message, for the activity LEDs

    // On-screen and computer keyboard
    int keyboardChannel_ = 0;
    int velocity_ = 100;
    int keyboardOctave_ = 4;  // Octave of the lower row of the computer keyboard (C4 = MIDI 60)
    int program_ = 1;
    bool showKeyboard_ = true;        // View > Keyboard: the keyboard in the Play and Performance tabs, the Tone tab's keys
    bool showPartials_ = true;        // View > Partials: the Play tab's partial display
    bool keyboardDrawn_ = false;      // This frame
    // Measured at the last frame, to share the next one's room: the keyboard less its keys, and each tab's content.
    float keyboardControlsHeight_ = 0.0f;
    std::map<std::string, float> keyboardContentHeights_;
    int mouseNote_ = -1;
    uint8_t mouseChannel_ = 0;        // The channel of the note the mouse holds
    std::array<bool, 128> keyHeld_ = {};  // Notes held on the computer keyboard
    std::array<bool, 256> scancodeDown_ = {};  // Physical keys currently down (from onKey)

    // The LCD's colours
    int lcdScheme_ = 0;         // Lcd::schemeName(), or -1 for the user's own colours (lcdCustom_)
    // The user's own LCD colours (glass, unlit dots, lit dots; RGB 0-1), and as they were when the editor opened.
    std::array<std::array<float, 3>, 3> lcdCustom_{};
    std::array<std::array<float, 3>, 3> lcdCustomOpened_{};
    bool lcdCustomSet_ = false;       // Loaded or chosen; until then, choosing them copies the preset in use
    bool lcdCustomDirty_ = false;     // Saved once no control is held
    bool showLcdColourEditor_ = false;
    bool focusLcdColourEditor_ = false;
    int lcdColourPart_ = 2;           // The colour the editor's picker shows: 0 glass, 1 unlit dots, 2 lit dots
    // The D-series reverb's tuning window.
    bool showReverbTuning_ = false;
    int reverbTuningType_ = -1;        // 0-7, -1 = the type the unit plays
    double reverbTestOffAt_ = -1.0;    // The test chord's note-offs
    uint8_t reverbTestChannel_ = 0;
    int d20ScrolledBar_ = -1;             // Track bar last scrolled into view
    int writeTimbreTarget_ = 0;
    int writePatchTarget_ = 0;
    std::string patchNameEdit_;
    bool patchNameActive_ = false;
    std::string requestedTab_;

    // Tone and timbre editors
    ToneEditor toneEditor_;
    ToneHost toneHost_{*this};
    int tonePart_ = -1;               // Part whose tone the editor shows
    double toneWriteTime_ = -1.0;     // Time of the editor's last write to the synth
    bool toneTabShown_ = false;       // This frame
    bool toneTabWasShown_ = false;    // Last frame (the computer keyboard plays the edited part then)
    bool performanceTabShown_ = false;     // This frame
    bool performanceTabWasShown_ = false;  // Last frame (the computer keyboard plays the performance then)
    bool patternsTabShown_ = false;   // This frame (the LCD shows the D-20's Pattern Play screen from the next on)
    int writeToneTarget_ = 0;         // 0-63 = i11-i88, 64-127 = c11-c88 on the card
    std::vector<Tone::FoundTone> toneFileTones_;  // Tones of a SysEx file, to pick one
    std::string toneFileName_;
    bool toneFileMt32_ = false;       // Translate the file's waves from the MT-32's
    bool openToneFilePicker_ = false;
    int timbreBank_ = 0;              // Timbres tab: 0 internal, 1 card
    std::array<uint8_t, 8> timbreClipboard_{};
    bool timbreClipboardFull_ = false;
    std::array<uint8_t, 128> keyChannel_{};  // Channel each computer-keyboard note started on
    int windowZoom_ = 100;            // Percent (setting window_zoom)
    bool showConfig_ = false;         // The configuration window (ROMs, audio, MIDI input)
    bool focusConfig_ = false;        // Bring it to the front at the next frame

    bool scrollLogToBottom_ = false;
    bool showLog_ = false;
    bool showDemo_ = false;
    bool openAbout_ = false;
    bool openInitializeConfirm_ = false;
    bool quitRequested_ = false;
    std::unique_ptr<Platform::PendingDialog> dialog_;  // A file dialog running beside the App
    PathAction dialogThen_;
};
