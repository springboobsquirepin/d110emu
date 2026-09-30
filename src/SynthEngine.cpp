#include "SynthEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

namespace {

// Live MIDI waiting for the next audio block; beyond this the input is dropped (the audio has stalled).
constexpr size_t kMaxPendingMidi = 65536;
constexpr size_t kMaxPendingSysexBytes = 1 << 20;

// Address as taken by Synth::readMemory(): the three 7-bit SysEx address bytes packed together.
constexpr uint32_t memoryAddress(uint32_t high, uint32_t mid, uint32_t low) {
    return (high << 14) | (mid << 7) | low;
}

constexpr uint32_t kPatchTempAddress = memoryAddress(0x03, 0x00, 0x00);  // 16 bytes per part
constexpr uint32_t kPatchTempSize = 16;
// 16-part mode: parts 9-15's temporary areas at 13 00 00 / 14 00 00, their reserves (00-06) and channels (08-0E)
// at 11 00 00.
constexpr uint32_t kExtraParts = kMaxPartCount - kBasePartCount;
constexpr uint32_t kExtraSystemSize = 16;
constexpr uint32_t kToneDataSize = 246;  // Common + 4 partials of a tone
constexpr uint32_t kExtraSystemAddress = memoryAddress(0x11, 0x00, 0x00);
constexpr uint8_t kExtraChannels = 0x08;

// Packed address of a part's timbre temporary area.
constexpr uint32_t partTempAddress(int part) {
    return part < kBasePartCount ? kPatchTempAddress + uint32_t(part) * kPatchTempSize
                                 : memoryAddress(0x13, 0x00, 0x00) + uint32_t(part - kBasePartCount) * kPatchTempSize;
}
constexpr uint32_t kSystemAddress = memoryAddress(0x10, 0x00, 0x00);
constexpr uint32_t kSystemSize = 0x17;
// Offsets within the system area.
constexpr uint8_t kSystemMasterTune = 0x00;
constexpr uint8_t kSystemReverbMode = 0x01;
constexpr uint8_t kSystemChannels = 0x0D;
constexpr uint8_t kSystemMasterVolume = 0x16;

constexpr uint32_t kMidiQueueSize = 4096;

// Fine pan (with nice panning): two bytes per part at 12 00 00 and per rhythm key at 12 01 00; 0 = follow the
// panpot, 1-129 = -64..+64.
constexpr uint32_t kPartFinePanAddress = memoryAddress(0x12, 0x00, 0x00);
constexpr uint32_t kRhythmFinePanAddress = memoryAddress(0x12, 0x01, 0x00);

// Converts a 7-bit SysEx address such as 0x030110 to the packed form used by Synth::readMemory(), and back.
constexpr uint32_t packAddress(uint32_t sysexAddress) {
    return (((sysexAddress >> 16) & 0x7F) << 14) | (((sysexAddress >> 8) & 0x7F) << 7) | (sysexAddress & 0x7F);
}
constexpr uint32_t unpackAddress(uint32_t packed) {
    return (((packed >> 14) & 0x7F) << 16) | (((packed >> 7) & 0x7F) << 8) | (packed & 0x7F);
}

// Appends Roland DT1 messages carrying `data` from `packedAddress` on, at most 256 bytes each (as bulk dumps do).
void appendDataSet(std::vector<uint8_t>& out, uint8_t deviceId, uint32_t packedAddress, const uint8_t* data, size_t length) {
    constexpr size_t kMaxChunk = 256;
    while (length > 0) {
        const size_t chunk = std::min(length, kMaxChunk);
        const uint32_t address = unpackAddress(packedAddress);
        const size_t start = out.size();
        const uint8_t header[] = {0xF0, 0x41, deviceId, 0x16, 0x12,
                                  uint8_t(address >> 16), uint8_t((address >> 8) & 0x7F), uint8_t(address & 0x7F)};
        out.insert(out.end(), header, header + sizeof(header));
        out.insert(out.end(), data, data + chunk);
        out.push_back(MT32Emu::Synth::calcSysexChecksum(&out[start + 5], uint32_t(3 + chunk)));
        out.push_back(0xF7);
        packedAddress += uint32_t(chunk);
        data += chunk;
        length -= chunk;
    }
}

// Calls f(message, length) for each complete F0 ... F7 message in the data.
template <typename F>
void forEachSysex(const uint8_t* data, size_t length, F&& f) {
    for (size_t i = 0; i < length;) {
        if (data[i] != 0xF0) {
            i++;
            continue;
        }
        size_t end = i + 1;
        while (end < length && data[end] != 0xF7) end++;
        if (end >= length) break;  // Unterminated message at the end of the data
        f(data + i, end + 1 - i);
        i = end + 1;
    }
}

// For a DT1 to a unit (device 10H and up) in the MT-32's map (model 16H) that writes any of the part channels (system
// area 10 00 0D-15): true, with `rest` holding DT1s for the other bytes it writes (none if it writes channels alone).
// Damaged messages are left as they are, for the synth to ignore.
bool withoutPartChannels(const uint8_t* sysex, uint32_t length, std::vector<uint8_t>& rest) {
    if (length < 11 || sysex[0] != 0xF0 || sysex[1] != 0x41 || sysex[2] < 0x10 || sysex[3] != 0x16 || sysex[4] != 0x12) return false;
    uint32_t end = 8;  // The F7
    while (end < length && sysex[end] != 0xF7) end++;
    if (end >= length || end < 10) return false;
    unsigned sum = 0;
    for (uint32_t i = 5; i < end; i++) sum += sysex[i];
    if (sum % 128 != 0) return false;
    const uint32_t start = (uint32_t(sysex[5]) << 14) | (uint32_t(sysex[6]) << 7) | sysex[7];
    const uint32_t stop = start + (end - 9);  // Data bytes 8 .. end - 2
    const uint32_t first = kSystemAddress + kSystemChannels;  // Packed, as `start`
    const uint32_t last = first + uint32_t(kBasePartCount);
    if (stop <= first || start >= last) return false;
    rest.clear();
    const auto append = [&](uint32_t from, uint32_t to) {
        if (from >= to) return;
        const size_t header = rest.size();
        rest.insert(rest.end(), {0xF0, 0x41, sysex[2], 0x16, 0x12, uint8_t(from >> 14), uint8_t((from >> 7) & 0x7F), uint8_t(from & 0x7F)});
        rest.insert(rest.end(), sysex + 8 + (from - start), sysex + 8 + (to - start));
        rest.push_back(MT32Emu::Synth::calcSysexChecksum(&rest[header + 5], uint32_t(rest.size() - header - 5)));
        rest.push_back(0xF7);
    };
    append(start, std::min(stop, first));
    append(std::max(start, last), stop);
    return true;
}

// Keeps a ROM file in memory while Synth::open() copies what it needs out of it.
struct LoadedRom {
    std::vector<uint8_t> data;
    std::unique_ptr<MT32Emu::ArrayFile> file;
    const MT32Emu::ROMImage* image = nullptr;

    LoadedRom() = default;
    LoadedRom(const LoadedRom&) = delete;
    LoadedRom& operator=(const LoadedRom&) = delete;
    ~LoadedRom() {
        if (image != nullptr) MT32Emu::ROMImage::freeROMImage(image);
    }

    const MT32Emu::ROMInfo* info() const { return image->getROMInfo(); }
};

bool loadRom(const std::filesystem::path& path, MT32Emu::ROMInfo::Type expectedType, LoadedRom& rom, std::string& error) {
    const std::string name = path.filename().u8string();
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "Cannot open ROM file " + path.u8string();
        return false;
    }
    rom.data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    rom.file.reset(new MT32Emu::ArrayFile(rom.data.data(), rom.data.size()));
    rom.image = MT32Emu::ROMImage::makeROMImage(rom.file.get());
    const MT32Emu::ROMInfo* info = rom.info();
    if (info == nullptr) {
        error = name + " is not a recognised ROM image (unknown size or SHA1)";
        return false;
    }
    if (info->type != expectedType) {
        error = name + (expectedType == MT32Emu::ROMInfo::Control ? " is not a Control ROM" : " is not a PCM ROM");
        return false;
    }
    if (info->pairType != MT32Emu::ROMInfo::Full) {
        error = name + " is only part of a ROM; a complete image is needed";
        return false;
    }
    return true;
}

MT32Emu::AnalogOutputMode resolveAnalogMode(AnalogMode mode, uint32_t sampleRate) {
    if (mode == AnalogMode::Auto) return MT32Emu::SampleRateConverter::getBestAnalogOutputMode(sampleRate);
    return MT32Emu::AnalogOutputMode(int(mode));
}

