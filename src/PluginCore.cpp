// D110Emu inside a plugin host (see PluginCore.h): the part the VST2 and VST3 plugins share.

#include "PluginCore.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <system_error>

#include "Platform.h"

#if defined(_M_X64) || defined(_M_IX86) || defined(__SSE__)
#include <xmmintrin.h>
#define D110EMU_HAVE_SSE 1
#endif

namespace {

const char kStateMagic[4] = {'D', '1', '1', 'V'};
constexpr uint32_t kStateVersion = 1;
const char kSettingsTag[4] = {'S', 'E', 'T', 'T'};  // The App's settings, as "key = value" lines
const char kMemoryTag[4] = {'M', 'E', 'M', 'O'};    // The battery-backed memory, as SysEx

// Settings that belong to the computer rather than to a project: new instances start with them. (Not the keyboard: a
// new instance starts without it, and the project keeps what its instance shows.)
const char* const kGlobalKeys[] = {"rom_folder", "control_rom", "pcm_rom", "mt32_preset_rom", "lcd_scheme",
                                   "lcd_custom_glass", "lcd_custom_dot_off", "lcd_custom_dot_on", "midi_folder"};
const char* const kRomKeys[] = {"rom_folder", "control_rom", "pcm_rom", "mt32_preset_rom"};
const char* const kZoomKey = "editor_zoom";
const char* const kMultiPairsKey = "multi_output_pairs";  // MULTI 1-6 as stereo pairs in instances made from now on
const char* const kGlobalFileName = "d110emu-vst.ini";

constexpr uint32_t kMaxRenderFrames = 4096;   // Longer host blocks are rendered in pieces
constexpr size_t kMaxQueuedEvents = 65536;     // Per block; more are dropped
constexpr size_t kMaxSysexLength = 1 << 16;    // Longest SysEx message taken

// The folder of the plugin's own files (d110emu-vst.ini, d110emu-reverb.ini): the user's D110Emu folder
// (Platform::appDataDirectory), which the standalone uses too. Earlier versions kept them next to the plugin (else in
// the user's settings folder): they are copied over the first time.
const std::filesystem::path& settingsFolder() {
    static const std::filesystem::path folder = [] {
        const std::filesystem::path folder = Platform::appDataDirectory("D110Emu");
        Platform::adoptOldFiles(folder, {Platform::moduleDirectory(), Platform::oldUserDataDirectory() / "D110Emu"}, kGlobalFileName,
                                {"d110emu-reverb.ini"});
        return folder;
    }();
    return folder;
}

// Where the plugin looks for a "roms" folder: inside its bundle (Contents/Resources of D110Emu.vst3 or D110Emu.component),
// next to the plugin and above it (a roms folder inside the bundle or beside it), then in the user's D110Emu folder.
std::vector<std::filesystem::path> romSearchDirs() {
    std::vector<std::filesystem::path> dirs;
    const std::filesystem::path module = Platform::moduleDirectory();
    const std::filesystem::path contents = module.parent_path();
    const std::filesystem::path bundle = contents.parent_path().extension();
    if (contents.filename() == "Contents" && (bundle == ".vst3" || bundle == ".component")) dirs.push_back(contents / "Resources");
    dirs.push_back(module);
    dirs.push_back(settingsFolder());
    return dirs;
}

void appendU32(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; i++) out.push_back(uint8_t(value >> (8 * i)));
}

uint32_t readU32(const uint8_t* bytes) {
    return uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 | uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
}

// Flushes denormals while the synth renders (its reverb and filters decay towards them), as hosts usually do anyway.
class DenormalGuard {
public:
#ifdef D110EMU_HAVE_SSE
    DenormalGuard() : saved_(_mm_getcsr()) { _mm_setcsr(saved_ | 0x8040); }  // Flush to zero, denormals are zero
    ~DenormalGuard() { _mm_setcsr(saved_); }
#else
    DenormalGuard() {}
    ~DenormalGuard() {}
#endif
    DenormalGuard(const DenormalGuard&) = delete;
    DenormalGuard& operator=(const DenormalGuard&) = delete;

private:
#ifdef D110EMU_HAVE_SSE
    unsigned int saved_;
#endif
};

}  // namespace

