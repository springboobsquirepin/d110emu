#include "SmfFile.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

namespace {

// Bounds-checked big-endian reader; `ok` turns false on any overrun.
struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    bool ok = true;

    size_t remaining() const { return size_t(end - p); }

    uint8_t u8() {
        if (p >= end) {
            ok = false;
            return 0;
        }
        return *p++;
    }

    uint32_t be(int bytes) {
        uint32_t value = 0;
        for (int i = 0; i < bytes; i++) value = (value << 8) | u8();
        return value;
    }

    uint32_t vlq() {
        uint32_t value = 0;
        for (int i = 0; i < 4; i++) {
            const uint8_t b = u8();
            value = (value << 7) | (b & 0x7F);
            if (!(b & 0x80)) return value;
        }
        ok = false;
        return value;
    }

    const uint8_t* take(size_t n) {
        if (remaining() < n) {
            ok = false;
            p = end;
            return nullptr;
        }
        const uint8_t* start = p;
        p += n;
        return start;
    }
};

enum class RawKind : uint8_t { Short, Sysex, Tempo, End };

struct RawEvent {
    uint64_t tick;
    uint32_t track;
    uint32_t sequence;  // Position within the track, keeps same-tick events in file order
    RawKind kind;
    uint32_t value;     // Short message, or microseconds per quarter note for tempo events
    uint32_t sysexOffset;
    uint32_t sysexLength;
};

// Appends the events of one MTrk chunk. Returns the tick of the track's last event.
uint64_t parseTrack(const uint8_t* data, size_t size, uint32_t track, uint64_t startTick,
                    std::vector<RawEvent>& raw, SmfFile& out) {
    Reader r{data, data + size};
    uint64_t tick = startTick;
    uint32_t sequence = 0;
    uint8_t runningStatus = 0;
    std::vector<uint8_t> sysex;
    bool sysexPending = false;

    auto push = [&](RawKind kind, uint32_t value, uint32_t offset = 0, uint32_t length = 0) {
        raw.push_back(RawEvent{tick, track, sequence++, kind, value, offset, length});
    };

    while (r.remaining() > 0) {
        tick += r.vlq();
        uint8_t status = r.u8();
        if (!r.ok) break;
        if (status < 0x80) {
            if (runningStatus == 0) break;  // Data byte without a status: corrupt track
            r.p--;
            status = runningStatus;
        }

        if (status < 0xF0) {
            runningStatus = status;
            const uint8_t type = status & 0xF0;
            const uint8_t data1 = r.u8();
            const uint8_t data2 = (type == 0xC0 || type == 0xD0) ? 0 : r.u8();
            if (!r.ok) break;
            push(RawKind::Short, uint32_t(status) | (uint32_t(data1 & 0x7F) << 8) | (uint32_t(data2 & 0x7F) << 16));
        } else if (status == 0xF0 || status == 0xF7) {
            // Running status is deliberately kept across SysEx/meta events: compliant files always
            // write a new status byte there, and some real-world files rely on it being kept.
            const uint32_t length = r.vlq();
            const uint8_t* bytes = r.take(length);
            if (!r.ok) break;
            if (status == 0xF0) {
                sysex.assign(1, 0xF0);
                sysex.insert(sysex.end(), bytes, bytes + length);
                sysexPending = true;
            } else if (sysexPending) {
                sysex.insert(sysex.end(), bytes, bytes + length);  // Continuation packet
            } else if (length >= 2 && bytes[0] == 0xF0) {
                sysex.assign(bytes, bytes + length);  // Escaped complete SysEx
                sysexPending = true;
            } else {
                continue;  // Other escaped bytes are of no use to the synth
            }
            if (sysexPending && sysex.back() == 0xF7) {
                push(RawKind::Sysex, 0, uint32_t(out.sysexData.size()), uint32_t(sysex.size()));
                out.sysexData.insert(out.sysexData.end(), sysex.begin(), sysex.end());
                sysexPending = false;
            }
        } else if (status == 0xFF) {
            const uint8_t type = r.u8();
            const uint32_t length = r.vlq();
            const uint8_t* bytes = r.take(length);
            if (!r.ok) break;
            if (type == 0x51 && length == 3) {
                push(RawKind::Tempo, (uint32_t(bytes[0]) << 16) | (uint32_t(bytes[1]) << 8) | bytes[2]);
            } else if (type == 0x03 && track == 0 && out.title.empty()) {
                out.title.assign(reinterpret_cast<const char*>(bytes), length);
            } else if (type == 0x2F) {
                break;
            }
        } else {
            break;  // System common messages are not valid in a track
        }
    }
    push(RawKind::End, 0);
    return tick;
}

}  // namespace