void applyOptions(MT32Emu::Synth& synth, const EngineOptions& options) {
    synth.setOutputGain(options.outputGain);
    synth.setReverbOutputGain(options.reverbGain);
    synth.setReverbEnabled(options.reverbEnabled);  // No-op until the synth is open
    synth.setReverbOverridden(options.reverbOverridden);
    synth.setReversedStereoEnabled(options.reversedStereo);
    synth.setNiceAmpRampEnabled(options.niceAmpRamp);
    synth.setNicePanningEnabled(options.nicePanning);
    synth.setNicePartialMixingEnabled(options.nicePartialMixing);
    synth.setDACInputMode(options.dacInputMode);
    synth.setControlChannel(options.controlChannel);
    synth.setDeviceID(uint8_t(std::clamp<int>(options.unitNumber, 17, 32) - 1));
    synth.setDSeriesReverbEnabled(options.dSeriesReverb);
    synth.setMIDIDelayMode(options.midiCableSpeed ? MT32Emu::MIDIDelayMode_DELAY_SHORT_MESSAGES_ONLY : MT32Emu::MIDIDelayMode_IMMEDIATE);
}

}  // namespace

SynthEngine::SynthEngine() : renderStartTime_(Clock::now()) {
    surroundMix_.resize(2 * size_t(kSurroundFrames));
    for (std::vector<float>& pair : surroundMulti_) pair.resize(2 * size_t(kSurroundFrames));
}

SynthEngine::~SynthEngine() {
    close();
}

bool SynthEngine::configure(const EngineConfig& config, std::string& error, bool preserveMemory) {
    LoadedRom control;
    LoadedRom pcm;
    if (!loadRom(config.controlRom, MT32Emu::ROMInfo::Control, control, error)) return false;
    if (!loadRom(config.pcmRom, MT32Emu::ROMInfo::PCM, pcm, error)) return false;

    const EngineOptions currentOptions = options();
    const MT32Emu::DSeriesReverbSettings reverbSettings = dSeriesReverbSettings();
    uint32_t mutedParts = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        mutedParts = mutedParts_;
    }
    const bool translation = mt32Translation();
    const MT32Emu::AnalogOutputMode analogMode = resolveAnalogMode(config.analogMode, config.outputSampleRate);

    std::unique_ptr<MT32Emu::Synth> synth(new MT32Emu::Synth(this));
    synth->setMIDIEventQueueSize(kMidiQueueSize);
    synth->preallocateReverbMemory(true);  // Reverb mode changes then never allocate on the audio thread
    synth->setDSeriesReverbSettings(reverbSettings);
    applyOptions(*synth, currentOptions);
    const uint32_t partialCount = std::clamp<uint32_t>(config.partialCount, 8, kMaxPartials);
    synth->setSixteenPartMode(config.sixteenParts);
    synth->setPartOutputsAvailable(config.partOutputs || config.multiOutputs, config.partOutputs);  // 7.1: the MULTI pairs alone
    if (!synth->open(*control.image, *pcm.image, partialCount, analogMode)) {
        error = std::string("The synth failed to start with ") + control.info()->description + " and " +
                pcm.info()->description + ". The Control and PCM ROMs must come from the same model.";
        return false;
    }
    applyOptions(*synth, currentOptions);
    synth->setMutedParts(mutedParts);
    synth->setPartOutputMask(partOutputMask());
    synth->setMultiOutputsEnabled(config.multiOutputs);
    synth->setMultiPairsStereo(multiPairsStereo());
    synth->setMasterVolumeEnabled(translation && synth->isD110Mode());  // Before the memory, which holds the volume
    // As updateMidiExtensionsLocked() has them (the player stops below).
    synth->setMIDIExtensionsEnabled(currentOptions.midiExtensions && synth->isD110Mode() && !translation);

    if (preserveMemory && synth->isD110Mode()) {
        std::vector<uint8_t> memory;
        bool card = false;
        uint8_t currentPatch = 0;
        uint8_t currentPerformance = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (synth_ && synth_->isD110Mode()) {
                card = synth_->isCardInserted();
                // The card first, so that parts playing card tones find them.
                memory = dumpSysexLocked(*synth_, (card ? unsigned(DumpCard) : 0u) | DumpEverything);
                currentPatch = synth_->getCurrentPatch();  // Not in the memory: the numbers the display shows
                currentPerformance = synth_->getCurrentPerformance();
            }
        }
        synth->setCardInserted(card);
        applySysexLocked(*synth, memory.data(), memory.size());  // The new synth is not shared yet
        synth->setCurrentPatch(currentPatch);
        synth->setCurrentPerformance(currentPerformance);
    }

    std::unique_ptr<MT32Emu::SampleRateConverter> converter;
    std::unique_ptr<MultiOutputConverter> busConverter;
    if (config.partOutputs || config.multiOutputs) {
        busConverter.reset(new MultiOutputConverter(*synth, double(config.outputSampleRate), config.resamplerQuality, config.partOutputs));
    } else {
        converter.reset(new MT32Emu::SampleRateConverter(*synth, double(config.outputSampleRate), config.resamplerQuality));
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        player_.stop(nullptr);  // The new synth starts from its power-on state, so rewind the song
        endGmStreamLocked();
        synth_.swap(synth);
        converter_.swap(converter);
        busConverter_.swap(busConverter);
        config_ = config;
        analogModeInUse_ = AnalogMode(int(analogMode));
        controlRomName_ = control.info()->description;
        pcmRomName_ = pcm.info()->description;
        renderStartSynthTime_ = 0;
        renderBlockSynthSamples_ = 0.0;
        renderStartTime_ = Clock::now();
        updateMt32PresetsLocked(false);  // A new synth starts with the ROM's presets
    }
    log("Synth started: " + controlRomName_ + " + " + pcmRomName_);
    return true;
    // The previous converters and synth are destroyed here, outside the lock (converters first).
}

void SynthEngine::close() {
    std::unique_ptr<MT32Emu::Synth> synth;
    std::unique_ptr<MT32Emu::SampleRateConverter> converter;
    std::unique_ptr<MultiOutputConverter> busConverter;
    std::lock_guard<std::mutex> lock(mutex_);
    player_.stop(nullptr);
    synth.swap(synth_);
    converter.swap(converter_);
    busConverter.swap(busConverter_);
    // The lock is released before the locals are destroyed (reverse declaration order: lock, converters, synth).
}

void SynthEngine::setPartOutputMask(uint32_t mask) {
    std::lock_guard<std::mutex> lock(mutex_);
    partOutputMask_ = mask;
    if (synth_) synth_->setPartOutputMask(mask);
}

uint32_t SynthEngine::partOutputMask() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return partOutputMask_;
}

void SynthEngine::setMultiPairsStereo(bool stereo) {
    std::lock_guard<std::mutex> lock(mutex_);
    multiPairsStereo_ = stereo;
    if (synth_) synth_->setMultiPairsStereo(stereo);
}

bool SynthEngine::multiPairsStereo() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return multiPairsStereo_;
}

bool SynthEngine::isOpen() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return synth_ != nullptr;
}

void SynthEngine::render(float* interleavedStereo, uint32_t frames, const TimedMidi* events, size_t eventCount, const uint8_t* sysex) {
    renderBuses(interleavedStereo, nullptr, nullptr, frames, events, eventCount, sysex);
}

