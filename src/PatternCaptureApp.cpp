#include "PatternCaptureApp.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>
#include <iterator>

#include "Platform.h"
#include "UiStyle.h"
#include "imgui.h"

using UiStyle::helpMarker;
using UiStyle::kErrorColor;
using UiStyle::kOkColor;

namespace {

std::string slotName(int slot) {
    return d20PatternName(slot) + " " + kD20PresetPatternNames[slot & 31];
}

std::string beatsName(int beats) {
    return std::to_string(beats) + "/4";
}

std::string noteName(int key) {
    static const char* const names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return std::string(names[key % 12]) + std::to_string(key / 12 - 1);  // 60 = C4
}

std::vector<uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool writeFile(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    return bool(out);
}

}  // namespace

PatternCaptureApp::PatternCaptureApp() : receiver_(*this) {}

PatternCaptureApp::~PatternCaptureApp() {
    inputs_.reset();  // No more callbacks into a half-destroyed app
}

void PatternCaptureApp::init(const PatternCaptureOptions& options) {
    options_ = options;
    if (!options_.settingsFile.empty()) settings_.load(options_.settingsFile);
    loadSettings();
    loadSession();
    if (options_.enableMidi) {
        inputs_.reset(new MidiInputManager(receiver_));
        refreshPorts();
        if (!inputName_.empty()) openInput(inputName_);
    }
}

void PatternCaptureApp::shutdown() {
    inputs_.reset();
    saveSettings();
}

void PatternCaptureApp::loadSettings() {
    inputName_ = settings_.getString("input");
    capture_.setChannel(std::clamp(settings_.getInt("rhythm_channel", 10), 1, 16) - 1);
    autoStore_ = settings_.getBool("store_at_stop", true);
    advance_ = settings_.getBool("select_next", true);
    slot_ = std::clamp(settings_.getInt("slot", 0), 0, kD20PresetPatterns - 1);
}

void PatternCaptureApp::saveSettings() {
    settings_.set("input", inputName_);
    settings_.set("rhythm_channel", capture_.channel() + 1);
    settings_.set("store_at_stop", autoStore_);
    settings_.set("select_next", advance_);
    settings_.set("slot", slot_);
    if (!options_.settingsFile.empty()) settings_.save(options_.settingsFile);
}

void PatternCaptureApp::refreshPorts() {
    ports_ = inputs_ ? inputs_->listPorts() : std::vector<std::string>();
}

void PatternCaptureApp::openInput(const std::string& name) {
    inputError_.clear();
    if (!inputs_) return;
    inputs_->closeAll();
    inputName_ = name;
    if (!name.empty() && !inputs_->open(name, inputError_)) inputName_.clear();
    saveSettings();
}

void PatternCaptureApp::loadSession() {
    if (options_.sessionFile.empty()) return;
    const std::vector<uint8_t> data = readFile(options_.sessionFile);
    if (!data.empty()) patterns_ = readD20PatternDump(data.data(), data.size());
}

void PatternCaptureApp::saveSession() {
    if (options_.sessionFile.empty()) return;
    const std::vector<uint8_t> dump = d20PatternDump(patterns_);
    if (dump.empty()) {
        std::error_code error;
        std::filesystem::remove(options_.sessionFile, error);
    } else if (!writeFile(options_.sessionFile, dump)) {
        status_ = "Cannot write " + Platform::toUtf8(options_.sessionFile);
    }
}

bool PatternCaptureApp::loadDump(const std::filesystem::path& path, std::string& error) {
    const std::vector<uint8_t> data = readFile(path);
    int found = 0;
    const std::array<D20Pattern, kD20PresetPatterns> loaded = readD20PatternDump(data.data(), data.size(), &found);
    if (found == 0) {
        error = Platform::toUtf8(path.filename()) + " holds no patterns in P-51-P-88";
        return false;
    }
    for (size_t n = 0; n < loaded.size(); n++) {
        if (loaded[n].present) patterns_[n] = loaded[n];
    }
    saveSession();
    return true;
}

void PatternCaptureApp::openFile(const std::filesystem::path& path) {
    std::string error;
    status_ = loadDump(path, error) ? "Opened " + Platform::toUtf8(path.filename()) : error;
}

