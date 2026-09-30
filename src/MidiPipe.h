#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "MidiInput.h"
#include "MidiOutput.h"
#include "Mt32Presets.h"
#include "Mt32Translator.h"
#include "SmfFile.h"

// How MIDI for an MT-32 is translated, and for which unit.
struct PipeSettings {
    PipeSettings();
    Mt32Translator::Target target = Mt32Translator::Target::D20;
    int unitNumber = 17;                                      // 17-32: the unit's device ID is 10H-1FH
    uint8_t unitChannels[9] = {0, 1, 2, 3, 4, 5, 6, 7, 9};    // D-20: parts 1-8 and rhythm (0-15, 16 = off)
    bool memoryInUnit = false;         // Mt32Translator::setMemoryInUnit (the D-20 needs it off)
    // StandIns, Hybrid or Exact (the last two need the MT-32's presets), and each preset's choice (built in unless
    // changed by ear).
    Mt32Translator::PresetMode presetMode = Mt32Translator::PresetMode::StandIns;
    std::array<Mt32Translator::PresetChoice, 128> presetChoices;
    bool roomyToms = false;            // Mt32Translator::setRoomyToms
    int cacheFirst = 0;                // Tone cache slots (0-63 = i11-i88); off when cacheFirst > cacheLast
    int cacheLast = -1;
    bool masterVolumeAsVolume = true;  // Mt32Translator::setMasterVolumeAsVolume (real units have no master volume)
    bool reduceLoad = true;            // Drops controller values the unit already has, merges waiting ones
    int sysexGapMs = 20;               // Pause after a 256-byte SysEx message (shorter ones less), on top of its transmission
};

// Sets a translator up for `settings` (not reset; the tone cache is left alone).
void configureTranslator(Mt32Translator& translator, const PipeSettings& settings, std::shared_ptr<const Mt32Presets> presets);

// The translation pipe of MT32Translator. MIDI from an MT-32 program (a game, a sequencer, a DOS emulator) arrives
// through MidiInputSink, goes through Mt32Translator and out to a D-series unit, paced so the unit keeps up with
// SysEx. An MT-32 answers handshake transfers (WSD, DAT, EOD) with ACK; the pipe does so on the reply output, so
// programs that load their timbres that way (many X68000 and PC-98 games) go on.
// Thread-safe: input arrives on driver threads, the UI calls the rest, and a sender thread sends.
class MidiPipe : public MidiInputSink {
public:
    using Clock = std::chrono::steady_clock;

    MidiPipe();
    ~MidiPipe() override;
    MidiPipe(const MidiPipe&) = delete;
    MidiPipe& operator=(const MidiPipe&) = delete;

    // Where messages go: the unit, and the program (handshake replies). nullptr = nowhere. A sender must stay alive
    // until it has been replaced here.
    void setOutputs(MidiSender* unit, MidiSender* reply);
    // Takes effect for what arrives next; send the power-on setup to bring the unit in line.
    void configure(const PipeSettings& settings, std::shared_ptr<const Mt32Presets> presets);

    // Queues the MT-32's power-on setup, as an MT-32 reset (7F 00 00) does.
    void powerOn();
    // Queues All Notes Off and Reset All Controllers on all 16 channels of the unit.
    void allNotesOff();
    // Translates and queues SysEx messages as if the program had sent them (e.g. a file). Handshake packets count as
    // data and get no replies.
    void sendSysexData(const std::vector<uint8_t>& data);

    // The tone cache (see Mt32Translator::setToneCache; its slots come from the settings).
    void preloadToneCache();
    void clearToneCache();
    Mt32Translator::CacheStats toneCacheStats() const;
    std::vector<std::vector<uint8_t>> toneCacheContents() const;
    void restoreToneCache(const std::vector<std::vector<uint8_t>>& contents);