bool parseSmf(const uint8_t* data, size_t size, SmfFile& out, std::string& error) {
    out = SmfFile();

    // RIFF MIDI (.rmi) wraps the SMF in a "data" chunk.
    if (size >= 12 && std::memcmp(data, "RIFF", 4) == 0 && std::memcmp(data + 8, "RMID", 4) == 0) {
        const uint8_t* p = data + 12;
        const uint8_t* end = data + size;
        bool found = false;
        while (end - p >= 8) {
            const uint32_t length = uint32_t(p[4]) | (uint32_t(p[5]) << 8) | (uint32_t(p[6]) << 16) | (uint32_t(p[7]) << 24);
            const uint8_t* body = p + 8;
            const size_t available = std::min<size_t>(length, size_t(end - body));
            if (std::memcmp(p, "data", 4) == 0) {
                data = body;
                size = available;
                found = true;
                break;
            }
            p = body + available + (available & 1);
        }
        if (!found) {
            error = "RIFF file has no MIDI data chunk";
            return false;
        }
    }

    Reader r{data, data + size};
    if (size < 14 || std::memcmp(data, "MThd", 4) != 0) {
        error = "Not a Standard MIDI File (no MThd header)";
        return false;
    }
    r.take(4);
    const uint32_t headerLength = r.be(4);
    if (headerLength < 6) {
        error = "Invalid MThd header";
        return false;
    }
    out.format = int(r.be(2));
    r.be(2);  // Declared track count; the chunks are counted instead since files sometimes lie
    const uint32_t division = r.be(2);
    r.take(headerLength - 6);
    if (!r.ok || division == 0) {
        error = "Invalid MThd header";
        return false;
    }

    std::vector<RawEvent> raw;
    uint32_t track = 0;
    uint64_t formatTwoOffset = 0;
    while (r.remaining() >= 8) {
        char id[4];
        std::memcpy(id, r.take(4), 4);
        const uint32_t length = r.be(4);
        const size_t available = std::min<size_t>(length, r.remaining());
        const uint8_t* body = r.take(available);
        if (std::memcmp(id, "MTrk", 4) != 0) continue;
        // Format 2 tracks are independent sequences played one after another.
        const uint64_t lastTick = parseTrack(body, available, track++, out.format == 2 ? formatTwoOffset : 0, raw, out);
        if (out.format == 2) formatTwoOffset = lastTick;
    }
    out.trackCount = int(track);
    if (track == 0) {
        error = "MIDI file contains no tracks";
        return false;
    }

    std::stable_sort(raw.begin(), raw.end(), [](const RawEvent& a, const RawEvent& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        if (a.track != b.track) return a.track < b.track;
        return a.sequence < b.sequence;
    });

    const bool smpte = (division & 0x8000) != 0;
    double secondsPerTick;
    if (smpte) {
        const int framesPerSecond = -int(int8_t(division >> 8));
        const int ticksPerFrame = int(division & 0xFF);
        const double fps = framesPerSecond == 29 ? 29.97 : double(framesPerSecond);
        secondsPerTick = 1.0 / (fps * std::max(ticksPerFrame, 1));
    } else {
        secondsPerTick = 0.5 / division;  // 120 BPM until the first tempo event
    }

    double seconds = 0.0;
    uint64_t lastTick = 0;
    out.events.reserve(raw.size());
    for (const RawEvent& e : raw) {
        seconds += double(e.tick - lastTick) * secondsPerTick;
        lastTick = e.tick;
        switch (e.kind) {
        case RawKind::Short:
            out.events.push_back(SmfEvent{seconds, e.value, 0, 0});
            break;
        case RawKind::Sysex:
            out.events.push_back(SmfEvent{seconds, 0, e.sysexOffset, e.sysexLength});
            break;
        case RawKind::Tempo:
            if (!smpte && e.value > 0) secondsPerTick = e.value / 1e6 / division;
            break;
        case RawKind::End:
            break;
        }
        out.duration = std::max(out.duration, seconds);
    }
    return true;
}

