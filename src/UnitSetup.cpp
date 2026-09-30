#include "UnitSetup.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

#include "PresetNames.h"
#include "UiStyle.h"
#include "imgui.h"

namespace {

constexpr uint32_t kPartsBase = RolandSysex::pack(0x030000);
constexpr uint32_t kRhythmBase = RolandSysex::pack(0x030110);

const char* const kReverbD[9] = {"Small Room", "Medium Room", "Medium Hall", "Large Hall", "Plate", "Delay 1", "Delay 2", "Delay 3", "Off"};
const char* const kReverbMt32[4] = {"Room", "Hall", "Plate", "Tap delay"};
const char* const kOutputs[8] = {"Mix", "Mix+Rev", "Multi 1", "Multi 2", "Multi 3", "Multi 4", "Multi 5", "Multi 6"};
const char* const kAssign[4] = {"Poly 1", "Poly 2", "Poly 3", "Poly 4"};
const ImVec4 kHintColor(1.0f, 0.8f, 0.35f, 1.0f);  // Amber, as the tone editor's "Edited"
const char* const kAssignHelp = "Poly 1: single assign, last note has priority. Poly 2: single, first note. Poly 3: multi "
                                "(a repeated key sounds again without cutting off the previous note), last note. Poly 4: "
                                "multi, first note.";

// Pan as the D-series displays it, left to right: 7> ... >< ... <7.
std::string panText(int leftToRight) {
    leftToRight = std::clamp(leftToRight, 0, 14);
    if (leftToRight == 7) return "><";
    return leftToRight < 7 ? std::to_string(7 - leftToRight) + ">" : "<" + std::to_string(leftToRight - 7);
}

std::string signedText(int value) {
    return value > 0 ? "+" + std::to_string(value) : std::to_string(value);
}

int patchNameFilter(ImGuiInputTextCallbackData* data) {
    return data->EventChar < 32 || data->EventChar > 126 ? 1 : 0;
}

std::string partName(int part) {
    return part == UnitSetup::kRhythmPart ? std::string("R") : std::to_string(part + 1);
}

// ImGui takes slider texts as format strings.
std::string formatText(const std::string& text) {
    std::string out;
    for (char c : text) {
        out += c;
        if (c == '%') out += '%';
    }
    return out;
}

}  // namespace

UnitSetup::UnitSetup() {
    forget();
}

uint32_t UnitSetup::partAddress(int part) {
    return kPartsBase + uint32_t(std::clamp(part, 0, kParts - 1)) * kPartSize;
}

uint32_t UnitSetup::timbreAddress(int timbre) {
    return kTimbreMemoryAddress + uint32_t(std::clamp(timbre, 0, kTimbres - 1)) * kTimbreSize;
}

uint32_t UnitSetup::patchAddress(int patch) {
    return kPatchMemoryAddress + uint32_t(std::clamp(patch, 0, kPatches - 1)) * kPatchSize;
}

uint32_t UnitSetup::rhythmKeyAddress(int key) {
    return kRhythmBase + uint32_t(std::clamp(key - kFirstRhythmKey, 0, kRhythmKeys - 1)) * 4;
}

int UnitSetup::systemSize(Tone::Model model) {
    switch (model) {
    case Tone::Model::D110: return 0x21;
    case Tone::Model::D20: return 0x32;
    case Tone::Model::MT32: return 0x17;
    }
    return 0x21;
}

void UnitSetup::setModel(Tone::Model model) {
    if (model == model_) return;
    model_ = model;
    forget();
}

void UnitSetup::forget() {
    parts_.reset(size_t(kParts) * kPartSize);
    rhythm_.reset(size_t(kRhythmKeys) * 4);
    system_.reset(kMaxSystemSize);
    timbres_.reset(size_t(kTimbres) * kTimbreSize);
    partTimbre_.fill(-1);
    patch_.reset(kPatchSize);
    patches_.reset(size_t(kPatches) * kPatchSize);
    currentPatch_ = -1;
    channelsFromUnit_ = false;
    channelsMissing_ = false;
}

void UnitSetup::requestAll(Host& host) {
    for (int part = 0; part < kParts; part++) requestPart(host, part);
    requestTimbres(host);
    host.request(kRhythmBase, uint32_t(kRhythmKeys) * 4);
    host.request(kSystemAddress, uint32_t(systemSize(model_)));
}

void UnitSetup::requestPart(Host& host, int part) {
    host.request(partAddress(part), kPartSize);
}

void UnitSetup::requestTimbres(Host& host) {
    // 32 timbres (256 bytes, one DT1) a request, each from a timbre's base address.
    for (int first = 0; first < kTimbres; first += 32) host.request(timbreAddress(first), 32 * kTimbreSize);
}

bool UnitSetup::take(uint32_t packedAddress, const uint8_t* data, size_t length) {
    bool taken = false;
    auto copy = [&](Area& area, uint32_t base) {
        const uint32_t end = base + uint32_t(area.bytes.size());
        const uint32_t from = std::max(packedAddress, base);
        const uint32_t to = std::min(packedAddress + uint32_t(length), end);
        for (uint32_t address = from; address < to; address++) {
            area.bytes[address - base] = data[address - packedAddress];
            area.known[address - base] = true;
        }
        return from < to ? std::make_pair(from - base, to - base) : std::make_pair(0u, 0u);
    };
    taken |= copy(parts_, kPartsBase).second > 0;
    taken |= copy(rhythm_, kRhythmBase).second > 0;
    taken |= copy(timbres_, kTimbreMemoryAddress).second > 0;
    taken |= copy(patch_, kPatchTempAddress).second > 0;
    taken |= copy(patches_, kPatchMemoryAddress).second > 0;
    const std::pair<uint32_t, uint32_t> system = copy(system_, kSystemAddress);
    if (system.second > 0) {
        taken = true;
        // The part channels at 0D-15. A D-10/D-20 has dummies there, which read the same for all nine parts (0: channel
        // 1); those are not taken, however the unit is set here.
        if (model_ != Tone::Model::D20 && system.first <= uint32_t(Channels) && system.second >= uint32_t(Channels + kParts)) {
            const uint8_t* read = &system_.bytes[Channels];
            const bool allSame = std::all_of(read, read + kParts, [&](uint8_t channel) { return channel == read[0]; });
            channelsMissing_ = allSame;
            if (!allSame) {
                for (int part = 0; part < kParts; part++) channels_[size_t(part)] = std::min<uint8_t>(read[part], 16);
                channelsFromUnit_ = true;
            }
        }
    }
    return taken;
}

bool UnitSetup::takeChannelsFromUnit(std::array<uint8_t, kParts>& channels) {
    if (!channelsFromUnit_) return false;
    channelsFromUnit_ = false;
    channels = channels_;
    return true;
}

int UnitSetup::partValue(int part, int offset) const {
    const size_t index = size_t(part) * kPartSize + size_t(offset);
    return part >= 0 && part < kParts && offset >= 0 && offset < kPartSize && parts_.known[index] ? parts_.bytes[index] : -1;
}

int UnitSetup::rhythmValue(int key, int offset) const {
    const int entry = key - kFirstRhythmKey;
    const size_t index = size_t(entry) * 4 + size_t(offset);
    return entry >= 0 && entry < kRhythmKeys && offset >= 0 && offset < 4 && rhythm_.known[index] ? rhythm_.bytes[index] : -1;
}

int UnitSetup::systemValue(int offset) const {
    return offset >= 0 && offset < kMaxSystemSize && system_.known[size_t(offset)] ? system_.bytes[size_t(offset)] : -1;
}

int UnitSetup::timbreValue(int timbre, int offset) const {
    const size_t index = size_t(timbre) * kTimbreSize + size_t(offset);
    return timbre >= 0 && timbre < kTimbres && offset >= 0 && offset < kTimbreSize && timbres_.known[index] ? timbres_.bytes[index] : -1;
}

int UnitSetup::partTimbre(int part) const {
    return part >= 0 && part < kParts ? partTimbre_[size_t(part)] : -1;
}

