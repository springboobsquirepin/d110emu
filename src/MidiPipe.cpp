#include "MidiPipe.h"

#include <algorithm>
#include <cstdio>
#include <functional>

namespace {

constexpr size_t kMaxLogLines = 500;
constexpr uint8_t kWsd = 0x40, kRqd = 0x41, kDat = 0x42, kAck = 0x43, kEod = 0x45, kErr = 0x4E, kRjc = 0x4F;
constexpr uint8_t kRq1 = 0x11, kDt1 = 0x12;

// Calls `f` for every complete F0 ... F7 message in `data`.
void forEachMessage(const uint8_t* data, size_t size, const std::function<void(const uint8_t*, size_t)>& f) {
    size_t i = 0;
    while (i < size) {
        if (data[i] != 0xF0) {
            i++;
            continue;
        }
        size_t end = i + 1;
        while (end < size && data[end] != 0xF7 && data[end] < 0x80) end++;
        if (end < size && data[end] == 0xF7) {
            f(data + i, end + 1 - i);
            i = end + 1;
        } else {
            i = end;  // Broken off by another status byte
        }
    }
}

bool checksumOk(const uint8_t* data, size_t length) {
    unsigned sum = 0;
    for (size_t i = 5; i + 1 < length; i++) sum += data[i];  // Address, data and the checksum itself
    return sum % 128 == 0;
}

std::string hexAddress(const uint8_t* data) {
    char text[16];
    std::snprintf(text, sizeof(text), "%02X %02X %02X", data[5], data[6], data[7]);
    return text;
}

// "DT1 04 00 00, 246 bytes", "WSD 08 00 00 (16384 bytes)", or the first bytes of anything else.
std::string describeSysex(const uint8_t* data, size_t length) {
    if (length >= 6 && data[1] == 0x41 && data[3] == 0x16) {
        const uint8_t command = data[4];
        char device[8];
        std::snprintf(device, sizeof(device), "%02X", data[2]);
        switch (command) {
        case kDt1:
        case kDat:
            if (length >= 10) {
                return std::string(command == kDt1 ? "DT1 " : "DAT ") + hexAddress(data) + ", " + std::to_string(length - 10) +
                       " bytes (device " + device + ")";
            }
            break;
        case kRq1:
        case kWsd:
        case kRqd:
            if (length >= 13) {
                const unsigned size = (unsigned(data[8]) << 14) | (unsigned(data[9]) << 7) | data[10];
                return std::string(command == kRq1 ? "RQ1 " : command == kWsd ? "WSD " : "RQD ") + hexAddress(data) + ", " +
                       std::to_string(size) + " bytes (device " + device + ")";
            }
            break;
        case kAck:
            return "ACK";
        case kEod:
            return "EOD";
        case kErr:
            return "ERR";
        case kRjc:
            return "RJC";
        default:
            break;
        }
    }
    std::string text = "SysEx";
    for (size_t i = 0; i < length && i < 8; i++) {
        char byte[4];
        std::snprintf(byte, sizeof(byte), " %02X", data[i]);
        text += byte;
    }
    return text + (length > 8 ? " ..." : "") + " (" + std::to_string(length) + " bytes)";
}

constexpr uint32_t pack(uint32_t high, uint32_t mid, uint32_t low) {
    return (high << 14) | (mid << 7) | low;
}

void appendDataSet(std::vector<uint8_t>& out, uint8_t device, uint32_t address, const uint8_t* data, size_t length) {
    const size_t start = out.size();
    const uint8_t header[] = {0xF0, 0x41, device, 0x16, kDt1, uint8_t((address >> 14) & 0x7F), uint8_t((address >> 7) & 0x7F),
                              uint8_t(address & 0x7F)};
    out.insert(out.end(), header, header + sizeof(header));
    out.insert(out.end(), data, data + length);
    unsigned sum = 0;
    for (size_t i = start + 5; i < out.size(); i++) sum += out[i];
    out.push_back(uint8_t((128 - sum % 128) % 128));
    out.push_back(0xF7);
}

}  // namespace

