// vsttest: D110Emu's VST2 plugin driven the way a host drives it, without a DAW. VSTPluginMain is linked in, and the
// test calls the dispatcher, effProcessEvents and processReplacing as a host does. Needs the D-110 ROMs, found as the
// plugin finds them (a "roms" folder next to this program or above it). Prints PASS/FAIL; exit code 0 when all pass.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "RolandSysex.h"
#include "Settings.h"
#include "TestDataFolder.h"
#include "Vst2.h"
#include "VstPlugin.h"

extern "C" vst2::AEffect* VSTPluginMain(vst2::HostCallback host);

namespace {

int g_failures = 0;
std::atomic<int> g_sizeRequests{0};

void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) g_failures++;
}

intptr_t VST2_CALL hostCallback(vst2::AEffect* /*effect*/, int32_t opcode, int32_t /*index*/, intptr_t /*value*/, void* /*ptr*/,
                                float /*opt*/) {
    switch (opcode) {
    case vst2::audioMasterVersion:
        return vst2::kVstVersion;
    case vst2::audioMasterSizeWindow:
        g_sizeRequests++;
        return 1;
    default:
        return 0;
    }
}

// A MIDI message (up to 3 bytes) or a SysEx message (F0 ... F7) at a frame of a block.
struct Event {
    int32_t frame;
    std::vector<uint8_t> bytes;
};

std::vector<uint8_t> dataSet(uint32_t sysexAddress, const std::vector<uint8_t>& data) {
    return RolandSysex::dataSet(0x10, RolandSysex::pack(sysexAddress), data.data(), data.size());  // Unit 17
}

// Audio as a host collects it.
struct Audio {
    std::vector<float> left;
    std::vector<float> right;
    size_t size() const { return left.size(); }
    // The first frame at or after `from` that is not silent, or -1.
    long onset(size_t from = 0) const {
        for (size_t i = from; i < left.size(); i++) {
            if (std::fabs(left[i]) + std::fabs(right[i]) > 1e-5f) return long(i);
        }
        return -1;
    }
    double peak(size_t from = 0) const {
        double level = 0.0;
        for (size_t i = from; i < left.size(); i++) level = std::max(level, double(std::max(std::fabs(left[i]), std::fabs(right[i]))));
        return level;
    }
};

class Host {
public:
    explicit Host(float sampleRate = 48000.0f, int32_t blockSize = 512, const std::vector<uint8_t>* state = nullptr) : blockSize_(blockSize) {
        effect_ = VSTPluginMain(&hostCallback);
        call(vst2::effOpen);
        call(vst2::effSetSampleRate, 0, 0, nullptr, sampleRate);
        call(vst2::effSetBlockSize, 0, blockSize);
        if (state != nullptr) setState(*state);  // A project loading: its state before the plugin resumes
        call(vst2::effMainsChanged, 0, 1);
    }
    ~Host() {
        call(vst2::effMainsChanged, 0, 0);
        call(vst2::effClose);
    }
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    vst2::AEffect* effect() { return effect_; }
    intptr_t call(int32_t opcode, int32_t index = 0, intptr_t value = 0, void* ptr = nullptr, float opt = 0.0f) {
        return effect_->dispatcher(effect_, opcode, index, value, ptr, opt);
    }
    std::string text(int32_t opcode) {
        char buffer[256] = {};
        call(opcode, 0, 0, buffer);
        return buffer;
    }

