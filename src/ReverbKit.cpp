#include "ReverbKit.h"

#include <algorithm>
#include <cstdio>
#include <fstream>

#include "Platform.h"
#include "RolandSysex.h"
#include "ToneModel.h"

namespace {

constexpr uint8_t kDevice = 0x10;         // Unit number 17
constexpr int kBurstKey = 60;
constexpr double kSetupTime = 3.0;        // The setup messages go out before this
constexpr double kBurstSlot = 6.0;        // Parameter change, 1 s, burst, then 5 s for the tail
constexpr double kPhraseSlot = 5.0;
constexpr double kChangeLead = 1.0;       // Settings are sent this long before the sound
constexpr int kPhraseTime = 5, kPhraseLevel = 5;
constexpr int kSweepType = 2, kSweepTime = 4;  // The level sweep: Medium Hall, Reverb Time 4

// The burst: partial 1 alone, the D-series' Noise (Loop) (b1-111) at full level for a few milliseconds, then off.
Tone::Data burstTone() {
    Tone::Data tone{};
    const char name[] = "Burst     ";
    std::copy(name, name + 10, tone.begin());
    tone[Tone::Common::Structure12] = 5;  // Structure 6: PCM + PCM
    tone[Tone::Common::Structure34] = 5;
    tone[Tone::Common::PartialMute] = 1;  // Partial 1 only
    tone[Tone::Common::EnvMode] = 0;
    static const uint8_t partial[Tone::kPartialSize] = {
        36, 50, 11, 1, 0, 110, 0, 7,                                     // WG: key follow 1, bank 1, Noise (Loop)
        0, 0, 0, 0, 0, 0, 0, 50, 50, 50, 50, 50,                         // Pitch envelope: flat
        0, 0, 0,                                                         // LFO off
        100, 0, 11, 0, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 100, 100, 100, 100,  // TVF (PCM partials have none)
        100, 50, 0, 12, 0, 12, 0, 0,                                     // TVA: full level, no velocity or bias
        0, 18, 0, 0, 0,                                                  // T1-T5: instant attack, a short hold, then off
        100, 100, 0, 0,                                                  // L1-L3, sustain
    };
    for (int p = 0; p < 4; p++) std::copy(partial, partial + Tone::kPartialSize, tone.begin() + Tone::partialBase(p));
    return tone;
}

class KitBuilder {
public:
    explicit KitBuilder(ReverbKitTarget target) : target_(target), smf_(new SmfFile) {}

    // A DT1 at `time`, returning when the next message may follow on a real MIDI input.
    double dataSet(double time, uint32_t sysexAddress, const std::vector<uint8_t>& data) {
        const std::vector<uint8_t> message = RolandSysex::dataSet(kDevice, RolandSysex::pack(sysexAddress), data.data(), data.size());
        SmfEvent event{time, 0, uint32_t(smf_->sysexData.size()), uint32_t(message.size())};
        smf_->sysexData.insert(smf_->sysexData.end(), message.begin(), message.end());
        smf_->events.push_back(event);
        return time + 0.00032 * double(message.size()) + 0.04;
    }

    void note(double time, int key, double length, int velocity = 127) {
        smf_->events.push_back(SmfEvent{time, 0x90u | uint32_t(key) << 8 | uint32_t(velocity) << 16, 0, 0});
        smf_->events.push_back(SmfEvent{time + length, 0x80u | uint32_t(key) << 8, 0, 0});
    }

    double setup(double time) {
        const Tone::Data tone = burstTone();
        if (target_ == ReverbKitTarget::D110) {
            time = dataSet(time, 0x100001, {8, 0, 0});  // Reverb off
            time = dataSet(time, 0x10000D, {0});        // Part 1 on MIDI channel 1
            // Part 1: a11 for now, key shift and fine tune 0, bender 12, poly, Mix + reverb, level 100, centre, all keys.
            time = dataSet(time, 0x030000, {0, 0, 24, 50, 12, 0, 1, 0, 100, 7, 0, 127});
        } else {
            // The performance patch: whole, upper a11 for now (reverb on), reverb off, level 100.
            std::vector<uint8_t> patch = {0, 24, 0, 0, 0, 0, 24, 24, 50, 50, 2, 2, 0, 0, 0, 1, 8, 0, 0, 50, 100};
            const char name[] = "Reverb Test     ";
            patch.insert(patch.end(), name, name + 16);
            patch.push_back(0);
            time = dataSet(time, 0x030400, patch);
        }
        return dataSet(time, 0x040000, std::vector<uint8_t>(tone.begin(), tone.end()));  // Part 1's (upper) tone
    }