bool UnitSetup::setTimbreValue(Host& host, int timbre, int offset, int value, int editedPart) {
    if (timbre < 0 || timbre >= kTimbres || offset < KeyShift || offset > Output) return false;  // The tone: setTimbreTone
    const size_t index = size_t(timbre) * kTimbreSize + size_t(offset);
    timbres_.bytes[index] = uint8_t(std::clamp(value, 0, partMax(offset)));
    timbres_.known[index] = true;
    host.sendData(timbreAddress(timbre) + uint32_t(offset), &timbres_.bytes[index], 1, true);
    for (int part = 0; part < kRhythmPart; part++) {
        if (partTimbre_[size_t(part)] == timbre) setPartValue(host, part, offset, timbres_.bytes[index]);
    }
    (void)editedPart;  // Key shift, tuning, bender, assign and output leave the part's tone alone
    return false;
}

bool UnitSetup::setTimbreTone(Host& host, int timbre, int group, int number, int editedPart) {
    if (timbre < 0 || timbre >= kTimbres) return false;
    const size_t index = size_t(timbre) * kTimbreSize;
    timbres_.bytes[index] = uint8_t(std::clamp(group, 0, 3));
    timbres_.bytes[index + 1] = uint8_t(std::clamp(number, 0, 63));
    timbres_.known[index] = timbres_.known[index + 1] = true;
    host.sendData(timbreAddress(timbre), &timbres_.bytes[index], 2, true);
    bool editedToneChanged = false;
    for (int part = 0; part < kRhythmPart; part++) {
        if (partTimbre_[size_t(part)] != timbre) continue;
        setPartTone(host, part, timbres_.bytes[index], timbres_.bytes[index + 1]);
        editedToneChanged = editedToneChanged || part == editedPart;
    }
    return editedToneChanged;
}

bool UnitSetup::pasteTimbre(Host& host, int timbre, const uint8_t* bytes, int editedPart) {
    if (timbre < 0 || timbre >= kTimbres) return false;
    const size_t index = size_t(timbre) * kTimbreSize;
    for (int offset = ToneGroup; offset <= Output; offset++) {
        timbres_.bytes[index + size_t(offset)] = uint8_t(std::clamp(int(bytes[offset]), 0, partMax(offset)));
        timbres_.known[index + size_t(offset)] = true;
    }
    host.sendData(timbreAddress(timbre), &timbres_.bytes[index], Output + 1, true);
    bool editedToneChanged = false;
    for (int part = 0; part < kRhythmPart; part++) {
        if (partTimbre_[size_t(part)] != timbre) continue;
        const size_t partIndex = size_t(part) * kPartSize;
        for (int offset = ToneGroup; offset <= Output; offset++) {
            parts_.bytes[partIndex + size_t(offset)] = timbres_.bytes[index + size_t(offset)];
            parts_.known[partIndex + size_t(offset)] = true;
        }
        sendPart(host, part, ToneGroup, Output + 1);
        editedToneChanged = editedToneChanged || part == editedPart;
    }
    return editedToneChanged;
}

bool UnitSetup::selectTimbre(Host& host, int part, int timbre) {
    if (part < 0 || part >= kRhythmPart || timbre < 0 || timbre >= kTimbres) return false;
    const uint8_t channel = channels_[size_t(part)];
    if (channel >= 16) return false;
    host.sendShort(uint32_t(0xC0 | channel) | uint32_t(timbre) << 8);
    partTimbre_[size_t(part)] = timbre;
    requestPart(host, part);  // The unit loads the timbre: read what the part plays now
    return true;
}

void UnitSetup::writeTimbre(Host& host, int part, int timbre) {
    if (model_ == Tone::Model::MT32 || part < 0 || part >= kRhythmPart || timbre < 0 || timbre >= kTimbres) return;
    const uint8_t request[2] = {uint8_t(timbre), 0};  // Timbre A11-B88, internal
    host.sendData(kTimbreWriteAddress + uint32_t(part) * 2, request, 2, false);
    // Memory now holds what the part plays; the part plays that timbre.
    const size_t index = size_t(timbre) * kTimbreSize;
    for (int offset = ToneGroup; offset <= Output; offset++) {
        const int value = partValue(part, offset);
        if (value < 0) continue;
        timbres_.bytes[index + size_t(offset)] = uint8_t(value);
        timbres_.known[index + size_t(offset)] = true;
    }
    partTimbre_[size_t(part)] = timbre;
    host.request(timbreAddress(timbre), kTimbreSize);  // Read back what the unit stored
}

void UnitSetup::requestPatch(Host& host) {
    requestPatchTemp(host);
    // Six patches (228 bytes, one DT1) a request, each from a patch's base address.
    for (int first = 0; first < kPatches; first += 6) host.request(patchAddress(first), uint32_t(std::min(6, kPatches - first) * kPatchSize));
}

void UnitSetup::requestPatchTemp(Host& host) {
    host.request(kPatchTempAddress, kPatchSize);
}

int UnitSetup::patchValue(int offset) const {
    return offset >= 0 && offset < kPatchSize && patch_.known[size_t(offset)] ? patch_.bytes[size_t(offset)] : -1;
}

int UnitSetup::patchMemoryValue(int patch, int offset) const {
    const size_t index = size_t(patch) * kPatchSize + size_t(offset);
    return patch >= 0 && patch < kPatches && offset >= 0 && offset < kPatchSize && patches_.known[index] ? patches_.bytes[index] : -1;
}

std::string UnitSetup::patchName(int patch) const {
    std::string name;
    for (int i = 0; i < kPatchNameLength; i++) {
        const int c = patchMemoryValue(patch, PatchName + i);
        if (c < 0) return std::string();
        name += c >= 32 && c < 127 ? char(c) : ' ';
    }
    name.erase(name.find_last_not_of(' ') + 1);
    return name;
}

int UnitSetup::patchMax(int offset) const {
    // The D-20's ranges (manual p.266): key mode, split point, the tones' groups and numbers, key shifts, fine tunes,
    // bender ranges, assign modes, reverb switches, reverb type, time and level, U/L balance, level, then the name.
    static const uint8_t maxima[PatchName] = {2, 61, 3, 63, 3, 63, 48, 48, 100, 100, 24, 24, 3, 3, 1, 1, 8, 7, 7, 100, 100};
    if (offset >= 0 && offset < PatchName) return maxima[offset];
    return offset < PatchName + kPatchNameLength ? 127 : 0;
}

void UnitSetup::setPatchValue(Host& host, int offset, int value) {
    if (offset < 0 || offset >= PatchName) return;
    patch_.bytes[size_t(offset)] = uint8_t(std::clamp(value, 0, patchMax(offset)));
    patch_.known[size_t(offset)] = true;
    host.sendData(kPatchTempAddress + uint32_t(offset), &patch_.bytes[size_t(offset)], 1, true);
}

void UnitSetup::setPatchTone(Host& host, bool upper, int group, int number) {
    const int offset = upper ? UpperToneGroup : LowerToneGroup;
    patch_.bytes[size_t(offset)] = uint8_t(std::clamp(group, 0, 3));
    patch_.bytes[size_t(offset) + 1] = uint8_t(std::clamp(number, 0, 63));
    patch_.known[size_t(offset)] = patch_.known[size_t(offset) + 1] = true;
    host.sendData(kPatchTempAddress + uint32_t(offset), &patch_.bytes[size_t(offset)], 2, true);
}

void UnitSetup::setPatchName(Host& host, const std::string& name) {
    for (int i = 0; i < kPatchNameLength; i++) {
        const char c = i < int(name.size()) ? name[size_t(i)] : ' ';
        patch_.bytes[size_t(PatchName + i)] = uint8_t(c) < 32 || uint8_t(c) > 126 ? uint8_t(' ') : uint8_t(c);
        patch_.known[size_t(PatchName + i)] = true;
    }
    host.sendData(kPatchTempAddress + PatchName, &patch_.bytes[PatchName], kPatchNameLength, true);
}

bool UnitSetup::selectPatch(Host& host, int patch) {
    if (patch < 0 || patch >= kPatches || performanceChannel_ >= 16) return false;
    host.sendShort(uint32_t(0xC0 | performanceChannel_) | uint32_t(patch) << 8);
    currentPatch_ = patch;
    patchWriteTarget_ = patch;
    host.request(kPatchTempAddress, kPatchSize);  // The unit loads the patch: read it
    return true;
}

