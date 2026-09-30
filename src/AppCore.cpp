#include "AppCore.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>

#include "Mt32Presets.h"
#include "Platform.h"
#include "ReverbSettingsFile.h"

using namespace UnitText;
using namespace UnitMemory;

// ---------------------------------------------------------------------------------------------
// Text and addresses

namespace UnitText {

namespace {
const char* const kNoteNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
}

std::string noteName(int note) {
    return kNoteNames[note % 12] + std::to_string(note / 12 - 1);
}

std::string masterTuneText(int value) {
    char text[16];
    std::snprintf(text, sizeof(text), "%.1f Hz", 440.0 * std::exp2((value - 64.0) / (128.0 * 12.0)));
    return text;
}

std::string formatTime(double seconds) {
    const int total = int(seconds);
    char text[16];
    std::snprintf(text, sizeof(text), "%d:%02d", total / 60, total % 60);
    return text;
}

std::string channelLabel(uint8_t channel) {
    return channel < 16 ? std::to_string(channel + 1) : "Off";
}

std::string partLabel(int part) {
    if (part == kRhythmPart) return "R";
    return std::to_string(part < kRhythmPart ? part + 1 : part);
}

std::string toneCode(int group, int number) {
    char text[16];
    std::snprintf(text, sizeof(text), "%c%d%d", kToneGroups[std::clamp(group, 0, kToneGroupCount - 1)], (number & 63) / 8 + 1, (number & 7) + 1);
    return text;
}

std::string timbreCode(int timbre) {
    char text[16];
    const char memory = timbre >= 256 ? 'P' : timbre >= 128 ? 'C' : 'I';
    const char bank = char((timbre >= 256 ? 'D' : 'A') + ((timbre & 127) < 64 ? 0 : 1));
    std::snprintf(text, sizeof(text), "%c-%c%d%d", memory, bank, (timbre & 63) / 8 + 1, (timbre & 7) + 1);
    return text;
}

std::string performanceCode(int patch) {
    char text[8];
    std::snprintf(text, sizeof(text), "%c%d%d", (patch & 127) < 64 ? 'A' : 'B', (patch & 63) / 8 + 1, (patch & 7) + 1);
    return text;
}

std::string patchCode(int patch) {
    char text[16];
    std::snprintf(text, sizeof(text), "%c-%d%d", patch >= 64 ? 'C' : 'I', (patch & 63) / 8 + 1, (patch & 7) + 1);
    return text;
}

std::string panLabel(int pan) {
    pan = std::clamp(pan, 0, 14);
    if (pan == 7) return "><";
    return pan < 7 ? std::to_string(7 - pan) + ">" : "<" + std::to_string(pan - 7);
}

std::string padded(const std::string& text, size_t width) {
    std::string result = text.substr(0, width);
    result.resize(width, ' ');
    return result;
}

std::vector<uint8_t> readBinaryFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace UnitText

namespace UnitMemory {

uint32_t addressPlus(uint32_t sysexAddress, uint32_t offset) {
    const uint32_t packed = (((sysexAddress >> 16) & 0x7F) << 14) + (((sysexAddress >> 8) & 0x7F) << 7) + (sysexAddress & 0x7F) + offset;
    return (((packed >> 14) & 0x7F) << 16) | (((packed >> 7) & 0x7F) << 8) | (packed & 0x7F);
}

}  // namespace UnitMemory

namespace {

std::vector<std::string> split(const std::string& text, char separator) {
    std::vector<std::string> parts;
    std::stringstream stream(text);
    std::string part;
    while (std::getline(stream, part, separator)) {
        if (!part.empty()) parts.push_back(part);
    }
    return parts;
}

// "1,2,3,4,5,6,7,8,10,..." (1-16, or "off") <-> 0-based channels with 16 = off, by part number.
// Settings from before the 16-part mode hold 9 entries, the others keep their defaults; settings from when it had
// a part 16 hold 17 entries, the last of which is ignored.
bool parseChannels(const std::string& text, std::array<uint8_t, kMaxPartCount>& channels, const std::array<uint8_t, kMaxPartCount>& defaults) {
    const std::vector<std::string> parts = split(text, ',');
    if (parts.size() != size_t(kBasePartCount) && parts.size() != size_t(kMaxPartCount) && parts.size() != size_t(kMaxPartCount + 1)) {
        return false;
    }
    channels = defaults;
    for (size_t i = 0; i < parts.size() && i < size_t(kMaxPartCount); i++) {
        if (parts[i] == "off") {
            channels[i] = kChannelOff;
            continue;
        }
        const int channel = std::atoi(parts[i].c_str());
        if (channel < 1 || channel > 16) return false;
        channels[i] = uint8_t(channel - 1);
    }
    return true;
}

std::string formatChannels(const std::array<uint8_t, kMaxPartCount>& channels) {
    std::string text;
    for (int i = 0; i < kMaxPartCount; i++) {
        if (i > 0) text += ",";
        text += channels[i] < 16 ? std::to_string(channels[i] + 1) : "off";
    }
    return text;
}

// What MT-32 translation can change (D-110 patches, D-20 performances and the card it leaves alone).
constexpr unsigned kMt32SnapshotContents = DumpTones | DumpTimbres | DumpRhythm | DumpSystem | DumpTemporary;

// A lock-free "at least" for the peak meters: the audio thread raises them, takeOutputPeaks() lowers them to zero.
void raise(std::atomic<float>& peak, float value) {
    float current = peak.load(std::memory_order_relaxed);
    while (value > current && !peak.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

}  // namespace

AppCore::AppCore() = default;

AppCore::~AppCore() {
    shutdown();
}

void useStandaloneFolder(AppOptions& options, const std::filesystem::path& folder, bool adoptOld) {
    options.settingsFile = folder / "d110emu.ini";
    options.memoryFile = folder / "d110emu-memory.syx";
    options.reverbSettingsFile = folder / "d110emu-reverb.ini";
    options.romSearchDirs.clear();
    const std::filesystem::path resources = Platform::bundleResourcesDirectory();
    if (!resources.empty()) options.romSearchDirs.push_back(resources);
    std::error_code ec;
    options.romSearchDirs.push_back(Platform::executableDirectory());
    options.romSearchDirs.push_back(std::filesystem::current_path(ec));
    options.romSearchDirs.push_back(folder);
    if (!adoptOld) return;
    const std::filesystem::path from =
        Platform::adoptOldFiles(folder, {Platform::programDirectory(), Platform::oldUserDataDirectory() / "D110Emu"}, "d110emu.ini",
                                {"d110emu-memory.syx", "d110emu-reverb.ini"});
    if (!from.empty()) {
        options.startupMessages.push_back("The settings and memory are kept in " + Platform::toUtf8(folder) + " now: copied from " +
                                          Platform::toUtf8(from) + ", where they are no longer used");
    }
}

void AppCore::init(const AppOptions& options) {
    options_ = options;
    battery_ = std::move(options_.memory);
    options_.memory.clear();
    engine_.reset(new SynthEngine);
    audio_.reset(new AudioOutput(options_.nullAudio));
    midi_.reset(new MidiInputManager(*engine_));

    for (const std::string& line : options_.startupMessages) addLog(line);
    loadSettings();
    // A plugin host's MIDI plays at the sample it is meant for; the on-screen keyboard's plays at once.
    if (options_.plugin) engineOptions_.midiTimestamping = false;
    engine_->setOptions(engineOptions_);
    engine_->setPartOutputMask(ownOutputs_);  // Kept by the engine across restarts
    engine_->setMultiPairsStereo(multiPairs_);
    loadReverbSettingsFile();
    scanRoms();
    if (options_.enableAudio) audioDevices_ = audio_->listDevices();
    startAudioAndSynth();  // Also restores the battery-backed memory of the last session
    if (options_.enableMidiInput) refreshMidiPorts();
}

void AppCore::shutdown() {
    if (!engine_) return;
    saveMemoryFile();
    saveCardFile();
    saveSettings();
    if (reverbSettingsDirty_) saveReverbSettingsFile();
    midi_->closeAll();
    audio_->close();
    engine_->close();
    midi_.reset();
    audio_.reset();
    engine_.reset();
}

double AppCore::now() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime_).count();
}

void AppCore::update() {
    if (bootMessagePending_) {
        bootMessageUntil_ = now() + 2.5;
        bootMessagePending_ = false;
    }
    for (const std::string& line : engine_->takeLog()) addLog(line);
    engine_->getStatus(status_);
    if (romPlaying_ >= 0 && status_.player.state != MidiPlayer::State::Playing && status_.player.state != MidiPlayer::State::Paused) {
        restoreAfterRomPlay();
    }
    if (d20Playing_ >= 0 && status_.player.state != MidiPlayer::State::Playing && status_.player.state != MidiPlayer::State::Paused) {
        stopD20();
    }
    if (d20Queued_ >= 0 && status_.player.queuedName.empty()) {
        // The queued pattern took over at the start of a bar (or the queue went with a stop or another file).
        if (status_.player.name == d20QueuedTitle_) d20Playing_ = d20Queued_;
        d20Queued_ = -1;
    }
    refreshMemoryCaches(false);
    // MIDI inputs plugged in (and those that left and came back: the ALSA and CoreMIDI inputs notice) open by
    // themselves.
    if (options_.enableMidiInput && now() >= nextMidiRefresh_) {
        nextMidiRefresh_ = now() + 2.0;
        refreshMidiPorts();
    }
}

// ---------------------------------------------------------------------------------------------
// Settings

void AppCore::loadSettings() {
    if (options_.settingsFile.empty()) {
        settings_.loadText(options_.settingsText);
    } else {
        settings_.load(options_.settingsFile);
    }
    romFolder_ = Platform::fromUtf8(settings_.getString("rom_folder"));
    analogMode_ = AnalogMode(std::clamp(settings_.getInt("analog_mode", -1), -1, 3));
    resamplerQuality_ = std::clamp(settings_.getInt("resampler_quality", 2), 0, 3);
    partialCount_ = std::clamp(settings_.getInt("partials", kDefaultPartials), kDefaultPartials, kMaxPartials);
    audioDevice_ = settings_.getString("audio_device");
    sampleRate_ = std::max(settings_.getInt("sample_rate", 0), 0);
    bufferFrames_ = std::clamp(settings_.getInt("buffer_frames", 512), 64, 8192);
    surround_ = !options_.plugin && settings_.getString("audio_channels") == "7.1";
    multiPairs_ = options_.plugin ? options_.multiOutputPairs : settings_.getBool("multi_output_pairs", false);
    enabledMidiPorts_ = split(settings_.getString("midi_inputs"), '|');
    sixteenParts_ = settings_.getBool("sixteen_parts", false);
    performanceMode_ = settings_.getBool("performance_mode", false);
    mt32Mode_ = settings_.getBool("mt32_translation", false);
    const std::string presetMode = settings_.getString("mt32_preset_mode");
    if (presetMode.empty()) {
        // Earlier versions had a switch for exact presets.
        mt32PresetMode_ = settings_.getBool("mt32_exact_presets", true) ? Mt32Translator::PresetMode::Exact : Mt32Translator::PresetMode::StandIns;
    } else {
        mt32PresetMode_ = presetMode == "standins" ? Mt32Translator::PresetMode::StandIns
                        : presetMode == "hybrid"   ? Mt32Translator::PresetMode::Hybrid
                                                   : Mt32Translator::PresetMode::Exact;
    }
    Mt32Translator::parsePresetChoices(settings_.getString("preset_choices"), mt32PresetChoices_);
    Mt32Translator::parseRhythmChoices(settings_.getString("rhythm_choices"), mt32RhythmChoices_);
    mt32RoomyToms_ = settings_.getBool("mt32_roomy_toms", false);
    d20Tempo_ = std::clamp(settings_.getInt("d20_tempo", 120), 20, 250);
    performanceMode_ = performanceMode_ && !mt32Mode_;
    cardFile_ = Platform::fromUtf8(settings_.getString("card_file"));
    performanceChannel_ = std::clamp(settings_.getInt("performance_channel", 1), 1, 16) - 1;
    customChannels_ = parseChannels(settings_.getString("part_channels"), channels_, defaultChannels());

    engineOptions_.outputGain = std::clamp(settings_.getFloat("output_gain", 1.0f), 0.0f, 4.0f);
    engineOptions_.reverbGain = std::clamp(settings_.getFloat("reverb_gain", 1.0f), 0.0f, 4.0f);
    engineOptions_.reverbEnabled = settings_.getBool("reverb_enabled", true);
    engineOptions_.reverbOverridden = settings_.getBool("reverb_locked", false);
    engineOptions_.dSeriesReverb = settings_.getBool("d_series_reverb", true);
    engineOptions_.reversedStereo = settings_.getBool("swap_stereo", false);
    engineOptions_.niceAmpRamp = settings_.getBool("nice_amp_ramp", true);
    engineOptions_.nicePanning = settings_.getBool("nice_panning", false);
    engineOptions_.nicePartialMixing = settings_.getBool("nice_partial_mixing", false);
    engineOptions_.dacInputMode = MT32Emu::DACInputMode(std::clamp(settings_.getInt("dac_input_mode", 0), 0, 3));
    engineOptions_.midiTimestamping = settings_.getBool("midi_timestamping", true);
    engineOptions_.midiExtensions = settings_.getBool("midi_extensions", true);
    engineOptions_.midiCableSpeed = settings_.getBool("midi_cable_speed", false);
    const int controlChannel = settings_.getInt("control_channel", 0);  // 1-16, 0 = off
    engineOptions_.controlChannel = uint8_t(controlChannel >= 1 && controlChannel <= 16 ? controlChannel - 1 : kChannelOff);
    engineOptions_.unitNumber = uint8_t(std::clamp(settings_.getInt("unit_number", 17), 17, 32));

    lastMidiFolder_ = Platform::fromUtf8(settings_.getString("midi_folder"));
    ownOutputs_ = options_.partOutputs ? uint32_t(settings_.getInt("own_outputs", 0)) & 0xFFFFu : 0;
    loadFrontEndSettings();
}

void AppCore::saveSettings() {
    settings_.set("rom_folder", Platform::toUtf8(romFolder_));
    settings_.set("control_rom", controlRom_ >= 0 ? roms_[controlRom_].fileName : std::string());
    settings_.set("pcm_rom", pcmRom_ >= 0 ? roms_[pcmRom_].fileName : std::string());
    settings_.set("analog_mode", int(analogMode_));
    settings_.set("resampler_quality", resamplerQuality_);
    settings_.set("partials", partialCount_);
    settings_.set("sixteen_parts", sixteenParts_);
    settings_.set("performance_mode", performanceMode_);
    settings_.set("mt32_translation", mt32Mode_);
    settings_.set("mt32_preset_mode", mt32PresetMode_ == Mt32Translator::PresetMode::StandIns ? "standins"
                                      : mt32PresetMode_ == Mt32Translator::PresetMode::Hybrid ? "hybrid"
                                                                                               : "exact");
    settings_.set("preset_choices", Mt32Translator::presetChoicesText(mt32PresetChoices_));
    settings_.set("rhythm_choices", Mt32Translator::rhythmChoicesText(mt32RhythmChoices_));
    settings_.set("mt32_roomy_toms", mt32RoomyToms_);
    settings_.set("mt32_preset_rom", mt32PresetRom_ >= 0 ? roms_[mt32PresetRom_].fileName : std::string());
    settings_.set("d20_tempo", d20Tempo_);
    settings_.set("card_file", Platform::toUtf8(cardFile_));
    settings_.set("performance_channel", performanceChannel_ + 1);
    settings_.set("audio_device", audioDevice_);
    settings_.set("sample_rate", sampleRate_);
    settings_.set("buffer_frames", bufferFrames_);
    if (!options_.plugin) {
        settings_.set("audio_channels", surround_ ? "7.1" : "stereo");
        settings_.set("multi_output_pairs", multiPairs_);
    }
    std::string ports;
    for (const std::string& port : enabledMidiPorts_) ports += (ports.empty() ? "" : "|") + port;
    settings_.set("midi_inputs", ports);
    settings_.set("part_channels", customChannels_ ? formatChannels(channels_) : std::string());

    settings_.set("output_gain", engineOptions_.outputGain);
    settings_.set("reverb_gain", engineOptions_.reverbGain);
    settings_.set("reverb_enabled", engineOptions_.reverbEnabled);
    settings_.set("reverb_locked", engineOptions_.reverbOverridden);
    settings_.set("d_series_reverb", engineOptions_.dSeriesReverb);
    settings_.set("swap_stereo", engineOptions_.reversedStereo);
    settings_.set("nice_amp_ramp", engineOptions_.niceAmpRamp);
    settings_.set("nice_panning", engineOptions_.nicePanning);
    settings_.set("nice_partial_mixing", engineOptions_.nicePartialMixing);
    settings_.set("dac_input_mode", int(engineOptions_.dacInputMode));
    settings_.set("midi_timestamping", engineOptions_.midiTimestamping);
    settings_.set("midi_extensions", engineOptions_.midiExtensions);
    settings_.set("midi_cable_speed", engineOptions_.midiCableSpeed);
    settings_.set("control_channel", engineOptions_.controlChannel < 16 ? engineOptions_.controlChannel + 1 : 0);
    settings_.set("unit_number", int(engineOptions_.unitNumber));
    if (status_.d110) {
        // Not in the memory: the patch numbers the display shows (the multi-timbral patch and the D-20 performance patch).
        settings_.set("current_patch", int(status_.currentPatch));
        settings_.set("current_performance", int(status_.currentPerformance));
    }

    settings_.set("midi_folder", Platform::toUtf8(lastMidiFolder_));
    if (options_.partOutputs) settings_.set("own_outputs", int(ownOutputs_));
    saveFrontEndSettings();
    if (!options_.settingsFile.empty()) settings_.save(options_.settingsFile);
    if (options_.settingsSaved) options_.settingsSaved(settings_);
}

std::string AppCore::settingsText() {
    saveSettings();
    return settings_.text();
}

std::vector<uint8_t> AppCore::memoryData() {
    saveMemoryFile();
    return battery_;
}

void AppCore::setOutputSampleRate(uint32_t rate) {
    if (rate == 0 || rate == options_.outputSampleRate) return;
    options_.outputSampleRate = rate;
    if (engine_ && engine_->isOpen()) restartSynth();
}

// ---------------------------------------------------------------------------------------------
// Synth, audio and MIDI setup

void AppCore::scanRoms() {
    std::error_code ec;
    if (romFolder_.empty() || !std::filesystem::is_directory(romFolder_, ec)) romFolder_ = findRomFolder(options_.romSearchDirs);
    roms_ = scanRomFolder(romFolder_);

    const std::string wantedControl = settings_.getString("control_rom");
    const std::string wantedPcm = settings_.getString("pcm_rom");
    controlRom_ = -1;
    pcmRom_ = -1;
    for (int i = 0; i < int(roms_.size()); i++) {
        if (roms_[i].isControl && controlRom_ < 0 && roms_[i].fileName == wantedControl) controlRom_ = i;
        if (!roms_[i].isControl && pcmRom_ < 0 && roms_[i].fileName == wantedPcm) pcmRom_ = i;
    }
    int defaultControl = -1;
    int defaultPcm = -1;
    if ((controlRom_ < 0 || pcmRom_ < 0) && pickDefaultRoms(roms_, defaultControl, defaultPcm)) {
        if (controlRom_ < 0) controlRom_ = defaultControl;
        if (pcmRom_ < 0) pcmRom_ = defaultPcm;
    }

    // The MT-32 presets for translation: the chosen control ROM, else an MT-32's before a CM-32L's.
    const std::string wantedPresets = settings_.getString("mt32_preset_rom");
    mt32PresetRom_ = -1;
    for (int i = 0; i < int(roms_.size()); i++) {
        const std::string family = romFamily(roms_[i]);
        if (!roms_[i].isControl || (family != "mt32" && family != "cm32l")) continue;
        if (roms_[i].fileName == wantedPresets) {
            mt32PresetRom_ = i;
            break;
        }
        if (mt32PresetRom_ < 0 || (romFamily(roms_[mt32PresetRom_]) != "mt32" && family == "mt32")) mt32PresetRom_ = i;
    }
    useMt32PresetRom();
}

void AppCore::useMt32PresetRom() {
    mt32PresetError_.clear();
    std::shared_ptr<Mt32Presets> presets;
    if (mt32PresetRom_ >= 0) {
        presets = std::make_shared<Mt32Presets>();
        if (!loadMt32Presets(roms_[mt32PresetRom_].path, *presets, mt32PresetError_)) presets.reset();
    }
    mt32Presets_ = presets;
    engine_->setMt32PresetMode(mt32PresetMode_);
    engine_->setMt32PresetChoices(mt32PresetChoices_, mt32RhythmChoices_);
    engine_->setMt32RoomyToms(mt32RoomyToms_);
    engine_->setMt32Presets(presets);
    cacheTime_ = -1.0;  // The tone names change
}

void AppCore::loadRomSongList() {
    romSongs_.clear();
    bootMessage_.clear();
    if (controlRom_ < 0) return;
    const std::vector<uint8_t> rom = readBinaryFile(roms_[controlRom_].path);
    romSongs_ = findRomSongs(rom);
    bootMessage_ = findBootMessage(rom);
    romSongChoice_ = std::min(romSongChoice_, int(romSongs_.size()));
}

uint32_t AppCore::outputRate() const {
    if (audio_->sampleRate() != 0) return audio_->sampleRate();
    if (options_.outputSampleRate != 0) return options_.outputSampleRate;
    return sampleRate_ != 0 ? uint32_t(sampleRate_) : 48000;
}

void AppCore::renderAudio(void* user, float* interleavedStereo, uint32_t frames) {
    AppCore& core = *static_cast<AppCore*>(user);
    core.engine_->render(interleavedStereo, frames);
    core.measurePeaks(interleavedStereo, frames, false);
}

void AppCore::renderSurroundAudio(void* user, float* interleaved, uint32_t frames) {
    AppCore& core = *static_cast<AppCore*>(user);
    core.engine_->renderSurround(interleaved, frames);
    core.measurePeaks(interleaved, frames, true);
}

void AppCore::measurePeaks(const float* samples, uint32_t frames, bool surround) {
    // Stereo: L R. 7.1 (AudioOutput's order): FL FR FC LFE BL BR SL SR, of which the left and right speakers count.
    const uint32_t channels = surround ? uint32_t(SynthEngine::kSurroundChannels) : 2u;
    float left = 0.0f;
    float right = 0.0f;
    for (uint32_t i = 0; i < frames; i++) {
        const float* frame = samples + size_t(i) * channels;
        left = std::max(left, std::fabs(frame[0]));
        right = std::max(right, std::fabs(frame[1]));
        if (surround) {
            left = std::max({left, std::fabs(frame[4]), std::fabs(frame[6])});
            right = std::max({right, std::fabs(frame[5]), std::fabs(frame[7])});
        }
    }
    raise(peakLeft_, left);
    raise(peakRight_, right);
}

void AppCore::takeOutputPeaks(float& left, float& right) {
    left = peakLeft_.exchange(0.0f, std::memory_order_relaxed);
    right = peakRight_.exchange(0.0f, std::memory_order_relaxed);
}

void AppCore::startAudioAndSynth() {
    audioError_.clear();
    audio_->close();
    bool audioOpen = false;
    if (options_.enableAudio) {
        audioOpen = audio_->open(audioDevice_, uint32_t(sampleRate_), uint32_t(bufferFrames_),
                                 surround_ ? AudioOutput::Layout::Surround71 : AudioOutput::Layout::Stereo,
                                 surround_ ? &renderSurroundAudio : &renderAudio, this, audioError_);
    }
    // The synth is sized to the device's actual rate before the device starts pulling samples.
    restartSynth();
    if (audioOpen && !audio_->start(audioError_)) audio_->close();
    if (!audioError_.empty()) addLog("Audio: " + audioError_);
}

void AppCore::setMultiPairs(bool pairs) {
    if (options_.plugin || pairs == multiPairs_) return;
    multiPairs_ = pairs;
    engine_->setMultiPairsStereo(pairs);  // From the next notes
    saveSettings();
}

const char* AppCore::outputName(int assign, bool compact) const {
    const int index = std::clamp(assign, 0, 7);
    if (multiPairsInUse()) return compact ? kOutputPairShortNames[index] : kOutputPairNames[index];
    return compact ? kOutputShortNames[index] : kOutputNames[index];
}

EngineConfig AppCore::engineConfig() const {
    EngineConfig config;
    config.controlRom = roms_[controlRom_].path;
    config.pcmRom = roms_[pcmRom_].path;
    config.analogMode = analogMode_;
    config.outputSampleRate = outputRate();
    config.resamplerQuality = MT32Emu::SamplerateConversionQuality(resamplerQuality_);
    config.partialCount = uint32_t(partialCount_);
    config.sixteenParts = sixteenParts_;
    config.partOutputs = options_.partOutputs;
    config.multiOutputs = options_.partOutputs || surround_;  // The plugins' MULTI outputs, 7.1 surround's speakers
    return config;
}

bool AppCore::restartSynth() {
    synthError_.clear();
    if (controlRom_ < 0 || pcmRom_ < 0) {
        engine_->close();
        synthError_ = roms_.empty() ? "No ROM images found." : "Choose a Control ROM and a PCM ROM.";
        return false;
    }
    const EngineConfig config = engineConfig();
    restoreAfterRomPlay();  // Carry the user's setup over to the new synth, not the ROM Play one
    engine_->getStatus(status_);
    const bool wasD110 = status_.d110;
    if (wasD110) {
        saveMemoryFile();  // In case the new ROM is not a D-110 one
        saveCardFile();
    }
    if (!engine_->configure(config, synthError_)) {
        addLog("Synth: " + synthError_);
        return false;
    }
    engine_->getStatus(status_);
    std::error_code ec;
    if (status_.d110 && !wasD110) {
        // Entering D-110 mode (at startup or after another ROM): restore the battery-backed memory.
        bool restored = false;
        if (!options_.memoryFile.empty() && std::filesystem::exists(options_.memoryFile, ec)) {
            std::string error;
            restored = engine_->loadSysexFile(options_.memoryFile, error, false);
            if (!restored) addLog("Memory: " + error);
        } else if (!battery_.empty()) {
            engine_->applySysex(battery_);
            restored = true;
        }
        if (restored) {
            engine_->setCurrentPatchNumber(settings_.getInt("current_patch", 0));
            engine_->setCurrentPerformanceNumber(settings_.getInt("current_performance", 0));
        }
    }
    loadD20RomFiles();  // The presets over whatever the memory held: they are the D-20's ROM
    if (customChannels_ && !status_.mt32Translation) engine_->setPartChannels(channels_);  // Not over the MT-32 setup
    if (status_.d110 && !status_.cardInserted && !cardFile_.empty() && std::filesystem::exists(cardFile_, ec)) {
        std::string error;
        if (!engine_->insertCard(cardFile_, error)) addLog("Card: " + error);
    }
    if (performanceMode_ && status_.d110) {
        // The new synth starts in multi-timbral mode; keep the snapshot taken when the mode was entered, if any.
        if (performanceSnapshot_.empty()) performanceSnapshot_ = engine_->dumpSysex(DumpSystem | DumpTemporary);
        engine_->setPerformanceMode(true, performanceChannel_);
    }
    if (mt32Mode_ && status_.d110) {
        if (!wasD110 || mt32Snapshot_.empty()) {
            // Fresh memory (at startup, or back from another ROM): keep it for later and set up the MT-32.
            mt32Snapshot_ = engine_->dumpSysex(kMt32SnapshotContents);
            engine_->setMt32Translation(true);
            engine_->mt32PowerOn();
        } else {
            engine_->setMt32Translation(true);  // The MT-32 setup came over with the memory
        }
    }
    loadRomSongList();
    bootMessagePending_ = true;
    cacheTime_ = -1.0;
    return true;
}

bool AppCore::resetSynth() {
    if (!restartSynth()) return false;
    engine_->resetPartMix();
    return true;
}

void AppCore::refreshMidiPorts() {
    midiPorts_ = midi_->listPorts();
    midiError_.clear();
    for (const std::string& name : enabledMidiPorts_) {
        if (midi_->isOpen(name) || std::find(midiPorts_.begin(), midiPorts_.end(), name) == midiPorts_.end()) continue;
        std::string error;
        if (!midi_->open(name, error)) midiError_ = error;
    }
}

void AppCore::setMidiPortEnabled(const std::string& name, bool enabled) {
    midiError_.clear();
    enabledMidiPorts_.erase(std::remove(enabledMidiPorts_.begin(), enabledMidiPorts_.end(), name), enabledMidiPorts_.end());
    if (enabled) {
        if (!midi_->open(name, midiError_)) return;
        enabledMidiPorts_.push_back(name);
    } else {
        midi_->close(name);
    }
    saveSettings();
}

std::array<uint8_t, kMaxPartCount> AppCore::defaultChannels() const {
    const bool d110 = controlRom_ >= 0 && romFamily(roms_[controlRom_]) == "d110";
    return d110 ? kD110Channels : kMT32Channels;
}

std::vector<int> AppCore::partOrder() const {
    std::vector<int> order;
    for (int part = 0; part < kRhythmPart; part++) order.push_back(part);
    for (int part = kBasePartCount; part < int(status_.partCount); part++) order.push_back(part);
    order.push_back(kRhythmPart);
    return order;
}

void AppCore::setChannels(const std::array<uint8_t, kMaxPartCount>& channels, bool remember) {
    engine_->setPartChannels(channels);
    if (!remember) return;
    channels_ = channels;
    customChannels_ = channels != defaultChannels();
    saveSettings();
}

void AppCore::setOptions() {
    engine_->setOptions(engineOptions_);
    saveSettings();
}

void AppCore::sendShort(uint8_t status, uint8_t data1, uint8_t data2) {
    engine_->onMidiShortMessage(uint32_t(status) | (uint32_t(data1) << 8) | (uint32_t(data2) << 16));
}

void AppCore::addLog(const std::string& line) {
    log_.push_back(line);
    while (log_.size() > 500) log_.pop_front();
    onLogAdded(line);
}

std::string AppCore::setupProblem(const std::string& hint) const {
    if (!status_.open) return (synthError_.empty() ? std::string("The synth is not running.") : synthError_) + hint;
    if (options_.enableAudio && !audio_->isRunning()) {
        return "Audio stopped" + (audioError_.empty() ? std::string(".") : ": " + audioError_) + hint;
    }
    if (!midiError_.empty()) return "MIDI input: " + midiError_;
    return std::string();
}

void AppCore::applyPartMutes() {
    const bool anySolo = std::find(partSolo_.begin(), partSolo_.end(), true) != partSolo_.end();
    uint32_t mask = 0;
    for (int part = 0; part < kMaxPartCount; part++) {
        if (partMuted_[size_t(part)] || (anySolo && !partSolo_[size_t(part)])) mask |= 1u << part;
    }
    engine_->setMutedParts(mask);
}

void AppCore::loadReverbSettingsFile() {
    std::error_code ec;
    if (!options_.reverbSettingsFile.empty() && std::filesystem::exists(options_.reverbSettingsFile, ec)) {
        std::string error;
        if (!loadReverbSettings(options_.reverbSettingsFile, reverbSettings_, error)) addLog("Reverb: " + error);
    }
    engine_->setDSeriesReverbSettings(reverbSettings_);
}

void AppCore::saveReverbSettingsFile() {
    reverbSettingsDirty_ = false;
    if (options_.reverbSettingsFile.empty()) return;
    std::string error;
    if (!saveReverbSettings(options_.reverbSettingsFile, reverbSettings_, error)) addLog("Reverb: " + error);
}

// ---------------------------------------------------------------------------------------------
// Files and memory

void AppCore::openFile(const std::filesystem::path& path) {
    std::string extension = Platform::toUtf8(path.extension());
    std::transform(extension.begin(), extension.end(), extension.begin(), [](char c) { return char(std::tolower(uint8_t(c))); });
    if (extension == ".syx") {
        loadSysexFile(path);
    } else {
        openMidiFile(path);
    }
}

void AppCore::loadSysexFile(const std::filesystem::path& path) {
    std::string error;
    if (!engine_->loadSysexFile(path, error)) addLog("SysEx: " + error);
}

void AppCore::openMidiFile(const std::filesystem::path& path) {
    restoreAfterRomPlay();
    stopD20();
    playerError_.clear();
    if (!engine_->loadMidiFile(path, playerError_)) {
        addLog("MIDI file: " + Platform::toUtf8(path.filename()) + ": " + playerError_);
        return;
    }
    lastMidiFolder_ = path.parent_path();
    engine_->playerPlay();
    saveSettings();
}

void AppCore::saveMemoryFile() {
    if (!engine_->isOpen()) return;
    EngineStatus status;
    engine_->getStatus(status);
    if (!status.d110) return;  // Only the D-110 has battery-backed memory
    std::vector<uint8_t> data = engine_->dumpSysex(DumpEverything);
    // During ROM Play the system area, rhythm setup and parts are the song's; the user's are those it put aside.
    if (romPlaying_ >= 0) data.insert(data.end(), romPlaySnapshot_.begin(), romPlaySnapshot_.end());
    // In performance mode parts 1-2 and the channels hold the performance; the setup to keep is the multi-timbral one.
    if (performanceMode_) data.insert(data.end(), performanceSnapshot_.begin(), performanceSnapshot_.end());
    // While translating MT-32 data, the D-110 setup kept aside replaces what the MT-32 side changed.
    if (mt32Mode_) data.insert(data.end(), mt32Snapshot_.begin(), mt32Snapshot_.end());
    battery_ = std::move(data);
    if (options_.memoryFile.empty()) return;
    std::ofstream out(options_.memoryFile, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(battery_.data()), std::streamsize(battery_.size()));
    if (!out) addLog("Memory: cannot write " + Platform::toUtf8(options_.memoryFile));
}

void AppCore::saveCardFile() {
    if (cardFile_.empty() || !engine_ || !engine_->isOpen()) return;
    EngineStatus status;
    engine_->getStatus(status);
    if (!status.cardInserted) return;
    std::string error;
    if (!engine_->saveCard(cardFile_, error)) addLog("Card: " + error);
}

void AppCore::initializeMemory() {
    if (controlRom_ < 0 || pcmRom_ < 0) return;
    if (engine_->configure(engineConfig(), synthError_, false)) {
        if (customChannels_) engine_->setPartChannels(channels_);
        if (performanceMode_) {
            performanceSnapshot_ = engine_->dumpSysex(DumpSystem | DumpTemporary);
            engine_->setPerformanceMode(true, performanceChannel_);
        }
        engine_->getStatus(status_);
        loadD20RomFiles();
        if (mt32Mode_ && status_.d110) {
            mt32Snapshot_ = engine_->dumpSysex(kMt32SnapshotContents);
            engine_->setMt32Translation(true);
            engine_->mt32PowerOn();
        }
        addLog("Memory initialized");
    }
}

// ---------------------------------------------------------------------------------------------
// Timbres

int AppCore::toneGroupOf(const uint8_t* timbre, bool cardTimbre) const {
    const int group = timbre[0] & 3;
    if (group == 2 && (cardTimbre || timbre[7] == MT32Emu::PART_CARD_TONES)) return kCardGroup;
    if (group < 2 && timbre[7] == MT32Emu::PART_ALT_TONES && status_.altTones) return kAltGroup + group;
    return group;
}

std::string AppCore::timbreLabel(int timbre) const {
    int group = 0;
    int number = timbre & 63;
    if (timbre >= 256) {
        group = kAltGroup + (timbre & 127) / 64;
    } else {
        const std::vector<uint8_t>& memory = timbre < 128 ? timbreMemory_ : cardTimbreMemory_;
        const size_t entry = size_t(timbre & 127) * 8;
        if (memory.size() < entry + 8) return timbreCode(timbre);
        group = toneGroupOf(&memory[entry], timbre >= 128);
        number = memory[entry + 1] & 63;
    }
    const size_t index = size_t(group * 64 + number);
    return timbreCode(timbre) + "  " + toneCode(group, number) + " " + (index < toneNames_.size() ? toneNames_[index] : "");
}

int AppCore::timbreCount() const {
    return status_.cardInserted ? 256 : 128;
}

bool AppCore::timbreSelectable(int timbre) const {
    if (timbre < 0) return false;
    if (timbre < 128) return true;
    if (timbre < 256) return status_.cardInserted;
    return timbre < 384 && status_.altTones;
}

int AppCore::partTimbre(const PartStatus& part) {
    if (part.program == 0xFF) return -1;
    return part.program + (part.programFromCard ? 128 : part.programFromAlt ? 256 : 0);
}

void AppCore::refreshMemoryCaches(bool force) {
    const double time = now();
    if (!force && cacheTime_ >= 0.0 && time - cacheTime_ < 0.5) return;
    cacheTime_ = time;
    if (!status_.open) {
        toneNames_.clear();
        timbreMemory_.clear();
        patchMemory_.clear();
        rhythmSetup_.clear();
        return;
    }
    toneNames_ = engine_->toneNames();
    timbreMemory_.resize(128 * 8);
    engine_->readMemory(0x050000, uint32_t(timbreMemory_.size()), timbreMemory_.data());
    rhythmSetup_.resize(85 * 4);
    engine_->readMemory(kRhythmSetupAddress, uint32_t(rhythmSetup_.size()), rhythmSetup_.data());
    rhythmFinePan_.assign(85 * 2, 0);
    if (status_.d110) engine_->readMemory(kRhythmFinePanAddress, uint32_t(rhythmFinePan_.size()), rhythmFinePan_.data());
    d20Rhythm_ = engine_->d20Rhythm();
    if (status_.d110) {
        patchMemory_.resize(64 * 128);
        engine_->readMemory(0x060000, uint32_t(patchMemory_.size()), patchMemory_.data());
        performanceMemory_.resize(128 * 38);
        engine_->readMemory(0x070000, uint32_t(performanceMemory_.size()), performanceMemory_.data());
    } else {
        patchMemory_.clear();
        performanceMemory_.clear();
    }
    if (status_.cardInserted) {
        cardTimbreMemory_.resize(128 * 8);
        engine_->readMemory(0x150000, uint32_t(cardTimbreMemory_.size()), cardTimbreMemory_.data());
        cardPatchMemory_.resize(64 * 128);
        engine_->readMemory(0x160000, uint32_t(cardPatchMemory_.size()), cardPatchMemory_.data());
    } else {
        cardTimbreMemory_.clear();
        cardPatchMemory_.clear();
    }
}

// ---------------------------------------------------------------------------------------------
// Modes

void AppCore::setPerformanceMode(bool enabled) {
    if (enabled == performanceMode_) return;
    if (enabled) setMt32Mode(false);
    performanceMode_ = enabled;
    if (enabled) {
        restoreAfterRomPlay();
        performanceSnapshot_ = engine_->dumpSysex(DumpSystem | DumpTemporary);
        engine_->setPerformanceMode(true, performanceChannel_);
    } else {
        engine_->setPerformanceMode(false, performanceChannel_);
        if (!performanceSnapshot_.empty()) engine_->applySysex(performanceSnapshot_);
        performanceSnapshot_.clear();
    }
    cacheTime_ = -1.0;
    saveSettings();
}

void AppCore::setMt32Mode(bool enabled) {
    if (enabled == mt32Mode_) return;
    if (enabled) setPerformanceMode(false);
    mt32Mode_ = enabled;
    engine_->getStatus(status_);
    if (!status_.d110) {
        engine_->setMt32Translation(enabled);  // Takes effect once D-110 ROMs are loaded (see restartSynth())
        mt32Snapshot_.clear();
    } else if (enabled) {
        restoreAfterRomPlay();
        mt32Snapshot_ = engine_->dumpSysex(kMt32SnapshotContents);
        engine_->setMt32Translation(true);
        engine_->mt32PowerOn();
        addLog("MT-32 translation on: parts 1-8 on MIDI channels 2-9, rhythm on 10");
    } else {
        restoreAfterRomPlay();
        engine_->setMt32Translation(false);
        if (!mt32Snapshot_.empty()) engine_->applySysex(mt32Snapshot_);
        mt32Snapshot_.clear();
        addLog("MT-32 translation off: the D-110 setup is back");
    }
    cacheTime_ = -1.0;
    saveSettings();
}

// ---------------------------------------------------------------------------------------------
// The display

void AppCore::dismissLcdMessage() {
    lcdDismissedSerial_ = status_.lcdMessageSerial;
    bootMessageUntil_ = 0.0;
    engine_->dismissLcdMessage();
}

void AppCore::lcdViewChanged() {
    // Like pressing a panel button: the display leaves any SysEx message.
    lcdDismissedSerial_ = status_.lcdMessageSerial;
    lcdPatchView_ = lcdPatchView_ && status_.d110;
}

std::string AppCore::lcdText() const {
    if (!status_.open) return padded("    D110Emu", 16) + padded("  no ROM loaded", 16);
    if (now() < bootMessageUntil_ && bootMessage_.size() == 32) return bootMessage_;
    if (status_.lcdMessageShown && status_.lcdMessageSerial != lcdDismissedSerial_) return padded(status_.lcdMessage, 32);

    if (lcdPatternPlay_ && status_.d110 && romPlaying_ < 0) {
        // The D-20's Pattern Play screen (its manual p.26): the selected pattern's number and name in 11 characters, as
        // the user's D-20 shows them ("Electric Pop" as "Elec Pop", the programmable patterns as "UserPattern").
        std::string name = d20Selected_ < kD20PresetPatterns ? kD20PresetPatternNames[d20Selected_] : "UserPattern";
        if (name.rfind("Electric Pop ", 0) == 0) name = "Elec Pop " + name.substr(13);
        std::string number = d20PatternName(d20Selected_);
        // A pattern waiting for the next bar blinks its number, as the D-20 does: shown
        // for the first half of each blink, from the moment it was chosen.
        constexpr double kBlinkPeriod = 60.0 / 60.0;
        if (d20Queued_ >= 0 && d20Queued_ == d20Selected_ && std::fmod(now() - d20QueuedAt_, kBlinkPeriod) >= kBlinkPeriod / 2.0) {
            number.assign(number.size(), ' ');
        }
        return padded("Pattern Play", 16) + padded(number + ":" + name.substr(0, 11), 16);
    }

    if (status_.performanceMode && romPlaying_ < 0) {
        const uint8_t* const temp = status_.performanceTemp;
        if (lcdToneView_) {
            // The D-20's DISPLAY buttons show the patch's tones: "U:a01" and "L:b01" (numbered 01-64 in each group, as
            // on the D-20) with their names, which parts 1 (upper) and 2 (lower) play.
            auto toneLine = [&](const char* label, int groupOffset, int part) {
                char code[8];
                std::snprintf(code, sizeof(code), "%c%02d", "abir"[temp[groupOffset] & 3], (temp[groupOffset + 1] & 63) + 1);
                return padded(std::string(label) + code + " " + status_.parts[part].name, 16);
            };
            return toneLine("U:", 0x04, 0) + toneLine("L:", 0x02, 1);
        }
        // The D-10/D-20's performance display: "I-A11 SPLIT C4" (the split key only in Split), then the patch name.
        static const char* const keyModes[3] = {"WHOLE", "DUAL", "SPLIT"};
        const int keyMode = std::min<int>(temp[0x00], 2);
        std::string line1 = "I-" + performanceCode(status_.currentPerformance) + " " + keyModes[keyMode];
        if (keyMode == 2) line1 += " " + noteName(36 + std::min<int>(temp[0x01], 61));
        const std::string name(reinterpret_cast<const char*>(&temp[0x15]), 16);
        return padded(line1, 16) + padded(name, 16);
    }

    // Play mode: part numbers turn dark while the part sounds.
    std::string line1 = "12345678R";
    for (int i = 0; i < kBasePartCount; i++) {
        if (status_.parts[i].active) line1[size_t(i)] = char(uint8_t(line1[size_t(i)]) | 0x80);
    }
    std::string line2;
    if (romPlaying_ >= 0) {
        line1 += " RomPly";
        line2 = romPlaying_ < int(romSongs_.size()) ? std::to_string(romPlaying_ + 1) + ":" + romSongs_[size_t(romPlaying_)].name
                                                    : std::string("Chain of Songs");
    } else if (lcdPatchView_ && status_.d110) {
        line1 += "  PATCH";
        line2 = patchCode(status_.currentPatch) + ":" + status_.patchName;
    } else if (lcdPart_ == kRhythmPart) {
        line1 += "  PartR";
        line2 = "Rhythm Part";
    } else {
        const PartStatus& part = status_.parts[lcdPart_];
        const std::string label = partLabel(lcdPart_);
        line1 += (label.size() > 1 ? " Part" : "  Part") + label;
        const std::string timbre = status_.d110 && part.program != 0xFF ? timbreCode(partTimbre(part)) : "  " + std::string(part.tone);
        line2 = timbre + ":" + part.name;
    }
    return padded(line1, 16) + padded(line2, 16);
}

void AppCore::setEditPart(int part) {
    editPart_ = std::clamp(part, 0, kMaxPartCount - 1);
    lcdPart_ = editPart_;
}

// ---------------------------------------------------------------------------------------------
// ROM Play

void AppCore::startRomPlay(int song) {
    if (romSongs_.empty() || !status_.open) return;
    setPerformanceMode(false);  // ROM Play needs the multi-timbral parts
    restoreAfterRomPlay();
    playerError_.clear();
    stopD20();
    const bool chain = song >= int(romSongs_.size());
    std::vector<RomSong> songs = chain ? romSongs_ : std::vector<RomSong>{romSongs_[size_t(song)]};
    // ROM Play sets its own channels, rhythm setup and timbres; the user's are put back when it ends.
    romPlaySnapshot_ = engine_->dumpSysex(DumpSystem | DumpRhythm | DumpTemporary);
    if (status_.partCount > uint32_t(kBasePartCount)) {
        // The songs use channels 2-10; parts 9-15 must not play along (part 9 listens on channel 9).
        std::array<uint8_t, kMaxPartCount> channels = channels_;
        for (int part = 0; part < kMaxPartCount; part++) channels[size_t(part)] = part < kBasePartCount ? status_.parts[part].channel : kChannelOff;
        engine_->setPartChannels(channels);
    }
    engine_->loadMidi(romSongsToSmf(songs, uint8_t(engineOptions_.unitNumber - 1)), chain ? std::string("Chain of Songs") : songs[0].name,
                      MidiFileKind::UnitSong);  // D-110 data: never translated, played without the MIDI extensions
    engine_->playerPlay();
    romPlaying_ = chain ? int(romSongs_.size()) : song;
    romSongChoice_ = romPlaying_;
    lcdDismissedSerial_ = status_.lcdMessageSerial;
}

void AppCore::playRomSong(int song) {
    startRomPlay(song < 0 ? int(romSongs_.size()) : song);
}

void AppCore::restoreAfterRomPlay() {
    if (romPlaying_ < 0) return;
    romPlaying_ = -1;
    engine_->playerStop();
    if (!romPlaySnapshot_.empty()) engine_->applySysex(romPlaySnapshot_);
    romPlaySnapshot_.clear();
}

// ---------------------------------------------------------------------------------------------
// The D-20's rhythm machine

void AppCore::playD20(int what) {
    const uint8_t channel = status_.parts[kRhythmPart].channel;
    if (!status_.open || channel >= 16) return;
    restoreAfterRomPlay();
    stopD20();
    const bool track = what >= kD20PatternCount;
    std::unique_ptr<SmfFile> smf = track ? d20TrackToSmf(d20Rhythm_, channel, kD20BaseTempo, &d20BarStarts_)
                                         : d20PatternToSmf(d20Rhythm_, what, channel, kD20BaseTempo);
    const std::string name = smf->title;
    engine_->loadMidi(std::move(smf), name, MidiFileKind::UnitRhythm);  // D-110 notes: never translated
    d20SavedLoop_ = status_.player.loop;
    engine_->playerSetLoop(!track);  // As on the D-20: a pattern repeats, the track plays through once and stops
    engine_->playerSetSpeed(d20Tempo_ / kD20BaseTempo);
    engine_->playerPlay();
    d20Playing_ = what;
    if (!track) d20Selected_ = what;
}

void AppCore::stopD20() {
    d20Queued_ = -1;
    if (d20Playing_ < 0) return;
    if (status_.player.state == MidiPlayer::State::Playing || status_.player.state == MidiPlayer::State::Paused) engine_->playerStop();
    engine_->playerSetLoop(d20SavedLoop_);
    d20Playing_ = -1;
    d20BarStarts_.clear();
}

void AppCore::queueD20(int pattern) {
    const uint8_t channel = status_.parts[kRhythmPart].channel;
    if (d20Playing_ < 0 || d20Playing_ >= kD20PatternCount || channel >= 16) return;  // Only while a pattern repeats
    if (pattern == d20Playing_) {
        engine_->playerQueueNext(nullptr, std::string());  // It keeps repeating
        d20Queued_ = -1;
        return;
    }
    // As on the D-20's rhythm machine, the new pattern starts where the playing one ends: on the next bar's first beat.
    std::unique_ptr<SmfFile> smf = d20PatternToSmf(d20Rhythm_, pattern, channel, kD20BaseTempo);
    d20QueuedTitle_ = smf->title;
    engine_->playerQueueNext(std::move(smf), d20QueuedTitle_);
    d20Queued_ = pattern;
    d20QueuedAt_ = now();
}

void AppCore::loadD20RomFiles() {
    if (!status_.d110 || romFolder_.empty()) return;
    std::error_code ec;
    std::string error;
    const std::filesystem::path presets = romFolder_ / kD20PresetFile;
    if (std::filesystem::exists(presets, ec) && !engine_->loadD20PresetPatterns(presets, error)) addLog("Patterns: " + error);
    const std::filesystem::path initial = romFolder_ / kD20InitialFile;
    if (std::filesystem::exists(initial, ec) && !engine_->loadD20InitialPatterns(initial, error)) addLog("Patterns: " + error);
    cacheTime_ = -1.0;
}
