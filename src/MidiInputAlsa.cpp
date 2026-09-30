// ALSA sequencer MIDI input (Linux). The program is a sequencer client named after it (midiProgramName(): "D110Emu"),
// with one input port that other programs can send to (aconnect, aplaymidi -p D110Emu), or a private one; opening a
// port connects it to that one. MIDI interfaces (USB, the kernel's serial and gadget drivers) and programs' output ports
// are all sequencer ports. The ALSA library is loaded when the program runs (MidiAlsa.h).

#include "MidiInput.h"

#include <fcntl.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include "MidiAlsa.h"
#include "MidiStreamParser.h"

using Alsa::Address;
using Alsa::PortEntry;

struct MidiInputManager::Port {
    std::string name;
    Address address;
    std::atomic<uint32_t> messages{0};
    std::atomic<bool> gone{false};  // It left the sequencer (unplugged), and the connection with it
};

struct MidiInputManager::System {
    const Alsa::Api* api = nullptr;
    Alsa::Seq* seq = nullptr;
    int client = -1;
    int port = -1;         // Ours, which the ports are connected to
    bool ownPort = false;  // Other programs can connect to it too
    std::string error;
    std::thread reader;
    int wake[2] = {-1, -1};  // A pipe that ends the reader's poll()
    std::mutex portsMutex;   // Guards the manager's ports_ against the reader thread

    // The readable ports as the sequencer has them now, in its order (by client and port number).
    std::vector<PortEntry> list() const { return Alsa::listPorts(*api, seq, Alsa::kCapRead | Alsa::kCapSubsRead, client); }

    std::string errorText(int code) const { return api->strerror(code); }
};

namespace {

// The reader thread: waits for events, and passes their MIDI on until the wake pipe is written.
void readEvents(MidiInputManager::System& system, MidiInputSink& sink, std::vector<std::unique_ptr<MidiInputManager::Port>>& ports) {
    const Alsa::Api& api = *system.api;
    Alsa::MidiEvent* decoder = nullptr;
    if (api.midiEventNew(256, &decoder) < 0) return;
    api.midiEventNoStatus(decoder, 1);  // Every message with its status byte
    std::vector<unsigned char> bytes(256);
    std::map<int, MidiStreamParser> parsers;  // By source (client * 256 + port): SysEx comes in pieces

    const int count = api.seqPollDescriptorsCount(system.seq, POLLIN);
    std::vector<pollfd> fds(size_t(std::max(count, 0)) + 1);
    api.seqPollDescriptors(system.seq, fds.data() + 1, unsigned(fds.size() - 1), POLLIN);
    fds[0] = {system.wake[0], POLLIN, 0};

    // Marks the open ports at an address (or all of a client's, port < 0) as gone.
    auto markGone = [&](int client, int port) {
        std::lock_guard<std::mutex> lock(system.portsMutex);
        for (const auto& open : ports) {
            if (open->address.client == client && (port < 0 || open->address.port == port)) open->gone = true;
        }
    };

    for (;;) {
        for (pollfd& fd : fds) fd.revents = 0;
        if (poll(fds.data(), nfds_t(fds.size()), -1) < 0 && errno != EINTR) break;
        if (fds[0].revents != 0) break;
        for (;;) {
            Alsa::SeqEvent* event = nullptr;
            const int result = api.seqEventInput(system.seq, &event);
            if (result == -EAGAIN || (result < 0 && result != -ENOSPC)) break;  // -ENOSPC: the input overran, go on
            if (event == nullptr) continue;
            switch (event->type) {
            case Alsa::kEventClock:
                sink.onMidiRealTime(0xF8);
                continue;
            case Alsa::kEventStart:
                sink.onMidiRealTime(0xFA);
                continue;
            case Alsa::kEventContinue:
                sink.onMidiRealTime(0xFB);
                continue;
            case Alsa::kEventStop:
                sink.onMidiRealTime(0xFC);
                continue;
            case Alsa::kEventSensing:
            case Alsa::kEventReset:
                continue;
            case Alsa::kEventPortExit:
                markGone(event->data[0], event->data[1]);  // snd_seq_addr_t: the client and port that left
                continue;
            case Alsa::kEventClientExit:
                markGone(event->data[0], -1);
                continue;
            case Alsa::kEventPortStart:
            case Alsa::kEventPortChange:
                continue;
            default:
                break;
            }
            long length = api.midiEventDecode(decoder, bytes.data(), long(bytes.size()), event);
            if (length == -ENOMEM) {
                // A long SysEx: its length is in the event (snd_seq_ev_ext: len, then the data's pointer).
                uint32_t needed = 0;
                std::memcpy(&needed, event->data, sizeof(needed));
                if (needed <= MidiStreamParser::kMaxSysexLength + 1) {
                    bytes.resize(needed);
                    length = api.midiEventDecode(decoder, bytes.data(), long(bytes.size()), event);
                }
            }
            if (length <= 0) continue;  // Not MIDI (queue control, echoes, notifications)
            const int source = event->sourceClient * 256 + event->sourcePort;
            const uint32_t messages = parsers[source].feed(sink, bytes.data(), size_t(length));
            if (messages == 0) continue;
            std::lock_guard<std::mutex> lock(system.portsMutex);
            for (const auto& open : ports) {
                if (open->address.client == event->sourceClient && open->address.port == event->sourcePort) open->messages += messages;
            }
        }
    }
    api.midiEventFree(decoder);
}

}  // namespace