void SynthEngine::renderBuses(float* mix, float* const* parts, float* const* multi, uint32_t frames, const TimedMidi* events,
                              size_t eventCount, const uint8_t* sysex) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Output frames [offset, offset + count) of the mix and of the parts and MULTI pairs wanted.
    const auto produce = [&](uint32_t offset, uint32_t count) {
        if (busConverter_) {
            float* partStreams[kPartOutputs];
            float* multiStreams[kMultiPairs];
            for (int part = 0; part < kPartOutputs; part++) {
                partStreams[part] = parts != nullptr && parts[part] != nullptr ? parts[part] + 2 * size_t(offset) : nullptr;
            }
            for (int pair = 0; pair < kMultiPairs; pair++) {
                multiStreams[pair] = multi != nullptr && multi[pair] != nullptr ? multi[pair] + 2 * size_t(offset) : nullptr;
            }
            busConverter_->getOutputSamples(mix + 2 * size_t(offset), partStreams, multiStreams, count);
            return;
        }
        converter_->getOutputSamples(mix + 2 * size_t(offset), count);
        for (int part = 0; parts != nullptr && part < kPartOutputs; part++) {
            if (parts[part] != nullptr) std::fill(parts[part] + 2 * size_t(offset), parts[part] + 2 * size_t(offset + count), 0.0f);
        }
        for (int pair = 0; multi != nullptr && pair < kMultiPairs; pair++) {
            if (multi[pair] != nullptr) std::fill(multi[pair] + 2 * size_t(offset), multi[pair] + 2 * size_t(offset + count), 0.0f);
        }
    };
    const auto toSynthSamples = [&](double outputFrames) {
        return busConverter_ ? busConverter_->convertOutputToSynthTimestamp(outputFrames) : converter_->convertOutputToSynthTimestamp(outputFrames);
    };
    if (!synth_) {
        std::lock_guard<std::mutex> pendingLock(pendingMutex_);
        pending_.clear();
        pendingSysex_.clear();
        std::fill(mix, mix + 2 * size_t(frames), 0.0f);
        for (int part = 0; parts != nullptr && part < kPartOutputs; part++) {
            if (parts[part] != nullptr) std::fill(parts[part], parts[part] + 2 * size_t(frames), 0.0f);
        }
        for (int pair = 0; multi != nullptr && pair < kMultiPairs; pair++) {
            if (multi[pair] != nullptr) std::fill(multi[pair], multi[pair] + 2 * size_t(frames), 0.0f);
        }
        return;
    }
    const uint32_t now = synth_->getInternalRenderedSampleCount();
    const double block = toSynthSamples(frames);
    updateMidiExtensionsLocked();
    const MidiPlayer::PlayFunction playFileEvent = [this](uint32_t message, const uint8_t* data, uint32_t length, uint32_t timestamp) {
        return playExternalLocked(message, data, length, timestamp, playerKind_ == MidiFileKind::External, MidiSource::Player);
    };
    if (eventCount == 0) {
        // MIDI file events for this block go first: the synth's queue plays events in order, so live events
        // (scheduled a block ahead) queued before them would hold them back.
        player_.pump(now + uint32_t(std::ceil(block)), playFileEvent);
        playPendingMidi(now);
        renderStartTime_ = Clock::now();
        renderStartSynthTime_ = now;
        renderBlockSynthSamples_ = block;
        produce(0, frames);
        return;
    }
    // The host's events split the block: each plays at the synth's time when the rendering reaches its frame, and the
    // MIDI file is fed piece by piece, so the queue stays in time order (live MIDI goes first, at once, for the same reason).
    playPendingMidi(now, true);
    renderStartTime_ = Clock::now();
    renderStartSynthTime_ = now;
    renderBlockSynthSamples_ = block;
    uint32_t done = 0;
    size_t next = 0;
    for (;;) {
        const uint32_t pieceStart = synth_->getInternalRenderedSampleCount();
        // The events at this frame; at the end of the block, any the host placed beyond it.
        for (; next < eventCount && (events[next].frame <= done || done >= frames); next++) {
            const TimedMidi& event = events[next];
            if (event.shortMessage == 0 && sysex == nullptr) continue;
            midiEvents_++;
            if (!playExternalLocked(event.shortMessage, event.shortMessage != 0 ? nullptr : sysex + event.sysexOffset, event.sysexLength,
                                    pieceStart, true, MidiSource::Live)) {
                queueOverflows_++;
            }
        }
        if (done >= frames) break;
        const uint32_t end = next < eventCount ? std::min(events[next].frame, frames) : frames;
        player_.pump(pieceStart + uint32_t(std::ceil(toSynthSamples(end - done))), playFileEvent);
        produce(done, end - done);
        done = end;
    }
}

void SynthEngine::renderSurround(float* interleaved, uint32_t frames) {
    float* multi[kMultiPairs];
    for (int pair = 0; pair < kMultiPairs; pair++) multi[pair] = surroundMulti_[pair].data();
    // In one piece as a rule (a device's period is shorter), so that live MIDI keeps its timing within the block.
    for (uint32_t done = 0; done < frames;) {
        const uint32_t count = std::min(frames - done, kSurroundFrames);
        renderBuses(surroundMix_.data(), nullptr, multi, count);
        const float* mix = surroundMix_.data();
        float* out = interleaved + size_t(kSurroundChannels) * done;
        for (uint32_t i = 0; i < count; i++, out += kSurroundChannels) {
            out[0] = mix[2 * i] + multi[0][2 * i];          // Front left: the mix's left and MULTI 1
            out[1] = mix[2 * i + 1] + multi[0][2 * i + 1];  // Front right: its right and MULTI 2
            out[2] = 0.0f;                                  // Centre
            out[3] = 0.0f;                                  // LFE
            out[4] = multi[1][2 * i];                       // Back (rear) left: MULTI 3
            out[5] = multi[1][2 * i + 1];                   // Back right: MULTI 4
            out[6] = multi[2][2 * i];                       // Side left: MULTI 5
            out[7] = multi[2][2 * i + 1];                   // Side right: MULTI 6
        }
        done += count;
    }
}

// Live events are scheduled one audio block after the moment they arrived, at the same offset into
// the block, measured from the block that was playing then. That adds one block of latency but removes
// the jitter of snapping every event to the start of the next block.
uint32_t SynthEngine::liveTimestamp(Clock::time_point arrival, uint32_t now) const {
    if (!options_.midiTimestamping || renderBlockSynthSamples_ <= 0.0) return now;
    const double elapsed = std::chrono::duration<double>(arrival - renderStartTime_).count() * MT32Emu::SAMPLE_RATE;
    const uint32_t timestamp = renderStartSynthTime_ + uint32_t(std::clamp(elapsed, 0.0, renderBlockSynthSamples_) + renderBlockSynthSamples_);
    return int32_t(timestamp - now) < 0 ? now : timestamp;
}

void SynthEngine::playPendingMidi(uint32_t now, bool atOnce) {
    {
        std::lock_guard<std::mutex> pendingLock(pendingMutex_);
        playing_.swap(pending_);
        playingSysex_.swap(pendingSysex_);
    }
    for (const PendingMidi& event : playing_) {
        const uint32_t timestamp = atOnce ? now : liveTimestamp(event.arrival, now);
        midiEvents_++;
        playExternalLocked(event.shortMessage, event.shortMessage != 0 ? nullptr : &playingSysex_[event.sysexOffset], event.sysexLength,
                           timestamp, true, MidiSource::Live);
    }
    playing_.clear();
    playingSysex_.clear();
}

bool SynthEngine::translatingLocked() const {
    return mt32Translation_ && synth_ && synth_->isD110Mode();
}

bool SynthEngine::playExternalLocked(uint32_t shortMessage, const uint8_t* sysex, uint32_t sysexLength, uint32_t timestamp, bool translate,
                                     MidiSource source) {
    if (shortMessage == 0 && synth_->isMIDIExtensionsEnabled()) {
        const int stream = int(source);
        if (MT32Emu::Synth::isMIDIResetMessage(sysex, sysexLength)) {
            gmStream_[stream] = true;
        } else if (gmStream_[stream] && withoutPartChannels(sysex, sysexLength, gmStreamSysex_)) {
            if (!gmStreamLogged_[stream]) {
                gmStreamLogged_[stream] = true;
                log("Kept the part channels: MT-32 SysEx after a GM/GS reset is for another module (MIDI extensions)");
            }
            bool queued = true;
            forEachSysex(gmStreamSysex_.data(), gmStreamSysex_.size(), [&](const uint8_t* message, size_t length) {
                if (!playExternalMessageLocked(0, message, uint32_t(length), timestamp, translate)) queued = false;
            });
            return queued;
        }
    }
    return playExternalMessageLocked(shortMessage, sysex, sysexLength, timestamp, translate);
}

void SynthEngine::updateMidiExtensionsLocked() {
    const MidiPlayer::State state = player_.state();
    const bool romPlay = playerKind_ == MidiFileKind::UnitSong && (state == MidiPlayer::State::Playing || state == MidiPlayer::State::Paused);
    synth_->setMIDIExtensionsEnabled(options_.midiExtensions && synth_->isD110Mode() && !translatingLocked() && !romPlay);
}

void SynthEngine::endGmStreamLocked(int source) {
    for (int stream = 0; stream < 2; stream++) {
        if (source >= 0 && stream != source) continue;
        gmStream_[stream] = false;
        gmStreamLogged_[stream] = false;
    }
}