    // One block: its events through effProcessEvents, then processReplacing (or the accumulating process).
    void block(Audio& audio, const std::vector<Event>& events = {}, int32_t frames = 0, bool accumulate = false, float bias = 0.0f) {
        if (frames == 0) frames = blockSize_;
        std::vector<vst2::VstMidiEvent> midi(events.size());
        std::vector<vst2::VstMidiSysexEvent> sysex(events.size());
        std::vector<vst2::VstEvent*> pointers;
        for (size_t i = 0; i < events.size(); i++) {
            const Event& event = events[i];
            if (event.bytes.size() <= 3 && event.bytes[0] != 0xF0) {
                vst2::VstMidiEvent& m = midi[i];
                m.type = vst2::kVstMidiType;
                m.byteSize = int32_t(sizeof(m));
                m.deltaFrames = event.frame;
                for (size_t b = 0; b < event.bytes.size() && b < 3; b++) m.midiData[b] = char(event.bytes[b]);
                pointers.push_back(reinterpret_cast<vst2::VstEvent*>(&m));
            } else {
                vst2::VstMidiSysexEvent& s = sysex[i];
                s.type = vst2::kVstSysExType;
                s.byteSize = int32_t(sizeof(s));
                s.deltaFrames = event.frame;
                s.dumpBytes = int32_t(event.bytes.size());
                s.sysexDump = const_cast<char*>(reinterpret_cast<const char*>(event.bytes.data()));
                pointers.push_back(reinterpret_cast<vst2::VstEvent*>(&s));
            }
        }
        if (!pointers.empty()) {
            // VstEvents with as many pointers as there are events.
            std::vector<uint8_t> storage(offsetof(vst2::VstEvents, events) + pointers.size() * sizeof(vst2::VstEvent*));
            const int32_t count = int32_t(pointers.size());
            std::memcpy(storage.data() + offsetof(vst2::VstEvents, numEvents), &count, sizeof(count));
            std::memcpy(storage.data() + offsetof(vst2::VstEvents, events), pointers.data(), pointers.size() * sizeof(vst2::VstEvent*));
            call(vst2::effProcessEvents, 0, 0, storage.data());
        }
        // All the outputs: the mix, a pair per part, then MULTI 1-6.
        std::vector<std::vector<float>> channels(size_t(effect_->numOutputs), std::vector<float>(size_t(frames), bias));
        std::vector<float*> outputs;
        for (std::vector<float>& channel : channels) outputs.push_back(channel.data());
        if (accumulate) {
            effect_->process(effect_, nullptr, outputs.data(), frames);
        } else {
            effect_->processReplacing(effect_, nullptr, outputs.data(), frames);
        }
        audio.left.insert(audio.left.end(), channels[0].begin(), channels[0].end());
        audio.right.insert(audio.right.end(), channels[1].begin(), channels[1].end());
        taken.resize(channels.size());
        for (size_t c = 0; c < channels.size(); c++) taken[c].insert(taken[c].end(), channels[c].begin(), channels[c].end());
    }
    // RMS of an output (0-1 the mix, a pair per part, then MULTI 1-6 at 34-39) over what the blocks rendered.
    double level(size_t output) const {
        if (output >= taken.size() || taken[output].empty()) return 0.0;
        double sum = 0.0;
        for (float sample : taken[output]) sum += double(sample) * sample;
        return std::sqrt(sum / double(taken[output].size()));
    }
    std::vector<std::vector<float>> taken;
    void silence(Audio& audio, int blocks) {
        for (int i = 0; i < blocks; i++) block(audio);
    }

    std::vector<uint8_t> state(int32_t index = 0) {
        void* data = nullptr;
        const intptr_t size = call(vst2::effGetChunk, index, 0, &data);
        if (size <= 0 || data == nullptr) return {};
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        return std::vector<uint8_t>(bytes, bytes + size);
    }
    bool setState(const std::vector<uint8_t>& state, int32_t index = 0) {
        return call(vst2::effSetChunk, index, intptr_t(state.size()), const_cast<uint8_t*>(state.data())) != 0;
    }

private:
    vst2::AEffect* effect_ = nullptr;
    int32_t blockSize_;
};

bool contains(const std::vector<uint8_t>& data, const std::string& text) {
    return std::search(data.begin(), data.end(), text.begin(), text.end()) != data.end();
}

const std::vector<uint8_t> kNoteOn = {0x90, 60, 100};    // Part 1 on the D-110's channel 1
const std::vector<uint8_t> kNoteOnPart2 = {0x91, 64, 100};

// Frames from a note's event to its first sound, the note at `delta` in a block of `blockSize` after `before` blocks.
long noteLatency(int32_t delta, int32_t blockSize, int before) {
    Host host(48000.0f, blockSize);
    Audio audio;
    host.silence(audio, before);
    const size_t start = audio.size();
    host.block(audio, {{delta, kNoteOn}});
    host.block(audio);
    const long at = audio.onset();
    return at < 0 ? -1000000 : at - long(start) - delta;
}

