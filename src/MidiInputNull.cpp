// MIDI input for platforms without a backend yet, and for the builds that take no MIDI from ports (the plugins, the
// tests): no ports are ever listed.

#include "MidiInput.h"

struct MidiInputManager::Port {
    std::string name;
};

struct MidiInputManager::System {};

MidiInputManager::MidiInputManager(MidiInputSink& sink, bool /*ownPort*/) : sink_(sink) {
    (void)sink_;
}

MidiInputManager::~MidiInputManager() = default;

std::vector<std::string> MidiInputManager::listPorts() const {
    return {};
}

bool MidiInputManager::open(const std::string& name, std::string& error) {
    error = "MIDI input is not supported on this platform: " + name;
    return false;
}

void MidiInputManager::close(const std::string& /*name*/) {}

void MidiInputManager::closeAll() {}

bool MidiInputManager::isOpen(const std::string& /*name*/) const {
    return false;
}

std::vector<std::string> MidiInputManager::openPorts() const {
    return {};
}

uint32_t MidiInputManager::messageCount(const std::string& /*name*/) const {
    return 0;
}

std::string MidiInputManager::ownPortName() const {
    return std::string();
}

std::string MidiInputManager::ownPortHint() const {
    return std::string();
}

std::string MidiInputManager::systemError() const {
    return "MIDI input ports are not available in this build";
}