bool SynthEngine::playExternalMessageLocked(uint32_t shortMessage, const uint8_t* sysex, uint32_t sysexLength, uint32_t timestamp,
                                            bool translate) {
    if (!translate || !translatingLocked()) {
        return shortMessage != 0 ? synth_->playMsg(shortMessage, timestamp) : synth_->playSysex(sysex, sysexLength, timestamp);
    }
    translated_.clear();
    translatedPrograms_.clear();
    if (shortMessage != 0) {
        translator_.translateShort(shortMessage, synth_->getDeviceID(), translatedPrograms_, translated_);
        bool queued = true;
        for (uint32_t message : translatedPrograms_) {
            if (!synth_->playMsg(message, timestamp)) queued = false;
        }
        forEachSysex(translated_.data(), translated_.size(), [&](const uint8_t* message, size_t length) {
            if (!synth_->playSysex(message, uint32_t(length), timestamp)) queued = false;
        });
        return queued;
    }
    translator_.translateSysex(sysex, sysexLength, synth_->getDeviceID(), translated_, translatedPrograms_);
    // An MT-32 reset (7F 00 00) comes back as the power-on setup.
    if (sysexLength > 8 && sysex[1] == 0x41 && sysex[3] == 0x16 && sysex[5] == 0x7F) appendMt32PowerOnExtras(translated_);
    bool queued = true;
    forEachSysex(translated_.data(), translated_.size(), [&](const uint8_t* message, size_t length) {
        if (!synth_->playSysex(message, uint32_t(length), timestamp)) queued = false;
    });
    for (uint32_t message : translatedPrograms_) {
        if (!synth_->playMsg(message, timestamp)) queued = false;
    }
    return queued;
}

void SynthEngine::appendMt32PowerOnExtras(std::vector<uint8_t>& messages) {
    if (synth_->getPartCount() <= uint32_t(kBasePartCount)) return;
    uint8_t off[kExtraParts];
    std::fill(off, off + kExtraParts, kChannelOff);
    appendDataSet(messages, synth_->getDeviceID(), kExtraSystemAddress + kExtraChannels, off, kExtraParts);
}

void SynthEngine::setMt32Translation(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    mt32Translation_ = enabled;
    endGmStreamLocked();
    if (synth_) {
        synth_->setMasterVolumeEnabled(translatingLocked());
        updateMidiExtensionsLocked();
    }
    updateMt32PresetsLocked(false);  // Switching on is followed by mt32PowerOn(), off by the user's setup
}

void SynthEngine::setMt32Presets(std::shared_ptr<const Mt32Presets> presets) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (presets == mt32Presets_) return;
    mt32Presets_ = std::move(presets);
    updateMt32PresetsLocked(true);
}

void SynthEngine::setMt32PresetMode(Mt32Translator::PresetMode mode) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mode == mt32PresetMode_) return;
    mt32PresetMode_ = mode;
    updateMt32PresetsLocked(true);
}

void SynthEngine::setMt32PresetChoices(const Mt32Translator::PresetChoices& presets, const Mt32Translator::RhythmChoices& rhythm) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool changed = false;
    for (int timbre = 0; timbre < 128; timbre++) {
        if (translator_.presetChoice(timbre) == presets[size_t(timbre)]) continue;
        translator_.setPresetChoice(timbre, presets[size_t(timbre)]);
        changed = true;
    }
    for (int timbre = 0; timbre < 64; timbre++) {
        if (translator_.rhythmChoice(timbre) == rhythm[size_t(timbre)]) continue;
        translator_.setRhythmChoice(timbre, rhythm[size_t(timbre)]);
        changed = true;
    }
    if (changed) updateMt32PresetsLocked(true);
}

void SynthEngine::setMt32RoomyToms(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (translator_.roomyToms() == enabled) return;
    translator_.setRoomyToms(enabled);
    updateMt32PresetsLocked(true);  // The toms' stand-ins change, and the sample in the MT-32's own timbres
}

void SynthEngine::updateMt32PresetsLocked(bool resend) {
    translator_.setPresets(mt32Presets_);
    translator_.setPresetMode(mt32PresetMode_);
    translator_.setPresetBanks(translatingLocked() && mt32PresetMode_ != Mt32Translator::PresetMode::StandIns);
    if (!synth_ || !synth_->isD110Mode()) return;
    loadPresetBanksLocked();
    if (resend && translatingLocked()) {
        std::vector<uint8_t> messages;
        translator_.resend(synth_->getDeviceID(), messages);
        synth_->flushMIDIQueue();
        applySysexLocked(*synth_, messages.data(), messages.size());
    }
}

void SynthEngine::loadPresetBanksLocked() {
    MT32Emu::Synth& synth = *synth_;
    synth.restorePresetTimbres();
    const std::shared_ptr<const Mt32Presets>& presets = translator_.presets();
    constexpr size_t kToneSize = 246;
    uint8_t tone[kToneSize];
    if (translator_.presetBanks()) {
        // The D-110's presets (a, b and r) as the ROM has them, the stand-ins' source.
        std::vector<uint8_t> rom(256 * kToneSize);
        for (uint16_t t = 0; t < 256; t++) {
            if (t < 128 || t >= 192) synth.readTone(t, &rom[t * kToneSize]);
        }
        for (int t = 0; t < 128; t++) {
            if (translator_.presetIsExact(t)) {
                translator_.convertTone(presets->melodic[size_t(t)].data(), tone);
                synth.setPresetTimbre(uint32_t(t), tone);
            } else {
                synth.setPresetTimbre(uint32_t(t), &rom[translator_.presetChoice(t).tone * kToneSize]);
            }
            synth.setAltTimbre(uint32_t(t), &rom[size_t(t) * kToneSize]);
        }
        for (int t = 0; t < 63; t++) {
            const int standIn = translator_.rhythmStandIn(t);
            if (translator_.rhythmIsExact(t)) {
                translator_.convertTone(presets->rhythm[size_t(t)].data(), tone, true);
                synth.setPresetTimbre(uint32_t(192 + t), tone);
            } else if (standIn != Mt32Translator::kRhythmOff) {
                synth.setPresetTimbre(uint32_t(192 + t), &rom[size_t(192 + standIn) * kToneSize]);
            }
        }
    } else if (presets) {
        for (int t = 0; t < 128; t++) {
            translator_.convertTone(presets->melodic[size_t(t)].data(), tone);
            synth.setAltTimbre(uint32_t(t), tone);
        }
    }
    synth.setAltTimbresEnabled(presets != nullptr);
}

bool SynthEngine::mt32Translation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mt32Translation_;
}

void SynthEngine::mt32PowerOn() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!synth_ || !synth_->isD110Mode()) return;
    std::vector<uint8_t> messages;
    std::vector<uint32_t> programs;
    translator_.powerOn(synth_->getDeviceID(), messages, programs);
    appendMt32PowerOnExtras(messages);
    synth_->flushMIDIQueue();
    applySysexLocked(*synth_, messages.data(), messages.size());
    for (uint32_t message : programs) synth_->playMsgNow(message);
    synth_->resetMIDIChannels();
}

void SynthEngine::onMidiShortMessage(uint32_t message) {
    const Clock::time_point arrival = Clock::now();
    std::lock_guard<std::mutex> lock(pendingMutex_);
    if (pending_.size() >= kMaxPendingMidi) {
        queueOverflows_++;
        return;
    }
    pending_.push_back(PendingMidi{arrival, message, 0, 0});
}

void SynthEngine::onMidiSysex(const uint8_t* data, size_t length) {
    if (length < 2 || length > MT32Emu::MAX_STREAM_BUFFER_SIZE) return;
    const Clock::time_point arrival = Clock::now();
    std::lock_guard<std::mutex> lock(pendingMutex_);
    if (pending_.size() >= kMaxPendingMidi || pendingSysex_.size() + length > kMaxPendingSysexBytes) {
        queueOverflows_++;
        return;
    }
    pending_.push_back(PendingMidi{arrival, 0, uint32_t(pendingSysex_.size()), uint32_t(length)});
    pendingSysex_.insert(pendingSysex_.end(), data, data + length);
}

void SynthEngine::allNotesOff() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (synth_) MidiPlayer::silence(*synth_);
}

void SynthEngine::resetMidiChannels() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!synth_) return;
    synth_->flushMIDIQueue();
    synth_->resetMIDIChannels();
    resetPartMixLocked();
}

void SynthEngine::resetPartMix() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (synth_) resetPartMixLocked();
}