void testIdentity() {
    Host host;
    vst2::AEffect* effect = host.effect();
    check(effect->magic == vst2::kEffectMagic, "The AEffect has the VST magic number");
    check((effect->flags & vst2::effFlagsIsSynth) != 0 && (effect->flags & vst2::effFlagsCanReplacing) != 0 &&
              (effect->flags & vst2::effFlagsProgramChunks) != 0,
          "It is an instrument with processReplacing and its state in chunks");
    check(effect->numInputs == 0 && effect->numOutputs == 40 && effect->numParams == 0 && effect->numPrograms == 1,
          "No inputs, 40 outputs (the mix, a pair per part and MULTI 1-6), no parameters, one program");
    check(effect->uniqueID == int32_t((uint32_t('D') << 24) | (uint32_t('1') << 16) | (uint32_t('1') << 8) | uint32_t('m')),
          "Its unique ID is 'D11m'");
    check(effect->processReplacing != nullptr && effect->process != nullptr && effect->object != nullptr, "Its functions are set");
    check(host.text(vst2::effGetEffectName) == "D110Emu" && host.text(vst2::effGetVendorString) == "D110Emu" &&
              host.text(vst2::effGetProductString) == "D110Emu - Roland D-110 emulator",
          "Its names: " + host.text(vst2::effGetEffectName) + ", " + host.text(vst2::effGetProductString));
    check(host.call(vst2::effGetPlugCategory) == vst2::kPlugCategSynth && host.call(vst2::effGetVstVersion) == 2400,
          "Category synth, VST 2.4");
    check(host.call(vst2::effCanDo, 0, 0, const_cast<char*>("receiveVstEvents")) == 1 &&
              host.call(vst2::effCanDo, 0, 0, const_cast<char*>("receiveVstMidiEvent")) == 1 &&
              host.call(vst2::effCanDo, 0, 0, const_cast<char*>("sendVstEvents")) == -1 &&
              host.call(vst2::effCanDo, 0, 0, const_cast<char*>("somethingElse")) == 0,
          "It takes MIDI events and sends none");
    check(host.call(vst2::effGetNumMidiInputChannels) == 16, "16 MIDI input channels");
    vst2::VstPinProperties pin = {};
    std::vector<std::string> labels;
    bool pairs = true;
    bool mono = true;
    std::string shortLabels;
    for (int32_t i = 0; i < 40; i++) {
        pin = {};
        const bool answered = host.call(vst2::effGetOutputProperties, i, 0, &pin) == 1;
        if (i < 34) {
            pairs = pairs && answered && ((pin.flags & vst2::kVstPinIsStereo) != 0) == (i % 2 == 0) && pin.arrangementType == 1;
        } else {
            mono = mono && answered && pin.flags == vst2::kVstPinIsActive && pin.arrangementType == 0;
            shortLabels += std::string(shortLabels.empty() ? "" : " ") + pin.shortLabel;
        }
        labels.push_back(pin.label);
    }
    std::printf("      outputs: %s, %s, %s, %s ... %s, %s ... %s, %s ... %s (%s)\n", labels[0].c_str(), labels[1].c_str(), labels[2].c_str(),
                labels[3].c_str(), labels[18].c_str(), labels[20].c_str(), labels[33].c_str(), labels[34].c_str(), labels[39].c_str(),
                shortLabels.c_str());
    check(pairs && labels[0] == "D-110 L" && labels[2] == "Part 1 L" && labels[17] == "Part 8 R" && labels[18] == "Rhythm L" &&
              labels[20] == "Part 9 L" && labels[33] == "Part 15 R",
          "Its outputs: stereo pairs for the mix, parts 1-8, rhythm and parts 9-15");
    check(mono && labels[34] == "Multi 1" && labels[39] == "Multi 6" && shortLabels == "M1 M2 M3 M4 M5 M6" &&
              host.call(vst2::effGetOutputProperties, 40, 0, &pin) == 0,
          "Then MULTI 1-6, mono");
    char name[vst2::kVstMaxProgNameLen] = {};
    host.call(vst2::effGetProgramName, 0, 0, name);
    check(std::string(name) == "D-110", "Its program is named");
    check(host.call(1234) == 0, "Unknown opcodes answer 0");
#if !defined(_WIN32)
    check((effect->flags & vst2::effFlagsHasEditor) == 0 && host.call(vst2::effEditOpen) == 0, "No editor in this build");
#endif
}

