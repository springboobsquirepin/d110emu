#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Roland exclusive messages for the LA synthesizers (model ID 16H: D-110, D-10, D-20, MT-32): one-way data set (DT1)
// and request (RQ1), and the 7-bit addresses they carry. Addresses are either SysEx addresses as written in the
// manuals (0x040176 = 04 01 76) or "packed" 21-bit numbers ((a1 << 14) | (a2 << 7) | a3), which can be added to.
namespace RolandSysex {

constexpr uint8_t kRoland = 0x41;
constexpr uint8_t kModelLA = 0x16;
constexpr uint8_t kRq1 = 0x11, kDt1 = 0x12, kDat = 0x42;

constexpr uint32_t pack(uint32_t sysexAddress) {
    return (((sysexAddress >> 16) & 0x7F) << 14) | (((sysexAddress >> 8) & 0x7F) << 7) | (sysexAddress & 0x7F);
}

constexpr uint32_t unpack(uint32_t packed) {
    return (((packed >> 14) & 0x7F) << 16) | (((packed >> 7) & 0x7F) << 8) | (packed & 0x7F);
}

// Roland's checksum: the 7-bit value that makes address, data (or size) and checksum add up to a multiple of 128.
inline uint8_t checksum(const uint8_t* bytes, size_t length) {
    unsigned sum = 0;
    for (size_t i = 0; i < length; i++) sum += bytes[i];
    return uint8_t((128 - sum % 128) % 128);
}

// F0 41 dev 16 12 <address> <data> <sum> F7, appended to `out`.
inline void appendDataSet(std::vector<uint8_t>& out, uint8_t device, uint32_t packedAddress, const uint8_t* data, size_t length) {
    const size_t start = out.size();
    const uint8_t header[] = {0xF0, kRoland, device, kModelLA, kDt1, uint8_t((packedAddress >> 14) & 0x7F),
                              uint8_t((packedAddress >> 7) & 0x7F), uint8_t(packedAddress & 0x7F)};
    out.insert(out.end(), header, header + sizeof(header));
    out.insert(out.end(), data, data + length);
    out.push_back(checksum(out.data() + start + 5, out.size() - start - 5));
    out.push_back(0xF7);
}

inline std::vector<uint8_t> dataSet(uint8_t device, uint32_t packedAddress, const uint8_t* data, size_t length) {
    std::vector<uint8_t> out;
    appendDataSet(out, device, packedAddress, data, length);
    return out;
}

// F0 41 dev 16 11 <address> <size> <sum> F7: asks the unit to send `size` bytes from `packedAddress` as DT1.
inline std::vector<uint8_t> request(uint8_t device, uint32_t packedAddress, uint32_t size) {
    std::vector<uint8_t> out = {0xF0, kRoland, device, kModelLA, kRq1,
                                uint8_t((packedAddress >> 14) & 0x7F), uint8_t((packedAddress >> 7) & 0x7F), uint8_t(packedAddress & 0x7F),
                                uint8_t((size >> 14) & 0x7F), uint8_t((size >> 7) & 0x7F), uint8_t(size & 0x7F)};
    out.push_back(checksum(out.data() + 5, 6));
    out.push_back(0xF7);
    return out;
}

// A DT1 or DAT message for model 16H with a correct checksum: its device ID, packed address and data.
struct DataMessage {
    uint8_t device = 0;
    uint8_t command = 0;
    uint32_t address = 0;  // Packed
    const uint8_t* data = nullptr;
    size_t length = 0;
};

inline bool parseDataSet(const uint8_t* message, size_t length, DataMessage& out) {
    if (length < 11 || message[0] != 0xF0 || message[1] != kRoland || message[3] != kModelLA || message[length - 1] != 0xF7) return false;
    if (message[4] != kDt1 && message[4] != kDat) return false;
    unsigned sum = 0;
    for (size_t i = 5; i + 1 < length; i++) {
        if (message[i] > 0x7F) return false;
        sum += message[i];
    }
    if (sum % 128 != 0) return false;
    out.device = message[2];
    out.command = message[4];
    out.address = (uint32_t(message[5]) << 14) | (uint32_t(message[6]) << 7) | message[7];
    out.data = message + 8;
    out.length = length - 10;
    return true;
}

// Calls `f(message, length)` for every complete F0 ... F7 message in a byte stream (a .syx file).
template <class F>
void forEachMessage(const uint8_t* data, size_t size, F&& f) {
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

}  // namespace RolandSysex