    // Sets the reverb (type 0-7, 8 = off; time 1-8; level 0-7); on a D-20 the burst tone again, as a patch write may
    // reload the upper tone.
    void reverb(double time, int type, int reverbTime, int level, bool burst) {
        const std::vector<uint8_t> values = {uint8_t(type), uint8_t(reverbTime - 1), uint8_t(level)};
        if (target_ == ReverbKitTarget::D110) {
            dataSet(time, 0x100001, values);
        } else {
            time = dataSet(time, 0x030410, values);
            if (burst) {
                const Tone::Data tone = burstTone();
                dataSet(time, 0x040000, std::vector<uint8_t>(tone.begin(), tone.end()));
            }
        }
    }

    // Part 1 (the upper tone) plays a11 AcouPiano1 for the phrases.
    double pianoTone(double time) {
        if (target_ == ReverbKitTarget::D110) return dataSet(time, 0x030000, {0, 0});
        return dataSet(time, 0x030404, {0, 0});
    }

    std::unique_ptr<SmfFile> finish(double duration) {
        std::stable_sort(smf_->events.begin(), smf_->events.end(), [](const SmfEvent& a, const SmfEvent& b) { return a.time < b.time; });
        smf_->duration = duration;
        smf_->trackCount = 1;
        smf_->title = target_ == ReverbKitTarget::D110 ? "D-110 reverb test" : "D-10/D-20 reverb test";
        return std::move(smf_);
    }

private:
    ReverbKitTarget target_;
    std::unique_ptr<SmfFile> smf_;
};

}  // namespace

std::unique_ptr<SmfFile> reverbKitSmf(ReverbKitTarget target, std::vector<ReverbKitSegment>& schedule) {
    KitBuilder kit(target);
    schedule.clear();
    kit.setup(0.0);
    double slot = kSetupTime;
    // Dry bursts (reverb off): to find the start and to subtract from every segment.
    kit.reverb(slot, 8, 1, 0, true);
    for (int i = 0; i < 3; i++) {
        const double start = slot + kChangeLead + i;
        kit.note(start, kBurstKey, 0.1);
        schedule.push_back({start, "dry", 8, 1, 0});
    }
    slot += kChangeLead + 3.0 + 1.0;
    // Every type at every Reverb Time, level 7; then the Reverb Level 0-7 of one type.
    auto burst = [&](int type, int reverbTime, int level) {
        kit.reverb(slot, type, reverbTime, level, true);
        const double start = slot + kChangeLead;
        kit.note(start, kBurstKey, 0.1);
        schedule.push_back({start, "burst", type, reverbTime, level});
        slot += kBurstSlot;
    };
    for (int type = 0; type < 8; type++) {
        for (int reverbTime = 1; reverbTime <= 8; reverbTime++) burst(type, reverbTime, 7);
    }
    for (int level = 0; level <= 7; level++) burst(kSweepType, kSweepTime, level);
    // A short phrase through every type, to compare by ear.
    kit.pianoTone(slot);
    slot += 0.5;
    for (int type = 0; type < 8; type++) {
        kit.reverb(slot, type, kPhraseTime, kPhraseLevel, false);
        const double start = slot + kChangeLead;
        const int arpeggio[4] = {60, 64, 67, 72};
        for (int i = 0; i < 4; i++) kit.note(start + 0.18 * i, arpeggio[i], 0.15, 100);
        for (int key : {60, 64, 67}) kit.note(start + 0.9, key, 0.6, 100);
        schedule.push_back({start, "phrase", type, kPhraseTime, kPhraseLevel});
        slot += kPhraseSlot;
    }
    return kit.finish(slot);
}

