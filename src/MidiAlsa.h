#pragma once

// What the ALSA sequencer MIDI input and output (MidiInputAlsa.cpp, MidiOutputAlsa.cpp) share: the ALSA library,
// loaded when the program runs (libasound.so.2, present wherever ALSA is), as miniaudio loads it for the audio, so
// building needs no ALSA development package; and the listing of the sequencer's ports. The declarations are alsa-lib's
// (seq.h, seqmid.h, seq_event.h, seq_midi_event.h), checked against alsa-lib 1.2.14's headers.

#include <dlfcn.h>
#include <poll.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

#include "MidiInput.h"

namespace Alsa {

// alsa-lib's opaque types.
struct Seq;         // snd_seq_t
struct ClientInfo;  // snd_seq_client_info_t
struct PortInfo;    // snd_seq_port_info_t
struct MidiEvent;   // snd_midi_event_t

// snd_seq_event_t, as far as it is read and written here (28 bytes; the data union holds, for SysEx, its length and a
// pointer, packed).
struct SeqEvent {
    uint8_t type = 0;
    uint8_t flags = 0;
    uint8_t tag = 0;
    uint8_t queue = 0;
    uint32_t time[2] = {0, 0};
    uint8_t sourceClient = 0;
    uint8_t sourcePort = 0;
    uint8_t destClient = 0;
    uint8_t destPort = 0;
    uint8_t data[12] = {};  // Announcements: the client and port that came or went; SysEx: its length (then the pointer)
};
static_assert(sizeof(SeqEvent) == 28, "snd_seq_event_t is 28 bytes");

// seq.h
constexpr int kOpenOutput = 1;      // SND_SEQ_OPEN_OUTPUT
constexpr int kOpenInput = 2;       // SND_SEQ_OPEN_INPUT
constexpr int kNonBlock = 1;        // SND_SEQ_NONBLOCK
constexpr int kSystemClient = 0;    // SND_SEQ_CLIENT_SYSTEM
constexpr int kAnnouncePort = 1;    // SND_SEQ_PORT_SYSTEM_ANNOUNCE
constexpr uint8_t kAddressUnknown = 253;      // SND_SEQ_ADDRESS_UNKNOWN
constexpr uint8_t kAddressSubscribers = 254;  // SND_SEQ_ADDRESS_SUBSCRIBERS
constexpr uint8_t kQueueDirect = 253;         // SND_SEQ_QUEUE_DIRECT
constexpr unsigned kCapRead = 1u << 0;       // SND_SEQ_PORT_CAP_READ
constexpr unsigned kCapWrite = 1u << 1;      // SND_SEQ_PORT_CAP_WRITE
constexpr unsigned kCapSubsRead = 1u << 5;   // SND_SEQ_PORT_CAP_SUBS_READ
constexpr unsigned kCapSubsWrite = 1u << 6;  // SND_SEQ_PORT_CAP_SUBS_WRITE
constexpr unsigned kCapNoExport = 1u << 7;   // SND_SEQ_PORT_CAP_NO_EXPORT
constexpr unsigned kTypeMidiGeneric = 1u << 1;   // SND_SEQ_PORT_TYPE_MIDI_GENERIC
constexpr unsigned kTypeSynthesizer = 1u << 18;  // SND_SEQ_PORT_TYPE_SYNTHESIZER
constexpr unsigned kTypeApplication = 1u << 20;  // SND_SEQ_PORT_TYPE_APPLICATION

// seq_event.h: the event types used here (SND_SEQ_EVENT_NONE, what the encoder leaves while a message is incomplete, is
// 255: type 0 is a system event).
enum EventType : uint8_t {
    kEventNone = 255, kEventStart = 30, kEventContinue = 31, kEventStop = 32, kEventClock = 36,
    kEventReset = 41, kEventSensing = 42,
    kEventClientExit = 61, kEventPortStart = 63, kEventPortExit = 64, kEventPortChange = 65,
};

struct Api {
    int (*seqOpen)(Seq** handle, const char* name, int streams, int mode) = nullptr;
    int (*seqClose)(Seq* handle) = nullptr;
    int (*seqSetClientName)(Seq* seq, const char* name) = nullptr;
    int (*seqClientId)(Seq* handle) = nullptr;
    int (*seqCreateSimplePort)(Seq* seq, const char* name, unsigned int caps, unsigned int type) = nullptr;
    int (*seqDeleteSimplePort)(Seq* seq, int port) = nullptr;
    int (*seqConnectFrom)(Seq* seq, int myPort, int srcClient, int srcPort) = nullptr;
    int (*seqDisconnectFrom)(Seq* seq, int myPort, int srcClient, int srcPort) = nullptr;
    int (*seqConnectTo)(Seq* seq, int myPort, int destClient, int destPort) = nullptr;
    int (*seqDisconnectTo)(Seq* seq, int myPort, int destClient, int destPort) = nullptr;
    int (*seqEventInput)(Seq* handle, SeqEvent** event) = nullptr;
    int (*seqEventOutputDirect)(Seq* handle, SeqEvent* event) = nullptr;
    int (*seqPollDescriptorsCount)(Seq* handle, short events) = nullptr;
    int (*seqPollDescriptors)(Seq* handle, pollfd* pfds, unsigned int space, short events) = nullptr;
    int (*seqClientInfoMalloc)(ClientInfo** info) = nullptr;
    void (*seqClientInfoFree)(ClientInfo* info) = nullptr;
    void (*seqClientInfoSetClient)(ClientInfo* info, int client) = nullptr;
    int (*seqClientInfoGetClient)(const ClientInfo* info) = nullptr;
    const char* (*seqClientInfoGetName)(ClientInfo* info) = nullptr;
    int (*seqClientInfoGetPid)(const ClientInfo* info) = nullptr;  // alsa-lib 1.1.5 and later; may be missing
    int (*seqQueryNextClient)(Seq* handle, ClientInfo* info) = nullptr;
    int (*seqPortInfoMalloc)(PortInfo** info) = nullptr;
    void (*seqPortInfoFree)(PortInfo* info) = nullptr;
    void (*seqPortInfoSetClient)(PortInfo* info, int client) = nullptr;
    void (*seqPortInfoSetPort)(PortInfo* info, int port) = nullptr;
    int (*seqPortInfoGetPort)(const PortInfo* info) = nullptr;
    const char* (*seqPortInfoGetName)(const PortInfo* info) = nullptr;
    unsigned int (*seqPortInfoGetCapability)(const PortInfo* info) = nullptr;
    int (*seqQueryNextPort)(Seq* handle, PortInfo* info) = nullptr;
    int (*midiEventNew)(size_t bufferSize, MidiEvent** coder) = nullptr;
    void (*midiEventFree)(MidiEvent* coder) = nullptr;
    void (*midiEventNoStatus)(MidiEvent* coder, int on) = nullptr;
    long (*midiEventDecode)(MidiEvent* coder, unsigned char* buffer, long count, const SeqEvent* event) = nullptr;
    long (*midiEventEncode)(MidiEvent* coder, const unsigned char* buffer, long count, SeqEvent* event) = nullptr;
    void (*midiEventResetEncode)(MidiEvent* coder) = nullptr;
    const char* (*strerror)(int error) = nullptr;
};

// The library, loaded once for the process and kept (like miniaudio's contexts, never unloaded while in use). An inline
// function's statics are the program's only ones, whichever file calls it.
inline const Api* load(std::string& error) {
    static Api api;
    static std::string loadError;
    static std::once_flag once;
    std::call_once(once, [] {
        void* library = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
        if (library == nullptr) library = dlopen("libasound.so", RTLD_NOW | RTLD_LOCAL);
        if (library == nullptr) {
            loadError = "The ALSA library (libasound.so.2) is not installed";
            return;
        }
        bool complete = true;
        auto symbol = [&](auto& function, const char* name, bool required = true) {
            function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(dlsym(library, name));
            if (required) complete = complete && function != nullptr;
        };
        symbol(api.seqOpen, "snd_seq_open");
        symbol(api.seqClose, "snd_seq_close");
        symbol(api.seqSetClientName, "snd_seq_set_client_name");
        symbol(api.seqClientId, "snd_seq_client_id");
        symbol(api.seqCreateSimplePort, "snd_seq_create_simple_port");
        symbol(api.seqDeleteSimplePort, "snd_seq_delete_simple_port");
        symbol(api.seqConnectFrom, "snd_seq_connect_from");
        symbol(api.seqDisconnectFrom, "snd_seq_disconnect_from");
        symbol(api.seqConnectTo, "snd_seq_connect_to");
        symbol(api.seqDisconnectTo, "snd_seq_disconnect_to");
        symbol(api.seqEventInput, "snd_seq_event_input");
        symbol(api.seqEventOutputDirect, "snd_seq_event_output_direct");
        symbol(api.seqPollDescriptorsCount, "snd_seq_poll_descriptors_count");
        symbol(api.seqPollDescriptors, "snd_seq_poll_descriptors");
        symbol(api.seqClientInfoMalloc, "snd_seq_client_info_malloc");
        symbol(api.seqClientInfoFree, "snd_seq_client_info_free");
        symbol(api.seqClientInfoSetClient, "snd_seq_client_info_set_client");
        symbol(api.seqClientInfoGetClient, "snd_seq_client_info_get_client");
        symbol(api.seqClientInfoGetName, "snd_seq_client_info_get_name");
        symbol(api.seqClientInfoGetPid, "snd_seq_client_info_get_pid", false);
        symbol(api.seqQueryNextClient, "snd_seq_query_next_client");
        symbol(api.seqPortInfoMalloc, "snd_seq_port_info_malloc");
        symbol(api.seqPortInfoFree, "snd_seq_port_info_free");
        symbol(api.seqPortInfoSetClient, "snd_seq_port_info_set_client");
        symbol(api.seqPortInfoSetPort, "snd_seq_port_info_set_port");
        symbol(api.seqPortInfoGetPort, "snd_seq_port_info_get_port");
        symbol(api.seqPortInfoGetName, "snd_seq_port_info_get_name");
        symbol(api.seqPortInfoGetCapability, "snd_seq_port_info_get_capability");
        symbol(api.seqQueryNextPort, "snd_seq_query_next_port");
        symbol(api.midiEventNew, "snd_midi_event_new");
        symbol(api.midiEventFree, "snd_midi_event_free");
        symbol(api.midiEventNoStatus, "snd_midi_event_no_status");
        symbol(api.midiEventDecode, "snd_midi_event_decode");
        symbol(api.midiEventEncode, "snd_midi_event_encode");
        symbol(api.midiEventResetEncode, "snd_midi_event_reset_encode");
        symbol(api.strerror, "snd_strerror");
        if (!complete) {
            loadError = "The ALSA library (libasound.so.2) lacks the sequencer";
            dlclose(library);
            api = Api();
        }
    });
    error = loadError;
    return loadError.empty() ? &api : nullptr;
}

struct Address {
    int client = -1;
    int port = -1;
    bool operator==(const Address& other) const { return client == other.client && port == other.port; }
    bool operator!=(const Address& other) const { return !(*this == other); }
};

struct PortEntry {
    std::string name;  // "Client:Port", with " #2" on a repeated name
    Address address;
};

// The ports with all of `caps` (readable ones for input, writable ones for output) as the sequencer has them now, in
// its order (by client and port number): not the system's, not those of this process's own clients (the program's
// input and output ports, which would make a loop), not those hidden from other programs.
inline std::vector<PortEntry> listPorts(const Api& api, Seq* seq, unsigned caps, int ownClient) {
    std::vector<PortEntry> entries;
    if (seq == nullptr) return entries;
    ClientInfo* clientInfo = nullptr;
    PortInfo* portInfo = nullptr;
    if (api.seqClientInfoMalloc(&clientInfo) < 0) return entries;
    if (api.seqPortInfoMalloc(&portInfo) < 0) {
        api.seqClientInfoFree(clientInfo);
        return entries;
    }
    const int pid = int(getpid());
    std::map<std::string, int> seen;
    api.seqClientInfoSetClient(clientInfo, -1);
    while (api.seqQueryNextClient(seq, clientInfo) >= 0) {
        const int clientId = api.seqClientInfoGetClient(clientInfo);
        if (clientId == kSystemClient || clientId == ownClient) continue;
        const std::string clientName = api.seqClientInfoGetName(clientInfo);
        // Without the process IDs (older ALSA), the program's other clients go by its name.
        if (api.seqClientInfoGetPid != nullptr ? api.seqClientInfoGetPid(clientInfo) == pid : clientName == midiProgramName()) continue;
        api.seqPortInfoSetClient(portInfo, clientId);
        api.seqPortInfoSetPort(portInfo, -1);
        while (api.seqQueryNextPort(seq, portInfo) >= 0) {
            const unsigned portCaps = api.seqPortInfoGetCapability(portInfo);
            if ((portCaps & caps) != caps || (portCaps & kCapNoExport) != 0) continue;
            PortEntry entry;
            entry.name = clientName + ":" + api.seqPortInfoGetName(portInfo);
            const int occurrence = ++seen[entry.name];
            if (occurrence > 1) entry.name += " #" + std::to_string(occurrence);
            entry.address = {clientId, api.seqPortInfoGetPort(portInfo)};
            entries.push_back(entry);
        }
    }
    api.seqPortInfoFree(portInfo);
    api.seqClientInfoFree(clientInfo);
    return entries;
}

}  // namespace Alsa
