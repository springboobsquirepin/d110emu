// MIDI output stand-in for the builds that send no MIDI (the tests): no ports.

#include "MidiOutput.h"

struct MidiOutputPort::Impl {};

MidiOutputPort::MidiOutputPort(const std::string& /*ownName*/) : impl_(new Impl) {}
MidiOutputPort::~MidiOutputPort() = default;

std::vector<std::string> MidiOutputPort::listPorts() {
    return {};
}

std::string MidiOutputPort::systemError() {
    return "MIDI output ports are not available in this build";
}

bool MidiOutputPort::open(const std::string& name, std::string& error) {
    error = "MIDI output is not available in this build (" + name + ")";
    return false;
}

void MidiOutputPort::close() {}

bool MidiOutputPort::isOpen() const {
    return false;
}

std::string MidiOutputPort::name() const {
    return std::string();
}

bool MidiOutputPort::reconnect() {
    return false;
}

bool MidiOutputPort::connected() const {
    return false;
}

bool MidiOutputPort::sendShort(uint32_t) {
    return false;
}

bool MidiOutputPort::sendSysex(const uint8_t*, size_t) {
    return false;
}