PipeSettings::PipeSettings() {
    for (int timbre = 0; timbre < 128; timbre++) presetChoices[size_t(timbre)] = Mt32Translator::defaultPresetChoice(timbre);
}

void configureTranslator(Mt32Translator& translator, const PipeSettings& settings, std::shared_ptr<const Mt32Presets> presets) {
    translator.setTarget(settings.target);
    translator.setUnitChannels(settings.unitChannels);
    translator.setMemoryInUnit(settings.memoryInUnit);
    translator.setMasterVolumeAsVolume(settings.masterVolumeAsVolume);
    translator.setPresets(std::move(presets));
    translator.setPresetMode(settings.presetMode);
    for (int timbre = 0; timbre < 128; timbre++) translator.setPresetChoice(timbre, settings.presetChoices[size_t(timbre)]);
    translator.setRoomyToms(settings.roomyToms);
}

MidiPipe::MidiPipe() {
    configureTranslator(translator_, settings_, nullptr);
    forgetControllersLocked();
}

MidiPipe::~MidiPipe() {
    stop();
}

void MidiPipe::setOutputs(MidiSender* unit, MidiSender* reply) {
    std::lock_guard<std::mutex> sendLock(sendMutex_);  // Not while the sender uses the old ones
    std::lock_guard<std::mutex> lock(mutex_);
    unit_ = unit;
    reply_ = reply;
    warnedNoReply_ = false;
}

void MidiPipe::configure(const PipeSettings& settings, std::shared_ptr<const Mt32Presets> presets) {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool cacheChanged = settings.cacheFirst != settings_.cacheFirst || settings.cacheLast != settings_.cacheLast;
    settings_ = settings;
    settings_.unitNumber = std::clamp(settings_.unitNumber, 17, 32);
    settings_.sysexGapMs = std::clamp(settings_.sysexGapMs, 0, 500);
    configureTranslator(translator_, settings_, std::move(presets));
    if (cacheChanged) translator_.setToneCache(settings_.cacheFirst, settings_.cacheLast);
}

void MidiPipe::preloadToneCache() {
    std::lock_guard<std::mutex> lock(mutex_);
    sysexScratch_.clear();
    translator_.preloadToneCache(deviceIdLocked(), sysexScratch_);
    queueLocked({}, sysexScratch_, true);
    logLocked(sysexScratch_.empty() ? "Tone cache: nothing to preload (no MT-32 timbres, or they are all stored)"
                                    : "Tone cache: preloading the program's timbres");
    wake_.notify_all();
}

void MidiPipe::clearToneCache() {
    std::lock_guard<std::mutex> lock(mutex_);
    translator_.clearToneCache();
    logLocked("Tone cache forgotten: its slots fill again as tones are used");
}

Mt32Translator::CacheStats MidiPipe::toneCacheStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return translator_.toneCacheStats();
}

std::vector<std::vector<uint8_t>> MidiPipe::toneCacheContents() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return translator_.toneCacheContents();
}

void MidiPipe::restoreToneCache(const std::vector<std::vector<uint8_t>>& contents) {
    std::lock_guard<std::mutex> lock(mutex_);
    translator_.restoreToneCache(contents);
}

void MidiPipe::onUnitSysex(const uint8_t* data, size_t length) {
    // A write request's result: F0 41 dev 16 12 40 10 00 result sum F7.
    if (length != 11 || data[0] != 0xF0 || data[1] != 0x41 || data[3] != 0x16 || data[4] != kDt1 || data[5] != 0x40 ||
        data[6] != 0x10 || data[7] != 0x00) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const uint8_t result = data[8];
    if (result == 0) {
        stats_.writesConfirmed++;
        return;
    }
    static const char* const reasons[] = {"done", "card not ready", "write protected", "wrong mode"};
    sysexScratch_.clear();
    if (!translator_.writeRequestResult(result, deviceIdLocked(), sysexScratch_)) {
        logLocked(std::string("The unit refused a tone write (") + reasons[std::min<uint8_t>(result, 3)] +
                  "): the tone cache is off. Turn memory protect off, then turn the cache on again.");
        queueLocked({}, sysexScratch_, true);
        wake_.notify_all();
    }
}