void SynthEngine::resetPartMixLocked() {
    if (!synth_->isD110Mode()) return;  // An MT-32's own ROMs keep the MT-32's levels and pans
    if (translatingLocked()) {
        // The MT-32 side's part temps (parts 1-8, then the rhythm part at 03 01 00): level 100, pan 7.
        std::vector<uint8_t> converted;
        std::vector<uint32_t> programs;
        for (uint32_t part = 0; part < 9; part++) {
            const uint32_t address = (3u << 14) + part * 16 + 8;
            std::vector<uint8_t> message = {0xF0, 0x41, 0x10, 0x16, 0x12, uint8_t(address >> 14), uint8_t((address >> 7) & 0x7F),
                                            uint8_t(address & 0x7F), 100, 7};
            message.push_back(MT32Emu::Synth::calcSysexChecksum(&message[5], 5));
            message.push_back(0xF7);
            converted.clear();
            programs.clear();
            translator_.translateSysex(message.data(), message.size(), synth_->getDeviceID(), converted, programs);
            applySysexLocked(*synth_, converted.data(), converted.size());
            for (uint32_t program : programs) synth_->playMsgNow(program);
        }
    }
    synth_->resetPartLevelsAndPans();  // All parts (parts 9-15 too), and the performance patch's parts 1 and 2 again
}

void SynthEngine::sendDataSet(uint8_t addressHigh, uint8_t addressMid, uint8_t addressLow, const uint8_t* data, size_t length) {
    // Roland DT1: F0 41 <device ID = unit number less one> <model 16> 12 <address> <data> <checksum> F7
    std::vector<uint8_t> message = {0xF0, 0x41, synth_->getDeviceID(), 0x16, 0x12, addressHigh, addressMid, addressLow};
    message.insert(message.end(), data, data + length);
    message.push_back(MT32Emu::Synth::calcSysexChecksum(&message[5], uint32_t(3 + length)));
    message.push_back(0xF7);
    synth_->playSysex(message.data(), uint32_t(message.size()));
}

void SynthEngine::setPartChannels(const std::array<uint8_t, kMaxPartCount>& channels) {
    std::array<uint8_t, kMaxPartCount> clamped;
    for (int i = 0; i < kMaxPartCount; i++) clamped[i] = std::min(channels[i], kChannelOff);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!synth_) return;
    sendDataSet(0x10, 0x00, kSystemChannels, clamped.data(), kBasePartCount);
    translator_.setPartChannels(clamped.data());
    if (synth_->getPartCount() > uint32_t(kBasePartCount)) {
        sendDataSet(0x11, 0x00, kExtraChannels, clamped.data() + kBasePartCount, kMaxPartCount - kBasePartCount);
    }
}

void SynthEngine::setMasterVolume(int volume) {
    const uint8_t value = uint8_t(std::clamp(volume, 0, 100));
    std::lock_guard<std::mutex> lock(mutex_);
    if (synth_) sendDataSet(0x10, 0x00, kSystemMasterVolume, &value, 1);
}

void SynthEngine::setReverb(int mode, int time, int level) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!synth_) return;
    const int maxMode = synth_->isD110Mode() ? 8 : 3;
    const uint8_t values[3] = {uint8_t(std::clamp(mode, 0, maxMode)), uint8_t(std::clamp(time, 0, 7)), uint8_t(std::clamp(level, 0, 7))};
    sendDataSet(0x10, 0x00, kSystemReverbMode, values, 3);
}

void SynthEngine::setMutedParts(uint32_t partMask) {
    std::lock_guard<std::mutex> lock(mutex_);
    mutedParts_ = partMask;
    if (synth_) synth_->setMutedParts(partMask);
}

void SynthEngine::setDSeriesReverbSettings(const MT32Emu::DSeriesReverbSettings& settings) {
    std::lock_guard<std::mutex> lock(mutex_);
    dSeriesReverbSettings_ = settings;
    dSeriesReverbSettings_.clamp();
    if (synth_) synth_->setDSeriesReverbSettings(dSeriesReverbSettings_);
}

MT32Emu::DSeriesReverbSettings SynthEngine::dSeriesReverbSettings() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dSeriesReverbSettings_;
}

void SynthEngine::setMasterTune(int value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!synth_) return;
    const uint8_t tune = uint8_t(std::clamp(value, 0, 127));
    sendDataSet(0x10, 0x00, kSystemMasterTune, &tune, 1);
}

void SynthEngine::recallPatch(int patchNumber) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (synth_) synth_->recallPatch(uint8_t(std::clamp(patchNumber, 0, 127)));
}

void SynthEngine::setPerformanceMode(bool enabled, int channel) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (synth_) synth_->setPerformanceMode(enabled, uint8_t(std::clamp(channel, 0, 15)));
}

void SynthEngine::recallPerformance(int patchNumber) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (synth_) synth_->recallPerformance(uint8_t(std::clamp(patchNumber, 0, 127)));
}

void SynthEngine::setCurrentPatchNumber(int patchNumber) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (synth_) synth_->setCurrentPatch(uint8_t(std::clamp(patchNumber, 0, 127)));
}

void SynthEngine::setCurrentPerformanceNumber(int patchNumber) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (synth_) synth_->setCurrentPerformance(uint8_t(std::clamp(patchNumber, 0, 127)));
}

void SynthEngine::setPartProgram(int part, int timbreNumber) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (synth_ && part >= 0 && part < int(synth_->getPartCount()) && part != kRhythmPart) {
        synth_->selectPartTimbre(uint8_t(part), uint16_t(std::clamp(timbreNumber, 0, 383)));
    }
}

bool SynthEngine::insertCard(const std::filesystem::path& path, std::string& error) {
    std::vector<uint8_t> data;
    if (!path.empty()) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            error = "Cannot open " + path.u8string();
            return false;
        }
        data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!synth_ || !synth_->isD110Mode()) {
        error = "Memory cards need the D-110 ROMs";
        return false;
    }
    clearCardLocked();
    synth_->setCardInserted(true);
    applySysexLocked(*synth_, data.data(), data.size());
    return true;
}

void SynthEngine::ejectCard() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!synth_) return;
    clearCardLocked();
    synth_->setCardInserted(false);
}

bool SynthEngine::saveCard(const std::filesystem::path& path, std::string& error) {
    std::vector<uint8_t> data;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!synth_ || !synth_->isCardInserted()) {
            error = "No memory card is inserted";
            return false;
        }
        data = dumpSysexLocked(*synth_, DumpCard);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    if (!out) {
        error = "Cannot write " + path.u8string();
        return false;
    }
    return true;
}

// Copies bytes between two areas with the same layout, at once and as SysEx (so that the synth refreshes what uses them).
void SynthEngine::copyAreaLocked(uint32_t fromPacked, uint32_t toPacked, uint32_t entrySize, uint32_t entries, uint32_t entryStride) {
    std::vector<uint8_t> buffer(entrySize);
    std::vector<uint8_t> messages;
    for (uint32_t entry = 0; entry < entries; entry++) {
        synth_->readMemory(fromPacked + entry * entryStride, entrySize, buffer.data());
        appendDataSet(messages, synth_->getDeviceID(), toPacked + entry * entryStride, buffer.data(), entrySize);
    }
    applySysexLocked(*synth_, messages.data(), messages.size());
}

void SynthEngine::copyMemoryToCard() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!synth_ || !synth_->isCardInserted()) return;
    copyAreaLocked(packAddress(0x080000), packAddress(0x180000), kToneDataSize, 64, 256);
    copyAreaLocked(packAddress(0x050000), packAddress(0x150000), 128 * 8, 1, 0);
    copyAreaLocked(packAddress(0x060000), packAddress(0x160000), 128, 64, 128);
}

void SynthEngine::copyCardToMemory() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!synth_ || !synth_->isCardInserted()) return;
    copyAreaLocked(packAddress(0x180000), packAddress(0x080000), kToneDataSize, 64, 256);
    copyAreaLocked(packAddress(0x150000), packAddress(0x050000), 128 * 8, 1, 0);
    copyAreaLocked(packAddress(0x160000), packAddress(0x060000), 128, 64, 128);
}

void SynthEngine::clearCardLocked() {
    const std::vector<uint8_t> zeros(128 * 8, 0);
    std::vector<uint8_t> messages;
    const uint8_t device = synth_->getDeviceID();
    for (uint32_t tone = 0; tone < 64; tone++) appendDataSet(messages, device, packAddress(0x180000) + tone * 256, zeros.data(), kToneDataSize);
    appendDataSet(messages, device, packAddress(0x150000), zeros.data(), 128 * 8);
    for (uint32_t patch = 0; patch < 64; patch++) appendDataSet(messages, device, packAddress(0x160000) + patch * 128, zeros.data(), 128);
    applySysexLocked(*synth_, messages.data(), messages.size());
}

