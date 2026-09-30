#include "ToneEditorApp.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <thread>

#include "Mt32Translator.h"
#include "Piano.h"
#include "Platform.h"
#include "RolandSysex.h"
#include "UiStyle.h"
#include "imgui.h"

namespace {

constexpr size_t kMaxLogLines = 1000;
using UiStyle::helpMarker;
using UiStyle::kErrorColor;
using UiStyle::kOkColor;

constexpr uint32_t kTimbreTemp = RolandSysex::pack(0x030000);   // + 10H per part: tone group, tone number, ...
constexpr uint32_t kToneWrite = RolandSysex::pack(0x400000);    // + 2 per part: tone number, internal/card
constexpr uint32_t kTimbreWrite = RolandSysex::pack(0x400100);  // + 2 per part: timbre number, internal/card
constexpr uint32_t kPatchWrite = RolandSysex::pack(0x400300);   // D-10/D-20: patch number, internal/card
constexpr uint32_t kWriteResult = RolandSysex::pack(0x401000);  // 0 done, 1 card not ready, 2 write protected, 3 wrong mode

std::vector<std::string> split(const std::string& text, char separator) {
    std::vector<std::string> parts;
    std::stringstream stream(text);
    std::string part;
    while (std::getline(stream, part, separator)) {
        if (!part.empty()) parts.push_back(part);
    }
    return parts;
}

std::vector<uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool writeFile(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    return bool(out);
}

// Copies the part of a received message that falls into [base, base + size) to `out` (indexed from base). Returns the
// offset of the first byte copied, -1 if none.
int copyOverlap(const UnitLink::Received& received, uint32_t base, uint32_t size, uint8_t* out, int& count) {
    const uint32_t start = std::max(received.address, base);
    const uint32_t end = std::min(received.address + uint32_t(received.data.size()), base + size);
    count = 0;
    if (start >= end) return -1;
    count = int(end - start);
    std::memcpy(out + (start - base), received.data.data() + (start - received.address), size_t(count));
    return int(start - base);
}

const char* modelKey(Tone::Model model) {
    return model == Tone::Model::D110 ? "d110" : model == Tone::Model::MT32 ? "mt32" : "d20";
}

}  // namespace

ToneEditorApp::ToneEditorApp() {
    partToneSelection_.fill(-1);
}

ToneEditorApp::~ToneEditorApp() {
    shutdown();
}

void ToneEditorApp::init(const ToneEditorOptions& options) {
    options_ = options;
    // Other programs see its MIDI ports as ToneEditor's (ALSA, CoreMIDI): its own input plays the edited part as a MIDI
    // keyboard does; the unit's answers come on a port of their own.
    if (options_.enableMidi) setMidiProgramName("ToneEditor");
    unitInput_.reset(new MidiInputManager(link_, false));
    keyboards_.reset(new MidiInputManager(keyboardSink_, true));
    loadSettings();
    loadLibraryFile();
    applyUnitSettings();
    link_.start();
    if (options_.enableMidi) {
        refreshPorts();
        openUnitOutput(unitOutputName_);
        openUnitInput(unitInputName_);
        for (const std::string& name : std::vector<std::string>(keyboardNames_)) openKeyboard(name, true);
    }
    addLog(std::string("ToneEditor ready for a ") + Tone::modelName(model_) + ", unit number " + std::to_string(unitNumber_));
    if (!unitInputName_.empty() && unitInput_->isOpen(unitInputName_)) {
        requestPartTone();
        setup_.requestAll(*this);
    }
}