PluginCore::PluginCore(PluginFormat format) : format_(format) {
    // The outputs are settled now, before the host asks for them. The Audio Unit always has pairs: Logic Pro cannot load a
    // plugin that has mono outputs beside stereo ones.
    loadGlobalSettings();
    multiPairsForNew_ = global_.getBool(kMultiPairsKey, false);
    multiPairs_ = format == PluginFormat::Au || multiPairsForNew_;
    events_.reserve(4096);
    sysex_.reserve(size_t(1) << 16);
    incoming_.reserve(4096);
    incomingSysex_.reserve(size_t(1) << 16);
    buffer_.resize(2 * size_t(kMaxRenderFrames));
    partBuffers_.assign(SynthEngine::kPartOutputs, std::vector<float>(2 * size_t(kMaxRenderFrames)));
    multiBuffers_.assign(SynthEngine::kMultiPairs, std::vector<float>(2 * size_t(kMaxRenderFrames)));
}

PluginCore::~PluginCore() {
    std::unique_ptr<App> app;
    {
        std::lock_guard<std::recursive_mutex> lock(appMutex_);
        std::lock_guard<std::mutex> audioLock(audioMutex_);
        engine_ = nullptr;
        app = std::move(app_);
    }
    app.reset();  // Its last settings still reach d110emu-vst.ini (appGeneration_ is its generation)
}

// ---------------------------------------------------------------------------------------------
// The host's setup

void PluginCore::setSampleRate(double sampleRate) {
    if (sampleRate <= 0.0) return;
    std::lock_guard<std::recursive_mutex> lock(appMutex_);
    const bool changed = sampleRate != sampleRate_;
    sampleRate_ = sampleRate;
    if (app_ && changed) app_->setOutputSampleRate(uint32_t(std::lround(sampleRate)));  // Restarts the synth, keeping its memory
}

void PluginCore::activate() {
    // Built here rather than when the host creates the plugin, so that a host scanning its plugins does not load ROMs,
    // and so that a project's state (usually loaded before this) builds it once.
    std::lock_guard<std::recursive_mutex> lock(appMutex_);
    ensureApp();
}

void PluginCore::setHostOutputs(uint32_t mask) {
    hostOutputs_ = mask;
}

// ---------------------------------------------------------------------------------------------
// The audio thread

void PluginCore::queueMidi(uint32_t frame, uint8_t status, uint8_t data1, uint8_t data2) {
    if (status < 0x80 || status >= 0xF0) return;  // Channel messages only
    const uint32_t second = (status & 0xE0) == 0xC0 ? 0 : data2 & 0x7F;  // Cx and Dx have one data byte
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (incoming_.size() >= kMaxQueuedEvents) return;
    incoming_.push_back(SynthEngine::TimedMidi{frame, uint32_t(status) | uint32_t(data1 & 0x7F) << 8 | second << 16, 0, 0});
}

void PluginCore::queueSysex(uint32_t frame, const uint8_t* data, size_t length) {
    if (data == nullptr || length < 2 || length > kMaxSysexLength) return;
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (incoming_.size() >= kMaxQueuedEvents) return;
    const size_t offset = incomingSysex_.size();
    if (data[0] != 0xF0) incomingSysex_.push_back(0xF0);
    incomingSysex_.insert(incomingSysex_.end(), data, data + length);
    if (incomingSysex_.back() != 0xF7) incomingSysex_.push_back(0xF7);
    incoming_.push_back(SynthEngine::TimedMidi{frame, 0, uint32_t(offset), uint32_t(incomingSysex_.size() - offset)});
}