void testTiming() {
    // A note at frame 50 and one at frame 300 of a block sound 250 frames apart; so do notes in a long block, which the
    // plugin renders in pieces of 4096 frames.
    const long early = noteLatency(50, 512, 20);
    const long late = noteLatency(300, 512, 20);
    const long longBlock = noteLatency(9000, 10000, 2);
    const long firstFrame = noteLatency(0, 256, 40);
    std::printf("      note latency: %ld frames at frame 50, %ld at 300, %ld at 9000 of 10000, %ld at 0\n", early, late, longBlock, firstFrame);
    check(early >= 0 && early < 128, "A note sounds within 128 frames of its event");
    check(std::labs(late - early) <= 2, "MIDI plays at its frame within the block, to 2 frames");
    check(std::labs(longBlock - early) <= 2, "... also in blocks longer than 4096 frames");
    check(std::labs(firstFrame - early) <= 2, "... and at the first frame");
}

void testState() {
    // SysEx through the host reaches the memory, which the project's state keeps.
    Host a;
    Audio audio;
    a.silence(audio, 4);
    const std::string toneName = "VST Test  ";
    a.block(audio, {{100, dataSet(0x080000, std::vector<uint8_t>(toneName.begin(), toneName.end()))},  // Tone i11's name
                    {200, dataSet(0x10000D, {0x10})}});  // Part 1's MIDI channel off (system area)
    a.silence(audio, 4);
    const std::vector<uint8_t> state = a.state();
    std::string settings;
    std::vector<uint8_t> memory;
    check(VstPlugin::decodeState(state.data(), state.size(), settings, memory), "The project state decodes");
    std::printf("      project state: %zu bytes (settings %zu, memory %zu)\n", state.size(), settings.size(), memory.size());
    check(contains(memory, "VST Test"), "SysEx from the host is in the memory the project keeps");
    check(settings.find("unit_number = 17") != std::string::npos, "The settings are in the state");

    std::vector<uint8_t> programState = a.state(1);
    check(programState.size() > 8 && std::memcmp(programState.data(), state.data(), 4) == 0, "The program chunk has the same format");

    // Channel 1 has no part now.
    const size_t start = audio.size();
    a.block(audio, {{0, kNoteOn}});
    a.silence(audio, 30);
    check(audio.peak(start) == 0.0, "SysEx from the host reaches the synth (part 1's channel off)");

    // Another instance, loaded with that state, as a host loads a project.
    Host b(48000.0f, 512, &state);
    Audio other;
    b.silence(other, 4);
    b.block(other, {{0, kNoteOn}});
    b.silence(other, 30);
    check(other.peak() == 0.0, "The project's system settings come back (part 1's channel)");
    const std::vector<uint8_t> again = b.state();
    std::string settingsAgain;
    std::vector<uint8_t> memoryAgain;
    check(VstPlugin::decodeState(again.data(), again.size(), settingsAgain, memoryAgain) && memoryAgain == memory,
          "The memory comes back unchanged");
    check(settingsAgain == settings, "The settings come back unchanged");

    // Two fresh instances, one loaded from the other's state, play the same notes alike, sample for sample. Both have
    // rendered as much before the notes: the synth runs at 32 kHz, and where its samples fall between the host's
    // (48 kHz) depends on how many frames have gone before, modulo 3. And both start from the same rand() seed: munt
    // varies its pitch envelopes' timer with rand(), as the real unit's timer varies.
    Host first;
    Audio setup;
    first.block(setup, {{10, dataSet(0x030019, {0})}});  // Part 2's panpot hard left
    first.silence(setup, 4);
    const std::vector<uint8_t> firstState = first.state();
    Host second(48000.0f, 512, &firstState);
    second.silence(setup, 5);
    Audio fromFirst;
    Audio fromSecond;
    for (Host* host : {&first, &second}) {
        Audio& out = host == &first ? fromFirst : fromSecond;
        host->silence(out, 8);
        std::srand(110);
        host->block(out, {{17, kNoteOnPart2}, {17, {0x91, 67, 90}}});
        host->silence(out, 10);
        host->block(out, {{400, {0x81, 64, 0}}, {401, {0x81, 67, 0}}});
        host->silence(out, 40);
    }
    bool same = fromFirst.size() == fromSecond.size() && fromFirst.peak() > 0.01;
    for (size_t i = 0; same && i < fromFirst.size(); i++) {
        same = fromFirst.left[i] == fromSecond.left[i] && fromFirst.right[i] == fromSecond.right[i];
    }
    if (!same && fromFirst.size() == fromSecond.size()) {
        double largest = 0.0;
        long firstDifference = -1;
        for (size_t i = 0; i < fromFirst.size(); i++) {
            const double difference = std::max(std::fabs(fromFirst.left[i] - fromSecond.left[i]), std::fabs(fromFirst.right[i] - fromSecond.right[i]));
            if (difference > 0.0 && firstDifference < 0) firstDifference = long(i);
            largest = std::max(largest, difference);
        }
        std::printf("      first difference at frame %ld (the notes start at %d), largest %g, onsets %ld / %ld\n", firstDifference, 8 * 512 + 17,
                    largest, fromFirst.onset(), fromSecond.onset());
    }
    check(same, "An instance loaded from a project plays exactly as the one it was saved from");
    double leftPeak = 0.0;
    double rightPeak = 0.0;
    for (size_t i = 0; i < fromSecond.size(); i++) {
        leftPeak = std::max(leftPeak, double(std::fabs(fromSecond.left[i])));
        rightPeak = std::max(rightPeak, double(std::fabs(fromSecond.right[i])));
    }
    std::printf("      part 2 panned left: peak %.3f left, %.3f right\n", leftPeak, rightPeak);
    check(rightPeak < 0.25 * leftPeak, "... with the pan set over SysEx");

    // A state that is not D110Emu's is refused, and the instance goes on.
    check(!b.setState({1, 2, 3, 4, 5, 6, 7, 8, 9}), "A foreign state is refused");
    Audio after;
    b.silence(after, 2);
    b.block(after, {{0, kNoteOnPart2}});
    b.silence(after, 10);
    check(after.peak() > 0.01, "... and the instance keeps playing");
}

