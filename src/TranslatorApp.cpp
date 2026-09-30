#include "TranslatorApp.h"

#include <algorithm>

#include "Platform.h"
#include "PresetChoiceTable.h"
#include "UiStyle.h"
#include "imgui.h"

namespace {

constexpr int kPartCount = TranslatorCore::kPartCount;

using UiStyle::helpMarker;
using UiStyle::kErrorColor;

// Tone cache slot 0-63 as the unit shows it: i11-i88.
std::string slotName(int slot) {
    return std::string("i") + char('1' + slot / 8) + char('1' + slot % 8);
}

const char* partLabel(int part) {
    static const char* const labels[kPartCount] = {"1", "2", "3", "4", "5", "6", "7", "8", "R"};
    return labels[part];
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Files

void TranslatorApp::openFile(const std::filesystem::path& path) {
    if (isMidiFile(path)) {
        translateFile(path);
    } else {
        sendSysexFile(path);
    }
}

void TranslatorApp::translateFile(const std::filesystem::path& path) {
    if (!canTranslate(path)) return;
    std::filesystem::path target;
    const Platform::FileKind kind = isMidiFile(path) ? Platform::FileKind::Midi : Platform::FileKind::Sysex;
    if (!Platform::saveFileDialog(kind, translatedName(path), target)) return;
    translateFileTo(path, target);
}

// ---------------------------------------------------------------------------------------------
// UI

void TranslatorApp::frame() {
    update();

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("MT32Translator", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    drawMenu();
    const float leftWidth = std::min(ImGui::GetFontSize() * 27.0f, ImGui::GetContentRegionAvail().x * 0.5f);
    ImGui::BeginChild("settings", ImVec2(leftWidth, 0.0f), ImGuiChildFlags_Borders);
    drawPorts();
    drawUnit();
    drawPresets();
    drawCache();
    drawTiming();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("activity", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    drawActions();
    if (ImGui::BeginTabBar("right")) {
        if (ImGui::BeginTabItem("Monitor", nullptr, rightTab_ == 2 ? ImGuiTabItemFlags_SetSelected : 0)) {
            drawMonitor();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Presets", nullptr, rightTab_ == 3 ? ImGuiTabItemFlags_SetSelected : 0)) {
            drawPresetEditor();
            ImGui::EndTabItem();
        }
        rightTab_ = 0;
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    ImGui::End();
}

void TranslatorApp::drawMenu() {
    if (!ImGui::BeginMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Send SysEx file to the unit...")) {
            std::filesystem::path path;
            if (Platform::openFileDialog(Platform::FileKind::MidiOrSysex, path)) sendSysexFile(path);
        }
        if (ImGui::MenuItem("Translate file...")) {
            std::filesystem::path path;
            if (Platform::openFileDialog(Platform::FileKind::MidiOrSysex, path)) translateFile(path);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) requestQuit();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Unit")) {
        if (ImGui::MenuItem("MT-32 reset (power-on setup)")) pipe_.powerOn();
        if (ImGui::MenuItem("All notes off")) pipe_.allNotesOff();
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
}

void TranslatorApp::drawPorts() {
    if (!ImGui::CollapsingHeader("MIDI ports", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::TextUnformatted("From the MT-32 program");
    helpMarker("The MIDI input(s) the game, sequencer or emulator plays into, as it would play into an MT-32: a USB "
               "MIDI interface from another computer, or a virtual port (loopMIDI on Windows) from software on this "
               "one. An input that is unplugged opens again when it comes back.");
    if (inputPorts_.empty()) ImGui::TextDisabled("  No MIDI inputs found.");
    for (const std::string& name : inputPorts_) {
        bool open = inputs_->isOpen(name);
        if (ImGui::Checkbox((name + "##in").c_str(), &open)) {
            openInput(name, open);
            saveSettings();
        }
        if (open) {
            ImGui::SameLine();
            ImGui::TextDisabled("%u", inputs_->messageCount(name));
        }
    }
    // Chosen, but not there (unplugged, or its program not started): opened when it comes.
    for (const std::string& name : std::vector<std::string>(enabledInputs_)) {
        if (std::find(inputPorts_.begin(), inputPorts_.end(), name) != inputPorts_.end()) continue;
        bool open = true;
        if (ImGui::Checkbox((name + "##gone").c_str(), &open)) {
            openInput(name, false);
            saveSettings();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("not connected");
    }
    const std::string own = ownInputName();
    if (!own.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("Programs on this computer can send to %s (%s).", own.c_str(), ownInputHint().c_str());
        ImGui::PopStyleColor();
    }

    auto outputCombo = [&](const char* label, const std::string& current, const MidiOutputPort& port, bool allowNone,
                           void (TranslatorApp::*open)(const std::string&)) {
        ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 8.5f);
        // A port chosen that is not there (unplugged, or its program not started) is connected when it comes.
        const std::string preview = current.empty() ? std::string("(none)") : port.connected() ? current : current + " (not connected)";
        if (ImGui::BeginCombo(label, preview.c_str())) {
            if (allowNone && ImGui::Selectable("(none)", current.empty())) {
                (this->*open)(std::string());
                saveSettings();
            }
            for (const std::string& name : outputPorts_) {
                if (ImGui::Selectable(name.c_str(), name == current) && name != current) {
                    (this->*open)(name);
                    saveSettings();
                }
            }
            ImGui::EndCombo();
        }
    };
    outputCombo("To the unit", unitPortName_, unitPort_, true, &TranslatorApp::openUnitOutput);
    helpMarker("The MIDI output the D-110, D-10 or D-20 is connected to.");
    outputCombo("Replies", replyPortName_, replyPort_, true, &TranslatorApp::openReplyOutput);
    helpMarker("The MIDI output back to the MT-32 program. Programs that load their sounds with a handshake transfer "
               "(many X68000 and PC-98 games) wait for the MT-32 to acknowledge each packet; the translator answers "
               "for it here. Not needed for programs that only send.");
    if (ImGui::Button("Refresh ports")) refreshPorts();
    if (!portError_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
        ImGui::TextWrapped("%s", portError_.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
}

void TranslatorApp::drawUnit() {
    if (!ImGui::CollapsingHeader("Unit", ImGuiTreeNodeFlags_DefaultOpen)) return;
    bool changed = false;
    int target = pipeSettings_.target == Mt32Translator::Target::D110 ? 0 : 1;
    bool targetChanged = ImGui::RadioButton("D-110", &target, 0);
    ImGui::SameLine();
    targetChanged |= ImGui::RadioButton("D-10 / D-20", &target, 1);
    if (targetChanged) {
        setTarget(target == 0 ? Mt32Translator::Target::D110 : Mt32Translator::Target::D20);
        changed = true;
    }
    helpMarker("D-110: the MT-32's address map, including part channels and memories.\n\nD-10/D-20 (multi-timbral mode): "
               "their parts listen on the channels set on the panel (below). They take timbre and tone data for the parts, "
               "and changes to their memory only while Memory Protect is off (TUNE/FUNCTION, then the DISPLAY up button, "
               "then the Value knob; it turns back on when the unit is switched off). Turn on MIDI Exclusive and set the "
               "unit number in the MIDI function menu.");

    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
    if (ImGui::InputInt("Unit number", &pipeSettings_.unitNumber)) {
        pipeSettings_.unitNumber = std::clamp(pipeSettings_.unitNumber, 17, 32);
        changed = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("device ID %02XH", pipeSettings_.unitNumber - 1);
    helpMarker("The unit number set on the unit (17-32). The translator sends its SysEx there, whatever device ID the "
               "MT-32 program uses.");

    if (pipeSettings_.target == Mt32Translator::Target::D20) {
        ImGui::TextUnformatted("Part channels on the unit");
        helpMarker("The MIDI channels of the unit's parts, as set in its MIDI function menu. Each part of the MT-32 "
                   "program (parts 1-8 on channels 2-9 and rhythm on 10 unless it moves them) plays on its part here.");
        if (ImGui::BeginTable("channels", kPartCount + 1, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("Part");
            for (int part = 0; part < kPartCount; part++) {
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(partLabel(part));
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("Ch");
            for (int part = 0; part < kPartCount; part++) {
                ImGui::TableNextColumn();
                ImGui::PushID(part);
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 2.0f);
                const uint8_t channel = pipeSettings_.unitChannels[part];
                const std::string current = channel < 16 ? std::to_string(channel + 1) : std::string("-");
                if (ImGui::BeginCombo("##channel", current.c_str(), ImGuiComboFlags_HeightLarge | ImGuiComboFlags_NoArrowButton)) {
                    for (int choice = 0; choice <= 16; choice++) {
                        const std::string label = choice < 16 ? std::to_string(choice + 1) : std::string("off");
                        if (ImGui::Selectable(label.c_str(), choice == channel)) {
                            pipeSettings_.unitChannels[part] = uint8_t(choice);
                            changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (ImGui::SmallButton("1-8, R 10")) {
            for (int part = 0; part < 8; part++) pipeSettings_.unitChannels[part] = uint8_t(part);
            pipeSettings_.unitChannels[8] = 9;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("2-9, R 10")) {
            for (int part = 0; part < 8; part++) pipeSettings_.unitChannels[part] = uint8_t(part + 1);
            pipeSettings_.unitChannels[8] = 9;
            changed = true;
        }
    }

    if (ImGui::Checkbox("Write the MT-32's memories to the unit", &pipeSettings_.memoryInUnit)) changed = true;
    helpMarker("On: the MT-32 program's patch memory becomes the unit's timbre memory (A11-B88) and its timbres the "
               "unit's tones i11-i88, overwriting what the unit had there; program changes then select them on the "
               "unit.\n\nOff: the translator keeps them, and a program change sends the part's timbre and tone as SysEx "
               "(about 90 ms each). The unit's memory stays as it is. A D-20 in multi-timbral mode needs this off.");
    if (ImGui::Checkbox("Master volume as CC 7", &pipeSettings_.masterVolumeAsVolume)) changed = true;
    helpMarker("The D-series units have no master volume parameter. With this on, the MT-32's master volume scales the "
               "parts' MIDI volume (CC 7) instead.");
    int toms = pipeSettings_.roomyToms ? 1 : 0;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    if (ImGui::Combo("Toms", &toms, "MT-32's (TomTom2)\0Roomy (TomTom1)\0")) {
        pipeSettings_.roomyToms = toms == 1;
        changed = true;
    }
    helpMarker("The MT-32 plays all its toms with one sample, the one the unit's TomTom2 set (r31-r33) uses, at the same "
               "pitches. Roomy plays the TomTom1 set (r28-r30) instead, which suits some music better; the MT-32's own "
               "timbres then use its sample too. Send the MT-32 reset (or let the program set up its rhythm) to hear it.");
    if (changed) {
        applyPipeSettings();
        saveSettings();
    }
    ImGui::Spacing();
}

void TranslatorApp::drawPresets() {
    if (!ImGui::CollapsingHeader("MT-32 presets", ImGuiTreeNodeFlags_DefaultOpen)) return;
    int mode = pipeSettings_.presetMode == Mt32Translator::PresetMode::StandIns ? 0 : pipeSettings_.presetMode == Mt32Translator::PresetMode::Hybrid ? 1 : 2;
    bool changed = ImGui::RadioButton("The unit's closest presets", &mode, 0);
    helpMarker("The unit's presets are not the MT-32's. Each MT-32 preset plays the unit's closest one, octave-corrected "
               "(Syn Brass 1 becomes the richer Brass 1, Syn Bass 1 is ElecBass 2 an octave up). Instant, but some have "
               "no real counterpart. Change them on the Presets tab.");
    ImGui::BeginDisabled(!presets_);
    changed |= ImGui::RadioButton("Closest, MT-32's own if none fits", &mode, 1);
    helpMarker("Presets without a good counterpart on the unit (Elec Org 3 and 4, Elec Gtr 2, Doctor Solo, Shakuhachi; "
               "choose more on the Presets tab) play the MT-32's own timbre, translated from its control ROM and sent "
               "as the part's tone when it is selected. The tone cache makes that instant after the first time.");
    changed |= ImGui::RadioButton("The MT-32's own presets", &mode, 2);
    helpMarker("Every MT-32 preset plays the MT-32's own timbre, sent as the part's tone when it is selected (a 246-byte "
               "message, about 80 ms, unless the tone cache has it). Rhythm sounds stay the unit's.");
    ImGui::EndDisabled();
    if (changed) {
        pipeSettings_.presetMode = mode == 0 ? Mt32Translator::PresetMode::StandIns : mode == 1 ? Mt32Translator::PresetMode::Hybrid
                                                                                                : Mt32Translator::PresetMode::Exact;
        applyPipeSettings();
        saveSettings();
    }
    if (presets_) {
        ImGui::TextDisabled("%s", (presets_->description + " (" + Platform::toUtf8(presetRom_.filename()) + ")").c_str());
    } else if (!presetError_.empty()) {
        ImGui::TextColored(kErrorColor, "%s", presetError_.c_str());
    } else {
        ImGui::TextDisabled("No MT-32 control ROM: put one in a \"roms\" folder, or pick it.");
    }
    if (ImGui::Button("Control ROM...")) {
        std::filesystem::path path;
        if (Platform::openFileDialog(Platform::FileKind::Rom, path)) {
            presetRom_ = path;
            loadPresets();
            applyPipeSettings();
            saveSettings();
        }
    }
    UiStyle::setItemTooltip("An MT-32 or CM-32L control ROM image, the source of the MT-32's presets.");
    ImGui::SameLine();
    if (ImGui::Button("Edit presets...")) rightTab_ = 3;
    ImGui::Spacing();
}

void TranslatorApp::drawCache() {
    if (!ImGui::CollapsingHeader("Tone cache", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::BeginDisabled(pipeSettings_.memoryInUnit);
    bool changed = ImGui::Checkbox("Keep tones in the unit's memory", &cacheEnabled_);
    helpMarker("The tones the translator sends (the program's timbres, the MT-32's own presets) are also stored in the "
               "unit's internal tones below, with write requests, the first time. After that, selecting one again is a "
               "short message and instant, and rhythm keys that play the program's timbres get them too.\n\nIt "
               "overwrites those tones on the unit (save them first if you need them), and the unit's Memory Protect "
               "must be off (a D-10/D-20 turns it back on when switched off). The cache is kept between sessions: after using the unit's memory otherwise, click Forget. "
               "Works while the translator keeps the MT-32's memories.");
    const float slotWidth = ImGui::GetFontSize() * 4.0f;
    auto slotCombo = [&](const char* label, int& slot, int low, int high) {
        ImGui::SetNextItemWidth(slotWidth);
        bool picked = false;
        if (ImGui::BeginCombo(label, slotName(slot).c_str(), ImGuiComboFlags_HeightLarge)) {
            for (int s = low; s <= high; s++) {
                if (ImGui::Selectable(slotName(s).c_str(), s == slot)) {
                    slot = s;
                    picked = true;
                }
            }
            ImGui::EndCombo();
        }
        return picked;
    };
    ImGui::BeginDisabled(!cacheEnabled_);
    ImGui::TextUnformatted("Tones");
    ImGui::SameLine();
    changed |= slotCombo("##first", cacheFirst_, 0, cacheLast_);
    ImGui::SameLine();
    ImGui::TextUnformatted("to");
    ImGui::SameLine();
    changed |= slotCombo("##last", cacheLast_, cacheFirst_, 63);
    const Mt32Translator::CacheStats stats = pipe_.toneCacheStats();
    if (stats.failed) {
        ImGui::TextColored(kErrorColor, "The unit refused a write: off (memory protect?)");
    } else if (stats.slots > 0) {
        ImGui::TextDisabled("%d of %d hold tones; %llu found, %llu stored", stats.used, stats.slots, static_cast<unsigned long long>(stats.hits),
                            static_cast<unsigned long long>(stats.misses));
    }
    if (ImGui::Button("Preload")) pipe_.preloadToneCache();
    UiStyle::setItemTooltip("Stores the program's timbres now (through part 8), so its songs start without delay. Best at "
                          "a quiet moment, e.g. at the program's title screen after it has loaded its sounds.");
    ImGui::SameLine();
    if (ImGui::Button("Forget")) pipe_.clearToneCache();
    UiStyle::setItemTooltip("Forgets what the tones hold (nothing is sent); they are stored again as they are used.");
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (pipeSettings_.memoryInUnit) ImGui::TextDisabled("(while the translator keeps the memories)");
    ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 8.5f);
    const std::string unitInput = unitInputName_.empty()                  ? std::string("(none)")
                                  : unitInputs_->isOpen(unitInputName_) ? unitInputName_
                                                                          : unitInputName_ + " (not connected)";
    if (ImGui::BeginCombo("From the unit", unitInput.c_str())) {
        if (ImGui::Selectable("(none)", unitInputName_.empty())) {
            openUnitInput(std::string());
            saveSettings();
        }
        for (const std::string& name : inputPorts_) {
            if (ImGui::Selectable(name.c_str(), name == unitInputName_) && name != unitInputName_) {
                openUnitInput(name);
                saveSettings();
            }
        }
        ImGui::EndCombo();
    }
    helpMarker("Optional: the MIDI input the unit's MIDI OUT is connected to. The unit answers each write request; if it "
               "refuses one (memory protect on), the translator turns the cache off and sends the parts' tones directly, "
               "instead of pointing them at tones that were not stored.");
    if (changed) {
        applyPipeSettings();
        saveSettings();
    }
    ImGui::Spacing();
}

void TranslatorApp::drawTiming() {
    if (!ImGui::CollapsingHeader("Timing", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 8.5f);
    if (ImGui::SliderInt("SysEx pause", &pipeSettings_.sysexGapMs, 0, 100, "%d ms")) applyPipeSettings();
    if (ImGui::IsItemDeactivatedAfterEdit()) saveSettings();
    helpMarker("After each SysEx message the translator waits for its transmission (0.32 ms per byte) and then this "
               "long, so the unit can store the data before more arrives. Raise it if the unit misses parts of long "
               "transfers.");
    if (ImGui::Checkbox("Reduce MIDI load", &pipeSettings_.reduceLoad)) {
        applyPipeSettings();
        saveSettings();
    }
    helpMarker("Leaves out controller values and pitch bends the unit already has, and when several wait behind a SysEx "
               "message, sends only the newest. Older D-series firmware processes envelopes late when busy (attacks "
               "of Violin, Bassoon, Shakuhachi or Strings 1 break up); less MIDI to parse helps.");
    if (ImGui::Checkbox("MT-32 power-on setup at start", &powerOnAtStart_)) saveSettings();
    helpMarker("Sends the MT-32's power-on state when the translator starts (parts, rhythm setup, reverb; the patch "
               "memory too when memories are written to the unit), so the unit matches an MT-32 that was just switched "
               "on. Programs that reset the MT-32 themselves do not need it.");
    ImGui::Spacing();
}

void TranslatorApp::drawPresetEditor() {
    const bool hybrid = pipeSettings_.presetMode == Mt32Translator::PresetMode::Hybrid && presets_;
    const bool exactAll = pipeSettings_.presetMode == Mt32Translator::PresetMode::Exact && presets_;
    ImGui::TextWrapped("What each MT-32 preset plays on the unit: its closest preset there (with a key shift where the "
                       "unit's is an octave away), or the MT-32's own timbre (%s).",
                       hybrid ? "chosen per preset in this mode" : exactAll ? "all of them in this mode" : "needs the mode above it and the MT-32's control ROM");
    if (ImGui::SmallButton("All built in")) {
        setPresetChoicesBuiltIn();
        applyPipeSettings();
        saveSettings();
    }
    UiStyle::setItemTooltip("Back to the choices that come with the translator.");
    PresetChoiceTable::Context context;
    context.presets = presets_.get();
    context.mode = pipeSettings_.presetMode;
    context.unitName = "Unit's";
    context.roomyToms = pipeSettings_.roomyToms;
    if (PresetChoiceTable::drawPresets(pipeSettings_.presetChoices, context)) {
        applyPipeSettings();
        saveSettings();
    }
}

void TranslatorApp::drawActions() {
    if (ImGui::Button("MT-32 reset")) pipe_.powerOn();
    UiStyle::setItemTooltip("Sends the MT-32's power-on setup, as an MT-32 reset does.");
    ImGui::SameLine();
    if (ImGui::Button("All notes off")) pipe_.allNotesOff();
    ImGui::SameLine();
    if (ImGui::Button("Send SysEx file...")) {
        std::filesystem::path path;
        if (Platform::openFileDialog(Platform::FileKind::MidiOrSysex, path)) sendSysexFile(path);
    }
    UiStyle::setItemTooltip("Translates a SysEx file for the MT-32 (.syx, or a game's .dat) and sends it to the unit, as "
                          "if the program had sent it.");
    ImGui::SameLine();
    if (ImGui::Button("Translate file...")) {
        std::filesystem::path path;
        if (Platform::openFileDialog(Platform::FileKind::MidiOrSysex, path)) translateFile(path);
    }
    UiStyle::setItemTooltip("Saves a translated copy of an MT-32 file: a SysEx file becomes one that loads into the unit's "
                          "memory (with its Memory Protect off), a MIDI file one that plays on the unit, with the MT-32's "
                          "power-on setup first.");
    ImGui::Separator();
}

void TranslatorApp::drawMonitor() {
    const MidiPipe::Stats stats = pipe_.stats();
    ImGui::Text("Received %llu   Sent %llu (%llu SysEx bytes)   Waiting %zu", static_cast<unsigned long long>(stats.received),
                static_cast<unsigned long long>(stats.sent), static_cast<unsigned long long>(stats.sysexBytesSent), stats.queued);
    ImGui::Text("Handshake replies %llu   Transfers %llu", static_cast<unsigned long long>(stats.replies),
                static_cast<unsigned long long>(stats.handshakes));
    if (stats.dropped > 0) {
        ImGui::SameLine();
        ImGui::TextColored(kErrorColor, "   Not passed on %llu", static_cast<unsigned long long>(stats.dropped));
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) log_.clear();
    ImGui::BeginChild("log", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
    ImGuiListClipper clipper;
    clipper.Begin(int(log_.size()));
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) ImGui::TextUnformatted(log_[size_t(i)].c_str());
    }
    if (atBottom) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}