void ToneEditorApp::shutdown() {
    if (!unitInput_) return;
    releaseAllNotes();
    saveSettings();
    saveLibraryFile();
    unitInput_->closeAll();
    keyboards_->closeAll();
    // Let the last messages (note offs) go out before the port closes.
    for (int i = 0; i < 100 && link_.stats().queued > 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    link_.setOutput(nullptr);
    link_.stop();
    unitPort_.close();
    unitInput_.reset();
    keyboards_.reset();
}

// ---------------------------------------------------------------------------------------------
// Settings and files

void ToneEditorApp::loadSettings() {
    settings_.load(options_.settingsFile);
    const std::string model = settings_.getString("model", "d20");
    model_ = model == "d110" ? Tone::Model::D110 : model == "mt32" ? Tone::Model::MT32 : Tone::Model::D20;
    unitNumber_ = std::clamp(settings_.getInt("unit_number", 17), 17, 32);
    part_ = std::clamp(settings_.getInt("part", 1), 1, 8) - 1;
    // Parts 1-8 and rhythm (older settings have parts 1-8 only; "off" = no channel).
    const std::vector<std::string> channels = split(settings_.getString("part_channels"), ',');
    std::array<uint8_t, UnitSetup::kParts>& partChannels = setup_.channels();
    // All parts on one channel is what an earlier version took from a D-20's dummies (all 1): the defaults instead.
    const bool allSame = std::all_of(channels.begin(), channels.end(), [&](const std::string& channel) { return channel == channels[0]; });
    if ((channels.size() == 8 || channels.size() == size_t(UnitSetup::kParts)) && !allSame) {
        for (size_t part = 0; part < channels.size(); part++) {
            partChannels[part] = channels[part] == "off" ? uint8_t(16) : uint8_t(std::clamp(std::atoi(channels[part].c_str()), 1, 16) - 1);
        }
    } else if (model_ == Tone::Model::MT32) {
        for (int part = 0; part < 8; part++) partChannels[size_t(part)] = uint8_t(part + 1);  // The MT-32's parts 1-8 on 2-9
    }
    pauseMs_ = std::clamp(settings_.getInt("sysex_pause_ms", 20), 0, 200);
    setup_.performanceChannel() = uint8_t(std::clamp(settings_.getInt("performance_channel", 1), 1, 16) - 1);
    performanceMode_ = settings_.getBool("performance_mode", false);
    if (performanceTones()) part_ = std::min(part_, 1);
    followUnit_ = settings_.getBool("follow_unit", true);
    writeSlot_ = std::clamp(settings_.getInt("write_slot", 0), 0, 63);
    unitOutputName_ = settings_.getString("unit_output");
    unitInputName_ = settings_.getString("unit_input");
    keyboardNames_ = split(settings_.getString("keyboards"), '|');
    keyboardOctave_ = std::clamp(settings_.getInt("keyboard_octave", 4), 0, 7);
    keyboardVelocity_ = std::clamp(settings_.getInt("keyboard_velocity", 100), 1, 127);
    showLog_ = settings_.getBool("show_log", false);
    ToneEditor::Audition& audition = editor_.audition();
    audition.key = std::clamp(settings_.getInt("audition_key", 60), 24, 108);
    audition.velocity = std::clamp(settings_.getInt("audition_velocity", 100), 1, 127);
    audition.lengthMs = std::clamp(settings_.getInt("audition_length", 700), 50, 4000);
    audition.repeatMs = std::clamp(settings_.getInt("audition_repeat", 1200), 200, 4000);
    audition.restrike = settings_.getBool("restrike", true);
}

void ToneEditorApp::saveSettings() {
    settings_.set("model", modelKey(model_));
    settings_.set("unit_number", unitNumber_);
    settings_.set("part", part_ + 1);
    std::string channels;
    for (size_t part = 0; part < size_t(UnitSetup::kParts); part++) {
        const uint8_t channel = setup_.channels()[part];
        channels += (part ? "," : "") + (channel < 16 ? std::to_string(channel + 1) : std::string("off"));
    }
    settings_.set("part_channels", channels);
    settings_.set("sysex_pause_ms", pauseMs_);
    settings_.set("performance_channel", setup_.performanceChannel() + 1);
    settings_.set("performance_mode", performanceMode_);
    settings_.set("follow_unit", followUnit_);
    settings_.set("write_slot", writeSlot_);
    settings_.set("unit_output", unitOutputName_);
    settings_.set("unit_input", unitInputName_);
    std::string keyboards;
    for (const std::string& name : keyboardNames_) keyboards += (keyboards.empty() ? "" : "|") + name;
    settings_.set("keyboards", keyboards);
    settings_.set("keyboard_octave", keyboardOctave_);
    settings_.set("keyboard_velocity", keyboardVelocity_);
    settings_.set("show_log", showLog_);
    const ToneEditor::Audition& audition = editor_.audition();
    settings_.set("audition_key", audition.key);
    settings_.set("audition_velocity", audition.velocity);
    settings_.set("audition_length", audition.lengthMs);
    settings_.set("audition_repeat", audition.repeatMs);
    settings_.set("restrike", audition.restrike);
    if (!options_.settingsFile.empty()) settings_.save(options_.settingsFile);
}

void ToneEditorApp::applyUnitSettings() {
    editor_.setModel(model_);
    setup_.setModel(model_);
    link_.setDevice(uint8_t(unitNumber_ - 1));
    link_.setPause(pauseMs_);
    thruChannel_ = partChannel() < 16 ? int(partChannel()) : -1;
}

void ToneEditorApp::loadLibraryFile() {
    if (options_.libraryFile.empty() || !std::filesystem::exists(options_.libraryFile)) return;
    const std::vector<uint8_t> data = readFile(options_.libraryFile);
    for (const Tone::FoundTone& tone : Tone::findTones(data.data(), data.size())) {
        if (tone.area != Tone::FoundTone::Area::Memory) continue;
        memory_[size_t(tone.index)] = tone.data;
        memoryKnown_[size_t(tone.index)] = true;
    }
}

void ToneEditorApp::saveLibraryFile() {
    if (options_.libraryFile.empty()) return;
    std::vector<std::pair<int, Tone::Data>> slots;
    for (int slot = 0; slot < 64; slot++) {
        if (memoryKnown_[size_t(slot)]) slots.emplace_back(slot, memory_[size_t(slot)]);
    }
    if (slots.empty()) return;
    writeFile(options_.libraryFile, Tone::toneMemoryDump(slots, 0x10));
}

void ToneEditorApp::openFile(const std::filesystem::path& path) {
    const std::vector<uint8_t> data = readFile(path);
    std::vector<Tone::FoundTone> tones = Tone::findTones(data.data(), data.size());
    if (tones.empty()) {
        addLog("No complete tones in " + Platform::toUtf8(path.filename()));
        return;
    }
    fileTones_ = std::move(tones);
    fileName_ = Platform::toUtf8(path.filename());
    std::string extension = Platform::toUtf8(path.extension());
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    // MT-32 timbres (a game's .dat file) need their waves translated for a D unit.
    fileMt32_ = model_ != Tone::Model::MT32 && extension == ".dat";
    addLog(std::to_string(fileTones_.size()) + " tone(s) in " + fileName_ + ": click one in the library to load it");
    if (fileTones_.size() == 1) loadIntoEditor(fileTones_[0].data, fileTones_[0].where + " of " + fileName_, fileMt32_);
}

void ToneEditorApp::browseFile() {
    std::filesystem::path path;
    if (Platform::openFileDialog(Platform::FileKind::MidiOrSysex, path)) openFile(path);
}

void ToneEditorApp::saveToneFile() {
    const Tone::Data tone = editor_.tone();
    std::string name = Tone::name(tone);
    for (char& c : name) {
        if (std::strchr("\\/:*?\"<>|", c) != nullptr) c = '_';
    }
    std::filesystem::path path;
    if (!Platform::saveFileDialog(Platform::FileKind::Sysex, (name.empty() ? std::string("Tone") : name) + ".syx", path)) return;
    if (!writeFile(path, RolandSysex::dataSet(0x10, Tone::toneTempAddress(0), tone.data(), Tone::kSize))) {
        addLog("Cannot write " + Platform::toUtf8(path));
        return;
    }
    editor_.markOriginal();
    addLog("Saved tone \"" + Tone::name(tone) + "\" to " + Platform::toUtf8(path.filename()));
}

void ToneEditorApp::saveLibrary() {
    std::vector<std::pair<int, Tone::Data>> slots;
    for (int slot = 0; slot < 64; slot++) {
        if (memoryKnown_[size_t(slot)]) slots.emplace_back(slot, memory_[size_t(slot)]);
    }
    if (slots.empty()) {
        addLog("The library holds no tones of the unit yet: read them first");
        return;
    }
    std::filesystem::path path;
    if (!Platform::saveFileDialog(Platform::FileKind::Sysex, "Tones.syx", path)) return;
    if (writeFile(path, Tone::toneMemoryDump(slots, uint8_t(unitNumber_ - 1)))) {
        addLog("Saved " + std::to_string(slots.size()) + " tones to " + Platform::toUtf8(path.filename()));
    } else {
        addLog("Cannot write " + Platform::toUtf8(path));
    }
}

// ---------------------------------------------------------------------------------------------
// MIDI

void ToneEditorApp::refreshPorts() {
    inputPorts_ = unitInput_ ? unitInput_->listPorts() : std::vector<std::string>();
    outputPorts_ = MidiOutputPort::listPorts();
}

void ToneEditorApp::openUnitOutput(const std::string& name) {
    link_.setOutput(nullptr);
    unitPort_.close();
    unitOutputName_ = name;
    if (name.empty()) return;
    std::string error;
    if (!unitPort_.open(name, error)) {
        portError_ = error;
        return;
    }
    portError_.clear();
    link_.setOutput(&unitPort_);
}

void ToneEditorApp::openUnitInput(const std::string& name) {
    if (!unitInputName_.empty()) unitInput_->close(unitInputName_);
    unitInputName_ = name;
    if (name.empty()) return;
    std::string error;
    if (!unitInput_->open(name, error)) portError_ = error;
}

void ToneEditorApp::openKeyboard(const std::string& name, bool open) {
    if (!open) {
        keyboards_->close(name);
        keyboardNames_.erase(std::remove(keyboardNames_.begin(), keyboardNames_.end(), name), keyboardNames_.end());
        return;
    }
    std::string error;
    if (!keyboards_->open(name, error)) {
        portError_ = error;
        return;
    }
    if (std::find(keyboardNames_.begin(), keyboardNames_.end(), name) == keyboardNames_.end()) keyboardNames_.push_back(name);
}

std::string ToneEditorApp::partText() const {
    if (performanceTones()) return part_ == 0 ? "the upper tone" : "the lower tone";
    return "part " + std::to_string(part_ + 1);
}

uint8_t ToneEditorApp::partChannel() const {
    if (performanceTones()) return setup_.performanceChannel();  // The upper and lower tones play there
    return setup_.channels()[size_t(part_)];  // 16 = off
}

void ToneEditorApp::sendData(uint32_t packedAddress, const uint8_t* data, size_t length, bool merge) {
    if (packedAddress >= kToneWrite && packedAddress < kWriteResult) lastWriteRequest_ = packedAddress;
    link_.sendData(packedAddress, data, length, merge);
}

void ToneEditorApp::request(uint32_t packedAddress, uint32_t size) {
    link_.request(packedAddress, size);
}

void ToneEditorApp::sendShort(uint32_t message) {
    link_.sendShort(message);
}

std::string ToneEditorApp::memoryToneName(int slot) {
    return slot >= 0 && slot < 64 && memoryKnown_[size_t(slot)] ? Tone::name(memory_[size_t(slot)]) : std::string();
}

void ToneEditorApp::showTab(const std::string& name) {
    static const char* const names[] = {"Tone", "Parts", "Timbres", "Rhythm", "System", "Performance"};
    for (int tab = 0; tab < 6; tab++) {
        if (name == names[tab]) requestedTab_ = tab;
    }
}

void ToneEditorApp::writeTone(int offset, const uint8_t* data, int length) {
    // Parameter changes merge while they wait; a whole tone always goes out.
    link_.sendData(Tone::toneTempAddress(part_) + uint32_t(offset), data, size_t(length), length < Tone::kSize);
    partTones_[size_t(part_)] = editor_.sentTone();
    partToneKnown_[size_t(part_)] = true;
}

void ToneEditorApp::noteOn(int key, int velocity) {
    if (partChannel() >= 16) return;  // The part has no channel
    link_.sendShort(uint32_t(0x90 | partChannel()) | uint32_t(key & 127) << 8 | uint32_t(std::clamp(velocity, 1, 127)) << 16);
}

void ToneEditorApp::noteOff(int key) {
    if (partChannel() >= 16) return;
    link_.sendShort(uint32_t(0x80 | partChannel()) | uint32_t(key & 127) << 8);
}

void ToneEditorApp::restrike() {
    for (int note = 0; note < 128; note++) {
        if (!keyHeld_[size_t(note)]) continue;
        link_.sendShort(uint32_t(0x80 | keyChannel_[size_t(note)]) | uint32_t(note) << 8);
        link_.sendShort(uint32_t(0x90 | keyChannel_[size_t(note)]) | uint32_t(note) << 8 | uint32_t(keyboardVelocity_) << 16);
    }
    std::lock_guard<std::mutex> lock(thruMutex_);
    const int channel = thruChannel_;
    if (channel < 0) return;
    for (int note = 0; note < 128; note++) {
        if (thruHeld_[size_t(note)] == 0) continue;
        link_.sendShort(uint32_t(0x80 | channel) | uint32_t(note) << 8);
        link_.sendShort(uint32_t(0x90 | channel) | uint32_t(note) << 8 | uint32_t(thruHeld_[size_t(note)]) << 16);
    }
}

void ToneEditorApp::soundingNotes(std::array<bool, 128>& notes) {
    for (int note = 0; note < 128; note++) notes[size_t(note)] = keyHeld_[size_t(note)];
    std::lock_guard<std::mutex> lock(thruMutex_);
    for (int note = 0; note < 128; note++) notes[size_t(note)] = notes[size_t(note)] || thruHeld_[size_t(note)] > 0;
}

void ToneEditorApp::KeyboardSink::onMidiShortMessage(uint32_t message) {
    const uint32_t status = message & 0xF0;
    if (status < 0x80 || status > 0xE0) return;  // Channel messages only
    const int channel = app_.thruChannel_;
    if (channel < 0) return;  // The edited part has no channel
    const uint32_t moved = (message & ~0x0Fu) | uint32_t(channel);
    {
        std::lock_guard<std::mutex> lock(app_.thruMutex_);
        const uint8_t key = uint8_t((message >> 8) & 0x7F), velocity = uint8_t((message >> 16) & 0x7F);
        if (status == 0x90 && velocity > 0) {
            app_.thruHeld_[key] = velocity;
        } else if (status == 0x80 || status == 0x90) {
            app_.thruHeld_[key] = 0;
        }
    }
    app_.link_.sendShort(moved);
}

void ToneEditorApp::releaseAllNotes() {
    editor_.releaseNotes(*this);
    for (int note = 0; note < 128; note++) {
        if (keyHeld_[size_t(note)]) link_.sendShort(uint32_t(0x80 | keyChannel_[size_t(note)]) | uint32_t(note) << 8);
        keyHeld_[size_t(note)] = false;
    }
    std::lock_guard<std::mutex> lock(thruMutex_);
    const int channel = thruChannel_;
    for (int note = 0; note < 128; note++) {
        if (thruHeld_[size_t(note)] > 0 && channel >= 0) link_.sendShort(uint32_t(0x80 | channel) | uint32_t(note) << 8);
        thruHeld_[size_t(note)] = 0;
    }
}

void ToneEditorApp::onKey(int scancode, bool down) {
    if (scancode > 0 && scancode < int(scancodeDown_.size())) scancodeDown_[size_t(scancode)] = down;
}

void ToneEditorApp::clearKeys() {
    scancodeDown_.fill(false);
}

void ToneEditorApp::handleComputerKeyboard() {
    const ImGuiIO& io = ImGui::GetIO();
    std::array<bool, 128> wanted = {};
    if (!io.WantTextInput && !io.KeyCtrl && !io.KeyAlt) {
        for (const Piano::Key& mapping : Piano::kKeys) {
            const int note = (keyboardOctave_ + 1) * 12 + mapping.semitone;
            if (note < 128 && scancodeDown_[mapping.scancode]) wanted[size_t(note)] = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp, false)) keyboardOctave_ = std::min(keyboardOctave_ + 1, 7);
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown, false)) keyboardOctave_ = std::max(keyboardOctave_ - 1, 0);
    }
    if (partChannel() >= 16) wanted.fill(false);  // The part has no channel
    for (int note = 0; note < 128; note++) {
        if (wanted[size_t(note)] && !keyHeld_[size_t(note)]) {
            keyChannel_[size_t(note)] = partChannel();
            link_.sendShort(uint32_t(0x90 | partChannel()) | uint32_t(note) << 8 | uint32_t(keyboardVelocity_) << 16);
        }
        if (!wanted[size_t(note)] && keyHeld_[size_t(note)]) link_.sendShort(uint32_t(0x80 | keyChannel_[size_t(note)]) | uint32_t(note) << 8);
        keyHeld_[size_t(note)] = wanted[size_t(note)];
    }
}

