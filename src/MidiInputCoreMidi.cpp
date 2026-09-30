// CoreMIDI input (macOS). The program is a MIDI client named after it (midiProgramName(): "D110Emu", in
// MidiCoreMidi.h), with a virtual MIDI destination of the same name that other programs can send to (they list it among
// their MIDI outputs); opening a port connects one of the system's MIDI sources to the client's input port. MIDI
// interfaces (USB, Bluetooth), network sessions, the IAC Driver's buses and other programs' virtual outputs are all
// sources.
//
// Messages come as Universal MIDI Packets in the MIDI 1.0 protocol (CoreMIDI converts what a source sends), on
// CoreMIDI's own thread, and go through MidiStreamParser as the bytes of the ALSA input do.

#include "MidiInput.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>

#include "MidiCoreMidi.h"
#include "MidiStreamParser.h"

namespace {

struct SourceEntry {
    std::string name;  // As CoreMIDI shows it ("UM-ONE", "IAC Driver Bus 1"), with " #2" on a repeated name
    MIDIEndpointRef endpoint = 0;
    uint32_t id = 0;   // Its unique ID, which stays the same when it goes and comes back
};

// The sources as CoreMIDI has them now, in its order; not those that are offline (unplugged, but remembered), nor the
// program's own.
std::vector<SourceEntry> listSources() {
    std::vector<SourceEntry> entries;
    std::map<std::string, int> seen;
    const ItemCount count = MIDIGetNumberOfSources();
    for (ItemCount i = 0; i < count; i++) {
        const MIDIEndpointRef endpoint = MIDIGetSource(i);
        if (endpoint == 0 || CoreMidi::isOffline(endpoint) || CoreMidi::isOwnEndpoint(endpoint)) continue;
        SourceEntry entry;
        entry.name = CoreMidi::endpointName(endpoint, "MIDI source");
        const int occurrence = ++seen[entry.name];
        if (occurrence > 1) entry.name += " #" + std::to_string(occurrence);
        entry.endpoint = endpoint;
        entry.id = uint32_t(CoreMidi::uniqueId(endpoint));
        entries.push_back(entry);
    }
    return entries;
}

// The 32-bit words of each Universal MIDI Packet message type.
constexpr uint32_t kUmpWords[16] = {1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4};

// What the receive blocks reach. The blocks hold it, so it stays while CoreMIDI may still call one; the manager cuts
// it off from the sink before it goes.
struct Receiver {
    struct Source {
        MidiStreamParser messages;  // Channel and system messages
        MidiStreamParser sysex;     // SysEx apart: other messages may come between its packets
        uint32_t count = 0;         // Short messages and SysEx messages
    };

    std::mutex mutex;
    MidiInputSink* sink = nullptr;
    std::map<uint32_t, Source> sources;  // By the source's unique ID; 0: programs sending to the destination

    void receive(const MIDIEventList* list, uint32_t id) {
        std::lock_guard<std::mutex> lock(mutex);
        if (sink == nullptr) return;
        Source& source = sources[id];
        const MIDIEventPacket* packet = &list->packet[0];
        for (UInt32 p = 0; p < list->numPackets; p++) {
            const UInt32* words = packet->words;
            for (UInt32 i = 0; i < packet->wordCount;) {
                const UInt32 word = words[i];
                const uint32_t type = word >> 28;
                const uint32_t size = kUmpWords[type];
                if (i + size > packet->wordCount) break;
                if (type == 0x1 || type == 0x2) {
                    // System messages (real-time and common) and MIDI 1.0 channel messages: a status byte and its data.
                    const uint8_t bytes[3] = {uint8_t(word >> 16), uint8_t(word >> 8), uint8_t(word)};
                    if (bytes[0] >= 0x80 && bytes[0] != 0xF0 && bytes[0] != 0xF7) {
                        source.count += source.messages.feed(*sink, bytes, size_t(1 + MidiStreamParser::dataBytes(bytes[0])));
                    }
                } else if (type == 0x3) {
                    // SysEx in pieces of up to six bytes: the whole message (0), its start (1), more (2) or its end (3).
                    const uint32_t form = (word >> 20) & 0xF;
                    const uint32_t count = std::min<uint32_t>((word >> 16) & 0xF, 6);
                    uint8_t bytes[8];
                    size_t length = 0;
                    if (form == 0 || form == 1) bytes[length++] = 0xF0;
                    for (uint32_t b = 0; b < count; b++) {
                        const UInt32 from = b < 2 ? word : words[i + 1];
                        const uint32_t shift = b < 2 ? 8 * (1 - b) : 8 * (5 - b);
                        bytes[length++] = uint8_t((from >> shift) & 0x7F);
                    }
                    if (form == 0 || form == 3) bytes[length++] = 0xF7;
                    source.count += source.sysex.feed(*sink, bytes, length);
                }
                i += size;
            }
            packet = reinterpret_cast<const MIDIEventPacket*>(&packet->words[packet->wordCount]);
        }
    }

    uint32_t count(uint32_t id) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = sources.find(id);
        return found != sources.end() ? found->second.count : 0;
    }
};

void* connectionTag(uint32_t id) {
    return reinterpret_cast<void*>(uintptr_t(id));
}

}  // namespace

struct MidiInputManager::Port {
    std::string name;
    MIDIEndpointRef endpoint = 0;
    uint32_t id = 0;
    std::atomic<bool> gone{false};  // It left the system's sources (unplugged): connected anew when it comes back
};

struct MidiInputManager::System {
    std::shared_ptr<Receiver> receiver;
    MIDIPortRef port = 0;             // The client's input port, which the sources are connected to
    MIDIEndpointRef destination = 0;  // What other programs send to (with ownPort)
    MIDIUniqueID destinationId = 0;
    std::string error;
};