std::string reverbKitScheduleCsv(const std::vector<ReverbKitSegment>& schedule) {
    std::string csv = "start,kind,type,time,level\n";
    for (const ReverbKitSegment& segment : schedule) {
        char line[96];
        std::snprintf(line, sizeof(line), "%.3f,%s,%d,%d,%d\n", segment.start, segment.kind.c_str(), segment.type, segment.time, segment.level);
        csv += line;
    }
    return csv;
}

namespace {

const char* const kReadme =
    "Reverb recording kit for d110emu\n"
    "================================\n"
    "\n"
    "These MIDI files play test sounds through every reverb setting of a real D-110 or D-10/D-20. A recording of them\n"
    "lets d110emu measure the unit's reverb (tools/reverb_analysis.py) and fit its D-series reverb to it.\n"
    "\n"
    "You need: the unit's MIX OUT (L and R) into an audio interface, a MIDI output to the unit's MIDI IN, and a program\n"
    "that plays a MIDI file while recording (a DAW), or a MIDI file player and a recorder started together.\n"
    "\n"
    "1. Prepare the unit.\n"
    "   D-110: unit number 17. The file uses part 1: it puts it on MIDI channel 1 with output Mix + reverb, loads its\n"
    "   own test tone and changes the reverb settings. Note your settings first, or save a bulk dump.\n"
    "   D-10/D-20: switch to performance mode, with MIDI Exclusive on, unit number 17 and the receive channel (Rx CH)\n"
    "   1. The file edits the current performance patch without storing it: select a patch afterwards to get yours\n"
    "   back.\n"
    "2. Set up the recording: 44.1 or 48 kHz, 24-bit if possible, stereo, no effects. Turn the unit's volume up to\n"
    "   about three quarters and set the input gain so that the bursts peak around -12 dBFS; they must not clip. Keep\n"
    "   the gain the same for the whole recording.\n"
    "3. Start recording, then play reverb-test-d110.mid (D-110) or reverb-test-d20.mid (D-10/D-20) from the start.\n"
    "   It lasts about 8 minutes: three dry bursts, a burst for every reverb type and Reverb Time at Reverb Level 7,\n"
    "   Medium Hall at Reverb Levels 0-7, and a short piano phrase through every type.\n"
    "4. Stop when the last phrase has died away. Save the recording as a WAV file in this folder, for example\n"
    "   reverb-d20.wav, without trimming or normalising it.\n"
    "\n"
    "reverb-test-schedule.csv lists when each burst and phrase starts (both files have the same timing).\n"
    "\n"
    "If the recording picks up hum or noise, try another cable or input: the quieter the background, the longer a\n"
    "tail can be followed.\n";

bool writeFile(const std::filesystem::path& path, const std::vector<uint8_t>& data, std::string& error) {
    std::ofstream out(path, std::ios::binary);
    if (out) out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    if (!out) error = "Cannot write " + Platform::toUtf8(path);
    return bool(out);
}

}  // namespace

bool writeReverbKit(const std::filesystem::path& folder, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    std::vector<ReverbKitSegment> schedule;
    for (ReverbKitTarget target : {ReverbKitTarget::D110, ReverbKitTarget::D20}) {
        const std::unique_ptr<SmfFile> smf = reverbKitSmf(target, schedule);
        // 480 ticks per quarter at 120 BPM: about 1 ms a tick.
        const std::vector<uint8_t> data = writeSmf(*smf, 480, 500000);
        if (!writeFile(folder / (target == ReverbKitTarget::D110 ? "reverb-test-d110.mid" : "reverb-test-d20.mid"), data, error)) return false;
    }
    const std::string csv = reverbKitScheduleCsv(schedule);
    const std::string readme = kReadme;
    return writeFile(folder / "reverb-test-schedule.csv", std::vector<uint8_t>(csv.begin(), csv.end()), error) &&
           writeFile(folder / "README.txt", std::vector<uint8_t>(readme.begin(), readme.end()), error);
}