void MidiPipe::powerOn() {
    std::lock_guard<std::mutex> lock(mutex_);
    forgetControllersLocked();
    sysexScratch_.clear();
    shortScratch_.clear();
    translator_.powerOn(deviceIdLocked(), sysexScratch_, shortScratch_);
    queueLocked(shortScratch_, sysexScratch_, true);
    logLocked("MT-32 power-on setup queued");
    wake_.notify_all();
}

void MidiPipe::allNotesOff() {
    std::lock_guard<std::mutex> lock(mutex_);
    forgetControllersLocked();
    std::vector<uint32_t> messages;
    for (uint32_t channel = 0; channel < 16; channel++) {
        messages.push_back(0xB0 | channel | (123u << 8));  // All Notes Off
        messages.push_back(0xB0 | channel | (121u << 8));  // Reset All Controllers
    }
    queueLocked(messages, {}, false);
    wake_.notify_all();
}

void MidiPipe::sendSysexData(const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    const size_t queuedBefore = queue_.size();
    forEachMessage(data.data(), data.size(), [&](const uint8_t* message, size_t length) {
        translateSysexLocked(message, length, false);
        count++;
    });
    const size_t queued = queue_.size() - std::min(queuedBefore, queue_.size());
    logLocked("File: " + std::to_string(count) + " SysEx messages translated, " + std::to_string(queued) + " for the unit" +
              (queued == 0 && !settings_.memoryInUnit ? " (memories are kept here until the parts select them)" : ""));
    wake_.notify_all();
}

void MidiPipe::onMidiShortMessage(uint32_t message) {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.received++;
    sysexScratch_.clear();
    shortScratch_.clear();
    translator_.translateShort(message, deviceIdLocked(), shortScratch_, sysexScratch_);
    if ((message & 0xF0) == 0xC0) {
        char text[64];
        std::snprintf(text, sizeof(text), "in  Program change %u on channel %u", unsigned((message >> 8) & 0x7F), unsigned(message & 0x0F) + 1);
        logLocked(text + std::string(sysexScratch_.empty() ? "" : " (sent as the part's timbre)"));
    }
    queueLocked(shortScratch_, sysexScratch_, false);
    wake_.notify_all();
}

void MidiPipe::onMidiSysex(const uint8_t* data, size_t length) {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.received++;
    translateSysexLocked(data, length, true);
    wake_.notify_all();
}

void MidiPipe::replyLocked(uint8_t device, uint8_t command) {
    if (reply_ == nullptr) {
        stats_.dropped++;
        if (!warnedNoReply_) logLocked("No reply output: the program's handshake gets no answer (choose \"Replies to\")");
        warnedNoReply_ = true;
        return;
    }
    replies_.push_back({0xF0, 0x41, device, 0x16, command, 0xF7});
    stats_.replies++;
}

