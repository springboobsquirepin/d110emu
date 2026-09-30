#pragma once

#include <array>
#include <atomic>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "MidiInput.h"
#include "MidiOutput.h"
#include "Settings.h"
#include "ToneEditor.h"
#include "ToneModel.h"
#include "UnitLink.h"
#include "UnitSetup.h"

struct ToneEditorOptions {
    std::filesystem::path settingsFile;
    std::filesystem::path libraryFile;  // The unit's tone memory as last read (SysEx), kept between sessions
    bool enableMidi = true;             // Off for headless snapshots
};

// ToneEditor: a realtime tone editor for a real D-110, D-10, D-20 or MT-32, like a PG-10 programmer on screen. It
// edits one part's tone temporary area over MIDI (UnitLink): every change goes out as a DT1 at once, and the part plays
// it from the next note. Its Parts, Rhythm and System tabs edit the rest of the unit's setup the same way (UnitSetup).
// With the unit's MIDI OUT connected, it reads from the unit (RQ1): the part's tone, the setup, and the whole tone
// memory for the library. Like App, the UI is platform-independent; MainToneEditorWin32.cpp hosts it.
class ToneEditorApp : private ToneEditor::Host, private UnitSetup::Host {
public:
    ToneEditorApp();
    ~ToneEditorApp() override;

    void init(const ToneEditorOptions& options);
    void frame();  // Builds the UI; call between ImGui::NewFrame() and ImGui::Render()
    void shutdown();
    bool quitRequested() const { return quitRequested_; }
    // A dropped SysEx file: its tones go to the library.
    void openFile(const std::filesystem::path& path);
    // Computer-keyboard piano by physical key (PC scancode set 1), as App::onKey.
    void onKey(int scancode, bool down);
    void clearKeys();

    UnitLink& link() { return link_; }
    ToneEditor& editor() { return editor_; }
    UnitSetup& setup() { return setup_; }
    // Shows a tab of the right side: "Tone", "Parts", "Timbres", "Performance" (D-10/D-20), "Rhythm" or "System".
    void showTab(const std::string& name);
    // Asks the unit for its setup (parts, rhythm setup, system area).
    void requestSetup() { setup_.requestAll(*this); }
    // Asks a D-10/D-20 for its performance patch and patch memory (the Performance tab does this when first shown).
    void requestPatches() {
        setup_.requestPatch(*this);
        patchRead_ = true;
    }
    // Changes a part's setting as the Parts tab does (sent at once); for scripts and tests.
    void setPartValue(int part, int offset, int value) { setup_.setPartValue(*this, part, offset, value); }
    // Takes in what the unit sent (UnitLink::takeReceived); frame() does this, tests call it directly.
    void processReceived();
    void setModel(Tone::Model model);
    void selectPart(int part);  // 0-7 (0-1 in performance mode)
    // D-10/D-20 performance mode: parts 1 and 2 are the patch's upper and lower tones, and notes go to the performance
    // channel (UnitSetup::performanceChannel).
    void setPerformanceMode(bool on);
    bool performanceTones() const { return performanceMode_ && model_ == Tone::Model::D20; }
    void requestPartTone();
    void requestLibrary();
    // Sets a byte of the edited tone as the editor's controls do (sent to the unit at once).
    void edit(int offset, int value) { editor_.setByte(*this, offset, value); }
    // The unit's tone write (MT-32: the timbre straight into memory): slot 0-63 = i11-i88.
    void writeToMemory(int slot, bool card = false);
    // A tone of the unit's memory as last read, nullptr if not read.
    const Tone::Data* libraryTone(int slot) const { return slot >= 0 && slot < 64 && memoryKnown_[size_t(slot)] ? &memory_[size_t(slot)] : nullptr; }

private:
    // ToneEditor::Host: the part's tone temporary area on the unit, notes on the part's channel.
    void writeTone(int offset, const uint8_t* data, int length) override;
    void noteOn(int key, int velocity) override;
    void noteOff(int key) override;

public:
    // The channel the edited tone plays on: the part's, or in performance mode the performance channel (16 = none).
    uint8_t partChannel() const;

private:
    void restrike() override;
    void soundingNotes(std::array<bool, 128>& notes) override;
    // UnitSetup::Host: the unit's setup areas.
    void sendData(uint32_t packedAddress, const uint8_t* data, size_t length, bool merge) override;
    void request(uint32_t packedAddress, uint32_t size) override;
    void sendShort(uint32_t message) override;
    std::string memoryToneName(int slot) override;