void UnitSetup::writePatch(Host& host, int patch) {
    if (patch < 0 || patch >= kPatches) return;
    const uint8_t request[2] = {uint8_t(patch), 0};  // Patch A11-B88, internal
    host.sendData(kPatchWriteAddress, request, 2, false);
    // Memory now holds the performance patch.
    for (int offset = 0; offset < kPatchSize; offset++) {
        if (!patch_.known[size_t(offset)]) continue;
        patches_.bytes[size_t(patch) * kPatchSize + size_t(offset)] = patch_.bytes[size_t(offset)];
        patches_.known[size_t(patch) * kPatchSize + size_t(offset)] = true;
    }
    currentPatch_ = patch;
    host.request(patchAddress(patch), kPatchSize);  // Read back what the unit stored
}

int UnitSetup::partMax(int offset) const {
    switch (offset) {
    case ToneGroup: return 3;
    case ToneNumber: return 63;
    case KeyShift: return 48;
    case FineTune: return 100;
    case BenderRange: return 24;
    case AssignMode: return 3;
    case Output: return model_ == Tone::Model::D110 ? 7 : 1;
    case Level: return 100;
    case Panpot: return 14;
    case KeyLow:
    case KeyHigh: return 127;
    default: return 0;
    }
}

int UnitSetup::rhythmMax(int offset) const {
    switch (offset) {
    case RhythmTone: return 127;
    case RhythmLevel: return 100;
    case RhythmPanpot: return 14;
    case RhythmOutput: return model_ == Tone::Model::D110 ? 7 : 1;
    default: return 0;
    }
}

int UnitSetup::systemMax(int offset) const {
    if (offset == MasterTune) return 127;
    if (offset == ReverbMode) return model_ == Tone::Model::MT32 ? 3 : 8;
    if (offset == ReverbTime || offset == ReverbLevel) return 7;
    if (offset >= Reserves && offset < Reserves + kParts) return 32;
    if (offset >= Channels && offset < Channels + kParts) return model_ == Tone::Model::D20 ? 0 : 16;
    if (offset == MasterVolume) return model_ == Tone::Model::MT32 ? 100 : 0;
    return 0;
}

void UnitSetup::sendPart(Host& host, int part, int offset, int length) {
    host.sendData(partAddress(part) + uint32_t(offset), &parts_.bytes[size_t(part) * kPartSize + size_t(offset)], size_t(length), true);
}

void UnitSetup::setPartValue(Host& host, int part, int offset, int value) {
    if (part < 0 || part >= kParts || offset < 0 || offset >= kPartSize) return;
    const size_t index = size_t(part) * kPartSize + size_t(offset);
    parts_.bytes[index] = uint8_t(std::clamp(value, 0, partMax(offset)));
    parts_.known[index] = true;
    sendPart(host, part, offset, 1);
}

void UnitSetup::setPartTone(Host& host, int part, int group, int number) {
    if (part < 0 || part >= kRhythmPart) return;
    const size_t index = size_t(part) * kPartSize;
    parts_.bytes[index] = uint8_t(std::clamp(group, 0, 3));
    parts_.bytes[index + 1] = uint8_t(std::clamp(number, 0, 63));
    parts_.known[index] = parts_.known[index + 1] = true;
    sendPart(host, part, ToneGroup, 2);
}

void UnitSetup::setRhythmValue(Host& host, int key, int offset, int value) {
    const int entry = key - kFirstRhythmKey;
    if (entry < 0 || entry >= kRhythmKeys || offset < 0 || offset >= 4) return;
    const size_t index = size_t(entry) * 4 + size_t(offset);
    rhythm_.bytes[index] = uint8_t(std::clamp(value, 0, rhythmMax(offset)));
    rhythm_.known[index] = true;
    host.sendData(rhythmKeyAddress(key) + uint32_t(offset), &rhythm_.bytes[index], 1, true);
}

void UnitSetup::setSystemValue(Host& host, int offset, int value) {
    if (offset < 0 || offset >= systemSize(model_) || systemMax(offset) == 0) return;
    system_.bytes[size_t(offset)] = uint8_t(std::clamp(value, 0, systemMax(offset)));
    system_.known[size_t(offset)] = true;
    host.sendData(kSystemAddress + uint32_t(offset), &system_.bytes[size_t(offset)], 1, true);
}

void UnitSetup::setReserve(Host& host, int part, int value) {
    if (part < 0 || part >= kParts) return;
    int others = 0;
    for (int other = 0; other < kParts; other++) {
        if (other != part) others += std::max(systemValue(Reserves + other), 0);
    }
    const int clamped = std::clamp(value, 0, std::max(0, 32 - others));
    system_.bytes[size_t(Reserves + part)] = uint8_t(clamped);
    for (int other = 0; other < kParts; other++) system_.known[size_t(Reserves + other)] = true;
    host.sendData(kSystemAddress + Reserves, &system_.bytes[Reserves], kParts, true);  // The package of nine
}

void UnitSetup::setChannel(Host& host, int part, int channel) {
    if (part < 0 || part >= kParts) return;
    channels_[size_t(part)] = uint8_t(std::clamp(channel, 0, 16));
    if (model_ == Tone::Model::D20) return;  // Set on the unit's panel
    setSystemValue(host, Channels + part, channels_[size_t(part)]);
}

bool UnitSetup::playRhythmKey(Host& host, int key, double now) {
    const uint8_t channel = channels_[kRhythmPart];
    if (channel >= 16 || key < 0 || key > 127) return false;
    host.sendShort(uint32_t(0x90 | channel) | uint32_t(key) << 8 | uint32_t(auditionVelocity_) << 16);
    noteOffs_.push_back({now + 0.5, uint32_t(0x80 | channel) | uint32_t(key) << 8});
    return true;
}

void UnitSetup::update(Host& host, double now) {
    for (size_t i = 0; i < noteOffs_.size();) {
        if (now >= noteOffs_[i].time) {
            host.sendShort(noteOffs_[i].message);
            noteOffs_.erase(noteOffs_.begin() + long(i));
        } else {
            i++;
        }
    }
}

std::string UnitSetup::masterTuneText(int value) {
    char text[16];
    std::snprintf(text, sizeof(text), "%.1f Hz", 440.0 * std::exp2((value - 64.0) / (128.0 * 12.0)));
    return text;
}

std::string UnitSetup::toneLabel(Host& host, int group, int number) const {
    group &= 3;
    number &= 63;
    std::string code;
    std::string name;
    char text[8];
    if (model_ == Tone::Model::MT32) {
        std::snprintf(text, sizeof(text), "%c%02d", "ABMR"[group], number + 1);
        code = text;
        if (group < 2) name = kMt32PresetNames[group * 64 + number];
        if (group == 2) name = host.memoryToneName(number);
    } else {
        if (group == 3) {
            std::snprintf(text, sizeof(text), "r%02d", number + 1);
        } else {
            std::snprintf(text, sizeof(text), "%c%d%d", "abi"[group], number / 8 + 1, number % 8 + 1);
        }
        code = text;
        name = group == 2 ? host.memoryToneName(number) : kDSeriesToneNames[group == 3 ? 192 + number : group * 64 + number];
    }
    return name.empty() ? code : code + " " + name;
}

const char* UnitSetup::memoryProtectNote() const {
    return model_ == Tone::Model::D20
               ? "Turn the unit's Memory Protect off (TUNE/FUNCTION, then the DISPLAY up button, then the Value knob), or it ignores "
                 "changes to its memory. It turns back on when the unit is switched off."
               : "Turn the unit's Memory Protect off (System Setup, Mem Protect), or it ignores changes to its memory.";
}

std::string UnitSetup::timbreCode(int timbre) const {
    char text[8];
    if (model_ == Tone::Model::MT32) {
        std::snprintf(text, sizeof(text), "%3d", (timbre & 127) + 1);
    } else {
        std::snprintf(text, sizeof(text), "%c%d%d", (timbre & 127) < 64 ? 'A' : 'B', (timbre & 63) / 8 + 1, (timbre & 7) + 1);
    }
    return text;
}

std::string UnitSetup::rhythmToneLabel(Host& host, int value) const {
    if (value < 0) return "?";
    const bool rhythm = value >= 64;
    const int number = value & 63;
    if (model_ != Tone::Model::MT32 && value == 127) return "OFF";
    char code[8];
    std::snprintf(code, sizeof(code), "%s%02d", model_ == Tone::Model::MT32 ? (rhythm ? "R" : "M") : (rhythm ? "r" : "i"), number + 1);
    const std::string name = rhythm ? (model_ == Tone::Model::MT32 ? std::string() : std::string(kDSeriesToneNames[192 + number]))
                                    : host.memoryToneName(number);
    return name.empty() ? std::string(code) : std::string(code) + " " + name;
}

