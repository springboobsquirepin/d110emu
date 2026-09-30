#include "UnitLink.h"

#include <algorithm>
#include <cstdio>

#include "RolandSysex.h"

namespace {

constexpr size_t kMaxLogLines = 200;
constexpr size_t kMaxReceived = 4096;

std::string hexAddress(uint32_t packed) {
    const uint32_t address = RolandSysex::unpack(packed);
    char text[16];
    std::snprintf(text, sizeof(text), "%02X %02X %02X", unsigned(address >> 16), unsigned((address >> 8) & 0x7F), unsigned(address & 0x7F));
    return text;
}

}  // namespace

UnitLink::UnitLink() = default;

UnitLink::~UnitLink() {
    stop();
}

void UnitLink::setOutput(MidiSender* output) {
    std::lock_guard<std::mutex> sendLock(sendMutex_);  // Not while the sender uses the old one
    std::lock_guard<std::mutex> lock(mutex_);
    output_ = output;
}

void UnitLink::setDevice(uint8_t device) {
    std::lock_guard<std::mutex> lock(mutex_);
    device_ = uint8_t(device & 0x7F);
}

uint8_t UnitLink::device() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return device_;
}

void UnitLink::setPause(int milliseconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    pauseMs_ = std::clamp(milliseconds, 0, 500);
}

void UnitLink::sendData(uint32_t packedAddress, const uint8_t* data, size_t length, bool merge) {
    if (length == 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (merge) {
        // The newest waiting message that touches these bytes takes the data, if it is the same kind of change;
        // anything else touching them (a whole tone) must not be overtaken.
        for (auto it = queue_.rbegin(); it != queue_.rend(); ++it) {
            if (it->length == 0) continue;  // Short message
            const bool overlaps = it->address < packedAddress + length && packedAddress < it->address + it->length;
            if (!overlaps) continue;
            if (it->merge && it->address == packedAddress && it->length == length) {
                it->sysex = RolandSysex::dataSet(device_, packedAddress, data, length);
                stats_.merged++;
                return;
            }
            break;
        }
    }
    Outgoing item;
    item.sysex = RolandSysex::dataSet(device_, packedAddress, data, length);
    item.address = packedAddress;
    item.length = length;
    item.merge = merge;
    queue_.push_back(std::move(item));
    stats_.queued = queue_.size();
    work_ = true;
    wake_.notify_all();
}

void UnitLink::request(uint32_t packedAddress, uint32_t size) {
    if (size == 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    requests_.push_back({packedAddress, size});
    stats_.requestsWaiting = requests_.size();
    work_ = true;
    wake_.notify_all();
}

void UnitLink::sendShort(uint32_t message) {
    std::lock_guard<std::mutex> lock(mutex_);
    Outgoing item;
    item.shortMessage = message;
    queue_.push_back(std::move(item));
    stats_.queued = queue_.size();
    work_ = true;
    wake_.notify_all();
}

void UnitLink::cancelRequests() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (requestOut_ && !requests_.empty()) {
        const Request out = requests_.front();
        requests_.clear();
        requests_.push_back(out);  // Its answer may still come
    } else {
        requests_.clear();
    }
    stats_.requestsWaiting = requests_.size();
}

void UnitLink::onMidiShortMessage(uint32_t message) {
    (void)message;
}

void UnitLink::onMidiSysex(const uint8_t* data, size_t length) {
    RolandSysex::DataMessage message;
    if (!RolandSysex::parseDataSet(data, length, message) || message.command != RolandSysex::kDt1) return;
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.received++;
    if (received_.size() < kMaxReceived) {
        received_.push_back({message.address, std::vector<uint8_t>(message.data, message.data + message.length)});
    }
    // A request is answered once its last byte has come (a unit may split a long answer into several messages).
    if (requestOut_ && !requests_.empty()) {
        const Request& out = requests_.front();
        const uint32_t last = out.address + out.size - 1;
        if (message.address <= last && message.address + message.length > last) {
            requests_.pop_front();
            requestOut_ = false;
            stats_.answered++;
            stats_.requestsWaiting = requests_.size();
            work_ = true;
            wake_.notify_all();
        }
    }
}

std::vector<UnitLink::Received> UnitLink::takeReceived() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Received> out;
    out.swap(received_);
    return out;
}