// ---------------------------------------------------------------------------------------------
// The unit

void ToneEditorApp::setModel(Tone::Model model) {
    releaseAllNotes();  // The channel may change
    model_ = model;
    applyUnitSettings();
    if (performanceTones() && part_ > 1) selectPart(0);
}

void ToneEditorApp::setPerformanceMode(bool on) {
    if (on == performanceMode_) return;
    releaseAllNotes();  // On the old channel
    performanceMode_ = on;
    if (performanceTones() && part_ > 1) {
        selectPart(0);
    } else if (performanceTones() && !unitInputName_.empty()) {
        setup_.requestPatchTemp(*this);  // Which tones the patch plays
    }
    thruChannel_ = partChannel() < 16 ? int(partChannel()) : -1;
}

void ToneEditorApp::selectPart(int part) {
    part = std::clamp(part, 0, performanceTones() ? 1 : 7);
    if (part == part_) return;
    releaseAllNotes();  // On the old part's channel
    partTones_[size_t(part_)] = editor_.sentTone();
    partToneKnown_[size_t(part_)] = true;
    part_ = part;
    thruChannel_ = partChannel() < 16 ? int(partChannel()) : -1;
    if (partToneKnown_[size_t(part_)]) {
        editor_.loadTone(partTones_[size_t(part_)]);
        editor_.clearHistory();
    }
    if (!unitInputName_.empty()) requestPartTone();
}

