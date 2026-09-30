#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "MidiInput.h"
#include "MidiOutput.h"

// ToneEditor's connection to a real unit (D-110, D-10, D-20, MT-32). Everything for the unit goes through one queue,
// in order, paced so the unit keeps up with SysEx (the D-110 shows "Exclusive Buffer Full" when flooded):
//  - data sets (DT1). A parameter change that is still waiting takes a newer value for the same bytes instead of
//    queueing another message, so dragging a slider sends as many changes as the unit can take, the latest last;
//  - requests (RQ1), one at a time: the next goes out once the unit answered (or after a timeout). The manuals ask
//    for requests at a block's start address ("parameter base address"), e.g. a part's tone temporary area;
//  - notes and other short messages.
// The unit's MIDI OUT comes in through MidiInputSink: its data sets (answers, and parameters it sends while being
// edited on its panel) wait in takeReceived() for the UI thread.
// Thread-safe: MIDI input arrives on driver threads, the UI calls the rest, and a sender thread sends.
class UnitLink : public MidiInputSink {
public:
    using Clock = std::chrono::steady_clock;

    UnitLink();
    ~UnitLink() override;
    UnitLink(const UnitLink&) = delete;
    UnitLink& operator=(const UnitLink&) = delete;

    // Where messages go (nullptr = nowhere). A sender must stay alive until it has been replaced here.
    void setOutput(MidiSender* output);
    void setDevice(uint8_t device);  // Device ID: the unit number less one (10H for unit 17)
    uint8_t device() const;
    // Pause after a 256-byte SysEx message, on top of its transmission (shorter messages pause less, at least a tenth).
    // Parameter changes are at least this far apart.
    void setPause(int milliseconds);

    // DT1 of `length` bytes at a packed address (see RolandSysex.h). `merge`: a waiting DT1 for the same address and
    // length takes this data instead.
    void sendData(uint32_t packedAddress, const uint8_t* data, size_t length, bool merge);
    // RQ1 for `size` bytes at a packed address.
    void request(uint32_t packedAddress, uint32_t size);
    void sendShort(uint32_t message);
    // Drops requests not yet sent.
    void cancelRequests();

    // MidiInputSink: the unit's MIDI OUT.
    void onMidiShortMessage(uint32_t message) override;
    void onMidiSysex(const uint8_t* data, size_t length) override;

    struct Received {
        uint32_t address = 0;  // Packed
        std::vector<uint8_t> data;
    };
    // Data sets from the unit since the last call, in arrival order.
    std::vector<Received> takeReceived();

    struct Stats {
        uint64_t sent = 0;           // Messages to the unit
        uint64_t sysexBytes = 0;
        uint64_t merged = 0;         // Parameter changes merged into a waiting one
        uint64_t received = 0;       // Data sets from the unit
        uint64_t answered = 0;       // Requests answered
        uint64_t unanswered = 0;     // Requests that timed out
        size_t queued = 0;           // Messages waiting
        size_t requestsWaiting = 0;  // Requests not answered yet, including the one out
    };
    Stats stats() const;
    std::vector<std::string> takeLog();

    // The sender. start() runs it on a thread; process() is one step, for tests: sends what is due at `now` and returns
    // when the next message is due (Clock::time_point::max() if nothing waits).
    void start();
    void stop();
    Clock::time_point process(Clock::time_point now);

    static constexpr int kRequestTimeoutMs = 1500;

private:
    struct Outgoing {
        uint32_t shortMessage = 0;
        std::vector<uint8_t> sysex;
        uint32_t address = 0;  // Of a DT1 that may be merged
        size_t length = 0;
        bool merge = false;
    };
    struct Request {
        uint32_t address = 0;
        uint32_t size = 0;
    };

    void logLocked(const std::string& line);
    void run();

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    MidiSender* output_ = nullptr;
    uint8_t device_ = 0x10;
    int pauseMs_ = 20;
    std::deque<Outgoing> queue_;
    std::deque<Request> requests_;
    bool requestOut_ = false;       // requests_.front() was sent and waits for its answer
    Clock::time_point requestSent_{};
    Clock::time_point nextSend_{};
    Clock::time_point nextMerged_{};
    std::vector<Received> received_;
    Stats stats_;
    std::deque<std::string> log_;

    std::mutex sendMutex_;  // One process() at a time; setOutput() waits for it
    std::thread thread_;
    bool stopping_ = false;
    bool work_ = false;     // Something new for the sender
};