UnitLink::Stats UnitLink::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

std::vector<std::string> UnitLink::takeLog() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> lines(log_.begin(), log_.end());
    log_.clear();
    return lines;
}

void UnitLink::logLocked(const std::string& line) {
    log_.push_back(line);
    while (log_.size() > kMaxLogLines) log_.pop_front();
}

UnitLink::Clock::time_point UnitLink::process(Clock::time_point now) {
    std::lock_guard<std::mutex> sendLock(sendMutex_);
    for (;;) {
        std::vector<uint8_t> sysex;
        uint32_t shortMessage = 0;
        MidiSender* target = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (requestOut_ && now - requestSent_ >= std::chrono::milliseconds(kRequestTimeoutMs)) {
                if (!requests_.empty()) {
                    logLocked("No answer from the unit to a request for " + hexAddress(requests_.front().address) +
                              ". Check its MIDI OUT connection and unit number (on a D-10/D-20: MIDI Exclusive on).");
                    requests_.pop_front();
                }
                requestOut_ = false;
                stats_.unanswered++;
                stats_.requestsWaiting = requests_.size();
            }
            const Clock::time_point requestDue = requestOut_ ? requestSent_ + std::chrono::milliseconds(kRequestTimeoutMs) : Clock::time_point::max();
            if (now < nextSend_) return std::min(nextSend_, requestDue);
            if (!queue_.empty()) {
                Outgoing& front = queue_.front();
                if (front.merge && now < nextMerged_) return std::min(nextMerged_, requestDue);
                const bool merged = front.merge;
                sysex = std::move(front.sysex);
                shortMessage = front.shortMessage;
                queue_.pop_front();
                stats_.queued = queue_.size();
                if (!sysex.empty()) {
                    // 0.32 ms per byte on a MIDI cable, then a pause for the unit to take the data in: the full pause after
                    // a 256-byte message (and between parameter changes), less after shorter ones (at least a tenth).
                    const auto transmission = std::chrono::microseconds(320 * int64_t(sysex.size()));
                    const int64_t pause = int64_t(pauseMs_) * 1000 * std::clamp<int64_t>(int64_t(sysex.size()), 26, 256) / 256;
                    nextSend_ = now + transmission + std::chrono::microseconds(pause);
                    if (merged) nextMerged_ = now + transmission + std::chrono::milliseconds(pauseMs_);
                    stats_.sysexBytes += sysex.size();
                }
            } else if (!requestOut_ && !requests_.empty()) {
                const Request& next = requests_.front();
                sysex = RolandSysex::request(device_, next.address, next.size);
                requestOut_ = true;
                requestSent_ = now;
                nextSend_ = now + std::chrono::microseconds(320 * int64_t(sysex.size()));
                stats_.sysexBytes += sysex.size();
            } else {
                return requestDue;
            }
            stats_.sent++;
            target = output_;
        }
        if (target == nullptr) continue;
        if (sysex.empty()) {
            target->sendShort(shortMessage);
        } else {
            target->sendSysex(sysex.data(), sysex.size());
        }
    }
}

void UnitLink::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (thread_.joinable()) return;
    stopping_ = false;
    thread_ = std::thread([this] { run(); });
}

void UnitLink::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!thread_.joinable()) return;
        stopping_ = true;
        wake_.notify_all();
    }
    thread_.join();
}

void UnitLink::run() {
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) return;
            work_ = false;
        }
        const Clock::time_point next = process(Clock::now());
        std::unique_lock<std::mutex> lock(mutex_);
        if (stopping_) return;
        if (work_) continue;
        if (next == Clock::time_point::max()) {
            wake_.wait(lock, [&] { return stopping_ || work_; });
        } else {
            wake_.wait_until(lock, next, [&] { return stopping_ || work_; });
        }
    }
}
