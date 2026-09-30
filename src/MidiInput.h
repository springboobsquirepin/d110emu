#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// The name other MIDI programs know this one by: its ALSA sequencer clients' (Linux), its CoreMIDI client's and MIDI
// destination's (macOS). "D110Emu" unless the program names itself before it makes its first MIDI port (MT32Translator
// and ToneEditor do).
inline std::string& midiProgramNameStorage() {
    static std::string name = "D110Emu";
    return name;
}
inline void setMidiProgramName(const std::string& name) {
    midiProgramNameStorage() = name;
}
inline const std::string& midiProgramName() {
    return midiProgramNameStorage();
}

// Receives messages from MIDI input ports, on the ports' driver threads.
class MidiInputSink {
public:
    virtual ~MidiInputSink() = default;
    virtual void onMidiShortMessage(uint32_t message) = 0;
    virtual void onMidiSysex(const uint8_t* data, size_t length) = 0;
    // System real-time messages: timing clock (F8H), start (FAH), continue (FBH) and stop (FCH). Most sinks ignore them.
    virtual void onMidiRealTime(uint8_t status) { (void)status; }
};

// Opens MIDI input ports by name: WinMM on Windows (MidiInputWin32.cpp), the ALSA sequencer on Linux
// (MidiInputAlsa.cpp), CoreMIDI on macOS (MidiInputCoreMidi.cpp); none in the builds that take no MIDI from ports
// (MidiInputNull.cpp: the plugins, the tests).
// System real-time messages go to the sink's onMidiRealTime(), except active sensing and reset, which are dropped;
// they do not count as messages for the activity indicators.
class MidiInputManager {
public:
    // With `ownPort`, other programs can send to this one's own input too (ALSA: the sequencer port "MIDI In" of the
    // program's client; CoreMIDI: a MIDI destination named after the program); without, only the ports opened here
    // arrive (an input for a unit's answers, say, which no program should mistake for the program's input).
    explicit MidiInputManager(MidiInputSink& sink, bool ownPort = true);
    ~MidiInputManager();
    MidiInputManager(const MidiInputManager&) = delete;
    MidiInputManager& operator=(const MidiInputManager&) = delete;

    // Port names; identical names get a " #2"-style suffix so they stay distinguishable.
    std::vector<std::string> listPorts() const;
    bool open(const std::string& name, std::string& error);
    void close(const std::string& name);
    void closeAll();
    bool isOpen(const std::string& name) const;
    std::vector<std::string> openPorts() const;
    // Messages received so far on an open port; drives the activity indicators.
    uint32_t messageCount(const std::string& name) const;
    // Where other programs can send MIDI to this one (ALSA: this program's sequencer port, "D110Emu:MIDI In (128:0)";
    // CoreMIDI: its MIDI destination, "D110Emu"), empty where there is none (Windows, or made without `ownPort`).
    std::string ownPortName() const;
    // How they send there, for a sentence's parentheses (ALSA: "aconnect, aplaymidi -p D110Emu, or their MIDI port
    // setting"), empty where there is no such port.
    std::string ownPortHint() const;
    // What keeps MIDI input from working at all (the ALSA sequencer or CoreMIDI cannot be opened), empty when nothing
    // does.
    std::string systemError() const;

    struct Port;
    struct System;  // What the ports share (ALSA: the sequencer client and its thread; CoreMIDI: the input port)

private:
    MidiInputSink& sink_;
    std::vector<std::unique_ptr<Port>> ports_;
    std::unique_ptr<System> system_;
};
