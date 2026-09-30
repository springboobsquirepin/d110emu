// WinMM MIDI input. Virtual loopback ports (e.g. loopMIDI) let DOSBox, ScummVM or a DAW drive the emulator.

#include "MidiInput.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <map>

namespace {

constexpr int kSysexBufferCount = 4;
constexpr DWORD kSysexBufferSize = 16 * 1024;
constexpr size_t kMaxSysexLength = 64 * 1024;

std::string toUtf8(const wchar_t* text) {
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) return std::string();
    std::string result(size_t(length - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, &result[0], length, nullptr, nullptr);
    return result;
}

// Port names by device index, with " #2"-style suffixes on duplicates.
std::vector<std::string> enumeratePorts() {
    std::vector<std::string> names;
    std::map<std::string, int> seen;
    const UINT count = midiInGetNumDevs();
    for (UINT i = 0; i < count; i++) {
        MIDIINCAPSW caps = {};
        std::string name = midiInGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR ? toUtf8(caps.szPname) : "MIDI In " + std::to_string(i + 1);
        const int occurrence = ++seen[name];
        if (occurrence > 1) name += " #" + std::to_string(occurrence);
        names.push_back(name);
    }
    return names;
}

}  // namespace

struct MidiInputManager::Port {
    std::string name;
    MidiInputSink* sink = nullptr;
    HMIDIIN handle = nullptr;
    std::atomic<uint32_t> messages{0};
    std::atomic<bool> closing{false};
    MIDIHDR headers[kSysexBufferCount] = {};
    std::vector<char> buffers[kSysexBufferCount];
    std::vector<uint8_t> sysex;  // Reassembly buffer; only touched on the driver's callback thread

    void onLongData(MIDIHDR* header) {
        const uint8_t* data = reinterpret_cast<const uint8_t*>(header->lpData);
        for (DWORD i = 0; i < header->dwBytesRecorded; i++) {
            const uint8_t byte = data[i];
            if (byte == 0xF0) sysex.clear();
            if (sysex.size() < kMaxSysexLength) sysex.push_back(byte);
            if (byte == 0xF7) {
                if (sysex.size() >= 2 && sysex[0] == 0xF0) {
                    messages++;
                    sink->onMidiSysex(sysex.data(), sysex.size());
                }
                sysex.clear();
            }
        }
    }
};

namespace {

void CALLBACK midiInProc(HMIDIIN handle, UINT message, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR /*param2*/) {
    MidiInputManager::Port* port = reinterpret_cast<MidiInputManager::Port*>(instance);
    switch (message) {
    case MIM_DATA:
    case MIM_MOREDATA: {
        const uint32_t shortMessage = uint32_t(param1);
        const uint8_t status = uint8_t(shortMessage & 0xFF);
        if (status >= 0xF8) {  // Real-time: clock and transport for the sinks that follow them
            if (status <= 0xFC) port->sink->onMidiRealTime(status);
            return;
        }
        port->messages++;
        port->sink->onMidiShortMessage(shortMessage);
        break;
    }
    case MIM_LONGDATA: {
        MIDIHDR* header = reinterpret_cast<MIDIHDR*>(param1);
        if (port->closing) return;  // midiInReset() hands back all buffers while closing
        if (header->dwBytesRecorded > 0) port->onLongData(header);
        // Re-queueing from the callback is what RtMidi and others do; WinMM tolerates it for input buffers.
        midiInAddBuffer(handle, header, sizeof(MIDIHDR));
        break;
    }
    default:
        break;
    }
}

}  // namespace

struct MidiInputManager::System {};

MidiInputManager::MidiInputManager(MidiInputSink& sink, bool /*ownPort*/) : sink_(sink) {}  // WinMM has no ports of a program's own

MidiInputManager::~MidiInputManager() {
    closeAll();
}

std::vector<std::string> MidiInputManager::listPorts() const {
    return enumeratePorts();
}

bool MidiInputManager::open(const std::string& name, std::string& error) {
    if (isOpen(name)) return true;
    const std::vector<std::string> names = enumeratePorts();
    const auto found = std::find(names.begin(), names.end(), name);
    if (found == names.end()) {
        error = "MIDI input not found: " + name;
        return false;
    }
    const UINT deviceId = UINT(found - names.begin());

    std::unique_ptr<Port> port(new Port);
    port->name = name;
    port->sink = &sink_;
    MMRESULT result = midiInOpen(&port->handle, deviceId, reinterpret_cast<DWORD_PTR>(&midiInProc),
                                 reinterpret_cast<DWORD_PTR>(port.get()), CALLBACK_FUNCTION | MIDI_IO_STATUS);
    if (result != MMSYSERR_NOERROR) {
        wchar_t text[MAXERRORLENGTH] = {};
        midiInGetErrorTextW(result, text, MAXERRORLENGTH);
        error = "Cannot open " + name + ": " + toUtf8(text);
        return false;
    }
    for (int i = 0; i < kSysexBufferCount; i++) {
        port->buffers[i].resize(kSysexBufferSize);
        MIDIHDR& header = port->headers[i];
        header.lpData = port->buffers[i].data();
        header.dwBufferLength = kSysexBufferSize;
        midiInPrepareHeader(port->handle, &header, sizeof(MIDIHDR));
        midiInAddBuffer(port->handle, &header, sizeof(MIDIHDR));
    }
    midiInStart(port->handle);
    ports_.push_back(std::move(port));
    return true;
}

void MidiInputManager::close(const std::string& name) {
    const auto it = std::find_if(ports_.begin(), ports_.end(), [&](const std::unique_ptr<Port>& p) { return p->name == name; });
    if (it == ports_.end()) return;
    Port& port = **it;
    port.closing = true;
    midiInStop(port.handle);
    midiInReset(port.handle);
    for (MIDIHDR& header : port.headers) {
        while (midiInUnprepareHeader(port.handle, &header, sizeof(MIDIHDR)) == MIDIERR_STILLPLAYING) Sleep(1);
    }
    midiInClose(port.handle);
    ports_.erase(it);
}

void MidiInputManager::closeAll() {
    while (!ports_.empty()) close(ports_.front()->name);
}

bool MidiInputManager::isOpen(const std::string& name) const {
    return std::any_of(ports_.begin(), ports_.end(), [&](const std::unique_ptr<Port>& p) { return p->name == name; });
}

std::vector<std::string> MidiInputManager::openPorts() const {
    std::vector<std::string> names;
    for (const auto& port : ports_) names.push_back(port->name);
    return names;
}

uint32_t MidiInputManager::messageCount(const std::string& name) const {
    for (const auto& port : ports_) {
        if (port->name == name) return port->messages.load();
    }
    return 0;
}

std::string MidiInputManager::ownPortName() const {
    return std::string();  // WinMM has no ports a program opens for others; loopMIDI makes them
}

std::string MidiInputManager::ownPortHint() const {
    return std::string();
}

std::string MidiInputManager::systemError() const {
    return std::string();
}
