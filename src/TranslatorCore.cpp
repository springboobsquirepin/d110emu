#include "TranslatorCore.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iterator>
#include <sstream>

#include "Platform.h"
#include "RomLibrary.h"
#include "SmfFile.h"

namespace {

constexpr size_t kMaxLogLines = 2000;
constexpr double kPortCheckSeconds = 2.0;

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

bool hasExtension(const std::filesystem::path& path, std::initializer_list<const char*> extensions) {
    std::string extension = Platform::toUtf8(path.extension());
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    for (const char* wanted : extensions) {
        if (extension == wanted) return true;
    }
    return false;
}

bool contains(const std::vector<std::string>& names, const std::string& name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

constexpr uint32_t pack(uint32_t high, uint32_t mid, uint32_t low) {
    return (high << 14) | (mid << 7) | low;
}

}  // namespace

TranslatorCore::TranslatorCore() = default;

TranslatorCore::~TranslatorCore() {
    shutdown();
}

bool TranslatorCore::isMidiFile(const std::filesystem::path& path) {
    return hasExtension(path, {".mid", ".midi", ".smf", ".rmi"});
}

void TranslatorCore::init(const TranslatorOptions& options) {
    options_ = options;
    // Other programs see its MIDI ports as MT32Translator's (ALSA, CoreMIDI). Its own input is where an MT-32 program
    // on this computer can send; the unit's answers come on a port of their own.
    if (options_.enableMidi) setMidiProgramName("MT32Translator");
    inputs_.reset(new MidiInputManager(pipe_, true));
    unitInputs_.reset(new MidiInputManager(pipe_.unitSink(), false));
    loadSettings();
    loadFrontEndSettings();
    loadPresets();
    applyPipeSettings();
    loadCacheFile();
    pipe_.start();
    if (options_.enableMidi) {
        refreshPorts();
        for (const std::string& name : std::vector<std::string>(enabledInputs_)) openInput(name, true);
        openUnitOutput(unitPortName_);
        openReplyOutput(replyPortName_);
        openUnitInput(unitInputName_);
        nextPortCheck_ = now() + kPortCheckSeconds;
    }
    if (powerOnAtStart_) pipe_.powerOn();
    addLog("MT32Translator ready: MIDI for an MT-32 in, " +
           std::string(pipeSettings_.target == Mt32Translator::Target::D20 ? "D-10/D-20" : "D-110") + " MIDI out");
}

void TranslatorCore::shutdown() {
    if (!inputs_) return;
    saveSettings();
    saveCacheFile();
    inputs_->closeAll();
    unitInputs_->closeAll();
    pipe_.setOutputs(nullptr, nullptr);
    pipe_.stop();
    unitPort_.close();
    replyPort_.close();
    inputs_.reset();
    unitInputs_.reset();
}

void TranslatorCore::update() {
    for (const std::string& line : pipe_.takeLog()) addLog(line);
    if (options_.enableMidi && inputs_ && now() >= nextPortCheck_) {
        nextPortCheck_ = now() + kPortCheckSeconds;
        checkPorts();
    }
}

double TranslatorCore::now() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------------------------
// Settings

void TranslatorCore::loadSettings() {
    settings_.load(options_.settingsFile);
    pipeSettings_.target = settings_.getString("target", "d20") == "d110" ? Mt32Translator::Target::D110 : Mt32Translator::Target::D20;
    pipeSettings_.unitNumber = std::clamp(settings_.getInt("unit_number", 17), 17, 32);
    const std::vector<std::string> channels = split(settings_.getString("unit_channels"), ',');
    if (channels.size() == size_t(kPartCount)) {
        for (int part = 0; part < kPartCount; part++) {
            const int channel = std::atoi(channels[size_t(part)].c_str());
            pipeSettings_.unitChannels[part] = uint8_t(channel >= 1 && channel <= 16 ? channel - 1 : 16);
        }
    }
    pipeSettings_.memoryInUnit = settings_.getBool("memory_in_unit", pipeSettings_.target == Mt32Translator::Target::D110);
    pipeSettings_.masterVolumeAsVolume = settings_.getBool("master_volume_cc7", true);
    const std::string mode = settings_.getString("preset_mode", settings_.getBool("exact_presets", false) ? "exact" : "hybrid");
    pipeSettings_.presetMode = mode == "exact"      ? Mt32Translator::PresetMode::Exact
                               : mode == "standins" ? Mt32Translator::PresetMode::StandIns
                                                    : Mt32Translator::PresetMode::Hybrid;
    Mt32Translator::parsePresetChoices(settings_.getString("preset_choices"), pipeSettings_.presetChoices);
    pipeSettings_.roomyToms = settings_.getBool("roomy_toms", false);
    pipeSettings_.reduceLoad = settings_.getBool("reduce_load", true);
    cacheEnabled_ = settings_.getBool("cache_enabled", false);
    cacheFirst_ = std::clamp(settings_.getInt("cache_first", 32), 0, 63);
    cacheLast_ = std::clamp(settings_.getInt("cache_last", 63), cacheFirst_, 63);
    unitInputName_ = settings_.getString("unit_input");
    pipeSettings_.sysexGapMs = std::clamp(settings_.getInt("sysex_gap_ms", 20), 0, 200);
    powerOnAtStart_ = settings_.getBool("power_on_at_start", false);
    presetRom_ = Platform::fromUtf8(settings_.getString("mt32_control_rom"));
    enabledInputs_ = split(settings_.getString("inputs"), '|');
    unitPortName_ = settings_.getString("unit_output");
    replyPortName_ = settings_.getString("reply_output");
}

void TranslatorCore::saveSettings() {
    settings_.set("target", pipeSettings_.target == Mt32Translator::Target::D110 ? "d110" : "d20");
    settings_.set("unit_number", pipeSettings_.unitNumber);
    std::string channels;
    for (int part = 0; part < kPartCount; part++) {
        if (part > 0) channels += ',';
        const uint8_t channel = pipeSettings_.unitChannels[part];
        channels += channel < 16 ? std::to_string(channel + 1) : std::string("off");
    }
    settings_.set("unit_channels", channels);
    settings_.set("memory_in_unit", pipeSettings_.memoryInUnit);
    settings_.set("master_volume_cc7", pipeSettings_.masterVolumeAsVolume);
    settings_.set("preset_mode", pipeSettings_.presetMode == Mt32Translator::PresetMode::Exact      ? "exact"
                                 : pipeSettings_.presetMode == Mt32Translator::PresetMode::StandIns ? "standins"
                                                                                                     : "hybrid");
    settings_.set("preset_choices", Mt32Translator::presetChoicesText(pipeSettings_.presetChoices));
    settings_.set("roomy_toms", pipeSettings_.roomyToms);
    settings_.set("reduce_load", pipeSettings_.reduceLoad);
    settings_.set("cache_enabled", cacheEnabled_);
    settings_.set("cache_first", cacheFirst_);
    settings_.set("cache_last", cacheLast_);
    settings_.set("unit_input", unitInputName_);
    settings_.set("sysex_gap_ms", pipeSettings_.sysexGapMs);
    settings_.set("power_on_at_start", powerOnAtStart_);
    settings_.set("mt32_control_rom", Platform::toUtf8(presetRom_));
    std::string inputs;
    for (const std::string& name : enabledInputs_) inputs += (inputs.empty() ? "" : "|") + name;
    settings_.set("inputs", inputs);
    settings_.set("unit_output", unitPortName_);
    settings_.set("reply_output", replyPortName_);
    settings_.save(options_.settingsFile);
}

void TranslatorCore::applyPipeSettings() {
    // The cache works while the translator keeps the memories.
    const bool cache = cacheEnabled_ && !pipeSettings_.memoryInUnit;
    pipeSettings_.cacheFirst = cache ? cacheFirst_ : 0;
    pipeSettings_.cacheLast = cache ? cacheLast_ : -1;
    pipe_.configure(pipeSettings_, presets_);
}

void TranslatorCore::setTarget(Mt32Translator::Target target) {
    pipeSettings_.target = target;
    pipeSettings_.memoryInUnit = target == Mt32Translator::Target::D110;  // Each unit's default
}

void TranslatorCore::setPresetChoicesBuiltIn() {
    for (int timbre = 0; timbre < 128; timbre++) pipeSettings_.presetChoices[size_t(timbre)] = Mt32Translator::defaultPresetChoice(timbre);
}

// The cache file: a DT1 to tone memory (08 xx 00) per slot the translator knows the content of.
void TranslatorCore::loadCacheFile() {
    if (options_.cacheFile.empty() || !cacheEnabled_) return;
    const std::vector<uint8_t> data = readFile(options_.cacheFile);
    std::vector<std::vector<uint8_t>> contents(size_t(cacheLast_ - cacheFirst_ + 1));
    size_t restored = 0;
    for (size_t i = 0; i + 10 <= data.size();) {
        const size_t end = size_t(std::find(data.begin() + long(i), data.end(), uint8_t(0xF7)) - data.begin());
        if (end >= data.size()) break;
        const uint8_t* m = &data[i];
        const size_t length = end + 1 - i;
        i = end + 1;
        if (length < 10 + 246 || m[0] != 0xF0 || m[1] != 0x41 || m[3] != 0x16 || m[4] != 0x12) continue;
        const uint32_t address = pack(m[5], m[6], m[7]);
        if (address < pack(0x08, 0, 0) || (address - pack(0x08, 0, 0)) % 256 != 0) continue;
        const int slot = int((address - pack(0x08, 0, 0)) / 256);
        if (slot < cacheFirst_ || slot > cacheLast_) continue;
        contents[size_t(slot - cacheFirst_)].assign(m + 8, m + 8 + 246);
        restored++;
    }
    pipe_.restoreToneCache(contents);
    if (restored > 0) addLog("Tone cache: " + std::to_string(restored) + " slots taken as holding their tones from the last session");
}

void TranslatorCore::saveCacheFile() {
    if (options_.cacheFile.empty()) return;
    const std::vector<std::vector<uint8_t>> contents = pipe_.toneCacheContents();
    std::vector<uint8_t> data;
    for (size_t i = 0; i < contents.size(); i++) {
        if (contents[i].size() != 246) continue;
        const uint32_t address = pack(0x08, 0, 0) + uint32_t(cacheFirst_ + int(i)) * 256;
        const size_t start = data.size();
        const uint8_t header[] = {0xF0, 0x41, 0x10, 0x16, 0x12, uint8_t(address >> 14), uint8_t((address >> 7) & 0x7F), uint8_t(address & 0x7F)};
        data.insert(data.end(), header, header + sizeof(header));
        data.insert(data.end(), contents[i].begin(), contents[i].end());
        unsigned sum = 0;
        for (size_t k = start + 5; k < data.size(); k++) sum += data[k];
        data.push_back(uint8_t((128 - sum % 128) % 128));
        data.push_back(0xF7);
    }
    std::error_code ec;
    if (data.empty()) {
        std::filesystem::remove(options_.cacheFile, ec);
        return;
    }
    std::ofstream out(options_.cacheFile, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
}

// The MT-32 presets: the chosen control ROM, else the first MT-32 (or CM-32L) control ROM in a "roms" folder.
void TranslatorCore::loadPresets() {
    presets_.reset();
    presetError_.clear();
    std::error_code ec;
    if (presetRom_.empty() || !std::filesystem::is_regular_file(presetRom_, ec)) {
        presetRom_.clear();
        std::filesystem::path cm32l;
        for (const RomEntry& rom : scanRomFolder(findRomFolder(options_.romSearchDirs))) {
            if (!rom.isControl) continue;
            const std::string family = romFamily(rom);
            if (family == "mt32") {
                presetRom_ = rom.path;
                break;
            }
            if (family == "cm32l" && cm32l.empty()) cm32l = rom.path;
        }
        if (presetRom_.empty()) presetRom_ = cm32l;
    }
    if (presetRom_.empty()) return;
    std::shared_ptr<Mt32Presets> presets = std::make_shared<Mt32Presets>();
    if (loadMt32Presets(presetRom_, *presets, presetError_)) {
        presets_ = presets;
    }
}

// ---------------------------------------------------------------------------------------------
// Ports

void TranslatorCore::refreshPorts() {
    inputPorts_ = inputs_->listPorts();
    outputPorts_ = MidiOutputPort::listPorts();
}

void TranslatorCore::openInput(const std::string& name, bool open) {
    if (open) {
        std::string error;
        if (!inputs_->isOpen(name) && !inputs_->open(name, error)) {
            portError_ = error;
            return;
        }
        if (std::find(enabledInputs_.begin(), enabledInputs_.end(), name) == enabledInputs_.end()) enabledInputs_.push_back(name);
    } else {
        inputs_->close(name);
        enabledInputs_.erase(std::remove(enabledInputs_.begin(), enabledInputs_.end(), name), enabledInputs_.end());
    }
}

void TranslatorCore::openUnitInput(const std::string& name) {
    for (const std::string& open : unitInputs_->openPorts()) unitInputs_->close(open);
    unitInputName_ = name;
    if (name.empty()) return;
    std::string error;
    if (!unitInputs_->open(name, error)) portError_ = error;
}

void TranslatorCore::openUnitOutput(const std::string& name) {
    pipe_.setOutputs(nullptr, replyPort_.isOpen() ? &replyPort_ : nullptr);
    unitPort_.close();
    unitPortName_ = name;
    if (name.empty()) return;
    std::string error;
    if (!unitPort_.open(name, error)) {
        portError_ = error;
        return;
    }
    pipe_.setOutputs(&unitPort_, replyPort_.isOpen() ? &replyPort_ : nullptr);
}

void TranslatorCore::openReplyOutput(const std::string& name) {
    pipe_.setOutputs(unitPort_.isOpen() ? &unitPort_ : nullptr, nullptr);
    replyPort_.close();
    replyPortName_ = name;
    if (name.empty()) return;
    std::string error;
    if (!replyPort_.open(name, error)) {
        portError_ = error;
        return;
    }
    pipe_.setOutputs(unitPort_.isOpen() ? &unitPort_ : nullptr, &replyPort_);
}

std::string TranslatorCore::ownInputName() const {
    return inputs_ ? inputs_->ownPortName() : std::string();
}

std::string TranslatorCore::ownInputHint() const {
    return inputs_ ? inputs_->ownPortHint() : std::string();
}

// The ports chosen that are not open are opened once they are there (a MIDI interface plugged in again, a program
// started after the translator); outputs that went are connected again when they are back.
void TranslatorCore::checkPorts() {
    refreshPorts();
    for (const std::string& name : enabledInputs_) {
        if (inputs_->isOpen(name) || !contains(inputPorts_, name)) continue;
        std::string error;
        if (inputs_->open(name, error)) {
            addLog("MIDI input " + name + ": connected");
            portError_.clear();
        }
    }
    if (!unitInputName_.empty() && !unitInputs_->isOpen(unitInputName_) && contains(inputPorts_, unitInputName_)) {
        std::string error;
        if (unitInputs_->open(unitInputName_, error)) addLog("MIDI input " + unitInputName_ + " (from the unit): connected");
    }
    checkOutput(unitPort_, unitPortName_, &TranslatorCore::openUnitOutput);
    checkOutput(replyPort_, replyPortName_, &TranslatorCore::openReplyOutput);
}

void TranslatorCore::checkOutput(MidiOutputPort& port, const std::string& wanted, void (TranslatorCore::*open)(const std::string&)) {
    if (wanted.empty()) return;
    if (!port.isOpen()) {
        if (!contains(outputPorts_, wanted)) return;
        (this->*open)(std::string(wanted));
        if (port.isOpen()) {
            addLog("MIDI output " + wanted + ": connected");
            portError_.clear();
        }
        return;
    }
    const bool was = port.connected();
    const bool is = port.reconnect();
    if (was && !is) addLog("MIDI output " + wanted + " went away: what is sent to it is lost until it is back");
    if (!was && is) addLog("MIDI output " + wanted + ": connected again");
}

// ---------------------------------------------------------------------------------------------
// Files

void TranslatorCore::sendSysexFile(const std::filesystem::path& path) {
    const std::vector<uint8_t> data = readFile(path);
    if (data.empty()) {
        addLog("Cannot read " + Platform::toUtf8(path.filename()));
        return;
    }
    addLog("Sending " + Platform::toUtf8(path.filename()) + " through the translation");
    pipe_.sendSysexData(data);
}

std::string TranslatorCore::translatedName(const std::filesystem::path& source) const {
    const std::string unit = pipeSettings_.target == Mt32Translator::Target::D20 ? "D-20" : "D-110";
    return Platform::toUtf8(source.stem()) + " (" + unit + ")" + (isMidiFile(source) ? ".mid" : ".syx");
}

bool TranslatorCore::canTranslate(const std::filesystem::path& source) {
    if (isMidiFile(source)) {
        SmfFile in;
        std::string error;
        if (loadSmfFile(source, in, error)) return true;
        addLog(error);
        return false;
    }
    if (!readFile(source).empty()) return true;
    addLog("Cannot read " + Platform::toUtf8(source.filename()));
    return false;
}

bool TranslatorCore::translateFileTo(const std::filesystem::path& source, const std::filesystem::path& target) {
    if (isMidiFile(source)) {
        SmfFile in;
        std::string error;
        if (!loadSmfFile(source, in, error)) {
            addLog(error);
            return false;
        }
        SmfFile out;
        translateMidiFile(in, out, pipeSettings_, presets_);
        if (!saveSmfFile(target, out, 480, 500000, error)) {
            addLog(error);
            return false;
        }
        addLog("Translated " + Platform::toUtf8(source.filename()) + " -> " + Platform::toUtf8(target.filename()));
        return true;
    }
    const std::vector<uint8_t> data = readFile(source);
    if (data.empty()) {
        addLog("Cannot read " + Platform::toUtf8(source.filename()));
        return false;
    }
    const std::vector<uint8_t> out = translateSysexFile(data, pipeSettings_, presets_);
    std::ofstream file(target, std::ios::binary);
    file.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
    if (!file) {
        addLog("Cannot write " + Platform::toUtf8(target));
        return false;
    }
    addLog("Translated " + Platform::toUtf8(source.filename()) + " -> " + Platform::toUtf8(target.filename()) + " (" +
           std::to_string(out.size()) + " bytes)");
    return true;
}

void TranslatorCore::addLog(const std::string& line) {
    log_.push_back(line);
    while (log_.size() > kMaxLogLines) log_.pop_front();
    onLogAdded(line);
}