// ---------------------------------------------------------------------------------------------
// UI

bool UnitSetup::tonePopup(Host& host, int group, int number, int& chosenGroup, int& chosenNumber) {
    if (!ImGui::BeginPopup("tones")) return false;
    bool chosen = false;
    const float fs = ImGui::GetFontSize();
    const bool mt32 = model_ == Tone::Model::MT32;
    static const char* const dTitles[4] = {"a (preset)", "b (preset)", "i (internal)", "r (rhythm)"};
    static const char* const mtTitles[4] = {"A (preset)", "B (preset)", "M (memory)", "R (rhythm)"};
    if (ImGui::BeginTabBar("groups")) {
        for (int g = 0; g < 4; g++) {
            const ImGuiTabItemFlags flags = ImGui::IsWindowAppearing() && g == group ? ImGuiTabItemFlags_SetSelected : 0;
            if (!ImGui::BeginTabItem(mt32 ? mtTitles[g] : dTitles[g], nullptr, flags)) continue;
            if (ImGui::BeginTable("grid", 4, ImGuiTableFlags_SizingFixedFit)) {
                for (int n = 0; n < 64; n++) {
                    ImGui::TableNextColumn();
                    const std::string label = toneLabel(host, g, n) + "##" + std::to_string(n);
                    if (ImGui::Selectable(label.c_str(), g == group && n == number, 0, ImVec2(fs * 8.5f, 0.0f))) {
                        chosenGroup = g;
                        chosenNumber = n;
                        chosen = true;
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndPopup();
    return chosen;
}

bool UnitSetup::drawProgramPicker(Host& host, int part) {
    if (!ImGui::BeginPopup("programs")) return false;
    bool sent = false;
    const float fs = ImGui::GetFontSize();
    const uint8_t channel = channels_[size_t(part)];
    ImGui::TextDisabled(model_ == Tone::Model::MT32 ? "Program change: patch memory 1-128" : "Program change: timbre memory A11-B88");
    ImGui::BeginChild("list", ImVec2(fs * 12.0f, fs * 18.0f));
    for (int program = 0; program < 128; program++) {
        char label[40];
        if (model_ == Tone::Model::MT32) {
            std::snprintf(label, sizeof(label), "%3d %s", program + 1, kMt32PresetNames[program]);
        } else {
            std::snprintf(label, sizeof(label), "%c%d%d", program < 64 ? 'A' : 'B', (program & 63) / 8 + 1, (program & 7) + 1);
        }
        if (ImGui::Selectable(label, program == partTimbre_[size_t(part)]) && channel < 16) {
            sent = selectTimbre(host, part, program);
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndChild();
    if (model_ == Tone::Model::MT32) ImGui::TextDisabled("Names: the MT-32's factory patches");
    ImGui::EndPopup();
    return sent;
}

void UnitSetup::drawParts(Host& host, int editedPart, PartsEvents& events) {
    const float fs = ImGui::GetFontSize();
    const bool d110 = model_ == Tone::Model::D110;
    const bool mt32 = model_ == Tone::Model::MT32;
    if (ImGui::Button("Read from the unit")) requestAll(host);
    UiStyle::setItemTooltip("Reads the parts, the rhythm setup and the system area (needs the \"From the unit\" port)");
    ImGui::SameLine();
    ImGui::TextDisabled("Changes go to the unit at once. Click a part's number to edit its tone.");
    if (model_ == Tone::Model::D20) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("D-10/D-20: multi-timbral mode. Its part channels are set on its panel; here they tell the editor where to play.");
        ImGui::PopStyleColor();
    }

    const int columns = d110 ? 14 : 12;
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollX;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float rowHeight = ImGui::GetFrameHeight() + style.CellPadding.y * 2.0f;
    if (!ImGui::BeginTable("parts", columns, flags, ImVec2(0.0f, rowHeight * float(kParts + 1) + style.ScrollbarSize + 4.0f))) return;
    ImGui::TableSetupColumn("Part", ImGuiTableColumnFlags_WidthFixed, fs * 2.0f);
    ImGui::TableSetupColumn("Ch", ImGuiTableColumnFlags_WidthFixed, fs * 3.0f);
    ImGui::TableSetupColumn("Tone", ImGuiTableColumnFlags_WidthFixed, fs * 9.0f);
    ImGui::TableSetupColumn("Prog", ImGuiTableColumnFlags_WidthFixed, fs * 2.6f);
    ImGui::TableSetupColumn("Shift", ImGuiTableColumnFlags_WidthFixed, fs * 3.2f);
    ImGui::TableSetupColumn("Fine", ImGuiTableColumnFlags_WidthFixed, fs * 3.2f);
    ImGui::TableSetupColumn("Bend", ImGuiTableColumnFlags_WidthFixed, fs * 2.8f);
    ImGui::TableSetupColumn("Assign", ImGuiTableColumnFlags_WidthFixed, fs * 4.0f);
    ImGui::TableSetupColumn(d110 ? "Output" : "Reverb", ImGuiTableColumnFlags_WidthFixed, fs * (d110 ? 4.6f : 3.2f));
    ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, fs * 3.4f);
    ImGui::TableSetupColumn("Pan", ImGuiTableColumnFlags_WidthFixed, fs * 2.8f);
    if (d110) {
        ImGui::TableSetupColumn("Low", ImGuiTableColumnFlags_WidthFixed, fs * 3.0f);
        ImGui::TableSetupColumn("High", ImGuiTableColumnFlags_WidthFixed, fs * 3.0f);
    }
    ImGui::TableSetupColumn("Reserve", ImGuiTableColumnFlags_WidthFixed, fs * 3.8f);
    ImGui::TableHeadersRow();
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));

    const bool reservesKnown = [&] {
        for (int part = 0; part < kParts; part++) {
            if (systemValue(Reserves + part) < 0) return false;
        }
        return true;
    }();
    for (int part = 0; part < kParts; part++) {
        const bool rhythm = part == kRhythmPart;
        ImGui::TableNextRow();
        ImGui::PushID(part);
        // A slider over a byte of the part: `shift` is subtracted for display, `text` shows the value.
        auto slider = [&](int offset, int low, int high, int shift, auto text) {
            const int value = partValue(part, offset);
            int shown = value >= 0 ? value - shift : low;
            const std::string label = value >= 0 ? formatText(text(shown)) : std::string("?");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::PushID(offset);
            if (ImGui::SliderInt("##v", &shown, low, high, label.c_str(), ImGuiSliderFlags_AlwaysClamp)) {
                setPartValue(host, part, offset, shown + shift);
            }
            ImGui::PopID();
        };
        auto combo = [&](int offset, const char* const* names, int count) {
            const int value = partValue(part, offset);
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::PushID(offset);
            if (ImGui::BeginCombo("##c", value >= 0 && value < count ? names[value] : "?", ImGuiComboFlags_NoArrowButton)) {
                for (int i = 0; i < count; i++) {
                    if (ImGui::Selectable(names[i], i == value)) setPartValue(host, part, offset, i);
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();
        };

        ImGui::TableNextColumn();
        const std::string number = partName(part);
        if (rhythm) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(number.c_str());
        } else if (ImGui::Selectable(number.c_str(), part == editedPart, 0, ImVec2(0.0f, ImGui::GetFrameHeight()))) {
            events.editPart = part;
        }
        if (!rhythm) UiStyle::setItemTooltip("Edit part %d's tone", part + 1);

        ImGui::TableNextColumn();
        const uint8_t channel = channels_[size_t(part)];
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##channel", channel < 16 ? std::to_string(channel + 1).c_str() : "off", ImGuiComboFlags_HeightLarge)) {
            for (int choice = 0; choice <= 16; choice++) {
                if (ImGui::Selectable(choice < 16 ? std::to_string(choice + 1).c_str() : "off", choice == channel)) setChannel(host, part, choice);
            }
            ImGui::EndCombo();
        }

        if (rhythm) {
            for (int column = 0; column < 7; column++) {
                ImGui::TableNextColumn();
                ImGui::TextDisabled("-");
            }
            ImGui::TableNextColumn();
            slider(Level, 0, 100, 0, [](int v) { return std::to_string(v); });
            ImGui::TableNextColumn();
            if (d110) {
                ImGui::TableNextColumn();
                ImGui::TableNextColumn();
            }
        } else {
            ImGui::TableNextColumn();
            const int group = partValue(part, ToneGroup), toneNumber = partValue(part, ToneNumber);
            const std::string tone = group >= 0 && toneNumber >= 0 ? toneLabel(host, group, toneNumber) : std::string("?");
            ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
            if (ImGui::Button((tone + "##tone").c_str(), ImVec2(-FLT_MIN, 0.0f))) ImGui::OpenPopup("tones");
            ImGui::PopStyleVar();
            UiStyle::setItemTooltip("%s: click to choose the tone the part plays", tone.c_str());
            int chosenGroup = 0, chosenNumber = 0;
            if (tonePopup(host, group, toneNumber, chosenGroup, chosenNumber)) {
                setPartTone(host, part, chosenGroup, chosenNumber);
                if (part == editedPart) events.editedToneChanged = true;
            }

            ImGui::TableNextColumn();
            ImGui::BeginDisabled(channel >= 16);
            if (ImGui::Button("PC", ImVec2(-FLT_MIN, 0.0f))) ImGui::OpenPopup("programs");
            ImGui::EndDisabled();
            UiStyle::setItemTooltip(mt32 ? "Program change on the part's channel: a patch" : "Program change on the part's channel: a timbre of memory");
            if (drawProgramPicker(host, part) && part == editedPart) events.editedToneChanged = true;

            ImGui::TableNextColumn();
            slider(KeyShift, -24, 24, 24, signedText);
            ImGui::TableNextColumn();
            slider(FineTune, -50, 50, 50, signedText);
            ImGui::TableNextColumn();
            slider(BenderRange, 0, 24, 0, [](int v) { return std::to_string(v); });
            ImGui::TableNextColumn();
            combo(AssignMode, kAssign, 4);
            UiStyle::setItemTooltip("%s", kAssignHelp);
            ImGui::TableNextColumn();
            if (d110) {
                combo(Output, kOutputs, 8);
            } else {
                static const char* const reverb[2] = {"Off", "On"};
                combo(Output, reverb, 2);
            }
            ImGui::TableNextColumn();
            slider(Level, 0, 100, 0, [](int v) { return std::to_string(v); });
            ImGui::TableNextColumn();
            {
                // Shown left to right; the MT-32 stores pan the other way round.
                const int value = partValue(part, Panpot);
                int shown = value < 0 ? 7 : (mt32 ? 14 - value : value);
                const std::string label = value >= 0 ? panText(shown) : std::string("?");
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::SliderInt("##pan", &shown, 0, 14, label.c_str(), ImGuiSliderFlags_AlwaysClamp)) {
                    setPartValue(host, part, Panpot, mt32 ? 14 - shown : shown);
                }
            }
            if (d110) {
                ImGui::TableNextColumn();
                slider(KeyLow, 0, 127, 0, [](int v) { return Tone::noteName(v); });
                ImGui::TableNextColumn();
                slider(KeyHigh, 0, 127, 0, [](int v) { return Tone::noteName(v); });
            }
        }

        ImGui::TableNextColumn();
        ImGui::BeginDisabled(!reservesKnown);
        int reserve = std::max(systemValue(Reserves + part), 0);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderInt("##reserve", &reserve, 0, 32, reservesKnown ? "%d" : "?", ImGuiSliderFlags_AlwaysClamp)) setReserve(host, part, reserve);
        ImGui::EndDisabled();
        UiStyle::setItemTooltip(reservesKnown ? "Partials kept for the part; all together at most 32 (sent as the package of nine)"
                                            : "Read the unit's setup first: the reserves go to the unit as a package of all nine");
        ImGui::PopID();
    }
    ImGui::PopStyleVar();
    ImGui::EndTable();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Shift: key shift. Bend: bender range.%s Prog: a program change on the part's channel.", d110 ? " Low/High: the key range." : "");
    ImGui::PopStyleColor();
}

void UnitSetup::drawTimbres(Host& host, int editedPart, PartsEvents& events) {
    const float fs = ImGui::GetFontSize();
    const bool d110 = model_ == Tone::Model::D110;
    const bool mt32 = model_ == Tone::Model::MT32;
    if (ImGui::Button("Read from the unit")) requestTimbres(host);
    UiStyle::setItemTooltip("Reads %s (needs the \"From the unit\" port)", mt32 ? "patch memory" : "timbre memory");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Click a %s to play it on part %d. Changes go to the unit at once, and to parts that play it.", mt32 ? "patch" : "timbre",
                       editedPart + 1);
    ImGui::PopStyleColor();
    if (!mt32) {
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Write part %d's timbre to", editedPart + 1);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(fs * 4.5f);
        if (ImGui::BeginCombo("##writetarget", timbreCode(writeTarget_).c_str(), ImGuiComboFlags_HeightLarge)) {
            for (int timbre = 0; timbre < kTimbres; timbre++) {
                if (ImGui::Selectable(timbreCode(timbre).c_str(), timbre == writeTarget_)) writeTarget_ = timbre;
                if (timbre == writeTarget_ && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Write##timbre")) writeTimbre(host, editedPart, writeTarget_);
        UiStyle::setItemTooltip("The unit's timbre write: stores what part %d plays (its tone and settings, as the unit holds them) in this "
                              "timbre. A D-10/D-20 does this in multi-timbral mode.", editedPart + 1);
    }
    if (!mt32) {
        ImGui::PushStyleColor(ImGuiCol_Text, kHintColor);
        ImGui::TextWrapped("%s", memoryProtectNote());
        ImGui::PopStyleColor();
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY;
    if (!ImGui::BeginTable("timbres", 7, flags, ImVec2(0.0f, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(mt32 ? "Patch" : "Timbre", ImGuiTableColumnFlags_WidthFixed, fs * 3.4f);
    ImGui::TableSetupColumn("Tone", ImGuiTableColumnFlags_WidthFixed, fs * 9.5f);
    ImGui::TableSetupColumn("Shift", ImGuiTableColumnFlags_WidthFixed, fs * 3.4f);
    ImGui::TableSetupColumn("Fine", ImGuiTableColumnFlags_WidthFixed, fs * 3.4f);
    ImGui::TableSetupColumn("Bend", ImGuiTableColumnFlags_WidthFixed, fs * 3.0f);
    ImGui::TableSetupColumn("Assign", ImGuiTableColumnFlags_WidthFixed, fs * 4.2f);
    ImGui::TableSetupColumn(d110 ? "Output" : "Reverb", ImGuiTableColumnFlags_WidthFixed, fs * (d110 ? 4.6f : 3.2f));
    ImGui::TableHeadersRow();
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
    const int playing = partTimbre(editedPart);
    for (int timbre = 0; timbre < kTimbres; timbre++) {
        ImGui::TableNextRow();
        ImGui::PushID(timbre);
        // A slider over a byte of the timbre: `shift` is subtracted for display, `text` shows the value.
        auto slider = [&](int offset, int low, int high, int shift, auto text) {
            const int value = timbreValue(timbre, offset);
            int shown = value >= 0 ? value - shift : low;
            const std::string label = value >= 0 ? formatText(text(shown)) : std::string("?");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::PushID(offset);
            if (ImGui::SliderInt("##v", &shown, low, high, label.c_str(), ImGuiSliderFlags_AlwaysClamp)) {
                setTimbreValue(host, timbre, offset, shown + shift, editedPart);
            }
            ImGui::PopID();
        };
        auto combo = [&](int offset, const char* const* names, int count) {
            const int value = timbreValue(timbre, offset);
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::PushID(offset);
            if (ImGui::BeginCombo("##c", value >= 0 && value < count ? names[value] : "?", ImGuiComboFlags_NoArrowButton)) {
                for (int i = 0; i < count; i++) {
                    if (ImGui::Selectable(names[i], i == value)) setTimbreValue(host, timbre, offset, i, editedPart);
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();
        };

        ImGui::TableNextColumn();
        if (ImGui::Selectable(timbreCode(timbre).c_str(), timbre == playing, 0, ImVec2(0.0f, ImGui::GetFrameHeight()))) {
            if (selectTimbre(host, editedPart, timbre)) events.editedToneChanged = true;
            writeTarget_ = timbre;
        }
        UiStyle::setItemTooltip("Plays %s %s on part %d (a program change). Right-click: copy, paste.", mt32 ? "patch" : "timbre",
                              timbreCode(timbre).c_str(), editedPart + 1);
        if (ImGui::BeginPopupContextItem("timbre")) {
            bool known = true;
            for (int offset = ToneGroup; offset <= Output; offset++) known = known && timbreValue(timbre, offset) >= 0;
            if (ImGui::MenuItem("Copy", nullptr, false, known)) {
                for (int offset = ToneGroup; offset <= Output; offset++) timbreClipboard_[size_t(offset)] = uint8_t(timbreValue(timbre, offset));
                timbreClipboardFull_ = true;
            }
            if (ImGui::MenuItem("Paste", nullptr, false, timbreClipboardFull_) && pasteTimbre(host, timbre, timbreClipboard_.data(), editedPart)) {
                events.editedToneChanged = true;
            }
            ImGui::EndPopup();
        }

        ImGui::TableNextColumn();
        const int group = timbreValue(timbre, ToneGroup), number = timbreValue(timbre, ToneNumber);
        const std::string tone = group >= 0 && number >= 0 ? toneLabel(host, group, number) : std::string("?");
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
        if (ImGui::Button((tone + "##tone").c_str(), ImVec2(-FLT_MIN, 0.0f))) ImGui::OpenPopup("tones");
        ImGui::PopStyleVar();
        int chosenGroup = 0, chosenNumber = 0;
        if (tonePopup(host, group, number, chosenGroup, chosenNumber) && setTimbreTone(host, timbre, chosenGroup, chosenNumber, editedPart)) {
            events.editedToneChanged = true;
        }

        ImGui::TableNextColumn();
        slider(KeyShift, -24, 24, 24, signedText);
        ImGui::TableNextColumn();
        slider(FineTune, -50, 50, 50, signedText);
        ImGui::TableNextColumn();
        slider(BenderRange, 0, 24, 0, [](int v) { return std::to_string(v); });
        ImGui::TableNextColumn();
        combo(AssignMode, kAssign, 4);
        UiStyle::setItemTooltip("%s", kAssignHelp);
        ImGui::TableNextColumn();
        if (d110) {
            combo(Output, kOutputs, 8);
        } else {
            static const char* const reverb[2] = {"Off", "On"};
            combo(Output, reverb, 2);
        }
        ImGui::PopID();
    }
    ImGui::PopStyleVar();
    ImGui::EndTable();
}

void UnitSetup::drawPatch(Host& host, PatchEvents& events) {
    const float fs = ImGui::GetFontSize();
    if (ImGui::Button("Read from the unit")) requestPatch(host);
    UiStyle::setItemTooltip("Reads the performance patch and patch memory (needs the \"From the unit\" port)");
    ImGui::SameLine(0.0f, fs);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Performance channel");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fs * 3.0f);
    if (ImGui::BeginCombo("##perfchannel", std::to_string(performanceChannel_ + 1).c_str(), ImGuiComboFlags_HeightLarge)) {
        for (int channel = 0; channel < 16; channel++) {
            if (ImGui::Selectable(std::to_string(channel + 1).c_str(), channel == performanceChannel_)) performanceChannel_ = uint8_t(channel);
        }
        ImGui::EndCombo();
    }
    UiStyle::setItemTooltip("The unit's receive channel in performance mode (Rx CH in its MIDI function menu). Program changes there "
                          "select patches, and notes play the patch.");
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("The D-10/D-20 must be in performance mode. Changes go to its performance patch at once. The upper tone plays in "
                       "part 1's tone and the lower in part 2's: Edit opens them on the Tone tab, which then plays notes on the "
                       "performance channel.");
    ImGui::PopStyleColor();

    const std::string title = currentPatch_ >= 0 ? "Performance patch " + timbreCode(currentPatch_) : std::string("Performance patch");
    ImGui::SeparatorText(title.c_str());
    // Name and level
    char name[kPatchNameLength + 1] = {};
    for (int i = 0; i < kPatchNameLength; i++) {
        const int c = patchValue(PatchName + i);
        name[i] = c >= 32 && c < 127 ? char(c) : ' ';
    }
    for (int i = kPatchNameLength - 1; i >= 0 && name[i] == ' '; i--) name[i] = 0;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Name");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fs * 9.5f);
    if (ImGui::InputText("##patchname", name, sizeof(name), ImGuiInputTextFlags_CallbackCharFilter, patchNameFilter)) setPatchName(host, name);
    auto slider = [&](const char* id, int offset, int low, int high, int shift, auto text, float width) {
        const int value = patchValue(offset);
        int shown = value >= 0 ? value - shift : low;
        const std::string label = value >= 0 ? formatText(text(shown)) : std::string("?");
        ImGui::SetNextItemWidth(width);
        if (ImGui::SliderInt(id, &shown, low, high, label.c_str(), ImGuiSliderFlags_AlwaysClamp)) setPatchValue(host, offset, shown + shift);
    };
    auto number = [](int v) { return std::to_string(v); };
    ImGui::SameLine(0.0f, fs);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Level");
    ImGui::SameLine();
    slider("##level", PatchLevel, 0, 100, 0, number, fs * 6.0f);

    // Key mode, split point and balance
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Keys");
    static const char* const keyModes[3] = {"Whole", "Dual", "Split"};
    const int mode = patchValue(KeyMode);
    for (int m = 0; m < 3; m++) {
        ImGui::SameLine();
        if (ImGui::RadioButton(keyModes[m], mode == m)) setPatchValue(host, KeyMode, m);
    }
    UiStyle::setItemTooltip("Whole: the upper tone only. Dual: both tones, weighted by the balance. Split: the lower tone below the split point.");
    ImGui::SameLine(0.0f, fs);
    ImGui::BeginDisabled(mode != 2);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Split at");
    ImGui::SameLine();
    slider("##split", SplitPoint, 0, 61, 0, [](int v) { return Tone::noteName(36 + v); }, fs * 5.0f);
    ImGui::EndDisabled();
    UiStyle::setItemTooltip("The upper tone plays from this key up, the lower tone below it (C2-C#7)");
    ImGui::SameLine(0.0f, fs);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Balance");
    ImGui::SameLine();
    slider("##balance", Balance, 0, 100, 0, [](int v) { return "L" + std::to_string(100 - v) + " U" + std::to_string(v); }, fs * 6.5f);
    UiStyle::setItemTooltip("Tone Balance: the volumes of the lower and upper tones, as the unit shows them. They always add up to 100; at 50 "
                          "both are equal.");

    // Reverb
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Reverb");
    ImGui::SameLine();
    const int reverbMode = patchValue(PatchReverbMode);
    ImGui::SetNextItemWidth(fs * 7.0f);
    if (ImGui::BeginCombo("##reverbtype", reverbMode >= 0 && reverbMode < 9 ? kReverbD[reverbMode] : "?")) {
        for (int i = 0; i < 9; i++) {
            if (ImGui::Selectable(kReverbD[i], i == reverbMode)) setPatchValue(host, PatchReverbMode, i);
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    slider("##reverbtime", PatchReverbTime, 1, 8, -1, [](int v) { return "time " + std::to_string(v); }, fs * 5.0f);
    ImGui::SameLine();
    slider("##reverblevel", PatchReverbLevel, 0, 7, 0, [](int v) { return "level " + std::to_string(v); }, fs * 5.0f);

    // The two tones
    struct ToneRow {
        const char* label;
        bool upper;
        int group, shift, fine, bend, assign, reverb;
    };
    const ToneRow rows[2] = {{"Upper", true, UpperToneGroup, UpperKeyShift, UpperFineTune, UpperBender, UpperAssign, UpperReverb},
                             {"Lower", false, LowerToneGroup, LowerKeyShift, LowerFineTune, LowerBender, LowerAssign, LowerReverb}};
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("tones", 8, flags)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, fs * 3.0f);
        ImGui::TableSetupColumn("Tone", ImGuiTableColumnFlags_WidthFixed, fs * 9.5f);
        ImGui::TableSetupColumn("Shift", ImGuiTableColumnFlags_WidthFixed, fs * 3.4f);
        ImGui::TableSetupColumn("Fine", ImGuiTableColumnFlags_WidthFixed, fs * 3.4f);
        ImGui::TableSetupColumn("Bend", ImGuiTableColumnFlags_WidthFixed, fs * 3.0f);
        ImGui::TableSetupColumn("Assign", ImGuiTableColumnFlags_WidthFixed, fs * 4.2f);
        ImGui::TableSetupColumn("Reverb", ImGuiTableColumnFlags_WidthFixed, fs * 3.2f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, fs * 3.0f);
        ImGui::TableHeadersRow();
        for (const ToneRow& row : rows) {
            ImGui::TableNextRow();
            ImGui::PushID(row.label);
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(row.label);
            ImGui::TableNextColumn();
            const int group = patchValue(row.group), toneNumber = patchValue(row.group + 1);
            const std::string tone = group >= 0 && toneNumber >= 0 ? toneLabel(host, group, toneNumber) : std::string("?");
            ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
            if (ImGui::Button((tone + "##tone").c_str(), ImVec2(-FLT_MIN, 0.0f))) ImGui::OpenPopup("tones");
            ImGui::PopStyleVar();
            int chosenGroup = 0, chosenNumber = 0;
            if (tonePopup(host, group, toneNumber, chosenGroup, chosenNumber)) {
                setPatchTone(host, row.upper, chosenGroup, chosenNumber);
                (row.upper ? events.upperToneChanged : events.lowerToneChanged) = true;
            }
            ImGui::TableNextColumn();
            slider("##shift", row.shift, -24, 24, 24, signedText, -FLT_MIN);
            ImGui::TableNextColumn();
            slider("##fine", row.fine, -50, 50, 50, signedText, -FLT_MIN);
            ImGui::TableNextColumn();
            slider("##bend", row.bend, 0, 24, 0, number, -FLT_MIN);
            ImGui::TableNextColumn();
            const int assign = patchValue(row.assign);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##assign", assign >= 0 && assign < 4 ? kAssign[assign] : "?", ImGuiComboFlags_NoArrowButton)) {
                for (int i = 0; i < 4; i++) {
                    if (ImGui::Selectable(kAssign[i], i == assign)) setPatchValue(host, row.assign, i);
                }
                ImGui::EndCombo();
            }
            UiStyle::setItemTooltip("%s", kAssignHelp);
            ImGui::TableNextColumn();
            const int reverb = patchValue(row.reverb);
            static const char* const onOff[2] = {"Off", "On"};
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##reverb", reverb >= 0 && reverb < 2 ? onOff[reverb] : "?", ImGuiComboFlags_NoArrowButton)) {
                for (int i = 0; i < 2; i++) {
                    if (ImGui::Selectable(onOff[i], i == reverb)) setPatchValue(host, row.reverb, i);
                }
                ImGui::EndCombo();
            }
            ImGui::TableNextColumn();
            if (ImGui::Button("Edit", ImVec2(-FLT_MIN, 0.0f))) events.editTone = row.upper ? 0 : 1;
            UiStyle::setItemTooltip("Edits the %s tone on the Tone tab (part %d's tone)", row.upper ? "upper" : "lower", row.upper ? 1 : 2);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // Writing, like the unit's patch write
    auto patchLabel = [&](int patch) {
        const std::string patchTitle = patchName(patch);
        return timbreCode(patch) + (patchTitle.empty() ? std::string() : " " + patchTitle);
    };
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Write the patch to");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fs * 10.0f);
    if (ImGui::BeginCombo("##patchtarget", patchLabel(patchWriteTarget_).c_str(), ImGuiComboFlags_HeightLarge)) {
        for (int patch = 0; patch < kPatches; patch++) {
            if (ImGui::Selectable(patchLabel(patch).c_str(), patch == patchWriteTarget_)) patchWriteTarget_ = patch;
            if (patch == patchWriteTarget_ && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Write##patch")) writePatch(host, patchWriteTarget_);
    UiStyle::setItemTooltip("The unit's patch write: stores the performance patch as this patch (in performance mode)");

    // Patch memory: a click selects the patch, as a program change on the performance channel does.
    ImGui::SeparatorText("Patches (click one to play it)");
    if (ImGui::BeginTable("patches", 4, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_ScrollY, ImVec2(0.0f, ImGui::GetContentRegionAvail().y))) {
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
        for (int patch = 0; patch < kPatches; patch++) {
            ImGui::TableNextColumn();
            ImGui::PushID(patch);
            if (ImGui::Selectable(patchLabel(patch).c_str(), patch == currentPatch_) && selectPatch(host, patch)) {
                events.upperToneChanged = events.lowerToneChanged = true;
            }
            ImGui::PopID();
        }
        ImGui::PopStyleVar();
        ImGui::EndTable();
    }
}

void UnitSetup::drawRhythm(Host& host, double now) {
    const float fs = ImGui::GetFontSize();
    const bool d110 = model_ == Tone::Model::D110;
    const bool mt32 = model_ == Tone::Model::MT32;
    if (ImGui::Button("Read from the unit")) requestAll(host);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Rhythm part level");
    ImGui::SameLine();
    {
        const int value = partValue(kRhythmPart, Level);
        int level = std::max(value, 0);
        ImGui::SetNextItemWidth(fs * 7.0f);
        if (ImGui::SliderInt("##rhythmlevel", &level, 0, 100, value >= 0 ? "%d" : "?", ImGuiSliderFlags_AlwaysClamp)) {
            setPartValue(host, kRhythmPart, Level, level);
        }
    }
    ImGui::SameLine();
    const uint8_t channel = channels_[kRhythmPart];
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("channel");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fs * 3.0f);
    if (ImGui::BeginCombo("##rhythmchannel", channel < 16 ? std::to_string(channel + 1).c_str() : "off", ImGuiComboFlags_HeightLarge)) {
        for (int choice = 0; choice <= 16; choice++) {
            if (ImGui::Selectable(choice < 16 ? std::to_string(choice + 1).c_str() : "off", choice == channel)) setChannel(host, kRhythmPart, choice);
        }
        ImGui::EndCombo();
    }
    UiStyle::setItemTooltip("The rhythm part's MIDI channel: Play sends its keys there. On a D-10/D-20, set it as in the unit's MIDI "
                          "function menu (usually 10).");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fs * 7.5f);
    ImGui::SliderInt("##velocity", &auditionVelocity_, 1, 127, "velocity %d", ImGuiSliderFlags_AlwaysClamp);
    UiStyle::setItemTooltip("Velocity of the Play buttons");

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY;
    if (!ImGui::BeginTable("rhythm", 6, flags, ImVec2(0.0f, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, fs * 4.0f);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, fs * 2.6f);
    ImGui::TableSetupColumn("Tone", ImGuiTableColumnFlags_WidthFixed, fs * 10.0f);
    ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, fs * 6.0f);
    ImGui::TableSetupColumn("Pan", ImGuiTableColumnFlags_WidthFixed, fs * 4.5f);
    ImGui::TableSetupColumn(d110 ? "Output" : "Reverb", ImGuiTableColumnFlags_WidthFixed, fs * 5.0f);
    ImGui::TableHeadersRow();
    for (int key = kFirstRhythmKey; key < kFirstRhythmKey + kRhythmKeys; key++) {
        ImGui::TableNextRow();
        ImGui::PushID(key);
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s  %d", Tone::noteName(key).c_str(), key);

        ImGui::TableNextColumn();
        ImGui::BeginDisabled(channel >= 16);
        if (ImGui::SmallButton("Play")) playRhythmKey(host, key, now);
        ImGui::EndDisabled();
        if (channel < 16) UiStyle::setItemTooltip("Plays key %d on channel %d", key, channel + 1);

        ImGui::TableNextColumn();
        const int tone = rhythmValue(key, RhythmTone);
        const std::string label = rhythmToneLabel(host, tone);
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
        if (ImGui::Button((label + "##tone").c_str(), ImVec2(-FLT_MIN, 0.0f))) ImGui::OpenPopup("rhythmtones");
        ImGui::PopStyleVar();
        if (ImGui::BeginPopup("rhythmtones")) {
            const bool offShown = !mt32;
            if (offShown && ImGui::Selectable("OFF", tone == 127)) {
                setRhythmValue(host, key, RhythmTone, 127);
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::BeginTabBar("groups")) {
                for (int group = 0; group < 2; group++) {
                    const bool rhythmGroup = group == 0;
                    const char* title = mt32 ? (rhythmGroup ? "R (rhythm)" : "M (memory)") : (rhythmGroup ? "r (rhythm)" : "i (internal)");
                    const ImGuiTabItemFlags tabFlags = ImGui::IsWindowAppearing() && ((tone >= 64) == rhythmGroup) ? ImGuiTabItemFlags_SetSelected : 0;
                    if (!ImGui::BeginTabItem(title, nullptr, tabFlags)) continue;
                    if (ImGui::BeginTable("grid", 4, ImGuiTableFlags_SizingFixedFit)) {
                        const int count = rhythmGroup && !mt32 ? 63 : 64;  // r64 is OFF on the D-series
                        for (int n = 0; n < count; n++) {
                            const int value = (rhythmGroup ? 64 : 0) + n;
                            ImGui::TableNextColumn();
                            const std::string item = rhythmToneLabel(host, value) + "##" + std::to_string(value);
                            if (ImGui::Selectable(item.c_str(), value == tone, 0, ImVec2(fs * 9.0f, 0.0f))) {
                                setRhythmValue(host, key, RhythmTone, value);
                                ImGui::CloseCurrentPopup();
                            }
                        }
                        ImGui::EndTable();
                    }
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            ImGui::EndPopup();
        }

        ImGui::TableNextColumn();
        const int level = rhythmValue(key, RhythmLevel);
        int shownLevel = std::max(level, 0);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderInt("##level", &shownLevel, 0, 100, level >= 0 ? "%d" : "?", ImGuiSliderFlags_AlwaysClamp)) {
            setRhythmValue(host, key, RhythmLevel, shownLevel);
        }

        ImGui::TableNextColumn();
        const int pan = rhythmValue(key, RhythmPanpot);
        int shownPan = pan < 0 ? 7 : (mt32 ? 14 - pan : pan);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderInt("##pan", &shownPan, 0, 14, pan >= 0 ? panText(shownPan).c_str() : "?", ImGuiSliderFlags_AlwaysClamp)) {
            setRhythmValue(host, key, RhythmPanpot, mt32 ? 14 - shownPan : shownPan);
        }

        ImGui::TableNextColumn();
        const int output = rhythmValue(key, RhythmOutput);
        static const char* const reverb[2] = {"Off", "On"};
        const char* const* names = d110 ? kOutputs : reverb;
        const int count = d110 ? 8 : 2;
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##output", output >= 0 && output < count ? names[output] : "?", ImGuiComboFlags_NoArrowButton)) {
            for (int i = 0; i < count; i++) {
                if (ImGui::Selectable(names[i], i == output)) setRhythmValue(host, key, RhythmOutput, i);
            }
            ImGui::EndCombo();
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void UnitSetup::drawSystem(Host& host) {
    const float fs = ImGui::GetFontSize();
    const bool mt32 = model_ == Tone::Model::MT32;
    const bool d20 = model_ == Tone::Model::D20;
    if (ImGui::Button("Read from the unit")) requestAll(host);
    if (d20) {
        ImGui::SameLine();
        ImGui::TextDisabled("A D-20 may take the system area only in data transfer mode (not confirmed).");
    }
    const float width = fs * 14.0f;

    ImGui::SeparatorText("Tuning and reverb");
    {
        const int value = systemValue(MasterTune);
        int tune = value >= 0 ? value : 64;
        ImGui::SetNextItemWidth(width);
        if (ImGui::SliderInt("Master tune", &tune, 0, 127, value >= 0 ? masterTuneText(tune).c_str() : "?", ImGuiSliderFlags_AlwaysClamp)) {
            setSystemValue(host, MasterTune, tune);
        }
        UiStyle::setItemTooltip(mt32 ? "A4 from 427.5 to 452.7 Hz (442.0 Hz at the MT-32's default, 74)"
                                   : "A4 from 427.5 to 452.7 Hz (440.0 Hz at the D-110's and D-10/D-20's factory default, 64)");
    }
    {
        const int value = systemValue(ReverbMode);
        const char* const* names = mt32 ? kReverbMt32 : kReverbD;
        const int count = mt32 ? 4 : 9;
        ImGui::SetNextItemWidth(width);
        if (ImGui::BeginCombo("Reverb type", value >= 0 && value < count ? names[value] : "?")) {
            for (int i = 0; i < count; i++) {
                if (ImGui::Selectable(names[i], i == value)) setSystemValue(host, ReverbMode, i);
            }
            ImGui::EndCombo();
        }
    }
    for (int offset : {int(ReverbTime), int(ReverbLevel)}) {
        const int value = systemValue(offset);
        int shown = offset == ReverbTime ? std::max(value, 0) + 1 : std::max(value, 0);
        ImGui::SetNextItemWidth(width);
        const int low = offset == ReverbTime ? 1 : 0;
        if (ImGui::SliderInt(offset == ReverbTime ? "Reverb time" : "Reverb level", &shown, low, low + 7, value >= 0 ? "%d" : "?",
                             ImGuiSliderFlags_AlwaysClamp)) {
            setSystemValue(host, offset, shown - low);
        }
    }
    if (mt32) {
        const int value = systemValue(MasterVolume);
        int volume = std::max(value, 0);
        ImGui::SetNextItemWidth(width);
        if (ImGui::SliderInt("Master volume", &volume, 0, 100, value >= 0 ? "%d" : "?", ImGuiSliderFlags_AlwaysClamp)) {
            setSystemValue(host, MasterVolume, volume);
        }
    }

    ImGui::SeparatorText("Partial reserves");
    int total = 0;
    bool known = true;
    for (int part = 0; part < kParts; part++) {
        const int value = systemValue(Reserves + part);
        known = known && value >= 0;
        total += std::max(value, 0);
    }
    ImGui::BeginDisabled(!known);
    // Labels and controls in rows of their own, so that all columns line up (a row aligns its text to the frames in it).
    if (ImGui::BeginTable("reserves", kParts, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableNextRow();
        for (int part = 0; part < kParts; part++) {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(partName(part).c_str());
        }
        ImGui::TableNextRow();
        for (int part = 0; part < kParts; part++) {
            ImGui::TableNextColumn();
            ImGui::PushID(part);
            int reserve = std::max(systemValue(Reserves + part), 0);
            if (ImGui::VSliderInt("##reserve", ImVec2(fs * 1.8f, fs * 6.0f), &reserve, 0, 32, known ? "%d" : "?", ImGuiSliderFlags_AlwaysClamp)) {
                setReserve(host, part, reserve);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndDisabled();
    if (known) {
        ImGui::TextDisabled("%d of 32 partials reserved. The units take the reserves as a package of all nine.", total);
    } else {
        ImGui::TextDisabled("Read the unit's setup first: the reserves go to the unit as a package of all nine.");
    }

    ImGui::SeparatorText("MIDI channels");
    if (d20) {
        ImGui::TextDisabled("A D-20's part channels are set in its MIDI function menu. The editor plays on these:");
    } else if (channelsMissing_) {
        ImGui::PushStyleColor(ImGuiCol_Text, UiStyle::kErrorColor);
        ImGui::TextWrapped("The unit's system area holds no part channels (all nine read the same), so it is probably a D-10 or "
                           "D-20: choose D-10/D-20 under Unit, and set the channels here as on its panel.");
        ImGui::PopStyleColor();
    }
    if (ImGui::BeginTable("channels", kParts, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableNextRow();
        for (int part = 0; part < kParts; part++) {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(partName(part).c_str());
        }
        ImGui::TableNextRow();
        for (int part = 0; part < kParts; part++) {
            ImGui::TableNextColumn();
            ImGui::PushID(part);
            const uint8_t channel = channels_[size_t(part)];
            ImGui::SetNextItemWidth(fs * 2.6f);
            if (ImGui::BeginCombo("##channel", channel < 16 ? std::to_string(channel + 1).c_str() : "off",
                                  ImGuiComboFlags_HeightLarge | ImGuiComboFlags_NoArrowButton)) {
                for (int choice = 0; choice <= 16; choice++) {
                    if (ImGui::Selectable(choice < 16 ? std::to_string(choice + 1).c_str() : "off", choice == channel)) setChannel(host, part, choice);
                }
                ImGui::EndCombo();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    // The usual layouts at once: the D-series' own, and the MT-32's (as MT32Translator uses them).
    for (int layout = 0; layout < 2; layout++) {
        if (layout > 0) ImGui::SameLine();
        if (ImGui::SmallButton(layout == 0 ? "1-8, R 10" : "2-9, R 10")) {
            for (int part = 0; part < kRhythmPart; part++) setChannel(host, part, part + layout);
            setChannel(host, kRhythmPart, 9);
        }
    }
}