void SynthEngine::setPartParameter(int part, uint8_t offset, uint8_t value) {
    writePartTemp(part, offset, &value, 1);
}

void SynthEngine::setPartFinePan(int part, int pan) {
    if (part < 0 || part >= kMaxPartCount) return;
    const uint32_t value = uint32_t(std::clamp(pan, -64, 64) + 65);
    const uint8_t data[2] = {uint8_t(value >> 7), uint8_t(value & 0x7F)};
    writeData(unpackAddress(kPartFinePanAddress + uint32_t(part) * 2), data, 2);
}

void SynthEngine::setRhythmFinePan(int key, int pan) {
    if (key < 24 || key > 108) return;
    const uint32_t value = uint32_t(std::clamp(pan, -64, 64) + 65);
    const uint8_t data[2] = {uint8_t(value >> 7), uint8_t(value & 0x7F)};
    writeData(unpackAddress(kRhythmFinePanAddress + uint32_t(key - 24) * 2), data, 2);
}

void SynthEngine::writePartTemp(int part, uint8_t offset, const uint8_t* data, size_t length) {
    if (part < 0 || part >= kMaxPartCount || offset + length > kPatchTempSize) return;
    writeData(unpackAddress(partTempAddress(part) + offset), data, length);
}

void SynthEngine::writeData(uint32_t sysexAddress, const uint8_t* data, size_t length) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (synth_) sendDataSet(uint8_t(sysexAddress >> 16), uint8_t(sysexAddress >> 8), uint8_t(sysexAddress), data, length);
}

void SynthEngine::readMemory(uint32_t sysexAddress, uint32_t length, uint8_t* out) {
    std::fill(out, out + length, uint8_t(0));
    std::lock_guard<std::mutex> lock(mutex_);
    if (synth_) synth_->readMemory(packAddress(sysexAddress), length, out);
}

std::vector<std::string> SynthEngine::toneNames() {
    std::vector<std::string> names;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!synth_) return names;
    names.reserve(MT32Emu::TIMBRE_COUNT);
    char name[11];
    for (uint16_t tone = 0; tone < MT32Emu::TIMBRE_COUNT; tone++) {
        synth_->getToneName(tone, name);
        names.emplace_back(name);
    }
    return names;
}

void SynthEngine::dismissLcdMessage() {
    std::lock_guard<std::mutex> lock(logMutex_);
    lcdMessageShown_ = false;
}

std::vector<uint8_t> SynthEngine::dumpSysexLocked(MT32Emu::Synth& synth, unsigned contents) {
    std::vector<uint8_t> out;
    std::vector<uint8_t> buffer;
    const bool d110 = synth.isD110Mode();
    const uint8_t device = synth.getDeviceID();
    auto dump = [&](uint32_t packedAddress, uint32_t length, bool skipIfBlank) {
        buffer.assign(length, 0);
        synth.readMemory(packedAddress, length, buffer.data());
        if (skipIfBlank && std::all_of(buffer.begin(), buffer.end(), [](uint8_t b) { return b == 0; })) return;
        appendDataSet(out, device, packedAddress, buffer.data(), length);
    };
    constexpr uint32_t kToneSize = 246;  // Common + 4 partials; the 10 padding bytes of each 256-byte slot are not sent
    // Order matters when the dump is played back: memories, then system, then the parts' temporary areas
    // (a timbre write loads its tone from memory, which the tone temporary area write then overrides).
    if (d110 && (contents & DumpCard)) {
        for (uint32_t tone = 0; tone < 64; tone++) dump(packAddress(0x180000) + tone * 256, kToneSize, false);
        dump(packAddress(0x150000), 128 * 8, false);
        for (uint32_t patch = 0; patch < 64; patch++) dump(packAddress(0x160000) + patch * 128, 128, false);
    }
    if (contents & DumpTones) {
        for (uint32_t tone = 0; tone < 64; tone++) dump(packAddress(0x080000) + tone * 256, kToneSize, false);
    }
    if (contents & DumpTimbres) dump(packAddress(0x050000), 128 * 8, false);
    if (d110 && (contents & DumpPatches)) {
        for (uint32_t patch = 0; patch < 64; patch++) dump(packAddress(0x060000) + patch * 128, 128, false);
    }
    if (contents & DumpRhythm) {
        dump(packAddress(0x030110), 85 * 4, false);
        if (d110) dump(kRhythmFinePanAddress, 85 * 2, false);  // After the panpots, whose writes drop fine pans
    }
    if (d110 && (contents & DumpD20Patches)) dump(packAddress(0x070000), 128 * 38, true);
    if (d110 && (contents & DumpD20Rhythm)) {
        // Patterns P-51-P-88 and the rhythm track where a D-20 keeps them, and P-11-P-48 (an extension) above.
        for (uint32_t pattern = 0; pattern < 32; pattern++) {
            dump(memoryAddress(0x0A, 0, 0) + pattern * uint32_t(kD20PatternSize), uint32_t(kD20PatternSize), true);
        }
        dump(memoryAddress(0x0C, 0, 0), uint32_t(kD20RhythmTrackSize), true);
        for (uint32_t pattern = 0; pattern < 32; pattern++) {
            dump(memoryAddress(0x0D, 0, 0) + pattern * uint32_t(kD20PatternSize), uint32_t(kD20PatternSize), true);
        }
    }
    const bool sixteenParts = synth.getPartCount() > uint32_t(kBasePartCount);
    if (contents & DumpSystem) {
        dump(kSystemAddress, d110 ? 33 : kSystemSize, false);
        if (sixteenParts) dump(kExtraSystemAddress, kExtraSystemSize, false);
    }
    if (contents & DumpTemporary) {
        dump(kPatchTempAddress, kPatchTempSize * kBasePartCount, false);
        for (uint32_t part = 0; part < 8; part++) dump(packAddress(0x040000) + part * kToneSize, kToneSize, false);
        if (sixteenParts) {
            dump(packAddress(0x130000), kPatchTempSize * kExtraParts, false);
            for (uint32_t part = 0; part < kExtraParts; part++) dump(packAddress(0x140000) + part * kToneSize, kToneSize, false);
        }
        if (d110) dump(kPartFinePanAddress, 2 * synth.getPartCount(), false);  // After all part temps
    }
    if (d110 && (contents & DumpPerformance)) dump(packAddress(0x030400), 38, false);  // D-20 performance patch temp
    return out;
}

size_t SynthEngine::applySysexLocked(MT32Emu::Synth& synth, const uint8_t* data, size_t length) {
    size_t applied = 0;
    std::vector<uint8_t> message;
    for (size_t i = 0; i < length;) {
        if (data[i] != 0xF0) {
            i++;
            continue;
        }
        size_t end = i + 1;
        while (end < length && data[end] != 0xF7) end++;
        if (end >= length) break;  // Unterminated message at the end of the data
        message.assign(data + i, data + end + 1);
        i = end + 1;
        const bool rolandDataSet = message.size() > 8 && message[1] == 0x41 && message[3] == 0x16 && message[4] == 0x12;
        if (rolandDataSet && message[2] >= 0x10) message[2] = synth.getDeviceID();  // Files may come from another unit number
        synth.playSysexNow(message.data(), uint32_t(message.size()));
        applied++;
    }
    return applied;
}

bool SynthEngine::loadSysexFile(const std::filesystem::path& path, std::string& error, bool translate) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "Cannot open " + path.u8string();
        return false;
    }
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t applied = 0;
    bool translated = false;
    bool d20Rhythm = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!synth_) {
            error = "The synth is not running";
            return false;
        }
        if (translate && translatingLocked() && !Mt32Translator::isDSeriesData(data.data(), data.size())) {
            translated = true;
            // Message by message, so that program changes from an MT-32 reset follow its setup.
            std::vector<uint8_t> converted;
            std::vector<uint32_t> programs;
            forEachSysex(data.data(), data.size(), [&](const uint8_t* message, size_t length) {
                converted.clear();
                programs.clear();
                translator_.translateSysex(message, length, synth_->getDeviceID(), converted, programs);
                if (length > 8 && message[1] == 0x41 && message[3] == 0x16 && message[5] == 0x7F) appendMt32PowerOnExtras(converted);
                if (applySysexLocked(*synth_, converted.data(), converted.size()) > 0) applied++;
                for (uint32_t program : programs) synth_->playMsgNow(program);
            });
        } else {
            applied = applySysexLocked(*synth_, data.data(), data.size());
            forEachSysex(data.data(), data.size(), [&](const uint8_t* message, size_t length) {
                if (length > 8 && message[1] == 0x41 && message[3] == 0x16 && message[5] >= 0x0A && message[5] <= 0x0C) d20Rhythm = true;
            });
            d20Rhythm = d20Rhythm && synth_->isD110Mode();
        }
    }
    if (applied == 0) {
        error = path.filename().u8string() + " contains no SysEx messages";
        return false;
    }
    std::string line = "Loaded " + std::to_string(applied) + (translated ? " MT-32 SysEx messages (translated) from " : " SysEx messages from ") +
                       path.filename().u8string();
    if (d20Rhythm) line += ", with D-20 rhythm patterns (Patterns tab)";
    log(line);
    return true;
}