void testSettings() {
    // A project with MT-32 translation on: parts 1-8 answer MIDI channels 2-9, so channel 1 has no part.
    Settings settings;
    settings.set("mt32_translation", true);
    settings.set("rom_folder", "/a/folder/on/another/computer");
    const std::vector<uint8_t> state = VstPlugin::encodeState(settings.text(), {});
    Host host(48000.0f, 512, &state);
    Audio audio;
    host.silence(audio, 4);
    host.block(audio, {{0, kNoteOn}});
    host.silence(audio, 30);
    const double channel1 = audio.peak();
    const size_t start = audio.size();
    host.block(audio, {{0, {0x91, 60, 100}}});
    host.silence(audio, 30);
    check(channel1 == 0.0 && audio.peak(start) > 0.01, "A project's settings apply (MT-32 translation: channel 2 plays part 1)");
    check(audio.peak(start) > 0.01, "A project from another computer finds this one's ROMs");
    std::string text;
    std::vector<uint8_t> memory;
    const std::vector<uint8_t> saved = host.state();
    check(VstPlugin::decodeState(saved.data(), saved.size(), text, memory) && text.find("mt32_translation = true") != std::string::npos,
          "... and stay in its state");

    // The user's own LCD colours ("#RRGGBB" in the settings) come back as they were.
    Settings lcd;
    lcd.set("lcd_scheme", "custom");
    lcd.set("lcd_custom_glass", "#1E3A5F");
    lcd.set("lcd_custom_dot_off", "#27496f");
    lcd.set("lcd_custom_dot_on", "#FFC24A");
    const std::vector<uint8_t> lcdState = VstPlugin::encodeState(lcd.text(), {});
    Host lcdHost(48000.0f, 512, &lcdState);
    const std::vector<uint8_t> lcdSaved = lcdHost.state();
    std::string lcdText;
    std::vector<uint8_t> lcdMemory;
    check(VstPlugin::decodeState(lcdSaved.data(), lcdSaved.size(), lcdText, lcdMemory) &&
              lcdText.find("lcd_scheme = custom\n") != std::string::npos && lcdText.find("lcd_custom_glass = #1E3A5F\n") != std::string::npos &&
              lcdText.find("lcd_custom_dot_off = #27496F\n") != std::string::npos &&
              lcdText.find("lcd_custom_dot_on = #FFC24A\n") != std::string::npos,
          "The user's own LCD colours come back from a project");
}