void PatternCaptureApp::receive(uint32_t message) {
    receive(message, std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void PatternCaptureApp::receive(uint32_t message, double seconds) {
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (queue_.size() < 100000) queue_.emplace_back(message, seconds);
}

int PatternCaptureApp::nextSlot(int from) const {
    for (int i = 1; i <= kD20PresetPatterns; i++) {
        const int slot = (from + i) % kD20PresetPatterns;
        if (!patterns_[size_t(slot)].present) return slot;
    }
    return (from + 1) % kD20PresetPatterns;
}

void PatternCaptureApp::takeFinished() {
    take_ = capture_.take();
    takeShown_ = true;
    takeBeats_ = PatternCapture::suggestedBeats(take_);
    const std::vector<int> repeating = PatternCapture::repeatingBeats(take_);
    const bool checked = std::find(repeating.begin(), repeating.end(), takeBeats_) != repeating.end();
    const D20Pattern pattern = PatternCapture::pattern(take_, takeBeats_);
    if (pattern.notes.empty()) {
        status_ = "No notes on MIDI channel " + std::to_string(capture_.channel() + 1) + " in the first bar.";
    } else if (!take_.aligned) {
        status_ = "Not stored: it began with Continue (START alone), so its first step is unknown. Hold STOP and press START.";
    } else if (!checked) {
        status_ = PatternCapture::wholeBars(take_, takeBeats_) < 2 ? "Not stored: too short to compare two bars. Let it play longer."
                                                                    : "Not stored: its bars differ. Choose the length and store it, or record it again.";
    } else if (autoStore_) {
        store(slot_);
    } else {
        status_ = "Recorded: check the length and store it.";
    }
}

void PatternCaptureApp::store(int slot) {
    const D20Pattern pattern = PatternCapture::pattern(take_, takeBeats_);
    patterns_[size_t(slot)] = pattern;
    takeShown_ = false;
    status_ = "Stored in " + slotName(slot) + ": " + beatsName(pattern.beats) + ", " + std::to_string(pattern.notes.size()) + " notes.";
    saveSession();
    if (advance_) slot_ = nextSlot(slot);
    saveSettings();
}

void PatternCaptureApp::frame() {
    std::vector<std::pair<uint32_t, double>> messages;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        messages.swap(queue_);
    }
    for (const auto& message : messages) {
        const uint32_t m = message.first;
        const bool note = (m & 0xF0) == 0x90 && (m & 0x0F) == uint32_t(capture_.channel()) && ((m >> 16) & 0x7F) > 0;
        if (note && !capture_.playing()) strayNotes_ = true;
        capture_.onMessage(m, message.second);
        if (capture_.playing()) strayNotes_ = false;
        if (capture_.takeFinished()) takeFinished();
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("PatternCapture", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    drawMenu();
    const float leftWidth = std::min(ImGui::GetFontSize() * 22.0f, ImGui::GetContentRegionAvail().x * 0.4f);
    ImGui::BeginChild("setup", ImVec2(leftWidth, 0.0f), ImGuiChildFlags_Borders);
    drawInput();
    drawFile();
    drawSteps();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("patterns", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    drawSlots();
    drawTake();
    ImGui::EndChild();
    ImGui::End();
}

void PatternCaptureApp::drawMenu() {
    if (!ImGui::BeginMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open dump...")) {
            std::filesystem::path path;
            if (Platform::openFileDialog(Platform::FileKind::Sysex, path)) openFile(path);
        }
        if (ImGui::MenuItem("Save dump...", nullptr, false, std::any_of(patterns_.begin(), patterns_.end(), [](const D20Pattern& p) { return p.present; }))) {
            std::filesystem::path path;
            if (Platform::saveFileDialog(Platform::FileKind::Sysex, "D-20 preset patterns.syx", path)) {
                status_ = writeFile(path, d20PatternDump(patterns_)) ? "Saved " + Platform::toUtf8(path.filename())
                                                                    : "Cannot write " + Platform::toUtf8(path);
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) quitRequested_ = true;
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
}

void PatternCaptureApp::drawInput() {
    if (!ImGui::CollapsingHeader("From the D-20", ImGuiTreeNodeFlags_DefaultOpen)) return;
    const float fontSize = ImGui::GetFontSize();
    ImGui::SetNextItemWidth(-fontSize * 5.0f);
    if (ImGui::BeginCombo("##input", inputName_.empty() ? "(none)" : inputName_.c_str())) {
        if (ImGui::Selectable("(none)", inputName_.empty())) openInput(std::string());
        for (const std::string& name : ports_) {
            if (ImGui::Selectable(name.c_str(), name == inputName_) && name != inputName_) openInput(name);
        }
        ImGui::EndCombo();
    }
    UiStyle::setItemTooltip("The MIDI input the D-20's MIDI OUT is connected to.");
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) refreshPorts();
    if (!inputs_) ImGui::TextDisabled("(MIDI input is off)");
    else if (ports_.empty()) ImGui::TextDisabled("No MIDI inputs found.");
    if (!inputError_.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(kErrorColor, "%s", inputError_.c_str());
        ImGui::PopTextWrapPos();
    }
    int channel = capture_.channel() + 1;
    ImGui::SetNextItemWidth(fontSize * 7.0f);
    if (ImGui::InputInt("Rhythm channel", &channel)) {
        capture_.setChannel(std::clamp(channel, 1, 16) - 1);
        saveSettings();
    }
    helpMarker("The D-20 sends the pattern's notes on its rhythm part's MIDI channel (10 unless changed).");
    if (ImGui::Checkbox("Store at STOP", &autoStore_)) saveSettings();
    helpMarker("A take goes into the selected slot when its bars repeat at the length chosen for it: 4/4 if the notes "
               "repeat at 4/4, else the shortest length they repeat at. Check the length of patterns that are not 4/4 "
               "(probably the Jazz Waltz), and store them again with the length changed if needed.");
    if (ImGui::Checkbox("Then select the next empty slot", &advance_)) saveSettings();

    ImGui::PushTextWrapPos(0.0f);
    if (capture_.playing()) {
        const int clocks = capture_.clocks();
        ImGui::TextColored(kOkColor, "Recording: bar %d, beat %d (in 4/4), %.0f BPM", clocks / 96 + 1, clocks % 96 / kD20StepsPerQuarter + 1,
                           capture_.bpm());
    } else if (strayNotes_) {
        ImGui::TextColored(kErrorColor, "Rhythm notes arrive, but no Start: hold STOP and press START on the D-20.");
    } else if (!inputName_.empty() && !capture_.hasClock()) {
        ImGui::TextDisabled("No MIDI clock from the D-20 yet. Its Clock Mode must be INTERNAL (see How to).");
    } else if (!inputName_.empty()) {
        ImGui::TextDisabled("Stopped. Hold STOP and press START on the D-20 to record.");
    }
    if (!status_.empty()) ImGui::TextUnformatted(status_.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
}

void PatternCaptureApp::drawSteps() {
    if (!ImGui::CollapsingHeader("How to", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextWrapped("1. On the D-20, set the Clock Mode to INTERNAL (hold TEMPO and press DISPLAY; its manual p.165), then "
                       "push RHYTHM and choose Pattern Play with DISPLAY.");
    ImGui::TextWrapped("2. Choose the pattern with BANK and NUMBER, and the same slot here (on the right).");
    ImGui::TextWrapped("3. Hold STOP and press START: the pattern plays from its first beat (START alone continues from "
                       "where it stopped). Let it play two bars or more, four for patterns longer than 4/4.");
    ImGui::TextWrapped("4. Press STOP. The take appears on the right; each MIDI clock is one of the pattern's steps.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
}

void PatternCaptureApp::drawFile() {
    if (!ImGui::CollapsingHeader("Patterns", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::AlignTextToFramePadding();
    const int captured = int(std::count_if(patterns_.begin(), patterns_.end(), [](const D20Pattern& p) { return p.present; }));
    ImGui::Text("%d of %d captured", captured, kD20PresetPatterns);
    ImGui::BeginDisabled(captured == 0);
    if (ImGui::Button("Save dump...")) {
        std::filesystem::path path;
        if (Platform::saveFileDialog(Platform::FileKind::Sysex, "D-20 preset patterns.syx", path)) {
            status_ = writeFile(path, d20PatternDump(patterns_)) ? "Saved " + Platform::toUtf8(path.filename())
                                                                : "Cannot write " + Platform::toUtf8(path);
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Open dump...")) {
        std::filesystem::path path;
        if (Platform::openFileDialog(Platform::FileKind::Sysex, path)) openFile(path);
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("The dump holds the patterns in P-51-P-88, as a D-20 sends its own. In d110emu, load it with the "
                        "Patterns tab's Load presets: the patterns become P-11-P-48. Sent to a D-20 (Memory Protect "
                        "off), it would overwrite P-51-P-88 with copies of the presets.");
    ImGui::TextDisabled("The captures are kept between sessions (patterncapture-session.syx).");
    ImGui::PopTextWrapPos();
}

void PatternCaptureApp::drawSlots() {
    const float fontSize = ImGui::GetFontSize();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float width = std::max(fontSize * 5.0f, std::floor((ImGui::GetContentRegionAvail().x - 7.0f * style.ItemSpacing.x) / 8.0f));
    const ImVec2 size(width, ImGui::GetTextLineHeight() * 2.0f + style.FramePadding.y * 2.0f);
    for (int bank = 0; bank < 4; bank++) {
        for (int number = 0; number < 8; number++) {
            const int slot = bank * 8 + number;
            const D20Pattern& pattern = patterns_[size_t(slot)];
            if (number > 0) ImGui::SameLine();
            ImGui::PushID(slot);
            int colors = 0;
            if (slot == slot_) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                colors++;
            } else if (pattern.present) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.42f, 0.26f, 1.0f));
                colors++;
            }
            const std::string label = d20PatternName(slot) + (pattern.present ? " " + beatsName(pattern.beats) : "") + "\n" + kD20PresetPatternShortNames[slot];
            if (ImGui::Button(label.c_str(), size)) {
                slot_ = slot;
                saveSettings();
            }
            ImGui::PopStyleColor(colors);
            if (pattern.present) {
                UiStyle::setItemTooltip("%s: %s, %zu notes. Right-click to clear.", slotName(slot).c_str(), beatsName(pattern.beats).c_str(),
                                      pattern.notes.size());
            } else {
                UiStyle::setItemTooltip("%s: not captured yet", slotName(slot).c_str());
            }
            if (pattern.present && ImGui::BeginPopupContextItem("slot")) {
                if (ImGui::MenuItem("Clear")) {
                    patterns_[size_t(slot)] = D20Pattern();
                    saveSession();
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
    }
    ImGui::Separator();
}

void PatternCaptureApp::drawTake() {
    const bool recording = capture_.playing();
    if (!recording && !takeShown_) {
        const D20Pattern& pattern = patterns_[size_t(slot_)];
        if (pattern.present) {
            ImGui::Text("%s: %s, %zu notes", slotName(slot_).c_str(), beatsName(pattern.beats).c_str(), pattern.notes.size());
            drawGrid(pattern, "stored");
        } else {
            ImGui::TextDisabled("%s: not captured yet. Play it on the D-20 (see How to).", slotName(slot_).c_str());
        }
        return;
    }
    const PatternCapture::Take take = recording ? capture_.take() : take_;
    if (recording) takeBeats_ = PatternCapture::suggestedBeats(take);
    const std::vector<int> repeating = PatternCapture::repeatingBeats(take);
    ImGui::Text("%s: %d clocks (%.1f bars of 4/4), %zu notes, %.0f BPM", recording ? "Recording" : "Take", take.clocks, take.clocks / 96.0,
                take.notes.size(), take.bpm);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Length");
    ImGui::BeginDisabled(recording);
    for (int beats = 1; beats <= 8; beats++) {
        ImGui::SameLine();
        const bool repeats = std::find(repeating.begin(), repeating.end(), beats) != repeating.end();
        if (repeats) ImGui::PushStyleColor(ImGuiCol_Text, kOkColor);
        ImGui::RadioButton(beatsName(beats).c_str(), &takeBeats_, beats);
        if (repeats) ImGui::PopStyleColor();
        UiStyle::setItemTooltip(repeats ? "The notes repeat at this length" : "The take does not show that the notes repeat at this length");
    }
    ImGui::EndDisabled();
    ImGui::PushTextWrapPos(0.0f);
    if (!take.aligned) {
        ImGui::TextColored(kErrorColor, "Began with Continue (START alone): the first step may not be the pattern's. Hold STOP and press START.");
    }
    if (std::find(repeating.begin(), repeating.end(), takeBeats_) == repeating.end() && !recording) {
        ImGui::TextColored(kErrorColor, "%s", PatternCapture::wholeBars(take, takeBeats_) < 2 ? "Fewer than two bars of this length: they cannot be compared."
                                                                                               : "The bars of this length differ.");
    }
    if (take.outOfRange > 0) ImGui::TextColored(kErrorColor, "%d notes outside keys 24-108 were left out.", take.outOfRange);
    ImGui::PopTextWrapPos();
    const D20Pattern pattern = PatternCapture::pattern(take, takeBeats_);
    if (!recording) {
        if (ImGui::Button(("Store in " + slotName(slot_)).c_str())) store(slot_);
        ImGui::SameLine();
        if (ImGui::Button("Discard")) takeShown_ = false;
        ImGui::SameLine();
        ImGui::TextDisabled("%zu notes in the first bar", pattern.notes.size());
    }
    drawGrid(pattern, "take");
}

void PatternCaptureApp::drawGrid(const D20Pattern& pattern, const char* id) {
    std::vector<int> keys;
    for (const D20Pattern::Note& note : pattern.notes) {
        if (std::find(keys.begin(), keys.end(), note.key) == keys.end()) keys.push_back(note.key);
    }
    std::sort(keys.begin(), keys.end(), std::greater<int>());
    if (keys.empty()) {
        ImGui::TextDisabled("(no notes)");
        return;
    }
    const float fontSize = ImGui::GetFontSize();
    const int steps = pattern.beats * kD20StepsPerQuarter;
    const float labelWidth = fontSize * 3.6f;
    const float cell = std::max(2.0f, std::floor((ImGui::GetContentRegionAvail().x - labelWidth) / float(steps)));
    const float row = std::floor(ImGui::GetTextLineHeight() + 2.0f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size(labelWidth + cell * float(steps), row * float(keys.size()));
    ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 grid(origin.x + labelWidth, origin.y);
    for (size_t r = 0; r < keys.size(); r++) {
        const float y = grid.y + row * float(r);
        if (r % 2 == 0) draw->AddRectFilled(ImVec2(grid.x, y), ImVec2(grid.x + cell * float(steps), y + row), IM_COL32(255, 255, 255, 12));
        const std::string label = std::to_string(keys[r]) + " " + noteName(keys[r]);
        draw->AddText(ImVec2(origin.x, y + 1.0f), ImGui::GetColorU32(ImGuiCol_TextDisabled), label.c_str());
    }
    for (int step = 0; step <= steps; step += 6) {  // 16ths, beats stronger
        const float x = grid.x + cell * float(step);
        draw->AddLine(ImVec2(x, grid.y), ImVec2(x, grid.y + size.y), step % kD20StepsPerQuarter == 0 ? IM_COL32(255, 255, 255, 70) : IM_COL32(255, 255, 255, 22));
    }
    const D20Pattern::Note* under = nullptr;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    for (const D20Pattern::Note& note : pattern.notes) {
        const size_t r = size_t(std::find(keys.begin(), keys.end(), note.key) - keys.begin());
        const float x = grid.x + cell * float(note.step), y = grid.y + row * float(r);
        const float level = float(note.velocity) / 127.0f;
        const ImU32 color = ImGui::ColorConvertFloat4ToU32(ImVec4(0.35f + 0.6f * level, 0.55f + 0.4f * level, 0.25f, 1.0f));
        const float width = std::max(cell, 3.0f);
        draw->AddRectFilled(ImVec2(x, y + 2.0f), ImVec2(x + width, y + row - 2.0f), color);
        if (hovered && mouse.x >= x && mouse.x < x + width && mouse.y >= y && mouse.y < y + row) under = &note;
    }
    if (under != nullptr) {
        UiStyle::setTooltip("Beat %d, step %d: key %d (%s), velocity %d", under->step / kD20StepsPerQuarter + 1, under->step % kD20StepsPerQuarter,
                          under->key, noteName(under->key).c_str(), under->velocity);
    }
}