MidiInputManager::MidiInputManager(MidiInputSink& sink, bool ownPort) : sink_(sink), system_(new System) {
    System& system = *system_;
    system.api = Alsa::load(system.error);
    if (system.api == nullptr) return;
    const Alsa::Api& api = *system.api;
    const int result = api.seqOpen(&system.seq, "default", Alsa::kOpenInput, Alsa::kNonBlock);
    if (result < 0) {
        system.seq = nullptr;
        system.error = "Cannot open the ALSA sequencer: " + system.errorText(result);
        if (result == -EACCES || result == -EPERM) system.error += " (is this user in the \"audio\" group?)";
        return;
    }
    api.seqSetClientName(system.seq, midiProgramName().c_str());
    system.client = api.seqClientId(system.seq);
    // Our port: other programs may connect to it (and list it) only when it is the program's own input.
    system.ownPort = ownPort;
    system.port = ownPort ? api.seqCreateSimplePort(system.seq, "MIDI In", Alsa::kCapWrite | Alsa::kCapSubsWrite,
                                                    Alsa::kTypeMidiGeneric | Alsa::kTypeSynthesizer | Alsa::kTypeApplication)
                          : api.seqCreateSimplePort(system.seq, "Input (private)", Alsa::kCapWrite | Alsa::kCapNoExport,
                                                    Alsa::kTypeMidiGeneric | Alsa::kTypeApplication);
    if (system.port < 0) {
        system.error = "Cannot create the ALSA sequencer port: " + system.errorText(system.port);
        api.seqClose(system.seq);
        system.seq = nullptr;
        return;
    }
    // Ports coming and going (MIDI interfaces plugged in and out), so that a port that left is opened again.
    api.seqConnectFrom(system.seq, system.port, Alsa::kSystemClient, Alsa::kAnnouncePort);
    if (::pipe(system.wake) != 0) {
        system.error = std::string("Cannot start the MIDI input: ") + std::strerror(errno);
        api.seqClose(system.seq);
        system.seq = nullptr;
        return;
    }
    fcntl(system.wake[0], F_SETFD, FD_CLOEXEC);
    fcntl(system.wake[1], F_SETFD, FD_CLOEXEC);
    system.reader = std::thread(readEvents, std::ref(system), std::ref(sink_), std::ref(ports_));
}