void testSampleRate() {
    // The host changes its rate: the synth restarts at the new one and keeps its memory.
    Host host(48000.0f, 512);
    Audio audio;
    const std::string toneName = "Rate Test ";
    host.block(audio, {{0, dataSet(0x080000, std::vector<uint8_t>(toneName.begin(), toneName.end()))}});
    host.silence(audio, 2);
    host.call(vst2::effMainsChanged, 0, 0);
    host.call(vst2::effSetSampleRate, 0, 0, nullptr, 44100.0f);
    host.call(vst2::effMainsChanged, 0, 1);
    Audio after;
    host.silence(after, 4);
    host.block(after, {{0, kNoteOn}});
    host.silence(after, 20);
    check(after.peak() > 0.01, "It plays after a change of sample rate");
    std::string settings;
    std::vector<uint8_t> memory;
    const std::vector<uint8_t> state = host.state();
    check(VstPlugin::decodeState(state.data(), state.size(), settings, memory) && contains(memory, "Rate Test"),
          "... with its memory");
}

// Part 1 on its own output (the project's own_outputs, which the Output menus set) plays out of outputs 3-4, not the mix.
void testPartOutputs() {
    const std::vector<uint8_t> own = VstPlugin::encodeState("own_outputs = 1\n", {});
    const auto play = [](Host& host) {
        Audio audio;
        host.block(audio, {{0, dataSet(0x030006, {0})}});  // Part 1's output assign: Mix (dry)
        host.block(audio, {{100, {0x90, 60, 100}}});
        for (int i = 0; i < 40; i++) host.block(audio);
    };
    Host routed(48000.0f, 512, &own);
    play(routed);
    Host plain;
    play(plain);
    std::printf("      part 1 on its own output: outputs 3-4 %.4f, mix %.6f; in the mix: mix %.4f, outputs 3-4 %.6f\n",
                routed.level(2) + routed.level(3), routed.level(0) + routed.level(1), plain.level(0) + plain.level(1),
                plain.level(2) + plain.level(3));
    check(routed.level(2) > 0.001 && routed.level(0) < 1e-6 && plain.level(0) > 0.001 && plain.level(2) == 0.0,
          "A part on its own output plays out of its pair of outputs, not in the mix");

    // MULTI 1-6: part 1 on MULTI 3 (the power-on reverb is on), the bass drum on MULTI 6; part 2 in the mix.
    Host multi;
    Audio audio;
    multi.block(audio, {{0, dataSet(0x030006, {4})}, {0, dataSet(0x030143, {7})}});
    multi.block(audio, {{100, {0x90, 60, 100}}, {100, kNoteOnPart2}, {100, {0x99, 36, 100}}});
    for (int i = 0; i < 40; i++) multi.block(audio);
    std::string levels;
    bool othersSilent = true;
    for (int n = 0; n < 6; n++) {
        char text[32];
        std::snprintf(text, sizeof(text), "%s%.4f", n == 0 ? "" : " ", multi.level(size_t(34 + n)));
        levels += text;
        if (n != 2 && n != 5) othersSilent = othersSilent && multi.level(size_t(34 + n)) == 0.0;
    }
    // Part 1 alone on MULTI 3, as the reference for its level (the other notes shift munt's rand()-driven envelope timing
    // a little, so not sample for sample).
    Host alone;
    Audio aloneAudio;
    alone.block(aloneAudio, {{0, dataSet(0x030006, {4})}});
    alone.block(aloneAudio, {{100, {0x90, 60, 100}}});
    for (int i = 0; i < 40; i++) alone.block(aloneAudio);
    double diff = 0.0, reference = 0.0;
    for (size_t i = 0; i < multi.taken[36].size() && i < alone.taken[36].size(); i++) {
        diff = std::max(diff, std::fabs(double(multi.taken[36][i]) - alone.taken[36][i]));
        reference = std::max(reference, std::fabs(double(alone.taken[36][i])));
    }
    std::printf("      MULTI 1-6: %s; the mix %.4f (part 2); part 1 alone on MULTI 3: %.4f, off by at most %.6f of %.4f\n", levels.c_str(),
                multi.level(0) + multi.level(1), alone.level(36), diff, reference);
    check(multi.level(36) > 0.001 && multi.level(39) > 0.001 && othersSilent && multi.level(0) > 0.001 &&
              std::fabs(multi.level(36) - alone.level(36)) < 0.01 * alone.level(36) && diff < 0.01 * reference,
          "Notes on MULTI 3 and 6 (a rhythm key's, with reverb on) play out of those outputs, as part 1 alone; the others "
          "stay silent");
}