MidiInputManager::MidiInputManager(MidiInputSink& sink, bool ownPort) : sink_(sink), system_(new System) {
    System& system = *system_;
    const CoreMidi::Client* client = CoreMidi::client(system.error);
    if (client == nullptr) return;
    system.receiver = std::make_shared<Receiver>();
    system.receiver->sink = &sink_;
    const std::shared_ptr<Receiver> receiver = system.receiver;
    OSStatus status = MIDIInputPortCreateWithProtocol(client->ref, CFSTR("MIDI In"), kMIDIProtocol_1_0, &system.port,
                                                      ^(const MIDIEventList* list, void* tag) {
                                                          receiver->receive(list, uint32_t(uintptr_t(tag)));
                                                      });
    if (status != noErr) {
        system.port = 0;
        system.error = "Cannot make a CoreMIDI input port (error " + std::to_string(int(status)) + ")";
        return;
    }
    if (!ownPort) return;
    CFStringRef name = CFStringCreateWithCString(kCFAllocatorDefault, midiProgramName().c_str(), kCFStringEncodingUTF8);
    status = MIDIDestinationCreateWithProtocol(client->ref, name, kMIDIProtocol_1_0, &system.destination,
                                               ^(const MIDIEventList* list, void*) {
                                                   receiver->receive(list, 0);
                                               });
    if (name != nullptr) CFRelease(name);
    if (status != noErr) {
        system.destination = 0;  // The sources still work
        return;
    }
    MIDIObjectSetIntegerProperty(system.destination, kMIDIPropertyUniqueID, CoreMidi::programUniqueId());
    system.destinationId = CoreMidi::uniqueId(system.destination);  // Another, when another instance has that one
    CoreMidi::OwnEndpoints& own = CoreMidi::ownEndpoints();
    std::lock_guard<std::mutex> lock(own.mutex);
    own.ids.insert(system.destinationId);
}

MidiInputManager::~MidiInputManager() {
    System& system = *system_;
    if (system.receiver) {
        // A block under way finishes first; later ones find no sink.
        std::lock_guard<std::mutex> lock(system.receiver->mutex);
        system.receiver->sink = nullptr;
    }
    closeAll();
    if (system.destination != 0) {
        {
            CoreMidi::OwnEndpoints& own = CoreMidi::ownEndpoints();
            std::lock_guard<std::mutex> lock(own.mutex);
            own.ids.erase(system.destinationId);
        }
        MIDIEndpointDispose(system.destination);
    }
    if (system.port != 0) MIDIPortDispose(system.port);
}

std::vector<std::string> MidiInputManager::listPorts() const {
    std::vector<std::string> names;
    if (system_->port == 0) return names;
    const std::vector<SourceEntry> sources = listSources();
    for (const SourceEntry& entry : sources) names.push_back(entry.name);
    // An open source that is no longer there was unplugged, or its program quit.
    for (const auto& port : ports_) {
        if (std::none_of(sources.begin(), sources.end(), [&](const SourceEntry& entry) { return entry.id == port->id; })) port->gone = true;
    }
    return names;
}

bool MidiInputManager::open(const std::string& name, std::string& error) {
    System& system = *system_;
    if (system.port == 0) {
        error = system.error;
        return false;
    }
    // A source that left is connected anew (its old connection, if CoreMIDI kept it, goes first).
    for (auto it = ports_.begin(); it != ports_.end();) {
        if ((*it)->gone) {
            MIDIPortDisconnectSource(system.port, (*it)->endpoint);
            it = ports_.erase(it);
        } else {
            ++it;
        }
    }
    if (isOpen(name)) return true;
    const std::vector<SourceEntry> sources = listSources();
    const auto found = std::find_if(sources.begin(), sources.end(), [&](const SourceEntry& entry) { return entry.name == name; });
    if (found == sources.end()) {
        error = "MIDI input not found: " + name;
        return false;
    }
    const OSStatus status = MIDIPortConnectSource(system.port, found->endpoint, connectionTag(found->id));
    if (status != noErr) {
        error = "Cannot open " + name + " (CoreMIDI error " + std::to_string(int(status)) + ")";
        return false;
    }
    std::unique_ptr<Port> port(new Port);
    port->name = name;
    port->endpoint = found->endpoint;
    port->id = found->id;
    ports_.push_back(std::move(port));
    return true;
}

void MidiInputManager::close(const std::string& name) {
    const auto it = std::find_if(ports_.begin(), ports_.end(), [&](const std::unique_ptr<Port>& p) { return p->name == name; });
    if (it == ports_.end()) return;
    MIDIPortDisconnectSource(system_->port, (*it)->endpoint);
    ports_.erase(it);
}

void MidiInputManager::closeAll() {
    while (!ports_.empty()) close(ports_.front()->name);
}

bool MidiInputManager::isOpen(const std::string& name) const {
    return std::any_of(ports_.begin(), ports_.end(), [&](const std::unique_ptr<Port>& p) { return p->name == name && !p->gone; });
}

std::vector<std::string> MidiInputManager::openPorts() const {
    std::vector<std::string> names;
    for (const auto& port : ports_) {
        if (!port->gone) names.push_back(port->name);
    }
    return names;
}

uint32_t MidiInputManager::messageCount(const std::string& name) const {
    for (const auto& port : ports_) {
        if (port->name == name) return system_->receiver ? system_->receiver->count(port->id) : 0;
    }
    return 0;
}

std::string MidiInputManager::ownPortName() const {
    return system_->destination != 0 ? midiProgramName() : std::string();
}

std::string MidiInputManager::ownPortHint() const {
    return system_->destination != 0 ? "it is among their MIDI outputs" : std::string();
}

std::string MidiInputManager::systemError() const {
    return system_->error;
}