void ToneEditorApp::followPartTone() {
    if (!unitInputName_.empty()) {
        requestPartTone();
    } else if (performanceTones()) {
        addLog(std::string(part_ == 0 ? "The upper" : "The lower") + " tone changed: connect the unit's MIDI OUT to edit it here");
    } else {
        addLog("Part " + std::to_string(part_ + 1) + " plays another tone now: connect the unit's MIDI OUT to edit it here");
    }
}

void ToneEditorApp::requestPartTone() {
    link_.request(Tone::toneTempAddress(part_), Tone::kSize);
    if (performanceTones()) {
        setup_.requestPatchTemp(*this);  // The patch selects the upper and lower tones
    } else {
        link_.request(kTimbreTemp + uint32_t(part_) * 16, 2);
    }
    toneRequested_ = true;
}

void ToneEditorApp::requestLibrary() {
    if (libraryRequested_ > 0 && link_.stats().requestsWaiting > 0) return;  // Already reading
    for (int slot = 0; slot < 64; slot++) link_.request(Tone::toneMemoryAddress(slot), Tone::kSize);
    libraryRequested_ = 64;
    addLog("Reading the unit's tones i11-i88 (about 7 seconds)");
}

void ToneEditorApp::writeToMemory(int slot, bool card) {
    editor_.settle(*this);  // The unit's tone temporary area holds the edit (no solo)
    slot = std::clamp(slot, 0, 63);
    const bool writeToCard = card && model_ != Tone::Model::MT32;
    const Tone::Data tone = editor_.tone();
    if (model_ == Tone::Model::MT32) {
        // The MT-32 has no write requests: the timbre goes to memory directly, and the part selects it.
        link_.sendData(Tone::toneMemoryAddress(slot), tone.data(), Tone::kSize, false);
        const uint8_t select[2] = {2, uint8_t(slot)};
        link_.sendData(kTimbreTemp + uint32_t(part_) * 16, select, 2, false);
        partToneSelection_[size_t(part_)] = 128 + slot;
    } else {
        // The unit's tone write: copies the part's tone temporary area into memory and points the part at it.
        const uint8_t request[2] = {uint8_t(slot), uint8_t(writeToCard ? 1 : 0)};
        lastWriteRequest_ = kToneWrite + uint32_t(part_) * 2;
        link_.sendData(lastWriteRequest_, request, 2, false);
        if (!writeToCard) partToneSelection_[size_t(part_)] = 128 + slot;
        // In performance mode the patch now plays the written tone as its upper or lower tone.
        if (performanceTones() && !unitInputName_.empty()) setup_.requestPatchTemp(*this);
    }
    if (!writeToCard) {
        memory_[size_t(slot)] = tone;
        memoryKnown_[size_t(slot)] = true;
    }
    editor_.markOriginal();
    std::string where = Tone::slotName(slot);
    if (writeToCard) where[0] = 'c';
    addLog("Tone \"" + Tone::name(tone) + "\" written to " + where + (model_ == Tone::Model::MT32 ? " (MT-32 memory timbre " + std::to_string(slot + 1) + ")" : ""));
}

