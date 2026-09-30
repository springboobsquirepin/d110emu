// ALSA sequencer MIDI output (Linux). The program has one output client, named after it (midiProgramName()), with a port
// for each open output, connected to the port it sends to: a MIDI interface's (USB, the kernel's serial and gadget
// drivers), or another program's input. Other programs may connect to those ports as well (aseqdump -p, to watch what
// goes to a unit). The client is made when first needed and kept for the process, as the ALSA library is (MidiAlsa.h).

#include "MidiOutput.h"

#include <algorithm>
#include <cerrno>
#include <mutex>

#include "MidiAlsa.h"
#include "MidiStreamParser.h"

namespace {

// The process's output client, which its output ports and the listing of the destinations share.
struct OutputClient {
    std::mutex mutex;  // A sender thread sends while the interface lists ports and checks them
    const Alsa::Api* api = nullptr;
    Alsa::Seq* seq = nullptr;
    int client = -1;
    std::string error;
};

// Made once; never closed (ports of static objects may outlive anything destroyed at exit).
OutputClient& outputClient() {
    static OutputClient* const client = [] {
        OutputClient* made = new OutputClient;
        made->api = Alsa::load(made->error);
        if (made->api == nullptr) return made;
        // Blocking: a burst waits for room in the sequencer rather than losing messages.
        const int result = made->api->seqOpen(&made->seq, "default", Alsa::kOpenOutput, 0);
        if (result < 0) {
            made->seq = nullptr;
            made->error = "Cannot open the ALSA sequencer: " + std::string(made->api->strerror(result));
            if (result == -EACCES || result == -EPERM) made->error += " (is this user in the \"audio\" group?)";
            return made;
        }
        made->api->seqSetClientName(made->seq, midiProgramName().c_str());
        made->client = made->api->seqClientId(made->seq);
        return made;
    }();
    return *client;
}

std::vector<Alsa::PortEntry> destinations(OutputClient& client) {
    if (client.seq == nullptr) return {};
    return Alsa::listPorts(*client.api, client.seq, Alsa::kCapWrite | Alsa::kCapSubsWrite, client.client);
}

}  // namespace

struct MidiOutputPort::Impl {
    std::string ownName;               // Our port's, as other programs see it
    int port = -1;                     // Ours in the output client, while open
    Alsa::Address destination;
    std::string name;                  // What was opened
    Alsa::MidiEvent* encoder = nullptr;
    bool connected = false;
};

MidiOutputPort::MidiOutputPort(const std::string& ownName) : impl_(new Impl) {
    impl_->ownName = ownName;
}

MidiOutputPort::~MidiOutputPort() {
    close();
}

std::vector<std::string> MidiOutputPort::listPorts() {
    OutputClient& client = outputClient();
    std::lock_guard<std::mutex> lock(client.mutex);
    std::vector<std::string> names;
    for (const Alsa::PortEntry& entry : destinations(client)) names.push_back(entry.name);
    return names;
}

std::string MidiOutputPort::systemError() {
    return outputClient().error;
}

bool MidiOutputPort::open(const std::string& name, std::string& error) {
    close();
    OutputClient& client = outputClient();
    std::lock_guard<std::mutex> lock(client.mutex);
    if (client.seq == nullptr) {
        error = client.error;
        return false;
    }
    const Alsa::Api& api = *client.api;
    const std::vector<Alsa::PortEntry> entries = destinations(client);
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const Alsa::PortEntry& entry) { return entry.name == name; });
    if (found == entries.end()) {
        error = "MIDI output not found: " + name;
        return false;
    }
    Impl& impl = *impl_;
    impl.port = api.seqCreateSimplePort(client.seq, impl.ownName.c_str(), Alsa::kCapRead | Alsa::kCapSubsRead,
                                        Alsa::kTypeMidiGeneric | Alsa::kTypeApplication);
    if (impl.port < 0) {
        error = "Cannot make an ALSA sequencer port: " + std::string(api.strerror(impl.port));
        impl.port = -1;
        return false;
    }
    int result = api.seqConnectTo(client.seq, impl.port, found->address.client, found->address.port);
    if (result >= 0) result = api.midiEventNew(256, &impl.encoder);  // SysEx goes in pieces of up to 256 bytes
    if (result < 0) {
        error = "Cannot open " + name + ": " + api.strerror(result);
        api.seqDeleteSimplePort(client.seq, impl.port);
        impl.port = -1;
        impl.encoder = nullptr;
        return false;
    }
    impl.destination = found->address;
    impl.name = name;
    impl.connected = true;
    return true;
}

