#pragma once

#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "MidiInput.h"
#include "MidiOutput.h"
#include "MidiPipe.h"
#include "Mt32Presets.h"
#include "Settings.h"

struct TranslatorOptions {
    std::filesystem::path settingsFile;
    std::filesystem::path cacheFile;  // What the unit's cache slots hold (SysEx), kept between sessions
    std::vector<std::filesystem::path> romSearchDirs;  // Where to look for a "roms" folder with an MT-32 control ROM
    bool enableMidi = true;                            // Off for headless snapshots
};

// MT32Translator without its interface: a translation box between an MT-32 program and a real D-110, D-10 or D-20 (see
// MidiPipe), with its settings (mt32translator.ini), its MIDI ports (opened again when they come back: a MIDI interface
// plugged in again, a program started after the translator), the MT-32's presets from a control ROM, the tone cache's
// file, files sent to the unit or translated, and the log. TranslatorApp shows it in a window (Dear ImGui),
// TranslatorTui in a terminal; both keep their settings in the same file.
class TranslatorCore {
public:
    static constexpr int kPartCount = 9;  // Parts 1-8 and rhythm

    TranslatorCore();
    virtual ~TranslatorCore();
    TranslatorCore(const TranslatorCore&) = delete;
    TranslatorCore& operator=(const TranslatorCore&) = delete;

    void init(const TranslatorOptions& options);
    void shutdown();
    // Once a frame: the pipe's log, and every two seconds the MIDI ports (those chosen that came back are opened).
    void update();
    bool quitRequested() const { return quitRequested_; }
    void requestQuit() { quitRequested_ = true; }
    MidiPipe& pipe() { return pipe_; }
    void log(const std::string& line) { addLog(line); }

    static bool isMidiFile(const std::filesystem::path& path);

protected:
    // Settings.
    void loadSettings();
    void saveSettings();
    void applyPipeSettings();  // The settings into the pipe (and the tone cache's slots)
    void loadPresets();        // The MT-32's presets: the chosen control ROM, else the first in a "roms" folder
    void setTarget(Mt32Translator::Target target);  // With that unit's default for the memories
    void setPresetChoicesBuiltIn();

    // MIDI ports: the lists, the ports chosen (by name, remembered), and what went wrong last.
    void refreshPorts();
    void openInput(const std::string& name, bool open);
    void openUnitInput(const std::string& name);
    void openUnitOutput(const std::string& name);
    void openReplyOutput(const std::string& name);
    // Where programs send to the translator itself ("MT32Translator:MIDI In (129:0)"; CoreMIDI: "MT32Translator"), and
    // how; empty on Windows.
    std::string ownInputName() const;
    std::string ownInputHint() const;

    // The tone cache's file: a DT1 to tone memory (08 xx 00) per slot the translator knows the content of.
    void loadCacheFile();
    void saveCacheFile();

    // Files.
    void sendSysexFile(const std::filesystem::path& path);
    // What a translated copy of `source` is called: "Song (D-20).mid", "Timbres (D-110).syx".
    std::string translatedName(const std::filesystem::path& source) const;
    // Whether `source` can be translated (a MIDI file that loads, a file that reads); logs why not.
    bool canTranslate(const std::filesystem::path& source);
    // Writes a translated copy of `source` to `target` (a MIDI file one that plays on the unit, after the MT-32's
    // power-on setup; a SysEx file one that loads into its memory) and logs it.
    bool translateFileTo(const std::filesystem::path& source, const std::filesystem::path& target);

    // After the settings are read, before the presets load and the ports open: a front-end's own choices (the
    // terminal version's command line).
    virtual void loadFrontEndSettings() {}
    void addLog(const std::string& line);
    virtual void onLogAdded(const std::string& line) { (void)line; }
    // Seconds on a steady clock: when the ports are looked at again (tests set another).
    virtual double now() const;

    TranslatorOptions options_;
    Settings settings_;
    MidiPipe pipe_;
    std::unique_ptr<MidiInputManager> inputs_;      // From the MT-32 program
    std::unique_ptr<MidiInputManager> unitInputs_;  // The unit's MIDI OUT: write request results
    MidiOutputPort unitPort_{"To the unit"};
    MidiOutputPort replyPort_{"Replies"};

    PipeSettings pipeSettings_;
    std::shared_ptr<Mt32Presets> presets_;
    std::filesystem::path presetRom_;  // The MT-32 control ROM for exact presets
    std::string presetError_;
    bool powerOnAtStart_ = false;
    bool cacheEnabled_ = false;
    int cacheFirst_ = 32;  // Slots i51-i88 by default
    int cacheLast_ = 63;

    std::vector<std::string> inputPorts_;
    std::vector<std::string> outputPorts_;
    std::vector<std::string> enabledInputs_;
    std::string unitPortName_;
    std::string replyPortName_;
    std::string unitInputName_;
    std::string portError_;

    std::deque<std::string> log_;
    bool quitRequested_ = false;

private:
    void checkPorts();
    void checkOutput(MidiOutputPort& port, const std::string& wanted, void (TranslatorCore::*open)(const std::string&));

    double nextPortCheck_ = 0.0;
};