    // A MIDI keyboard: its notes and controllers play the edited part (channel changed to the part's).
    class KeyboardSink : public MidiInputSink {
    public:
        explicit KeyboardSink(ToneEditorApp& app) : app_(app) {}
        void onMidiShortMessage(uint32_t message) override;
        void onMidiSysex(const uint8_t*, size_t) override {}

    private:
        ToneEditorApp& app_;
    };

    void loadSettings();
    void saveSettings();
    void applyUnitSettings();
    void loadLibraryFile();
    void saveLibraryFile();
    void refreshPorts();
    void openUnitOutput(const std::string& name);
    void openUnitInput(const std::string& name);
    void openKeyboard(const std::string& name, bool open);
    void addLog(const std::string& line);
    std::string partText() const;  // "part 3", or "the upper tone" in performance mode
    void followPartTone();         // The edited part plays another tone: read it from the unit
    void releaseAllNotes();
    void handleComputerKeyboard();
    void saveToneFile();
    void saveLibrary();
    void browseFile();
    void loadIntoEditor(const Tone::Data& tone, const std::string& where, bool mt32);

    void drawMenu();
    void drawPorts();
    void drawUnit();
    void drawTone();
    void drawLibrary();
    void drawLog();

    ToneEditorOptions options_;
    Settings settings_;
    UnitLink link_;
    ToneEditor editor_;
    UnitSetup setup_;       // Also holds the parts' MIDI channels (parts 1-8 and rhythm)
    int requestedTab_ = -1; // A right-side tab to show on the next frame: 0 Tone, 1 Parts, 2 Timbres, 3 Rhythm, 4 System, 5 Performance
    bool patchRead_ = false; // The Performance tab has asked the unit for its patches
    KeyboardSink keyboardSink_{*this};
    std::unique_ptr<MidiInputManager> unitInput_;  // The unit's MIDI OUT
    std::unique_ptr<MidiInputManager> keyboards_;
    MidiOutputPort unitPort_{"To the unit"};

    Tone::Model model_ = Tone::Model::D20;
    int unitNumber_ = 17;
    int part_ = 0;                                // 0-7
    int pauseMs_ = 20;
    bool followUnit_ = true;                      // Take in parameters the unit sends (panel edits)
    bool performanceMode_ = false;                // D-10/D-20: editing the performance's upper and lower tones
    uint32_t lastWriteRequest_ = 0;               // The last write request's address, for its result
    int writeSlot_ = 0;                           // 0-63 = i11-i88
    bool writeToCard_ = false;
    std::array<Tone::Data, 8> partTones_{};       // Last known tone of each part
    std::array<bool, 8> partToneKnown_{};
    std::array<int, 8> partToneSelection_{};      // What the unit's timbre temporary area selects: group * 64 + number, -1 unknown
    std::array<Tone::Data, 64> memory_{};         // The unit's tone memory as last read (library)
    std::array<bool, 64> memoryKnown_{};
    int libraryRequested_ = 0;                    // Tones asked for by the last "Read all"
    std::vector<Tone::FoundTone> fileTones_;      // Tones from SysEx files
    std::string fileName_;
    bool fileMt32_ = false;                       // The file holds MT-32 timbres (translate their waves)
    bool toneRequested_ = false;                  // Waiting for the part's tone

    std::vector<std::string> inputPorts_;
    std::vector<std::string> outputPorts_;
    std::string unitOutputName_;
    std::string unitInputName_;
    std::vector<std::string> keyboardNames_;
    std::string portError_;

    // Notes: computer keyboard and MIDI keyboards (thru), so that Restrike can play them again.
    std::array<bool, 256> scancodeDown_{};
    std::array<bool, 128> keyHeld_{};
    std::array<uint8_t, 128> keyChannel_{};
    int keyboardOctave_ = 4;
    int keyboardVelocity_ = 100;
    std::mutex thruMutex_;
    std::array<uint8_t, 128> thruHeld_{};         // Velocity, 0 = not held
    std::atomic<int> thruChannel_{0};

    std::deque<std::string> log_;
    bool showLog_ = false;
    bool quitRequested_ = false;
};