MidiInputManager::~MidiInputManager() {
    closeAll();
    System& system = *system_;
    if (system.reader.joinable()) {
        const char stop = 1;
        if (::write(system.wake[1], &stop, 1) != 1) {
            // The pipe is ours alone and empty; this cannot fail, but the thread must not be left waiting either.
        }
        system.reader.join();
    }
    for (int& fd : system.wake) {
        if (fd >= 0) ::close(fd);
        fd = -1;
    }
    if (system.seq != nullptr) system.api->seqClose(system.seq);
}

std::vector<std::string> MidiInputManager::listPorts() const {
    std::vector<std::string> names;
    for (const PortEntry& entry : system_->list()) names.push_back(entry.name);
    return names;
}

bool MidiInputManager::open(const std::string& name, std::string& error) {
    System& system = *system_;
    if (system.seq == nullptr) {
        error = system.error;
        return false;
    }
    {
        // A port that left the sequencer lost its connection: it is opened anew.
        std::lock_guard<std::mutex> lock(system.portsMutex);
        ports_.erase(std::remove_if(ports_.begin(), ports_.end(), [](const std::unique_ptr<Port>& p) { return p->gone.load(); }), ports_.end());
    }
    if (isOpen(name)) return true;
    const std::vector<PortEntry> entries = system.list();
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const PortEntry& entry) { return entry.name == name; });
    if (found == entries.end()) {
        error = "MIDI input not found: " + name;
        return false;
    }
    const int result = system.api->seqConnectFrom(system.seq, system.port, found->address.client, found->address.port);
    if (result < 0) {
        error = "Cannot open " + name + ": " + system.errorText(result);
        return false;
    }
    std::unique_ptr<Port> port(new Port);
    port->name = name;
    port->address = found->address;
    std::lock_guard<std::mutex> lock(system.portsMutex);
    ports_.push_back(std::move(port));
    return true;
}

void MidiInputManager::close(const std::string& name) {
    System& system = *system_;
    std::lock_guard<std::mutex> lock(system.portsMutex);
    const auto it = std::find_if(ports_.begin(), ports_.end(), [&](const std::unique_ptr<Port>& p) { return p->name == name; });
    if (it == ports_.end()) return;
    if (!(*it)->gone) system.api->seqDisconnectFrom(system.seq, system.port, (*it)->address.client, (*it)->address.port);
    ports_.erase(it);
}

void MidiInputManager::closeAll() {
    while (!ports_.empty()) close(ports_.front()->name);
}

bool MidiInputManager::isOpen(const std::string& name) const {
    std::lock_guard<std::mutex> lock(system_->portsMutex);
    return std::any_of(ports_.begin(), ports_.end(), [&](const std::unique_ptr<Port>& p) { return p->name == name && !p->gone; });
}

std::vector<std::string> MidiInputManager::openPorts() const {
    std::lock_guard<std::mutex> lock(system_->portsMutex);
    std::vector<std::string> names;
    for (const auto& port : ports_) {
        if (!port->gone) names.push_back(port->name);
    }
    return names;
}

uint32_t MidiInputManager::messageCount(const std::string& name) const {
    std::lock_guard<std::mutex> lock(system_->portsMutex);
    for (const auto& port : ports_) {
        if (port->name == name) return port->messages.load();
    }
    return 0;
}

std::string MidiInputManager::ownPortName() const {
    const System& system = *system_;
    if (system.seq == nullptr || !system.ownPort) return std::string();
    return midiProgramName() + ":MIDI In (" + std::to_string(system.client) + ":" + std::to_string(system.port) + ")";
}

std::string MidiInputManager::ownPortHint() const {
    const System& system = *system_;
    if (system.seq == nullptr || !system.ownPort) return std::string();
    return "aconnect, aplaymidi -p " + midiProgramName() + ", or their MIDI port setting";
}

std::string MidiInputManager::systemError() const {
    return system_->error;
}