void ToneEditorApp::loadIntoEditor(const Tone::Data& tone, const std::string& where, bool mt32) {
    Tone::Data data = tone;
    if (mt32 && model_ != Tone::Model::MT32) {
        Mt32Translator translator;
        translator.convertTone(tone.data(), data.data());
    }
    editor_.replaceTone(*this, data);
    addLog("Loaded \"" + Tone::name(data) + "\" (" + where + (mt32 && model_ != Tone::Model::MT32 ? ", MT-32 waves translated" : "") +
           ") into " + partText());
}

void ToneEditorApp::processReceived() {
    for (const UnitLink::Received& received : link_.takeReceived()) {
        // Tone temporary areas of parts 1-8: the part's tone as requested, or parameters from the unit's panel.
        for (int part = 0; part < 8; part++) {
            Tone::Data& tone = partTones_[size_t(part)];
            Tone::Data before = tone;
            int count = 0;
            const int first = copyOverlap(received, Tone::toneTempAddress(part), Tone::kSize, tone.data(), count);
            if (first < 0) continue;
            const bool whole = first == 0 && count == Tone::kSize;
            if (!whole && !partToneKnown_[size_t(part)]) {
                tone = before;
                continue;
            }
            partToneKnown_[size_t(part)] = true;
            if (part != part_) continue;
            if (whole && (toneRequested_ || followUnit_)) {
                editor_.loadTone(tone);
                if (toneRequested_) {
                    addLog("Got " + (performanceTones() ? partText() : partText() + "'s tone") + " \"" + Tone::name(tone) + "\" from the unit");
                }
                toneRequested_ = false;
            } else if (!whole && followUnit_) {
                editor_.takeExternal(first, tone.data() + first, count);
            }
        }
        // Tone memory i11-i88 (the MT-32's memory timbres).
        for (int slot = 0; slot < 64; slot++) {
            Tone::Data& tone = memory_[size_t(slot)];
            const Tone::Data before = tone;
            int count = 0;
            const int first = copyOverlap(received, Tone::toneMemoryAddress(slot), Tone::kSize, tone.data(), count);
            if (first < 0) continue;
            if (first == 0 && count == Tone::kSize) {
                memoryKnown_[size_t(slot)] = true;
            } else if (!memoryKnown_[size_t(slot)]) {
                tone = before;
            }
        }
        // Timbre temporary areas: which tone each part selects.
        for (int part = 0; part < 8; part++) {
            uint8_t bytes[2] = {0, 0};
            int count = 0;
            const int first = copyOverlap(received, kTimbreTemp + uint32_t(part) * 16, 2, bytes, count);
            if (first == 0 && count == 2) partToneSelection_[size_t(part)] = (bytes[0] & 3) * 64 + (bytes[1] & 63);
        }
        // The setup's areas (parts, rhythm setup, system area), and the channels the unit has for its parts.
        const bool missingBefore = setup_.channelsMissing();
        setup_.take(received.address, received.data.data(), received.data.size());
        std::array<uint8_t, UnitSetup::kParts> channels{};
        if (setup_.takeChannelsFromUnit(channels)) {
            thruChannel_ = partChannel() < 16 ? int(partChannel()) : -1;
            addLog("Read the parts' MIDI channels from the unit");
        }
        if (setup_.channelsMissing() && !missingBefore) {
            addLog("The unit's system area holds no part channels (a D-10/D-20?): choose D-10/D-20 under Unit and set its "
                   "channels on the System tab");
        }
        // A write request's result.
        if (received.address == kWriteResult && !received.data.empty()) {
            static const char* const results[] = {"done", "card not ready", "write protected (turn Memory Protect off)", "wrong mode"};
            std::string result = results[std::min<uint8_t>(received.data[0], 3)];
            // A D-10/D-20 writes timbres in multi-timbral mode only, and patches in performance mode only.
            if (received.data[0] >= 3 && lastWriteRequest_ >= kTimbreWrite && lastWriteRequest_ < kTimbreWrite + 16) {
                result += " (a D-10/D-20 writes timbres in multi-timbral mode)";
            } else if (received.data[0] >= 3 && lastWriteRequest_ == kPatchWrite) {
                result += " (a D-10/D-20 writes patches in performance mode)";
            }
            addLog("The unit's write: " + result);
        }
    }
}

void ToneEditorApp::addLog(const std::string& line) {
    log_.push_back(line);
    while (log_.size() > kMaxLogLines) log_.pop_front();
}

// ---------------------------------------------------------------------------------------------
// UI