// MULTI 1-6 as three stereo pairs, the computer's choice for new instances (d110emu-vst.ini, which the configuration window
// sets): the same 40 outputs, the last six paired and named Multi 1+2 L/R to Multi 5+6 L/R; part 1 on MULTI 3 still on
// output 37.
void testMultiPairs() {
    setTestPluginSetting("multi_output_pairs", "true");
    {
        Host host;
        vst2::VstPinProperties pin = {};
        bool paired = host.effect()->numOutputs == 40;
        std::string labels;
        std::string shortLabels;
        for (int32_t i = 34; i < 40; i++) {
            pin = {};
            paired = paired && host.call(vst2::effGetOutputProperties, i, 0, &pin) == 1 &&
                     ((pin.flags & vst2::kVstPinIsStereo) != 0) == (i % 2 == 0) && pin.arrangementType == 1;
            labels += std::string(labels.empty() ? "" : ", ") + pin.label;
            shortLabels += std::string(shortLabels.empty() ? "" : " ") + pin.shortLabel;
        }
        Audio audio;
        // Part 1 on MULTI 4, panned hard left: on the left of the second pair (as a mono output, MULTI 4 is output 38).
        host.block(audio, {{0, dataSet(0x030006, {5})}, {0, dataSet(0x030009, {0})}});
        host.block(audio, {{100, {0x90, 60, 100}}});
        for (int i = 0; i < 40; i++) host.block(audio);
        std::printf("      %s (%s); part 1 on MULTI 4, panned left: output 37 %.4f, 38 %.6f\n", labels.c_str(), shortLabels.c_str(),
                    host.level(36), host.level(37));
        check(paired && labels == "Multi 1+2 L, Multi 1+2 R, Multi 3+4 L, Multi 3+4 R, Multi 5+6 L, Multi 5+6 R" &&
                  shortLabels == "M1+2L M1+2R M3+4L M3+4R M5+6L M5+6R" && host.level(36) > 0.001 && host.level(37) == 0.0,
              "MULTI outputs as stereo pairs (the computer's choice): outputs 35-40 paired as Multi 1+2, 3+4 and 5+6, where "
              "notes play with their pan (part 1 on MULTI 4, panned left, on the left of the second pair)");
    }
    setTestPluginSetting("multi_output_pairs", "false");
}

void testProcess() {
    // The old accumulating process adds to what the outputs hold.
    Host host;
    Audio audio;
    host.silence(audio, 4);
    Audio added;
    host.block(added, {}, 0, true, 0.25f);
    bool kept = true;
    for (size_t i = 0; i < added.size(); i++) kept = kept && added.left[i] == 0.25f && added.right[i] == 0.25f;
    check(kept, "process() adds to the outputs");
    // SysEx without its F0 or F7, as some hosts pass it, still arrives.
    const std::string toneName = "No Framing";
    std::vector<uint8_t> message = dataSet(0x080000, std::vector<uint8_t>(toneName.begin(), toneName.end()));
    message.pop_back();
    message.erase(message.begin());
    Audio more;
    host.block(more, {{3, message}});
    std::string settings;
    std::vector<uint8_t> memory;
    const std::vector<uint8_t> state = host.state();
    check(VstPlugin::decodeState(state.data(), state.size(), settings, memory) && contains(memory, "No Framing"),
          "SysEx without F0 and F7 arrives");
}

void testThreads() {
    // A host loads a project while its audio thread plays: the App is replaced between blocks.
    Host source;
    const std::vector<uint8_t> state = source.state();
    Host host;
    std::atomic<bool> stop{false};
    std::atomic<int> blocks{0};
    std::thread audioThread([&] {
        Audio audio;
        while (!stop) {
            host.block(audio, {{0, kNoteOnPart2}, {200, {0x81, 64, 0}}});
            blocks++;
            if (audio.size() > size_t(48000) * 4) {
                audio.left.clear();
                audio.right.clear();
            }
        }
    });
    for (int i = 0; i < 3; i++) {
        host.setState(state);
        host.state();
    }
    stop = true;
    audioThread.join();
    check(blocks > 0, "Projects load while the audio thread plays (" + std::to_string(blocks.load()) + " blocks meanwhile)");
}

}  // namespace

int main() {
    useTestDataFolder("d110emu-vsttest", true);  // Not the user's own plugin settings
    testIdentity();
    testTiming();
    testState();
    testSettings();
    testSampleRate();
    testPartOutputs();
    testMultiPairs();
    testProcess();
    testThreads();
    std::printf("%s: %d failure%s\n", g_failures == 0 ? "ALL PASS" : "FAILED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