void PluginCore::render(float* left, float* right, float* const* partLeft, float* const* partRight, float* const* multi, uint32_t frames,
                        bool accumulate) {
    if (frames == 0) return;  // The MIDI waits for the next block
    {
        // What was queued since the last block (events_ and sysex_ are empty: the last render() cleared them).
        std::lock_guard<std::mutex> queueLock(queueMutex_);
        events_.swap(incoming_);
        sysex_.swap(incomingSysex_);
    }
    DenormalGuard denormals;
    std::lock_guard<std::mutex> lock(audioMutex_);
    const auto silence = [&](float* channel) {
        if (channel != nullptr && !accumulate) std::fill(channel, channel + frames, 0.0f);
    };
    if (engine_ == nullptr || left == nullptr || right == nullptr) {
        silence(left);
        silence(right);
        for (int part = 0; partLeft != nullptr && partRight != nullptr && part < SynthEngine::kPartOutputs; part++) {
            silence(partLeft[part]);
            silence(partRight[part]);
        }
        for (int output = 0; multi != nullptr && output < kMultiOutputs; output++) silence(multi[output]);
        events_.clear();
        sysex_.clear();
        return;
    }
    // In order of frame: hosts send them so, and an insertion sort keeps the order of events at the same frame.
    for (size_t i = 1; i < events_.size(); i++) {
        const SynthEngine::TimedMidi event = events_[i];
        size_t j = i;
        for (; j > 0 && events_[j - 1].frame > event.frame; j--) events_[j] = events_[j - 1];
        events_[j] = event;
    }
    // The interleaved buffers of the parts and the MULTI pairs whose outputs are wanted.
    float* parts[SynthEngine::kPartOutputs] = {};
    float* multiPairs[SynthEngine::kMultiPairs] = {};
    bool anyPart = false;
    bool anyMulti = false;
    for (int part = 0; partLeft != nullptr && partRight != nullptr && part < SynthEngine::kPartOutputs; part++) {
        if (partLeft[part] == nullptr || partRight[part] == nullptr) continue;
        parts[part] = partBuffers_[size_t(part)].data();
        anyPart = true;
    }
    for (int pair = 0; multi != nullptr && pair < SynthEngine::kMultiPairs; pair++) {
        if (multi[2 * pair] == nullptr && multi[2 * pair + 1] == nullptr) continue;
        multiPairs[pair] = multiBuffers_[size_t(pair)].data();
        anyMulti = true;
    }
    // One channel of an interleaved pair (0 left, 1 right) into `out`, where there is one.
    const auto extract = [accumulate](const float* in, int channel, float* out, uint32_t count) {
        if (out == nullptr) return;
        if (accumulate) {
            for (uint32_t i = 0; i < count; i++) out[i] += in[2 * i + unsigned(channel)];
        } else {
            for (uint32_t i = 0; i < count; i++) out[i] = in[2 * i + unsigned(channel)];
        }
    };
    uint32_t done = 0;
    size_t first = 0;
    while (done < frames) {
        const uint32_t count = std::min(frames - done, kMaxRenderFrames);
        const bool last = done + count >= frames;
        size_t end = first;
        for (; end < events_.size() && (last || events_[end].frame < done + count); end++) {
            events_[end].frame = events_[end].frame > done ? events_[end].frame - done : 0;
        }
        engine_->renderBuses(buffer_.data(), anyPart ? parts : nullptr, anyMulti ? multiPairs : nullptr, count, events_.data() + first,
                             end - first, sysex_.data());
        extract(buffer_.data(), 0, left + done, count);
        extract(buffer_.data(), 1, right + done, count);
        for (int part = 0; anyPart && part < SynthEngine::kPartOutputs; part++) {
            if (parts[part] == nullptr) continue;
            extract(parts[part], 0, partLeft[part] + done, count);
            extract(parts[part], 1, partRight[part] + done, count);
        }
        for (int pair = 0; anyMulti && pair < SynthEngine::kMultiPairs; pair++) {
            if (multiPairs[pair] == nullptr) continue;
            for (int channel = 0; channel < 2; channel++) {
                float* out = multi[2 * pair + channel];
                extract(multiPairs[pair], channel, out != nullptr ? out + done : nullptr, count);
            }
        }
        first = end;
        done += count;
    }
    events_.clear();
    sysex_.clear();
}