bool SynthEngine::loadD20PresetPatterns(const std::filesystem::path& path, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "Cannot open " + path.u8string();
        return false;
    }
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // P-51-P-88 (0A 00 00 - 0B 12 7F) move to P-11-P-48 (0D 00 00 - 0E 12 7F).
    const uint32_t patternsStart = memoryAddress(0x0A, 0, 0);
    const uint32_t patternsEnd = patternsStart + 32 * uint32_t(kD20PatternSize);
    const uint32_t presetsStart = memoryAddress(0x0D, 0, 0);
    std::vector<uint8_t> moved;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!synth_ || !synth_->isD110Mode()) {
        error = "D-20 rhythm patterns need the D-110 ROMs";
        return false;
    }
    size_t messages = 0;
    forEachSysex(data.data(), data.size(), [&](const uint8_t* message, size_t length) {
        if (length < 10 || message[1] != 0x41 || message[3] != 0x16 || message[4] != 0x12) return;
        const uint32_t address = memoryAddress(message[5], message[6], message[7]);
        const uint32_t count = uint32_t(length - 10);
        if (address < patternsStart || address + count > patternsEnd) return;
        appendDataSet(moved, synth_->getDeviceID(), address - patternsStart + presetsStart, message + 8, count);
        messages++;
    });
    if (messages == 0) {
        error = path.filename().u8string() + " holds no D-20 rhythm patterns";
        return false;
    }
    applySysexLocked(*synth_, moved.data(), moved.size());
    log("Loaded the D-20 patterns P-51-P-88 of " + path.filename().u8string() + " as the presets P-11-P-48");
    return true;
}

bool SynthEngine::loadD20InitialPatterns(const std::filesystem::path& path, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "Cannot open " + path.u8string();
        return false;
    }
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // The file's P-51-P-88 (0A 00 00 - 0B 12 7F) as one image of the area.
    const uint32_t patternsStart = memoryAddress(0x0A, 0, 0);
    const uint32_t patternSize = uint32_t(kD20PatternSize);
    std::vector<uint8_t> area(32 * size_t(patternSize), 0);
    bool found = false;
    forEachSysex(data.data(), data.size(), [&](const uint8_t* message, size_t length) {
        if (length < 10 || message[1] != 0x41 || message[3] != 0x16 || message[4] != 0x12) return;
        const uint32_t count = uint32_t(length - 10);
        if (MT32Emu::Synth::calcSysexChecksum(message + 5, 3 + count) != message[length - 2]) return;
        const uint32_t address = memoryAddress(message[5], message[6], message[7]);
        for (uint32_t k = 0; k < count; k++) {
            if (address + k < patternsStart || address + k >= patternsStart + area.size()) continue;
            area[address + k - patternsStart] = message[8 + k];
            found = true;
        }
    });
    if (!found) {
        error = path.filename().u8string() + " holds no D-20 rhythm patterns";
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!synth_ || !synth_->isD110Mode()) {
        error = "D-20 rhythm patterns need the D-110 ROMs";
        return false;
    }
    // A slot of zeros has never held a pattern: a D-20's patterns, even erased ones, carry their end mark.
    auto blank = [](const uint8_t* bytes, size_t length) { return std::all_of(bytes, bytes + length, [](uint8_t b) { return b == 0; }); };
    std::vector<uint8_t> fill, current(patternSize);
    int filled = 0;
    for (uint32_t pattern = 0; pattern < 32; pattern++) {
        const uint32_t slot = patternsStart + pattern * patternSize;
        const uint8_t* initial = &area[size_t(pattern) * patternSize];
        synth_->readMemory(slot, patternSize, current.data());
        if (!blank(current.data(), patternSize) || blank(initial, patternSize)) continue;
        appendDataSet(fill, synth_->getDeviceID(), slot, initial, patternSize);
        filled++;
    }
    if (filled > 0) {
        applySysexLocked(*synth_, fill.data(), fill.size());
        log("P-51-P-88: " + std::to_string(filled) + " empty slots take the D-20's initial patterns from " + path.filename().u8string());
    }
    return true;
}

void SynthEngine::resetD20RhythmTrack() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (synth_ && synth_->isD110Mode()) synth_->resetD20RhythmTrack();
}

D20Rhythm SynthEngine::d20Rhythm() {
    std::vector<uint8_t> patterns(kD20PatternCount * kD20PatternSize, 0);
    std::vector<uint8_t> track(kD20RhythmTrackSize, 0);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (synth_ && synth_->isD110Mode()) {
            synth_->readMemory(memoryAddress(0x0D, 0, 0), uint32_t(32 * kD20PatternSize), patterns.data());
            synth_->readMemory(memoryAddress(0x0A, 0, 0), uint32_t(32 * kD20PatternSize), patterns.data() + 32 * kD20PatternSize);
            synth_->readMemory(memoryAddress(0x0C, 0, 0), uint32_t(kD20RhythmTrackSize), track.data());
        }
    }
    return parseD20Rhythm(patterns.data(), track.data());
}

void SynthEngine::applySysex(const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (synth_) applySysexLocked(*synth_, data.data(), data.size());
}

std::vector<uint8_t> SynthEngine::dumpSysex(unsigned contents) {
    std::lock_guard<std::mutex> lock(mutex_);
    return synth_ ? dumpSysexLocked(*synth_, contents) : std::vector<uint8_t>();
}

bool SynthEngine::saveSysexFile(const std::filesystem::path& path, unsigned contents, std::string& error) {
    const std::vector<uint8_t> data = dumpSysex(contents);
    if (data.empty()) {
        error = "The synth is not running";
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    if (!out) {
        error = "Cannot write " + path.u8string();
        return false;
    }
    return true;
}

void SynthEngine::setOptions(const EngineOptions& options) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (options.midiExtensions != options_.midiExtensions) endGmStreamLocked();
    options_ = options;
    if (synth_) {
        applyOptions(*synth_, options_);
        updateMidiExtensionsLocked();
    }
}

EngineOptions SynthEngine::options() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return options_;
}

bool SynthEngine::loadMidiFile(const std::filesystem::path& path, std::string& error) {
    std::unique_ptr<SmfFile> file(new SmfFile);
    if (!loadSmfFile(path, *file, error)) return false;
    if (file->events.empty()) {
        error = "The MIDI file contains no playable events";
        return false;
    }
    std::string name = file->title.empty() ? path.filename().u8string() : path.filename().u8string() + " (" + file->title + ")";
    loadMidi(std::move(file), std::move(name));
    return true;
}

void SynthEngine::loadMidi(std::unique_ptr<SmfFile> file, std::string name, MidiFileKind kind) {
    std::lock_guard<std::mutex> lock(mutex_);
    player_.stop(synth_.get());
    player_.load(std::move(file), std::move(name));
    playerKind_ = kind;
    endGmStreamLocked(int(MidiSource::Player));
}

void SynthEngine::playerPlay() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (synth_) player_.play(synth_->getInternalRenderedSampleCount());
}

void SynthEngine::playerPause() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (synth_) player_.pause(*synth_, synth_->getInternalRenderedSampleCount());
}

void SynthEngine::playerStop() {
    std::lock_guard<std::mutex> lock(mutex_);
    player_.stop(synth_.get());
    endGmStreamLocked(int(MidiSource::Player));
}

void SynthEngine::playerSetLoop(bool loop) {
    std::lock_guard<std::mutex> lock(mutex_);
    player_.setLoop(loop);
}

void SynthEngine::playerSetSpeed(double speed) {
    std::lock_guard<std::mutex> lock(mutex_);
    player_.setSpeed(speed, synth_ ? synth_->getInternalRenderedSampleCount() : 0);
}