    // MIDI from the unit's MIDI OUT (optional): its answers to write requests. Feed it through unitSink().
    void onUnitSysex(const uint8_t* data, size_t length);
    class UnitSink : public MidiInputSink {
    public:
        explicit UnitSink(MidiPipe& pipe) : pipe_(pipe) {}
        void onMidiShortMessage(uint32_t) override {}
        void onMidiSysex(const uint8_t* data, size_t length) override { pipe_.onUnitSysex(data, length); }

    private:
        MidiPipe& pipe_;
    };
    UnitSink& unitSink() { return unitSink_; }

    void onMidiShortMessage(uint32_t message) override;
    void onMidiSysex(const uint8_t* data, size_t length) override;

    struct Stats {
        uint64_t received = 0;        // Messages from the program
        uint64_t sent = 0;            // Messages to the unit
        uint64_t sysexBytesSent = 0;
        uint64_t replies = 0;         // Handshake replies to the program
        uint64_t dropped = 0;         // Requests the pipe does not answer, replies without a reply output
        size_t queued = 0;            // Waiting for the unit
        uint64_t handshakes = 0;      // Transfers completed (EOD)
        uint64_t thinned = 0;         // Controller messages the unit did not need (reduceLoad)
        uint64_t writesConfirmed = 0; // Write requests the unit answered as done
    };
    Stats stats() const;
    // New log lines since the last call.
    std::vector<std::string> takeLog();

    // The sender. start() runs it on a thread; process() is one step, for tests: sends the replies and whatever is
    // due for the unit at `now`, and returns when the next message is due (Clock::time_point::max() if none waits).
    void start();
    void stop();
    Clock::time_point process(Clock::time_point now);

private:
    struct Outgoing {
        uint32_t shortMessage = 0;
        std::vector<uint8_t> sysex;
    };

    uint8_t deviceIdLocked() const { return uint8_t(settings_.unitNumber - 1); }
    void translateSysexLocked(const uint8_t* data, size_t length, bool fromProgram);
    void queueLocked(const std::vector<uint32_t>& shortMessages, const std::vector<uint8_t>& sysex, bool sysexFirst);
    bool thinLocked(uint32_t message);  // True if the message is not needed (or merged into a waiting one)
    void forgetControllersLocked();
    void replyLocked(uint8_t device, uint8_t command);
    void logLocked(const std::string& line);
    void run();

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    Mt32Translator translator_;
    PipeSettings settings_;
    MidiSender* unit_ = nullptr;
    MidiSender* reply_ = nullptr;
    std::deque<Outgoing> queue_;
    std::deque<std::vector<uint8_t>> replies_;
    Clock::time_point nextSend_{};
    Stats stats_;
    std::deque<std::string> log_;
    const Clock::time_point startTime_ = Clock::now();
    std::vector<uint8_t> sysexScratch_;
    std::vector<uint32_t> shortScratch_;
    bool warnedNoReply_ = false;
    std::array<std::array<int16_t, 128>, 16> controllers_{};  // Last value queued per unit channel, -1 = unknown
    std::array<int32_t, 16> bends_{};
    UnitSink unitSink_{*this};

    std::mutex sendMutex_;  // One process() at a time
    std::thread thread_;
    bool stopping_ = false;
};

// Translates a SysEx file (DT1 messages, or a handshake transfer's DAT packets) into DT1 messages that load the
// MT-32's data into the unit's memory, with its Memory Protect off (a D-10/D-20's rhythm setup then goes to its rhythm
// setup memory, 09 00 00). The file's memories are always written, whatever
// `settings.memoryInUnit` says, as the unit's memory is what the file is for.
std::vector<uint8_t> translateSysexFile(const std::vector<uint8_t>& data, const PipeSettings& settings,
                                        std::shared_ptr<const Mt32Presets> presets);

// Translates a MIDI file as the pipe would play it, after the MT-32's power-on setup (spaced as the unit needs it;
// the song moves back by as much). Returns the setup's length in seconds.
double translateMidiFile(const SmfFile& in, SmfFile& out, const PipeSettings& settings, std::shared_ptr<const Mt32Presets> presets);
