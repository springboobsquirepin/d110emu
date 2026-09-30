// CoreMIDI output (macOS 11 and later): an output port of the process's MIDI client (MidiCoreMidi.h) that sends to one
// of the system's MIDI destinations (MIDI interfaces, network sessions, the IAC Driver's buses, other programs' virtual
// inputs), as Universal MIDI Packets in the MIDI 1.0 protocol (MIDISendEventList), which CoreMIDI turns into bytes for
// MIDI 1.0 devices.

#include "MidiOutput.h"

#include <algorithm>
#include <map>
#include <mutex>

#include "MidiCoreMidi.h"
#include "MidiStreamParser.h"

namespace {

struct DestinationEntry {
    std::string name;  // As CoreMIDI shows it, with " #2" on a repeated name
    MIDIEndpointRef endpoint = 0;
    MIDIUniqueID id = 0;  // Stays the same when the destination goes and comes back
};

// The destinations as CoreMIDI has them now, in its order; not those that are offline (unplugged, but remembered), nor
// the program's own (its MIDI input).
std::vector<DestinationEntry> listDestinations() {
    std::vector<DestinationEntry> entries;
    std::map<std::string, int> seen;
    const ItemCount count = MIDIGetNumberOfDestinations();
    for (ItemCount i = 0; i < count; i++) {
        const MIDIEndpointRef endpoint = MIDIGetDestination(i);
        if (endpoint == 0 || CoreMidi::isOffline(endpoint) || CoreMidi::isOwnEndpoint(endpoint)) continue;
        DestinationEntry entry;
        entry.name = CoreMidi::endpointName(endpoint, "MIDI destination");
        const int occurrence = ++seen[entry.name];
        if (occurrence > 1) entry.name += " #" + std::to_string(occurrence);
        entry.endpoint = endpoint;
        entry.id = CoreMidi::uniqueId(endpoint);
        entries.push_back(entry);
    }
    return entries;
}

}  // namespace

struct MidiOutputPort::Impl {
    std::mutex mutex;              // The sender thread sends while the interface checks the port
    std::string ownName;
    MIDIPortRef port = 0;          // The client's output port, while open
    MIDIEndpointRef endpoint = 0;  // Where it sends
    MIDIUniqueID id = 0;
    std::string name;              // What was opened
    bool connected = false;

    // Sends messages of `size` words each (Universal MIDI Packets), as few event lists as they fit in.
    bool send(const uint32_t* words, size_t count, size_t size) {
        alignas(8) uint8_t buffer[1024];
        MIDIEventList* list = reinterpret_cast<MIDIEventList*>(buffer);
        MIDIEventPacket* packet = MIDIEventListInit(list, kMIDIProtocol_1_0);
        bool sent = true;
        for (size_t i = 0; i + size <= count; i += size) {
            // Time stamp 0: at once. Messages with the same time stamp share a packet while it has room.
            MIDIEventPacket* next = MIDIEventListAdd(list, sizeof(buffer), packet, 0, size, words + i);
            if (next == nullptr) {
                // The list is full: out with it, then on with a new one.
                if (MIDISendEventList(port, endpoint, list) != noErr) sent = false;
                packet = MIDIEventListInit(list, kMIDIProtocol_1_0);
                next = MIDIEventListAdd(list, sizeof(buffer), packet, 0, size, words + i);
                if (next == nullptr) return false;
            }
            packet = next;
        }
        if (list->numPackets > 0 && MIDISendEventList(port, endpoint, list) != noErr) sent = false;
        return sent;
    }
};

MidiOutputPort::MidiOutputPort(const std::string& ownName) : impl_(new Impl) {
    impl_->ownName = ownName;
}

MidiOutputPort::~MidiOutputPort() {
    close();
}

std::vector<std::string> MidiOutputPort::listPorts() {
    std::string error;
    if (CoreMidi::client(error) == nullptr) return {};
    std::vector<std::string> names;
    for (const DestinationEntry& entry : listDestinations()) names.push_back(entry.name);
    return names;
}

std::string MidiOutputPort::systemError() {
    std::string error;
    CoreMidi::client(error);
    return error;
}