// ---------------------------------------------------------------------------------------------
// The App

void PluginCore::setEditor(PluginEditor* editor) {
    std::lock_guard<std::recursive_mutex> lock(appMutex_);
    editor_ = editor;
}

void PluginCore::drawFrame() {
    std::lock_guard<std::recursive_mutex> lock(appMutex_);
    ensureApp();
    struct Depth {
        explicit Depth(int& value) : depth(value) { depth++; }
        ~Depth() { depth--; }
        int& depth;
    } depth(frameDepth_);
    app_->frame();
}

void PluginCore::afterFrame() {
    std::lock_guard<std::recursive_mutex> lock(appMutex_);
    if (!statePending_ || frameDepth_ > 0) return;
    std::vector<uint8_t> state;
    state.swap(pendingState_);
    statePending_ = false;
    loadState(state.data(), state.size());
}

void PluginCore::ensureApp() {
    if (app_) return;
    // A new instance: the power-on memory, with this computer's ROMs and view settings.
    loadGlobalSettings();
    Settings settings;
    for (const char* key : kGlobalKeys) {
        const std::string value = global_.getString(key);
        if (!value.empty()) settings.set(key, value);
    }
    const uint64_t generation = ++lastGeneration_;
    installApp(createApp(settings.text(), std::vector<uint8_t>(), generation), generation);
}

std::unique_ptr<App> PluginCore::createApp(const std::string& settingsText, std::vector<uint8_t> memory, uint64_t generation) {
    AppOptions options;
    options.plugin = true;
    options.pluginFormat = format_ == PluginFormat::Vst3 ? "VST3" : format_ == PluginFormat::Au ? "AU" : "VST2";
    options.partOutputs = true;
    options.multiOutputPairs = multiPairs_;
    if (format_ != PluginFormat::Au) {
        // Called from the App's frame, with appMutex_ held.
        options.multiPairsForNew = [this] { return multiPairsForNew_; };
        options.setMultiPairsForNew = [this](bool pairs) {
            loadGlobalSettings();
            global_.set(kMultiPairsKey, pairs);
            global_.save(settingsFolder() / kGlobalFileName);
            multiPairsForNew_ = pairs;
        };
    }
    if (format_ == PluginFormat::Vst3) options.hostOutputs = [this] { return hostOutputs_.load(); };
    options.enableAudio = false;
    options.enableMidiInput = false;
    options.settingsText = settingsText;
    options.memory = std::move(memory);
    options.outputSampleRate = uint32_t(std::lround(sampleRate_));
    options.romSearchDirs = romSearchDirs();
    options.reverbSettingsFile = settingsFolder() / "d110emu-reverb.ini";
    options.viewMenu = [this] {
        if (editor_ != nullptr) editor_->drawViewMenu();
    };
    options.settingsSaved = [this, generation](const Settings& settings) {
        if (generation == appGeneration_) saveGlobalSettings(settings);
    };
    std::unique_ptr<App> app(new App);
    app->init(options);
    return app;
}

void PluginCore::installApp(std::unique_ptr<App> app, uint64_t generation) {
    std::unique_ptr<App> previous;
    {
        std::lock_guard<std::mutex> lock(audioMutex_);
        previous = std::move(app_);
        app_ = std::move(app);
        engine_ = &app_->engine();
    }
    appGeneration_ = generation;
    previous.reset();  // Outside the audio lock: closing its synth takes a moment
}

std::vector<uint8_t> PluginCore::saveState() {
    std::lock_guard<std::recursive_mutex> lock(appMutex_);
    if (statePending_) return pendingState_;  // Loaded, and applied once the frame under way is over
    ensureApp();
    return encodeState(app_->settingsText(), app_->memoryData());
}

