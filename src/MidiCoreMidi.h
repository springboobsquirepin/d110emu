#pragma once

// What the CoreMIDI input and output (MidiInputCoreMidi.cpp, MidiOutputCoreMidi.cpp; macOS 11 and later) share: the
// process's MIDI client, endpoints' names, and the program's own endpoints, which the lists of ports leave out.

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "MidiInput.h"

namespace CoreMidi {

// The process's CoreMIDI client, named after the program (midiProgramName()), made by a thread that then runs its run
// loop for as long as the process lives: CoreMIDI keeps the process's list of endpoints current on the run loop of the
// thread that made the first client, so a program without one never sees a device plugged in. It is never disposed:
// CoreMIDI disposes clients when the process ends, and a process that disposes its last one cannot count on making
// another. An inline function's statics are the program's only ones, whichever file calls it.
struct Client {
    std::mutex mutex;
    std::condition_variable ready;
    bool started = false;
    MIDIClientRef ref = 0;
    OSStatus status = noErr;
};

inline const Client* client(std::string& error) {
    static Client* const made = [] {
        Client* client = new Client;
        const std::string name = midiProgramName();
        std::thread([client, name] {
            // A timer that never fires keeps the run loop running whatever CoreMIDI puts in it.
            CFRunLoopTimerRef keepAlive = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + 1.0e9, 1.0e9, 0, 0,
                                                               [](CFRunLoopTimerRef, void*) {}, nullptr);
            CFRunLoopAddTimer(CFRunLoopGetCurrent(), keepAlive, kCFRunLoopDefaultMode);
            // The notifications themselves are not needed (the endpoints are listed when asked), but with them on this
            // thread's run loop, CoreMIDI keeps the process's lists of endpoints up to date.
            CFStringRef clientName = CFStringCreateWithCString(kCFAllocatorDefault, name.c_str(), kCFStringEncodingUTF8);
            MIDIClientRef ref = 0;
            const OSStatus status = MIDIClientCreateWithBlock(clientName, &ref, ^(const MIDINotification*) {});
            if (clientName != nullptr) CFRelease(clientName);
            {
                std::lock_guard<std::mutex> lock(client->mutex);
                client->ref = ref;
                client->status = status;
                client->started = true;
            }
            client->ready.notify_all();
            if (status == noErr) CFRunLoopRun();
        }).detach();
        return client;
    }();
    std::unique_lock<std::mutex> lock(made->mutex);
    if (!made->ready.wait_for(lock, std::chrono::seconds(10), [] { return made->started; })) {
        error = "CoreMIDI does not answer (the MIDI server)";
        return nullptr;
    }
    if (made->status != noErr) {
        error = "CoreMIDI cannot be opened (error " + std::to_string(int(made->status)) + ")";
        return nullptr;
    }
    return made;
}

inline std::string utf8(CFStringRef text) {
    const CFIndex size = CFStringGetMaximumSizeForEncoding(CFStringGetLength(text), kCFStringEncodingUTF8) + 1;
    std::string buffer(size_t(size), '\0');
    if (!CFStringGetCString(text, &buffer[0], size, kCFStringEncodingUTF8)) return std::string();
    buffer.resize(std::strlen(buffer.c_str()));
    return buffer;
}

inline std::string stringProperty(MIDIObjectRef object, CFStringRef property) {
    CFStringRef value = nullptr;
    if (MIDIObjectGetStringProperty(object, property, &value) != noErr || value == nullptr) return std::string();
    const std::string text = utf8(value);
    CFRelease(value);
    return text;
}

// An endpoint as the lists name it: as CoreMIDI shows it ("UM-ONE", "IAC Driver Bus 1"), else its name.
inline std::string endpointName(MIDIEndpointRef endpoint, const char* fallback) {
    std::string name = stringProperty(endpoint, kMIDIPropertyDisplayName);
    if (name.empty()) name = stringProperty(endpoint, kMIDIPropertyName);
    return name.empty() ? std::string(fallback) : name;
}

inline MIDIUniqueID uniqueId(MIDIObjectRef object) {
    SInt32 id = 0;
    MIDIObjectGetIntegerProperty(object, kMIDIPropertyUniqueID, &id);
    return MIDIUniqueID(id);
}

inline bool isOffline(MIDIObjectRef object) {
    SInt32 offline = 0;
    return MIDIObjectGetIntegerProperty(object, kMIDIPropertyOffline, &offline) == noErr && offline != 0;
}

// The unique ID a program's MIDI destination asks for: the first four characters of its name as a four-character code
// ("D110" for D110Emu, "MT32" for MT32Translator), so that programs that remember their connections by it find it
// again. CoreMIDI gives another when a second instance already has it.
inline MIDIUniqueID programUniqueId() {
    const std::string name = midiProgramName() + "    ";
    return MIDIUniqueID(uint32_t(uint8_t(name[0])) << 24 | uint32_t(uint8_t(name[1])) << 16 | uint32_t(uint8_t(name[2])) << 8 | uint8_t(name[3]));
}

// The program's own endpoints (its MIDI destinations), by unique ID: the lists of outputs leave them out, as sending
// to them would make a loop.
struct OwnEndpoints {
    std::mutex mutex;
    std::set<MIDIUniqueID> ids;
};

inline OwnEndpoints& ownEndpoints() {
    static OwnEndpoints endpoints;
    return endpoints;
}

inline bool isOwnEndpoint(MIDIEndpointRef endpoint) {
    OwnEndpoints& own = ownEndpoints();
    std::lock_guard<std::mutex> lock(own.mutex);
    return own.ids.count(uniqueId(endpoint)) != 0;
}

}  // namespace CoreMidi
