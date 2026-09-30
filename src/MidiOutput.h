#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Anything MIDI can be sent to: an output port, or a recorder in the tests.
class MidiSender {
public:
    virtual ~MidiSender() = default;
    virtual bool sendShort(uint32_t message) = 0;  // status | data1 << 8 | data2 << 16
    // A complete F0 ... F7 message. Returns once the driver has taken it.
    virtual bool sendSysex(const uint8_t* data, size_t length) = 0;
};

// A MIDI output port by name: WinMM on Windows (MidiOutputWin32.cpp), the ALSA sequencer on Linux (MidiOutputAlsa.cpp),
// CoreMIDI on macOS (MidiOutputCoreMidi.cpp); none in the builds that send no MIDI (MidiOutputNull.cpp: the tests).
// One thread sends (MidiPipe's and UnitLink's sender threads); the others may check it (reconnect()) meanwhile.
class MidiOutputPort : public MidiSender {
public:
    // `ownName`: what other programs see this output as, where they see it (the port of the program's ALSA client that
    // sends, "MT32Translator:To the unit"); CoreMIDI and WinMM show them nothing of it.
    explicit MidiOutputPort(const std::string& ownName = "MIDI Out");
    ~MidiOutputPort() override;
    MidiOutputPort(const MidiOutputPort&) = delete;
    MidiOutputPort& operator=(const MidiOutputPort&) = delete;

    // Port names; identical names get a " #2"-style suffix so they stay distinguishable. The program's own ports (its
    // MIDI input, which would make a loop) are not among them.
    static std::vector<std::string> listPorts();
    // What keeps MIDI output from working at all (the ALSA sequencer or CoreMIDI cannot be opened), empty when nothing
    // does.
    static std::string systemError();
    bool open(const std::string& name, std::string& error);
    void close();
    bool isOpen() const;
    std::string name() const;
    // Whether the port opened is still there, connected again once it comes back (a MIDI interface unplugged and
    // plugged in again, another program's port that went with its program): false while it is gone, when messages go
    // nowhere. Look every few seconds.
    bool reconnect();
    // What open() or the last reconnect() found.
    bool connected() const;

    bool sendShort(uint32_t message) override;
    bool sendSysex(const uint8_t* data, size_t length) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