bool PluginCore::loadState(const uint8_t* data, size_t size) {
    std::string settings;
    std::vector<uint8_t> memory;
    if (!decodeState(data, size, settings, memory)) return false;
    std::lock_guard<std::recursive_mutex> lock(appMutex_);
    if (frameDepth_ > 0) {
        // The App is in the middle of a frame (its dialog let the host call in): it is replaced once the frame is over.
        pendingState_.assign(data, data + size);
        statePending_ = true;
        return true;
    }
    // A new App, as if D110Emu started with the project's settings and memory; the synth keeps playing until it is ready.
    const uint64_t generation = ++lastGeneration_;
    installApp(createApp(localSettings(settings), std::move(memory), generation), generation);
    return true;
}

std::string PluginCore::localSettings(const std::string& projectSettings) {
    Settings settings;
    settings.loadText(projectSettings);
    std::error_code ec;
    const std::string folder = settings.getString("rom_folder");
    if (folder.empty() || !std::filesystem::is_directory(Platform::fromUtf8(folder), ec)) {
        // The project was made on another computer: the ROMs are where this one keeps them.
        loadGlobalSettings();
        for (const char* key : kRomKeys) settings.set(key, global_.getString(key));
    }
    return settings.text();
}

void PluginCore::loadGlobalSettings() {
    global_.load(settingsFolder() / kGlobalFileName);  // Again each time: other instances may have changed it
}

void PluginCore::saveGlobalSettings(const Settings& appSettings) {
    loadGlobalSettings();
    bool changed = false;
    for (const char* key : kGlobalKeys) {
        const std::string value = appSettings.getString(key);
        if (value == global_.getString(key)) continue;
        global_.set(key, value);
        changed = true;
    }
    if (changed) global_.save(settingsFolder() / kGlobalFileName);
}

int PluginCore::editorZoom() {
    std::lock_guard<std::recursive_mutex> lock(appMutex_);
    loadGlobalSettings();
    return std::clamp(global_.getInt(kZoomKey, 100), 50, 300);
}

void PluginCore::setEditorZoom(int percent) {
    std::lock_guard<std::recursive_mutex> lock(appMutex_);
    loadGlobalSettings();
    global_.set(kZoomKey, percent);
    global_.save(settingsFolder() / kGlobalFileName);
}

// ---------------------------------------------------------------------------------------------
// The project state

std::vector<uint8_t> PluginCore::encodeState(const std::string& settings, const std::vector<uint8_t>& memory) {
    std::vector<uint8_t> out(kStateMagic, kStateMagic + 4);
    appendU32(out, kStateVersion);
    const auto section = [&out](const char* tag, const uint8_t* data, size_t size) {
        out.insert(out.end(), tag, tag + 4);
        appendU32(out, uint32_t(size));
        out.insert(out.end(), data, data + size);
    };
    section(kSettingsTag, reinterpret_cast<const uint8_t*>(settings.data()), settings.size());
    section(kMemoryTag, memory.data(), memory.size());
    return out;
}

bool PluginCore::decodeState(const uint8_t* data, size_t size, std::string& settings, std::vector<uint8_t>& memory) {
    if (data == nullptr || size < 8 || std::memcmp(data, kStateMagic, 4) != 0) return false;
    // Sections this version does not know (from a later one) are skipped.
    size_t position = 8;
    while (position + 8 <= size) {
        const uint8_t* tag = data + position;
        const size_t length = readU32(data + position + 4);
        if (length > size - position - 8) return false;
        const uint8_t* body = data + position + 8;
        if (std::memcmp(tag, kSettingsTag, 4) == 0) settings.assign(reinterpret_cast<const char*>(body), length);
        if (std::memcmp(tag, kMemoryTag, 4) == 0) memory.assign(body, body + length);
        position += 8 + length;
    }
    return position == size;
}