void MidiPipe::translateSysexLocked(const uint8_t* data, size_t length, bool fromProgram) {
    if (length < 2 || data[0] != 0xF0 || data[length - 1] != 0xF7) return;
    const bool mt32 = length >= 6 && data[1] == 0x41 && data[3] == 0x16;
    if (fromProgram) logLocked("in  " + describeSysex(data, length));
    if (mt32) {
        const uint8_t device = data[2];
        switch (data[4]) {
        case kWsd:  // The program wants to send data: go ahead
            if (fromProgram) replyLocked(device, kAck);
            return;
        case kDat:
            if (!checksumOk(data, length)) {
                if (fromProgram) replyLocked(device, kErr);  // The program sends the packet again
                logLocked("    bad checksum");
                return;
            }
            if (fromProgram) replyLocked(device, kAck);
            break;  // Translated as a DT1
        case kEod:
            if (fromProgram) {
                replyLocked(device, kAck);
                stats_.handshakes++;
            }
            return;
        case kRqd:  // The program wants data: the unit's would not be the MT-32's
            if (fromProgram) replyLocked(device, kRjc);
            stats_.dropped++;
            return;
        case kRq1:
            stats_.dropped++;
            logLocked("    data request not passed on: the unit's data would not be the MT-32's");
            return;
        case kAck:
        case kErr:
        case kRjc:
            return;  // Replies to transfers the pipe never makes
        default:
            break;
        }
    }
    sysexScratch_.clear();
    shortScratch_.clear();
    translator_.translateSysex(data, length, deviceIdLocked(), sysexScratch_, shortScratch_);
    queueLocked(shortScratch_, sysexScratch_, true);
}

void MidiPipe::queueLocked(const std::vector<uint32_t>& shortMessages, const std::vector<uint8_t>& sysex, bool sysexFirst) {
    auto pushShort = [&] {
        for (uint32_t message : shortMessages) {
            if (settings_.reduceLoad && thinLocked(message)) {
                stats_.thinned++;
                continue;
            }
            Outgoing item;
            item.shortMessage = message;
            queue_.push_back(std::move(item));
        }
    };
    if (!sysexFirst) pushShort();
    forEachMessage(sysex.data(), sysex.size(), [&](const uint8_t* message, size_t length) {
        Outgoing item;
        item.sysex.assign(message, message + length);
        queue_.push_back(std::move(item));
    });
    if (sysexFirst) pushShort();
    stats_.queued = queue_.size();
}

void MidiPipe::forgetControllersLocked() {
    for (std::array<int16_t, 128>& channel : controllers_) channel.fill(-1);
    bends_.fill(-1);
}

// Controller values and pitch bends the unit already has are not needed; one still waiting in the queue (behind a
// SysEx message) is replaced by the newer value. This lightens the unit's MIDI load, which on older D-series firmware
// delays envelope processing.
bool MidiPipe::thinLocked(uint32_t message) {
    const uint32_t status = message & 0xF0, channel = message & 0x0F;
    const int data1 = int((message >> 8) & 0x7F), data2 = int((message >> 16) & 0x7F);
    auto mergeWithWaiting = [&](uint32_t mask) {
        if (queue_.empty() || !queue_.back().sysex.empty() || (queue_.back().shortMessage & mask) != (message & mask)) return false;
        queue_.back().shortMessage = message;  // Not sent yet: the newer value takes its place
        return true;
    };
    if (status == 0xB0) {
        if (data1 == 121 || data1 >= 120) {
            controllers_[channel].fill(-1);  // Resets and mode messages: values are unknown again
            bends_[channel] = -1;
            return false;
        }
        if (data1 >= 96 && data1 <= 101) return false;  // Data increment and (N)RPN selection mean something each time
        if (data1 == 6 || data1 == 38) return false;    // Data entry: always wanted
        if (controllers_[channel][size_t(data1)] == data2) return true;
        controllers_[channel][size_t(data1)] = int16_t(data2);
        return mergeWithWaiting(0xFFFF);
    }
    if (status == 0xE0) {
        const int32_t value = data1 | (data2 << 7);
        if (bends_[channel] == value) return true;
        bends_[channel] = value;
        return mergeWithWaiting(0xFF);
    }
    return false;
}

void MidiPipe::logLocked(const std::string& line) {
    const double seconds = std::chrono::duration<double>(Clock::now() - startTime_).count();
    char stamp[24];
    std::snprintf(stamp, sizeof(stamp), "[%8.3f] ", seconds);
    log_.push_back(stamp + line);
    while (log_.size() > kMaxLogLines) log_.pop_front();
}