void MidiOutputPort::close() {
    Impl& impl = *impl_;
    if (impl.port < 0) return;
    OutputClient& client = outputClient();
    std::lock_guard<std::mutex> lock(client.mutex);
    client.api->seqDisconnectTo(client.seq, impl.port, impl.destination.client, impl.destination.port);  // Gone already, maybe
    client.api->seqDeleteSimplePort(client.seq, impl.port);
    client.api->midiEventFree(impl.encoder);
    impl.encoder = nullptr;
    impl.port = -1;
    impl.name.clear();
    impl.connected = false;
}

bool MidiOutputPort::isOpen() const {
    return impl_->port >= 0;
}

std::string MidiOutputPort::name() const {
    return impl_->name;
}

bool MidiOutputPort::reconnect() {
    Impl& impl = *impl_;
    if (impl.port < 0) return false;
    OutputClient& client = outputClient();
    std::lock_guard<std::mutex> lock(client.mutex);
    const std::vector<Alsa::PortEntry> entries = destinations(client);
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const Alsa::PortEntry& entry) { return entry.name == impl.name; });
    if (found == entries.end()) {
        impl.connected = false;
        return false;
    }
    // Back at another address (a MIDI interface plugged into another socket): the old connection went with the old one.
    if (found->address != impl.destination) {
        client.api->seqDisconnectTo(client.seq, impl.port, impl.destination.client, impl.destination.port);
        impl.destination = found->address;
    }
    // Connecting again does nothing but answer "busy" while the connection is there; when the port went and came back
    // at the same address, it is made anew.
    const int result = client.api->seqConnectTo(client.seq, impl.port, impl.destination.client, impl.destination.port);
    impl.connected = result >= 0 || result == -EBUSY;
    return impl.connected;
}

bool MidiOutputPort::connected() const {
    return impl_->connected;
}

namespace {

// Encodes MIDI bytes into sequencer events and sends each one at once to the port's connections.
bool sendBytes(OutputClient& client, int port, Alsa::MidiEvent* encoder, const uint8_t* data, size_t length) {
    const Alsa::Api& api = *client.api;
    api.midiEventResetEncode(encoder);  // Nothing left over from a message cut short
    bool sent = true;
    size_t position = 0;
    while (position < length) {
        Alsa::SeqEvent event;
        const long used = api.midiEventEncode(encoder, data + position, long(length - position), &event);
        if (used <= 0) return false;
        position += size_t(used);
        if (event.type == Alsa::kEventNone) continue;  // An incomplete message at the end
        event.queue = Alsa::kQueueDirect;
        event.sourcePort = uint8_t(port);
        event.destClient = Alsa::kAddressSubscribers;
        event.destPort = Alsa::kAddressUnknown;
        if (api.seqEventOutputDirect(client.seq, &event) < 0) sent = false;
    }
    return sent;
}

}  // namespace

bool MidiOutputPort::sendShort(uint32_t message) {
    Impl& impl = *impl_;
    const uint8_t bytes[3] = {uint8_t(message), uint8_t((message >> 8) & 0x7F), uint8_t((message >> 16) & 0x7F)};
    if (bytes[0] < 0x80) return false;
    OutputClient& client = outputClient();
    std::lock_guard<std::mutex> lock(client.mutex);
    if (impl.port < 0) return false;
    return sendBytes(client, impl.port, impl.encoder, bytes, size_t(1 + MidiStreamParser::dataBytes(bytes[0])));
}

bool MidiOutputPort::sendSysex(const uint8_t* data, size_t length) {
    Impl& impl = *impl_;
    if (length < 2 || data[0] != 0xF0) return false;
    OutputClient& client = outputClient();
    std::lock_guard<std::mutex> lock(client.mutex);
    if (impl.port < 0) return false;
    return sendBytes(client, impl.port, impl.encoder, data, length);
}