bool loadSmfFile(const std::filesystem::path& path, SmfFile& out, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "Cannot open file";
        return false;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return parseSmf(data.data(), data.size(), out, error);
}

namespace {

void appendVlq(std::vector<uint8_t>& out, uint32_t value) {
    uint8_t bytes[5];
    int count = 0;
    do {
        bytes[count++] = uint8_t(value & 0x7F);
        value >>= 7;
    } while (value != 0);
    while (count > 0) {
        const uint8_t b = bytes[--count];
        out.push_back(count > 0 ? uint8_t(b | 0x80) : b);
    }
}

}  // namespace

std::vector<uint8_t> writeSmf(const SmfFile& smf, uint16_t ticksPerQuarter, uint32_t microsPerQuarter) {
    microsPerQuarter = std::clamp<uint32_t>(microsPerQuarter, 1, 0xFFFFFF);
    const double ticksPerSecond = ticksPerQuarter * 1e6 / microsPerQuarter;
    std::vector<uint8_t> track;
    auto meta = [&](uint8_t type, const std::vector<uint8_t>& body) {
        track.push_back(0x00);
        track.push_back(0xFF);
        track.push_back(type);
        appendVlq(track, uint32_t(body.size()));
        track.insert(track.end(), body.begin(), body.end());
    };
    if (!smf.title.empty()) meta(0x03, std::vector<uint8_t>(smf.title.begin(), smf.title.end()));
    meta(0x51, {uint8_t(microsPerQuarter >> 16), uint8_t(microsPerQuarter >> 8), uint8_t(microsPerQuarter)});

    uint32_t lastTick = 0;
    for (const SmfEvent& e : smf.events) {
        const uint32_t tick = std::max(lastTick, uint32_t(std::lround(std::max(e.time, 0.0) * ticksPerSecond)));
        appendVlq(track, tick - lastTick);
        lastTick = tick;
        if (e.shortMessage != 0) {
            const uint8_t status = uint8_t(e.shortMessage);
            track.push_back(status);
            track.push_back(uint8_t(e.shortMessage >> 8));
            const uint8_t type = status & 0xF0;
            if (type != 0xC0 && type != 0xD0) track.push_back(uint8_t(e.shortMessage >> 16));
        } else {
            // SysEx: F0, length, then everything after the F0 (F7 included).
            track.push_back(0xF0);
            appendVlq(track, e.sysexLength - 1);
            const uint8_t* sysex = &smf.sysexData[e.sysexOffset];
            track.insert(track.end(), sysex + 1, sysex + e.sysexLength);
        }
    }
    const uint32_t endTick = std::max(lastTick, uint32_t(std::lround(smf.duration * ticksPerSecond)));
    appendVlq(track, endTick - lastTick);
    track.insert(track.end(), {0xFF, 0x2F, 0x00});

    std::vector<uint8_t> file = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, uint8_t(ticksPerQuarter >> 8), uint8_t(ticksPerQuarter)};
    const uint32_t length = uint32_t(track.size());
    const uint8_t trackHeader[] = {'M', 'T', 'r', 'k', uint8_t(length >> 24), uint8_t(length >> 16), uint8_t(length >> 8), uint8_t(length)};
    file.insert(file.end(), trackHeader, trackHeader + sizeof(trackHeader));
    file.insert(file.end(), track.begin(), track.end());
    return file;
}

bool saveSmfFile(const std::filesystem::path& path, const SmfFile& smf, uint16_t ticksPerQuarter, uint32_t microsPerQuarter,
                 std::string& error) {
    const std::vector<uint8_t> data = writeSmf(smf, ticksPerQuarter, microsPerQuarter);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    if (!out) {
        error = "Cannot write " + path.u8string();
        return false;
    }
    return true;
}