MidiPipe::Stats MidiPipe::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

std::vector<std::string> MidiPipe::takeLog() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> lines(log_.begin(), log_.end());
    log_.clear();
    return lines;
}

MidiPipe::Clock::time_point MidiPipe::process(Clock::time_point now) {
    std::lock_guard<std::mutex> sendLock(sendMutex_);
    // Replies first: the program waits for each one before it sends on.
    for (;;) {
        std::vector<uint8_t> reply;
        MidiSender* target = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (replies_.empty()) break;
            reply = std::move(replies_.front());
            replies_.pop_front();
            target = reply_;
        }
        if (target != nullptr) target->sendSysex(reply.data(), reply.size());
    }
    for (;;) {
        Outgoing item;
        MidiSender* target = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.empty()) return Clock::time_point::max();
            if (now < nextSend_) return nextSend_;
            item = std::move(queue_.front());
            queue_.pop_front();
            stats_.queued = queue_.size();
            target = unit_;
            stats_.sent++;
            if (!item.sysex.empty()) {
                // 0.32 ms per byte on a MIDI cable, then a pause for the unit to store the data: the full pause
                // after a 256-byte message, less after shorter ones (at least a tenth).
                const int64_t pause = int64_t(settings_.sysexGapMs) * 1000 * std::clamp<int64_t>(int64_t(item.sysex.size()), 26, 256) / 256;
                nextSend_ = now + std::chrono::microseconds(320 * int64_t(item.sysex.size()) + pause);
                stats_.sysexBytesSent += item.sysex.size();
            }
        }
        if (target == nullptr) continue;
        if (item.sysex.empty()) {
            target->sendShort(item.shortMessage);
        } else {
            target->sendSysex(item.sysex.data(), item.sysex.size());
        }
    }
}

void MidiPipe::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (thread_.joinable()) return;
    stopping_ = false;
    thread_ = std::thread([this] { run(); });
}

void MidiPipe::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!thread_.joinable()) return;
        stopping_ = true;
        wake_.notify_all();
    }
    thread_.join();
}

void MidiPipe::run() {
    for (;;) {
        const Clock::time_point next = process(Clock::now());
        std::unique_lock<std::mutex> lock(mutex_);
        if (stopping_) return;
        if (!replies_.empty()) continue;
        if (next == Clock::time_point::max()) {
            wake_.wait(lock, [&] { return stopping_ || !queue_.empty() || !replies_.empty(); });
        } else {
            wake_.wait_until(lock, next, [&] { return stopping_ || !replies_.empty(); });
        }
        if (stopping_) return;
    }
}

