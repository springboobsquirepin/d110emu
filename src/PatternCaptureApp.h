#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "D20Rhythm.h"
#include "MidiInput.h"
#include "PatternCapture.h"
#include "Settings.h"

struct PatternCaptureOptions {
    std::filesystem::path settingsFile;
    std::filesystem::path sessionFile;  // The captured patterns, kept between sessions (a dump, as Save writes)
    bool enableMidi = true;             // Off for headless snapshots
};

// PatternCapture: records the D-20's preset rhythm patterns P-11-P-48, which its bulk dumps leave out, from what it
// plays on its MIDI OUT (see PatternCapture), and saves them as a D-20 dump with the patterns in P-51-P-88, which
// d110emu's Patterns tab loads as its presets (Load presets). Like App, the UI is platform-independent;
// MainPatternCaptureWin32.cpp hosts it in a window.
class PatternCaptureApp {
public:
    PatternCaptureApp();
    ~PatternCaptureApp();

    void init(const PatternCaptureOptions& options);
    void frame();  // Builds the UI; call between ImGui::NewFrame() and ImGui::Render()
    void shutdown();
    bool quitRequested() const { return quitRequested_; }
    // A dropped dump: its P-51-P-88 fill the slots (as Open dump).
    void openFile(const std::filesystem::path& path);

    // MIDI from the D-20 (the input ports' threads): short messages and real-time status bytes, handled at the next
    // frame. `seconds` is the arrival time; receive() stamps it with a steady clock. For tests and snapshots too.
    void receive(uint32_t message);
    void receive(uint32_t message, double seconds);
    const std::array<D20Pattern, kD20PresetPatterns>& patterns() const { return patterns_; }
    int slot() const { return slot_; }

private:
    class Receiver : public MidiInputSink {
    public:
        explicit Receiver(PatternCaptureApp& app) : app_(app) {}
        void onMidiShortMessage(uint32_t message) override { app_.receive(message); }
        void onMidiSysex(const uint8_t* data, size_t length) override {
            (void)data;
            (void)length;
        }
        void onMidiRealTime(uint8_t status) override { app_.receive(status); }

    private:
        PatternCaptureApp& app_;
    };

    void loadSettings();
    void saveSettings();
    void refreshPorts();
    void openInput(const std::string& name);
    void loadSession();
    void saveSession();
    bool loadDump(const std::filesystem::path& path, std::string& error);
    void takeFinished();
    void store(int slot);
    int nextSlot(int from) const;  // The next slot without a pattern after `from`, else the next one

    void drawMenu();
    void drawInput();
    void drawSteps();
    void drawFile();
    void drawSlots();
    void drawTake();
    void drawGrid(const D20Pattern& pattern, const char* id);

    PatternCaptureOptions options_;
    Settings settings_;
    Receiver receiver_;
    std::unique_ptr<MidiInputManager> inputs_;
    std::vector<std::string> ports_;
    std::string inputName_;
    std::string inputError_;

    std::mutex queueMutex_;
    std::vector<std::pair<uint32_t, double>> queue_;  // From the input threads to frame()

    PatternCapture capture_;
    std::array<D20Pattern, kD20PresetPatterns> patterns_{};
    int slot_ = 0;                 // Where the next take goes (0 = P-11)
    bool takeShown_ = false;       // take_ is a finished take not stored yet
    PatternCapture::Take take_;
    int takeBeats_ = 4;
    std::string status_;           // What happened to the last take
    bool strayNotes_ = false;      // Rhythm notes arrived while not recording (no Start seen)
    bool autoStore_ = true;        // Store a take at Stop when its bars agree
    bool advance_ = true;          // Then select the next empty slot
    bool quitRequested_ = false;
};