void ToneEditorApp::frame() {
    processReceived();
    for (const std::string& line : link_.takeLog()) addLog(line);
    handleComputerKeyboard();

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("ToneEditor", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    drawMenu();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float statusHeight = ImGui::GetTextLineHeight() + style.ItemSpacing.y;
    const float bodyHeight = std::max(ImGui::GetContentRegionAvail().y - statusHeight, 100.0f);
    const float leftWidth = std::min(ImGui::GetFontSize() * 23.0f, ImGui::GetContentRegionAvail().x * 0.38f);
    ImGui::BeginChild("settings", ImVec2(leftWidth, bodyHeight), ImGuiChildFlags_Borders);
    drawPorts();
    drawUnit();
    drawTone();
    drawLibrary();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("editor", ImVec2(0.0f, bodyHeight), ImGuiChildFlags_Borders);
    const double now = ImGui::GetTime();
    bool toneShown = false;
    if (ImGui::BeginTabBar("views")) {
        const int wanted = requestedTab_;  // Requests made during this frame apply on the next
        requestedTab_ = -1;
        auto flags = [&](int tab) { return wanted == tab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None; };
        if (ImGui::BeginTabItem("Tone", nullptr, flags(0))) {
            editor_.draw(*this, now);
            toneShown = true;
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Parts", nullptr, flags(1))) {
            UnitSetup::PartsEvents events;
            setup_.drawParts(*this, part_, events);
            if (events.editPart >= 0) {
                setPerformanceMode(false);  // The parts play in multi-timbral mode
                selectPart(events.editPart);
                requestedTab_ = 0;
                saveSettings();
            }
            if (events.editedToneChanged) followPartTone();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(model_ == Tone::Model::MT32 ? "Patches" : "Timbres", nullptr, flags(2))) {
            UnitSetup::PartsEvents events;
            setup_.drawTimbres(*this, part_, events);
            if (events.editedToneChanged) followPartTone();
            ImGui::EndTabItem();
        }
        if (model_ == Tone::Model::D20 && ImGui::BeginTabItem("Performance", nullptr, flags(5))) {
            if (!patchRead_ && !unitInputName_.empty()) {
                setup_.requestPatch(*this);  // The first time: what the unit holds
                patchRead_ = true;
            }
            UnitSetup::PatchEvents events;
            setup_.drawPatch(*this, events);
            if (events.editTone >= 0) {
                setPerformanceMode(true);  // Notes on the performance channel
                selectPart(events.editTone);
                requestedTab_ = 0;
                saveSettings();
            }
            if ((events.upperToneChanged && part_ == 0) || (events.lowerToneChanged && part_ == 1)) followPartTone();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Rhythm", nullptr, flags(3))) {
            setup_.drawRhythm(*this, now);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("System", nullptr, flags(4))) {
            setup_.drawSystem(*this);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    if (!toneShown) editor_.update(*this, now);  // The audition's timers keep running
    setup_.update(*this, now);
    thruChannel_ = partChannel() < 16 ? int(partChannel()) : -1;
    const UnitLink::Stats stats = link_.stats();
    ImGui::TextDisabled("%s", log_.empty() ? "Ready" : log_.back().c_str());
    if (ImGui::IsItemClicked()) showLog_ = true;
    UiStyle::setItemTooltip("Click to show the log");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - ImGui::GetFontSize() * 17.0f);
    ImGui::TextDisabled("sent %llu, merged %llu, from unit %llu", static_cast<unsigned long long>(stats.sent),
                        static_cast<unsigned long long>(stats.merged), static_cast<unsigned long long>(stats.received));
    UiStyle::setItemTooltip("Messages to the unit, parameter changes merged into waiting ones (the unit gets the latest "
                          "value without being flooded), and data sets from the unit.");
    ImGui::End();
    if (showLog_) drawLog();
}

void ToneEditorApp::drawMenu() {
    if (!ImGui::BeginMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open SysEx file...")) browseFile();
        UiStyle::setItemTooltip("Tones from a SysEx file (a tone, a bulk dump, MT-32 timbres) go to the library");
        if (ImGui::MenuItem("Save tone as SysEx...")) saveToneFile();
        if (ImGui::MenuItem("Save the unit's tones as SysEx...")) saveLibrary();
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) quitRequested_ = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Unit")) {
        if (ImGui::MenuItem("Get the part's tone")) requestPartTone();
        if (ImGui::MenuItem("Send the tone to the part")) editor_.sendAll(*this);
        if (ImGui::MenuItem("Read all tones (i11-i88)")) requestLibrary();
        ImGui::Separator();
        if (ImGui::MenuItem("All notes off")) {
            releaseAllNotes();
            for (int channel = 0; channel < 16; channel++) link_.sendShort(uint32_t(0xB0 | channel) | 123u << 8);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Log", nullptr, &showLog_)) saveSettings();
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
}

void ToneEditorApp::drawPorts() {
    if (!ImGui::CollapsingHeader("MIDI ports", ImGuiTreeNodeFlags_DefaultOpen)) return;
    const float fontSize = ImGui::GetFontSize();
    auto combo = [&](const char* label, const std::vector<std::string>& ports, const std::string& current, bool output) {
        ImGui::SetNextItemWidth(-fontSize * 7.5f);
        if (ImGui::BeginCombo(label, current.empty() ? "(none)" : current.c_str())) {
            if (ImGui::Selectable("(none)", current.empty())) {
                output ? openUnitOutput(std::string()) : openUnitInput(std::string());
                saveSettings();
            }
            for (const std::string& name : ports) {
                if (ImGui::Selectable(name.c_str(), name == current) && name != current) {
                    output ? openUnitOutput(name) : openUnitInput(name);
                    saveSettings();
                    if (!output) {
                        requestPartTone();
                        setup_.requestAll(*this);
                    }
                }
            }
            ImGui::EndCombo();
        }
    };
    combo("To the unit", outputPorts_, unitOutputName_, true);
    helpMarker("The MIDI output connected to the unit's MIDI IN. Every change goes there at once.");
    combo("From the unit", inputPorts_, unitInputName_, false);
    helpMarker("The MIDI input connected to the unit's MIDI OUT (optional). With it, the editor reads tones from the unit "
               "(the part's tone, the tone memory) and follows parameters edited on the unit's panel.");
    ImGui::TextUnformatted("Keyboards");
    helpMarker("MIDI keyboards whose notes, bender and controllers play the edited part (on its channel), so held notes "
               "can be played again after a change. A D-20's own keyboard plays the unit directly and needs no port here.");
    if (inputPorts_.empty()) ImGui::TextDisabled("  No MIDI inputs found.");
    for (const std::string& name : inputPorts_) {
        if (name == unitInputName_) continue;  // Its notes would come back to the unit
        bool open = keyboards_->isOpen(name);
        if (ImGui::Checkbox((name + "##keyboard").c_str(), &open)) {
            openKeyboard(name, open);
            saveSettings();
        }
    }
    const std::string own = keyboards_ ? keyboards_->ownPortName() : std::string();
    if (!own.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("Programs on this computer can play the part through %s (%s).", own.c_str(), keyboards_->ownPortHint().c_str());
        ImGui::PopStyleColor();
    }
    if (ImGui::Button("Refresh ports")) refreshPorts();
    if (!portError_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
        ImGui::TextWrapped("%s", portError_.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
}

void ToneEditorApp::drawUnit() {
    if (!ImGui::CollapsingHeader("Unit", ImGuiTreeNodeFlags_DefaultOpen)) return;
    const float fontSize = ImGui::GetFontSize();
    bool changed = false;
    int model = model_ == Tone::Model::D110 ? 0 : model_ == Tone::Model::D20 ? 1 : 2;
    changed |= ImGui::RadioButton("D-110", &model, 0);
    ImGui::SameLine();
    changed |= ImGui::RadioButton("D-10/D-20", &model, 1);
    ImGui::SameLine();
    changed |= ImGui::RadioButton("MT-32", &model, 2);
    helpMarker("D-110 and D-10/D-20: the same sound engine and tones; they differ in their setup (the D-20 sets its part "
               "channels on its panel). On a D-10/D-20, set the unit number and turn MIDI Exclusive on in the MIDI function "
               "menu.\n\nMT-32 (and CM-32L): the MT-32's waves and setup. It has no tone write: Write stores the timbre "
               "directly.");
    if (changed) setModel(model == 0 ? Tone::Model::D110 : model == 1 ? Tone::Model::D20 : Tone::Model::MT32);

    ImGui::SetNextItemWidth(fontSize * 6.5f);
    if (ImGui::InputInt("Unit number", &unitNumber_)) {
        unitNumber_ = std::clamp(unitNumber_, 17, 32);
        changed = true;
    }
    helpMarker("As set on the unit (17-32; an MT-32 is 17). SysEx for the unit uses its device ID, the unit number less one.");

    if (model_ == Tone::Model::D20) {
        bool performance = performanceMode_;
        if (ImGui::Checkbox("Performance mode", &performance)) {
            setPerformanceMode(performance);
            changed = true;
        }
        helpMarker("Check this while the unit is in performance mode: Upper and Lower edit the patch's two tones (parts 1 and 2's "
                   "tone temporary areas), and the editor's notes go to the performance channel. The Performance tab edits the "
                   "patch itself.");
    }
    ImGui::AlignTextToFramePadding();
    if (performanceTones()) {
        ImGui::TextUnformatted("Tone");
        for (int part = 0; part < 2; part++) {
            ImGui::SameLine(0.0f, part == 0 ? -1.0f : 3.0f);
            const bool current = part_ == part;
            if (current) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(part == 0 ? "Upper" : "Lower") && !current) {
                selectPart(part);
                changed = true;
            }
            if (current) ImGui::PopStyleColor();
            UiStyle::setItemTooltip(part == 0 ? "The patch's upper tone (part 1's tone temporary area)"
                                            : "The patch's lower tone (part 2's tone temporary area)");
        }
    } else {
        ImGui::TextUnformatted("Part");
        for (int part = 0; part < 8; part++) {
            ImGui::SameLine(0.0f, part == 0 ? -1.0f : 3.0f);
            ImGui::PushID(part);
            const bool current = part_ == part;
            if (current) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(std::to_string(part + 1).c_str(), ImVec2(ImGui::GetFrameHeight(), 0.0f)) && !current) {
                selectPart(part);
                changed = true;
            }
            if (current) ImGui::PopStyleColor();
            ImGui::PopID();
        }
    }
    int channel = std::min<int>(partChannel(), 15) + 1;
    ImGui::SetNextItemWidth(fontSize * 6.5f);
    if (ImGui::InputInt("Channel", &channel)) {
        releaseAllNotes();
        if (performanceTones()) {
            setup_.performanceChannel() = uint8_t(std::clamp(channel, 1, 16) - 1);
        } else {
            setup_.setChannel(*this, part_, std::clamp(channel, 1, 16) - 1);
        }
        thruChannel_ = partChannel() < 16 ? int(partChannel()) : -1;
        changed = true;
    }
    helpMarker(performanceTones()
                   ? "The unit's receive channel in performance mode (Rx CH in its MIDI function menu). The editor's notes play "
                     "the whole patch there: the lower tone sounds in Dual mode, and below the split point in Split mode."
                   : "The part's MIDI channel, for the notes the editor plays. A D-110 or MT-32 takes it too; on a D-20 it is "
                     "set in its MIDI function menu. The System tab has all parts'.");
    if (model_ != Tone::Model::D20) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Read")) setup_.requestAll(*this);
        UiStyle::setItemTooltip("Reads the unit's setup, with the parts' channels (needs the \"From the unit\" port)");
    }
    ImGui::SetNextItemWidth(fontSize * 6.5f);
    if (ImGui::InputInt("SysEx pause", &pauseMs_)) {
        pauseMs_ = std::clamp(pauseMs_, 0, 200);
        changed = true;
    }
    helpMarker("Milliseconds between parameter changes, and after a whole tone (shorter messages pause less). Changes that "
               "come faster are merged, so the unit always gets the latest value. Raise it if the unit shows \"Exclusive "
               "Buffer Full\" or lags.");
    if (ImGui::Checkbox("Follow the unit's panel", &followUnit_)) changed = true;
    helpMarker("Parameters the unit sends while you edit on its panel (a D-110 sends each one; a D-20 with Timbre Dump on) "
               "update the editor. Needs the \"From the unit\" port.");
    if (changed) {
        applyUnitSettings();
        saveSettings();
    }
    ImGui::Spacing();
}

void ToneEditorApp::drawTone() {
    if (!ImGui::CollapsingHeader("Tone", ImGuiTreeNodeFlags_DefaultOpen)) return;
    const float fontSize = ImGui::GetFontSize();
    if (ImGui::Button("Get from the unit")) requestPartTone();
    UiStyle::setItemTooltip("Reads the part's tone from the unit (needs the \"From the unit\" port)");
    ImGui::SameLine();
    if (ImGui::Button("Send to the unit")) editor_.sendAll(*this);
    UiStyle::setItemTooltip("Sends the whole tone to the part, e.g. after switching the unit on");
    // What the part selects, as the unit last reported it.
    const int group = setup_.partValue(part_, UnitSetup::ToneGroup), number = setup_.partValue(part_, UnitSetup::ToneNumber);
    const int selection = group >= 0 && number >= 0 ? group * 64 + number : partToneSelection_[size_t(part_)];
    if (performanceTones()) {
        const int offset = part_ == 0 ? UnitSetup::UpperToneGroup : UnitSetup::LowerToneGroup;
        const int patchGroup = setup_.patchValue(offset), patchNumber = setup_.patchValue(offset + 1);
        if (patchGroup >= 0 && patchNumber >= 0) {
            ImGui::TextDisabled("The %s tone is %s", part_ == 0 ? "upper" : "lower", setup_.toneLabel(*this, patchGroup, patchNumber).c_str());
        }
    } else if (selection >= 0) {
        ImGui::TextDisabled("Part %d plays %s", part_ + 1, setup_.toneLabel(*this, selection / 64, selection % 64).c_str());
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Write to");
    ImGui::SameLine();
    auto slotLabel = [&](int slot) {
        std::string label = Tone::slotName(slot);
        if (memoryKnown_[size_t(slot)]) label += " " + Tone::name(memory_[size_t(slot)]);
        return label;
    };
    ImGui::SetNextItemWidth(fontSize * 8.0f);
    if (ImGui::BeginCombo("##slot", slotLabel(writeSlot_).c_str(), ImGuiComboFlags_HeightLarge)) {
        for (int slot = 0; slot < 64; slot++) {
            if (ImGui::Selectable(slotLabel(slot).c_str(), slot == writeSlot_)) writeSlot_ = slot;
            if (slot == writeSlot_ && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Write")) ImGui::OpenPopup("write");
    UiStyle::setItemTooltip(model_ == Tone::Model::MT32 ? "Stores the tone as this memory timbre and selects it on the part"
                                                      : "The unit's tone write: stores the part's tone in this slot and points the part at it");
    if (ImGui::BeginPopup("write")) {
        std::string where = Tone::slotName(writeSlot_);
        if (writeToCard_) where[0] = 'c';
        ImGui::Text("Replace %s%s with \"%s\"?", where.c_str(),
                    memoryKnown_[size_t(writeSlot_)] && !writeToCard_ ? (" \"" + Tone::name(memory_[size_t(writeSlot_)]) + "\"").c_str() : "",
                    Tone::name(editor_.tone()).c_str());
        if (model_ != Tone::Model::MT32) {
            ImGui::Checkbox("On the memory card", &writeToCard_);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
            ImGui::TextWrapped("%s", setup_.memoryProtectNote());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
        if (ImGui::Button("Write")) {
            writeToMemory(writeSlot_, writeToCard_);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::Button("Open SysEx...")) browseFile();
    ImGui::SameLine();
    if (ImGui::Button("Save tone...")) saveToneFile();
    ImGui::Spacing();
}

void ToneEditorApp::drawLibrary() {
    if (!ImGui::CollapsingHeader("Library", ImGuiTreeNodeFlags_DefaultOpen)) return;
    const UnitLink::Stats stats = link_.stats();
    if (ImGui::Button("Read all from the unit")) requestLibrary();
    UiStyle::setItemTooltip("Reads tones i11-i88 from the unit's memory (needs the \"From the unit\" port). They are kept "
                          "for the next session.");
    ImGui::SameLine();
    if (stats.requestsWaiting > 0 && libraryRequested_ > 0) {
        ImGui::TextDisabled("%d left", int(stats.requestsWaiting));
    } else {
        libraryRequested_ = 0;
        if (ImGui::Button("Save...")) saveLibrary();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Click a tone to load it into %s.", partText().c_str());
    ImGui::PopStyleColor();
    ImGui::BeginChild("library", ImVec2(0.0f, std::max(ImGui::GetContentRegionAvail().y, ImGui::GetFontSize() * 10.0f)), ImGuiChildFlags_Borders);
    ImGui::SeparatorText("Unit memory");
    int known = 0;
    for (int slot = 0; slot < 64; slot++) {
        if (!memoryKnown_[size_t(slot)]) continue;
        known++;
        const std::string label = Tone::slotName(slot) + "  " + Tone::name(memory_[size_t(slot)]) + "##memory" + std::to_string(slot);
        if (ImGui::Selectable(label.c_str())) loadIntoEditor(memory_[size_t(slot)], Tone::slotName(slot), false);
    }
    if (known == 0) ImGui::TextDisabled("Not read yet");
    if (!fileTones_.empty()) {
        ImGui::SeparatorText(fileName_.c_str());
        if (model_ != Tone::Model::MT32) {
            ImGui::Checkbox("Translate MT-32 waves", &fileMt32_);
            UiStyle::setItemTooltip("The MT-32's waves are other samples than the D-series'. This picks the closest D-series "
                                  "wave for each PCM partial and corrects its pitch, as MT32Translator does.");
        }
        for (size_t i = 0; i < fileTones_.size(); i++) {
            const Tone::FoundTone& found = fileTones_[i];
            const std::string label = found.where + "  " + Tone::name(found.data) + "##file" + std::to_string(i);
            if (ImGui::Selectable(label.c_str())) loadIntoEditor(found.data, found.where + " of " + fileName_, fileMt32_);
        }
    }
    ImGui::EndChild();
}

void ToneEditorApp::drawLog() {
    const float fontSize = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(fontSize * 40.0f, fontSize * 16.0f), ImGuiCond_FirstUseEver);
    const bool visible = ImGui::Begin("Log", &showLog_);
    if (!showLog_) saveSettings();
    if (visible) {
        if (ImGui::Button("Clear")) log_.clear();
        ImGui::BeginChild("lines", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        for (const std::string& line : log_) ImGui::TextUnformatted(line.c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }
    ImGui::End();
}