void SynthEngine::playerQueueNext(std::unique_ptr<SmfFile> file, std::string name) {
    std::lock_guard<std::mutex> lock(mutex_);
    player_.queueNext(std::move(file), std::move(name));
}

void SynthEngine::getStatus(EngineStatus& status) {
    {
        std::lock_guard<std::mutex> logLock(logMutex_);
        status.lcdMessage = lcdMessage_;
        status.lcdMessageShown = lcdMessageShown_;
        status.lcdMessageSerial = lcdMessageSerial_;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    status.open = synth_ != nullptr;
    status.controlRomName = controlRomName_;
    status.pcmRomName = pcmRomName_;
    status.analogMode = analogModeInUse_;
    status.outputSampleRate = synth_ ? config_.outputSampleRate : 0;
    status.midiEvents = midiEvents_;
    status.queueOverflows = queueOverflows_.load();

    const uint32_t now = synth_ ? synth_->getInternalRenderedSampleCount() : 0;
    status.player.state = player_.state();
    status.player.name = player_.name();
    status.player.queuedName = player_.queuedName();
    status.player.position = player_.position(now);
    status.player.duration = player_.duration();
    status.player.loop = player_.loop();

    if (!synth_) {
        for (PartStatus& part : status.parts) part = PartStatus();
        status.partialCount = 0;
        status.partialStates.clear();
        status.d110 = false;
        status.mt32Translation = false;
        status.mt32PresetBanks = false;
        status.altTones = false;
        status.mt32PresetRom = mt32Presets_ ? mt32Presets_->description : std::string();
        return;
    }

    status.d110 = synth_->isD110Mode();
    status.currentPatch = synth_->getCurrentPatch();
    status.performanceMode = synth_->isPerformanceMode();
    status.cardInserted = synth_->isCardInserted();
    status.mt32Translation = translatingLocked();
    status.mt32PresetBanks = status.mt32Translation && translator_.presetBanks();
    status.altTones = synth_->hasAltTimbres();
    status.mt32PresetRom = mt32Presets_ ? mt32Presets_->description : std::string();
    status.performanceChannel = synth_->getPerformanceChannel();
    status.currentPerformance = synth_->getCurrentPerformance();
    if (status.d110) synth_->readMemory(packAddress(0x030400), sizeof(status.performanceTemp), status.performanceTemp);
    uint8_t system[33] = {};
    synth_->readMemory(kSystemAddress, status.d110 ? 33 : kSystemSize, system);
    std::memcpy(status.patchName, &system[0x17], 10);
    status.patchName[10] = 0;
    status.masterTune = system[kSystemMasterTune];
    status.gsMasterTune = synth_->getGSMasterTune();
    status.midiExtensions = synth_->isMIDIExtensionsEnabled();
    status.reverbMode = system[kSystemReverbMode];
    status.reverbTime = system[kSystemReverbMode + 1];
    status.reverbLevel = system[kSystemReverbMode + 2];
    status.masterVolume = system[kSystemMasterVolume];

    status.partCount = synth_->getPartCount();
    uint8_t patchTemp[kPatchTempSize * kMaxPartCount] = {};
    synth_->readMemory(kPatchTempAddress, kPatchTempSize * kBasePartCount, patchTemp);
    uint8_t extraSystem[kExtraSystemSize] = {};
    uint8_t finePans[2 * kMaxPartCount] = {};
    if (status.d110) synth_->readMemory(kPartFinePanAddress, 2 * status.partCount, finePans);
    if (status.partCount > uint32_t(kBasePartCount)) {
        synth_->readMemory(packAddress(0x130000), kPatchTempSize * kExtraParts, &patchTemp[kPatchTempSize * kBasePartCount]);
        synth_->readMemory(kExtraSystemAddress, sizeof(extraSystem), extraSystem);
    }

    for (int i = 0; i < kMaxPartCount; i++) {
        PartStatus& part = status.parts[i];
        if (i >= int(status.partCount)) {
            part = PartStatus();
            continue;
        }
        std::memcpy(part.temp, &patchTemp[i * kPatchTempSize], kPatchTempSize);
        const int fineValue = finePans[2 * i] * 128 + finePans[2 * i + 1];
        part.finePanSet = fineValue >= 1 && fineValue <= 129;
        part.finePan = part.finePanSet ? fineValue - 65 : panpotToFinePan(part.temp[TimbreTemp::Panpot]);
        part.channel = i < kBasePartCount ? system[kSystemChannels + i] : extraSystem[kExtraChannels + (i - kBasePartCount)];
        part.reserve = i < kBasePartCount ? system[0x04 + i] : extraSystem[i - kBasePartCount];
        part.reservedPartials = synth_->getPartReservedPartialCount(uint8_t(i));
        part.activePartials = synth_->getPartActivePartialCount(uint8_t(i));
        part.program = synth_->getPartProgram(uint8_t(i));
        part.programFromCard = synth_->isPartProgramFromCard(uint8_t(i));
        part.programFromAlt = synth_->isPartProgramFromAlt(uint8_t(i));
        part.midiVolume = synth_->getPartMIDIVolume(uint8_t(i));
        part.expression = synth_->getPartExpression(uint8_t(i));
        part.bendRangeCents = synth_->getPartPitchBendRangeCents(uint8_t(i));
        part.active = synth_->isPartActive(uint8_t(i));
        const char* name = i == kRhythmPart ? "Rhythm" : synth_->getPatchName(uint8_t(i));
        std::snprintf(part.name, sizeof(part.name), "%s", name != nullptr ? name : "");
        if (i == kRhythmPart) {
            part.tone[0] = 0;
        } else {
            const uint8_t group = patchTemp[i * kPatchTempSize] & 3;
            const uint8_t number = patchTemp[i * kPatchTempSize + 1] & 63;
            const bool cardTone = group == 2 && patchTemp[i * kPatchTempSize + 7] == MT32Emu::PART_CARD_TONES;
            const bool altTone = group < 2 && patchTemp[i * kPatchTempSize + 7] == MT32Emu::PART_ALT_TONES && status.altTones;
            part.tone[0] = cardTone ? 'c' : altTone ? "de"[group] : "abir"[group];
            part.tone[1] = char('1' + number / 8);
            part.tone[2] = char('1' + number % 8);
            part.tone[3] = 0;
        }
        part.noteCount = synth_->getPlayingNotes(uint8_t(i), part.keys, part.velocities);
    }
    status.partialCount = synth_->getPartialCount();
    status.partialStates.resize(status.partialCount);
    synth_->getPartialStates(status.partialStates.data());
}

std::vector<std::string> SynthEngine::takeLog() {
    std::lock_guard<std::mutex> lock(logMutex_);
    std::vector<std::string> lines;
    lines.swap(log_);
    return lines;
}

void SynthEngine::log(const std::string& line) {
    std::lock_guard<std::mutex> lock(logMutex_);
    if (log_.size() < 1000) log_.push_back(line);
}

void SynthEngine::printDebug(const char* fmt, va_list list) {
    char buffer[1024];
    std::vsnprintf(buffer, sizeof(buffer), fmt, list);
    log(buffer);
}

void SynthEngine::showLCDMessage(const char* message) {
    std::lock_guard<std::mutex> lock(logMutex_);
    lcdMessage_ = message;
    lcdMessageShown_ = true;
    lcdMessageSerial_++;
    if (log_.size() < 1000) log_.push_back(std::string("Display: ") + message);
}

void SynthEngine::onDisplayReset() {
    std::lock_guard<std::mutex> lock(logMutex_);
    lcdMessageShown_ = false;
}

void SynthEngine::onPatchRecalled(MT32Emu::Bit8u patchNum) {
    char line[48];
    std::snprintf(line, sizeof(line), "Patch I-%d%d recalled", patchNum / 8 + 1, patchNum % 8 + 1);
    log(line);
}

void SynthEngine::onWriteRequestResult(MT32Emu::Bit8u result) {
    static const char* const results[] = {"completed", "card not ready", "write protected", "incorrect mode"};
    log(std::string("Write request: ") + results[result < 4 ? result : 3]);
}

void SynthEngine::onErrorControlROM() {
    log("Control ROM error");
}

void SynthEngine::onErrorPCMROM() {
    log("PCM ROM error");
}

bool SynthEngine::onMIDIQueueOverflow() {
    queueOverflows_++;
    return false;
}

void SynthEngine::onDeviceReset() {
    log("Synth reset by SysEx");
}
