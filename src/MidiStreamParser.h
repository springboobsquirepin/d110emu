#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "MidiInput.h"

// Splits MIDI bytes as a cable carries them into messages for a MidiInputSink: channel and system common messages
// (running status followed), SysEx from F0 to F7 (which may come in pieces, as the ALSA sequencer and CoreMIDI deliver
// a long one), and system real-time messages wherever they come (clock, start, continue and stop to onMidiRealTime;
// active sensing and reset dropped). A status byte in the middle of a SysEx message drops the unfinished message.
class MidiStreamParser {
public:
    static constexpr size_t kMaxSysexLength = 64 * 1024;

    // Passes the messages in `bytes` on; returns how many short messages and SysEx messages it passed (not real-time).
    uint32_t feed(MidiInputSink& sink, const uint8_t* bytes, size_t count) {
        uint32_t messages = 0;
        for (size_t i = 0; i < count; i++) {
            const uint8_t byte = bytes[i];
            if (byte >= 0xF8) {
                if (byte == 0xF8 || byte == 0xFA || byte == 0xFB || byte == 0xFC) sink.onMidiRealTime(byte);
                continue;
            }
            if (inSysex_) {
                if (byte == 0xF7) {
                    sysex_.push_back(byte);
                    if (sysex_.size() <= kMaxSysexLength) {
                        sink.onMidiSysex(sysex_.data(), sysex_.size());
                        messages++;
                    }
                    sysex_.clear();
                    inSysex_ = false;
                    continue;
                }
                if (byte < 0x80) {
                    if (sysex_.size() <= kMaxSysexLength) sysex_.push_back(byte);
                    continue;
                }
                sysex_.clear();  // Cut short by a status byte, which counts as it comes
                inSysex_ = false;
            }
            if (byte == 0xF0) {
                inSysex_ = true;
                sysex_.assign(1, byte);
                status_ = 0;  // SysEx cancels running status
                continue;
            }
            if (byte >= 0x80) {
                status_ = byte;
                have_ = 0;
                needed_ = dataBytes(byte);
                if (needed_ == 0) {  // Tune request (and F7 or undefined ones alone, which are ignored)
                    if (byte == 0xF6) {
                        sink.onMidiShortMessage(byte);
                        messages++;
                    }
                    status_ = 0;
                }
                continue;
            }
            if (status_ == 0) continue;  // Data without a status
            data_[have_++] = byte;
            if (have_ < needed_) continue;
            sink.onMidiShortMessage(uint32_t(status_) | (uint32_t(data_[0]) << 8) | (needed_ > 1 ? uint32_t(data_[1]) << 16 : 0u));
            messages++;
            have_ = 0;
            if (status_ >= 0xF0) status_ = 0;  // Running status is for channel messages only
        }
        return messages;
    }

    void reset() {
        status_ = 0;
        have_ = 0;
        needed_ = 0;
        inSysex_ = false;
        sysex_.clear();
    }

    // The data bytes that follow a status byte: 0-2 (0 for real-time messages, and for those not valid alone).
    static int dataBytes(uint8_t status) {
        switch (status & 0xF0) {
        case 0xC0:
        case 0xD0:
            return 1;
        case 0xF0:
            return status == 0xF1 || status == 0xF3 ? 1 : status == 0xF2 ? 2 : 0;
        default:
            return 2;
        }
    }

private:
    uint8_t status_ = 0;
    uint8_t data_[2] = {};
    int have_ = 0;
    int needed_ = 0;
    bool inSysex_ = false;
    std::vector<uint8_t> sysex_;
};