bool MidiOutputPort::open(const std::string& name, std::string& error) {
    close();
    const CoreMidi::Client* client = CoreMidi::client(error);
    if (client == nullptr) return false;
    const std::vector<DestinationEntry> entries = listDestinations();
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const DestinationEntry& entry) { return entry.name == name; });
    if (found == entries.end()) {
        error = "MIDI output not found: " + name;
        return false;
    }
    Impl& impl = *impl_;
    std::lock_guard<std::mutex> lock(impl.mutex);
    CFStringRef portName = CFStringCreateWithCString(kCFAllocatorDefault, impl.ownName.c_str(), kCFStringEncodingUTF8);
    const OSStatus status = MIDIOutputPortCreate(client->ref, portName, &impl.port);
    if (portName != nullptr) CFRelease(portName);
    if (status != noErr) {
        impl.port = 0;
        error = "Cannot make a CoreMIDI output port (error " + std::to_string(int(status)) + ")";
        return false;
    }
    impl.endpoint = found->endpoint;
    impl.id = found->id;
    impl.name = name;
    impl.connected = true;
    return true;
}

void MidiOutputPort::close() {
    Impl& impl = *impl_;
    std::lock_guard<std::mutex> lock(impl.mutex);
    if (impl.port == 0) return;
    MIDIPortDispose(impl.port);
    impl.port = 0;
    impl.endpoint = 0;
    impl.name.clear();
    impl.connected = false;
}

bool MidiOutputPort::isOpen() const {
    return impl_->port != 0;
}

std::string MidiOutputPort::name() const {
    return impl_->name;
}

bool MidiOutputPort::reconnect() {
    Impl& impl = *impl_;
    if (impl.port == 0) return false;
    // By its unique ID: a device plugged in again keeps it (and CoreMIDI often its endpoint too); a program's virtual
    // destination made anew under the same name gets another, and is found by name.
    const std::vector<DestinationEntry> entries = listDestinations();
    auto found = std::find_if(entries.begin(), entries.end(), [&](const DestinationEntry& entry) { return entry.id == impl.id; });
    if (found == entries.end()) {
        found = std::find_if(entries.begin(), entries.end(), [&](const DestinationEntry& entry) { return entry.name == impl.name; });
    }
    std::lock_guard<std::mutex> lock(impl.mutex);
    if (found == entries.end()) {
        impl.connected = false;
        return false;
    }
    impl.endpoint = found->endpoint;
    impl.id = found->id;
    impl.connected = true;
    return true;
}

bool MidiOutputPort::connected() const {
    return impl_->connected;
}

bool MidiOutputPort::sendShort(uint32_t message) {
    const uint32_t status = message & 0xFF;
    if (status < 0x80 || status == 0xF0 || status == 0xF7) return false;
    const int dataBytes = MidiStreamParser::dataBytes(uint8_t(status));
    const uint32_t data1 = dataBytes >= 1 ? (message >> 8) & 0x7F : 0;
    const uint32_t data2 = dataBytes >= 2 ? (message >> 16) & 0x7F : 0;
    // A MIDI 1.0 channel voice message (type 2) or a system message (type 1), group 0.
    const uint32_t word = (status >= 0xF0 ? 0x10000000u : 0x20000000u) | status << 16 | data1 << 8 | data2;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->port == 0) return false;
    return impl_->send(&word, 1, 1);
}

bool MidiOutputPort::sendSysex(const uint8_t* data, size_t length) {
    if (length < 2 || data[0] != 0xF0) return false;
    // The bytes between F0 and F7 in pieces of up to six (7-bit SysEx, type 3, two words each): the whole message in
    // one (0), or its start (1), more (2) and its end (3).
    const uint8_t* payload = data + 1;
    const size_t size = length - 1 - (data[length - 1] == 0xF7 ? 1 : 0);
    std::vector<uint32_t> words;
    words.reserve((size / 6 + 1) * 2);
    for (size_t start = 0; start == 0 || start < size; start += 6) {
        const size_t count = std::min<size_t>(6, size - start);
        const bool first = start == 0;
        const bool last = start + count >= size;
        const uint32_t form = first && last ? 0 : first ? 1 : last ? 3 : 2;
        uint8_t bytes[6] = {};
        for (size_t i = 0; i < count; i++) bytes[i] = payload[start + i] & 0x7F;
        words.push_back(0x30000000u | form << 20 | uint32_t(count) << 16 | uint32_t(bytes[0]) << 8 | bytes[1]);
        words.push_back(uint32_t(bytes[2]) << 24 | uint32_t(bytes[3]) << 16 | uint32_t(bytes[4]) << 8 | bytes[5]);
        if (size == 0) break;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->port == 0) return false;
    return impl_->send(words.data(), words.size(), 2);
}