std::vector<uint8_t> translateSysexFile(const std::vector<uint8_t>& data, const PipeSettings& settings,
                                        std::shared_ptr<const Mt32Presets> presets) {
    PipeSettings fileSettings = settings;
    fileSettings.memoryInUnit = true;
    fileSettings.masterVolumeAsVolume = false;
    Mt32Translator translator;
    configureTranslator(translator, fileSettings, std::move(presets));
    const uint8_t device = uint8_t(std::clamp(settings.unitNumber, 17, 32) - 1);
    std::vector<uint8_t> translated;
    std::vector<uint32_t> unused;
    forEachMessage(data.data(), data.size(), [&](const uint8_t* message, size_t length) {
        if (length >= 6 && message[1] == 0x41 && message[3] == 0x16 && message[4] != kDt1 && message[4] != kDat) return;  // Handshake
        translator.translateSysex(message, length, device, translated, unused);
    });
    if (settings.target != Mt32Translator::Target::D20) return translated;

    // The file is for the D-20's memory: its rhythm setup goes to rhythm setup memory (09 00 00), not the temporary area.
    constexpr uint32_t kRhythmTemp = pack(0x03, 0x01, 0x10), kRhythmTempEnd = kRhythmTemp + 85 * 4;
    constexpr uint32_t kRhythmMemory = pack(0x09, 0x00, 0x00);
    std::vector<uint8_t> out;
    std::vector<int16_t> rhythm(85 * 4, -1);
    forEachMessage(translated.data(), translated.size(), [&](const uint8_t* message, size_t length) {
        if (length < 10 || message[4] != kDt1) {
            out.insert(out.end(), message, message + length);
            return;
        }
        const uint32_t address = pack(message[5], message[6], message[7]);
        const size_t count = length - 10;
        if (address + count <= kRhythmTemp || address >= kRhythmTempEnd) {
            out.insert(out.end(), message, message + length);
            return;
        }
        for (size_t i = 0; i < count; i++) {
            const uint32_t a = address + uint32_t(i);
            if (a >= kRhythmTemp && a < kRhythmTempEnd) rhythm[a - kRhythmTemp] = message[8 + i];
        }
    });
    for (size_t start = 0; start < rhythm.size();) {
        if (rhythm[start] < 0) {
            start++;
            continue;
        }
        std::vector<uint8_t> run;
        size_t end = start;
        while (end < rhythm.size() && rhythm[end] >= 0 && run.size() < 256) run.push_back(uint8_t(rhythm[end++]));
        appendDataSet(out, device, kRhythmMemory + uint32_t(start), run.data(), run.size());
        start = end;
    }
    return out;
}

double translateMidiFile(const SmfFile& in, SmfFile& out, const PipeSettings& settings, std::shared_ptr<const Mt32Presets> presets) {
    Mt32Translator translator;
    configureTranslator(translator, settings, std::move(presets));
    const uint8_t device = uint8_t(std::clamp(settings.unitNumber, 17, 32) - 1);
    out = SmfFile();
    out.format = 0;
    out.trackCount = 1;
    out.title = in.title;
    auto addSysex = [&](double time, const uint8_t* data, size_t length) {
        out.events.push_back(SmfEvent{time, 0, uint32_t(out.sysexData.size()), uint32_t(length)});
        out.sysexData.insert(out.sysexData.end(), data, data + length);
    };

    // The power-on setup, each message after the one before has arrived and been stored.
    std::vector<uint8_t> setup;
    std::vector<uint32_t> setupShort;
    translator.powerOn(device, setup, setupShort);
    double time = 0.0;
    forEachMessage(setup.data(), setup.size(), [&](const uint8_t* message, size_t length) {
        addSysex(time, message, length);
        time += 0.00032 * double(length) + 0.001 * double(settings.sysexGapMs);
    });
    for (uint32_t message : setupShort) out.events.push_back(SmfEvent{time, message, 0, 0});
    const double offset = time + 0.1;

    std::vector<uint8_t> sysex;
    std::vector<uint32_t> shortMessages;
    for (const SmfEvent& event : in.events) {
        sysex.clear();
        shortMessages.clear();
        if (event.shortMessage != 0) {
            translator.translateShort(event.shortMessage, device, shortMessages, sysex);
            for (uint32_t message : shortMessages) out.events.push_back(SmfEvent{event.time + offset, message, 0, 0});
            forEachMessage(sysex.data(), sysex.size(), [&](const uint8_t* message, size_t length) { addSysex(event.time + offset, message, length); });
        } else if (event.sysexOffset + event.sysexLength <= in.sysexData.size()) {
            translator.translateSysex(&in.sysexData[event.sysexOffset], event.sysexLength, device, sysex, shortMessages);
            forEachMessage(sysex.data(), sysex.size(), [&](const uint8_t* message, size_t length) { addSysex(event.time + offset, message, length); });
            for (uint32_t message : shortMessages) out.events.push_back(SmfEvent{event.time + offset, message, 0, 0});
        }
    }
    out.duration = in.duration + offset;
    return offset;
}
