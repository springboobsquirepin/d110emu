// WinMM MIDI output: a USB MIDI interface to a real unit, or a virtual port (loopMIDI) to software.

#include "MidiOutput.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <map>
#include <mutex>

namespace {

std::string toUtf8(const wchar_t* text) {
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) return std::string();
    std::string result(size_t(length - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, &result[0], length, nullptr, nullptr);
    return result;
}

// Port names by device index, with " #2"-style suffixes on duplicates (as MidiInputWin32.cpp).
std::vector<std::string> enumeratePorts() {
    std::vector<std::string> names;
    std::map<std::string, int> seen;
    const UINT count = midiOutGetNumDevs();
    for (UINT i = 0; i < count; i++) {
        MIDIOUTCAPSW caps = {};
        std::string name = midiOutGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR ? toUtf8(caps.szPname) : "MIDI Out " + std::to_string(i + 1);
        const int occurrence = ++seen[name];
        if (occurrence > 1) name += " #" + std::to_string(occurrence);
        names.push_back(name);
    }
    return names;
}

std::string errorText(MMRESULT result) {
    wchar_t text[MAXERRORLENGTH] = {};
    if (midiOutGetErrorTextW(result, text, MAXERRORLENGTH) != MMSYSERR_NOERROR) return "error " + std::to_string(result);
    return toUtf8(text);
}

}  // namespace

struct MidiOutputPort::Impl {
    std::mutex mutex;          // The sender thread sends while the interface checks the port
    HMIDIOUT handle = nullptr;
    std::string name;          // What was opened, kept while the device is gone
    bool opened = false;       // Between open() and close()
    bool failed = false;       // A message did not go out: the device went away (unplugged)
    bool connected = false;
    std::vector<char> buffer;

    // Opens the device of that name; false (the handle null) when it is not there or cannot be opened.
    bool openDevice(std::string& error) {
        const std::vector<std::string> names = enumeratePorts();
        const auto found = std::find(names.begin(), names.end(), name);
        if (found == names.end()) {
            error = name + ": no such MIDI output";
            return false;
        }
        const MMRESULT result = midiOutOpen(&handle, UINT(found - names.begin()), 0, 0, CALLBACK_NULL);
        if (result != MMSYSERR_NOERROR) {
            handle = nullptr;
            error = name + ": " + errorText(result);
            return false;
        }
        failed = false;
        return true;
    }

    void closeDevice() {
        if (handle == nullptr) return;
        midiOutReset(handle);
        midiOutClose(handle);
        handle = nullptr;
    }
};

MidiOutputPort::MidiOutputPort(const std::string& /*ownName*/) : impl_(new Impl) {}  // WinMM shows other programs nothing

MidiOutputPort::~MidiOutputPort() {
    close();
}

std::vector<std::string> MidiOutputPort::listPorts() {
    return enumeratePorts();
}

std::string MidiOutputPort::systemError() {
    return std::string();
}

bool MidiOutputPort::open(const std::string& name, std::string& error) {
    close();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->name = name;
    if (!impl_->openDevice(error)) {
        impl_->name.clear();
        return false;
    }
    impl_->opened = true;
    impl_->connected = true;
    return true;
}

void MidiOutputPort::close() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->closeDevice();
    impl_->opened = false;
    impl_->connected = false;
    impl_->name.clear();
}

bool MidiOutputPort::isOpen() const {
    return impl_->opened;
}

std::string MidiOutputPort::name() const {
    return impl_->name;
}

bool MidiOutputPort::reconnect() {
    Impl& impl = *impl_;
    // A SysEx message on its way holds the port for up to a few hundred milliseconds: then it is evidently there.
    std::unique_lock<std::mutex> lock(impl.mutex, std::try_to_lock);
    if (!lock.owns_lock()) return impl.connected;
    if (!impl.opened) return false;
    const std::vector<std::string> names = enumeratePorts();
    if (std::find(names.begin(), names.end(), impl.name) == names.end()) {
        impl.closeDevice();  // Its handle went with it
        impl.connected = false;
        return false;
    }
    if (impl.handle == nullptr || impl.failed) {
        impl.closeDevice();
        std::string error;
        impl.connected = impl.openDevice(error);
        return impl.connected;
    }
    impl.connected = true;
    return true;
}

bool MidiOutputPort::connected() const {
    return impl_->connected;
}

bool MidiOutputPort::sendShort(uint32_t message) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->handle == nullptr) return false;
    if (midiOutShortMsg(impl_->handle, message) == MMSYSERR_NOERROR) return true;
    impl_->failed = true;
    return false;
}

bool MidiOutputPort::sendSysex(const uint8_t* data, size_t length) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->handle == nullptr || length == 0) return false;
    impl_->buffer.assign(reinterpret_cast<const char*>(data), reinterpret_cast<const char*>(data) + length);
    MIDIHDR header = {};
    header.lpData = impl_->buffer.data();
    header.dwBufferLength = DWORD(length);
    if (midiOutPrepareHeader(impl_->handle, &header, sizeof(header)) != MMSYSERR_NOERROR) {
        impl_->failed = true;
        return false;
    }
    bool sent = midiOutLongMsg(impl_->handle, &header, sizeof(header)) == MMSYSERR_NOERROR;
    // The driver sets MHDR_DONE when it has the data; at MIDI speed that takes 0.32 ms per byte.
    const DWORD deadline = GetTickCount() + 2000 + DWORD(length);
    while (sent && (header.dwFlags & MHDR_DONE) == 0) {
        if (int(GetTickCount() - deadline) > 0) {
            midiOutReset(impl_->handle);  // Stuck driver: take the buffer back
            sent = false;
            break;
        }
        Sleep(1);
    }
    while (midiOutUnprepareHeader(impl_->handle, &header, sizeof(header)) == MIDIERR_STILLPLAYING) Sleep(1);
    if (!sent) impl_->failed = true;
    return sent;
}
