#include "App.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>

#include "Lcd.h"
#include "Mt32Presets.h"
#include "Piano.h"
#include "ReverbSettingsFile.h"
#include "Platform.h"
#include "PresetChoiceTable.h"
#include "RolandSysex.h"
#include "UiStyle.h"
#include "imgui.h"
#include "imgui_stdlib.h"

using namespace UnitText;
using namespace UnitMemory;

namespace {

const int kKeyboardFirstNote = 36;  // C2
const int kKeyboardLastNote = 96;   // C7

const ImU32 kLedOn = IM_COL32(90, 220, 110, 255);
const ImU32 kLedOff = IM_COL32(70, 70, 75, 255);
const ImU32 kPartialColors[] = {
    IM_COL32(55, 55, 62, 255),    // Inactive
    IM_COL32(90, 220, 110, 255),  // Attack
    IM_COL32(240, 200, 70, 255),  // Sustain
    IM_COL32(225, 110, 80, 255),  // Release
};
const ImVec4 kErrorColor(1.0f, 0.45f, 0.4f, 1.0f);
const ImVec4 kOkColor(0.45f, 0.85f, 0.5f, 1.0f);
const ImVec4 kQueuedColor(1.0f, 0.8f, 0.35f, 1.0f);  // A D-20 pattern waiting for the next bar
const ImVec4 kWarningColor(1.0f, 0.8f, 0.35f, 1.0f);  // Amber: a part on an output the host has off

// The user's own LCD colours: lcdScheme_ for them, their names, and their keys in the settings ("#RRGGBB").
constexpr int kCustomLcdScheme = -1;
const char* const kLcdColourParts[3] = {"Glass", "Unlit dots", "Lit dots"};
const char* const kLcdColourKeys[3] = {"lcd_custom_glass", "lcd_custom_dot_off", "lcd_custom_dot_on"};

std::string colorText(const std::array<float, 3>& rgb) {
    char text[8];
    std::snprintf(text, sizeof(text), "#%02X%02X%02X", int(std::lround(std::clamp(rgb[0], 0.0f, 1.0f) * 255.0f)),
                  int(std::lround(std::clamp(rgb[1], 0.0f, 1.0f) * 255.0f)), int(std::lround(std::clamp(rgb[2], 0.0f, 1.0f) * 255.0f)));
    return text;
}

bool parseColorText(const std::string& text, std::array<float, 3>& rgb) {
    const std::string hex = !text.empty() && text[0] == '#' ? text.substr(1) : text;
    if (hex.size() != 6 || hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return false;
    const unsigned long value = std::strtoul(hex.c_str(), nullptr, 16);
    for (int i = 0; i < 3; i++) rgb[size_t(i)] = float((value >> (16 - 8 * i)) & 0xFF) / 255.0f;
    return true;
}

// A preset's colours as the editor holds them: glass, unlit dots, lit dots.
std::array<std::array<float, 3>, 3> presetLcdColors(int scheme) {
    const Lcd::Colors colors = Lcd::schemeColors(scheme);
    std::array<std::array<float, 3>, 3> rgb{};
    const uint32_t packed[3] = {colors.background, colors.dotOff, colors.dotOn};
    for (size_t i = 0; i < 3; i++) {
        const ImVec4 color = ImGui::ColorConvertU32ToFloat4(packed[i]);
        rgb[i] = {color.x, color.y, color.z};
    }
    return rgb;
}

// Small round indicator vertically centred on a frame-height line.
void led(bool on) {
    const float height = ImGui::GetFrameHeight();
    const float radius = ImGui::GetFontSize() * 0.28f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(radius * 2.0f, height));
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(pos.x + radius, pos.y + height * 0.5f), radius, on ? kLedOn : kLedOff);
}

void helpMarker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    UiStyle::setItemTooltip("%s", text);
}

// A control's width in the System tab: what the column leaves beside `labelSpace` font sizes of label (and help marker),
// at most `widest` font sizes, as a column can be wide.
float settingWidth(float labelSpace, float widest = 18.0f) {
    const float fontSize = ImGui::GetFontSize();
    return std::clamp(ImGui::GetContentRegionAvail().x - fontSize * labelSpace, fontSize * 6.0f, fontSize * widest);
}

// The VST3 plugin's own output of a part (part outputs), as the host lists it: "Part 1".."Part 15", "Rhythm".
std::string partOutputName(int part) {
    return part == kRhythmPart ? std::string("Rhythm") : "Part " + partLabel(part);
}

const char* const kAssignNames[] = {"Single, last note", "Single, first note", "Multi, last note", "Multi, first note"};
// Sets a timbre's tone group bytes (00, and the card and alt flags in 07) for a group numbered as above.
void setToneGroup(uint8_t* timbre, int group) {
    timbre[0] = uint8_t(group == kCardGroup ? 2 : group >= kAltGroup ? group - kAltGroup : group);
    timbre[7] = group == kCardGroup ? MT32Emu::PART_CARD_TONES : group >= kAltGroup ? MT32Emu::PART_ALT_TONES : 0;
}

// Fine pan (with nice panning): a slider from -64 (left) to +64 (right). Drag it, turn the mouse wheel (Shift: 10 at a
// time), double-click for the centre or Ctrl+click (Cmd+click on macOS) to type. Returns true when the value changed.
bool panSlider(const char* label, int& pan, float width) {
    ImGui::SetNextItemWidth(width);
    bool changed = ImGui::SliderInt(label, &pan, -64, 64, pan > 0 ? "+%d" : "%d", ImGuiSliderFlags_AlwaysClamp);
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemHovered()) {
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            pan = 0;
            changed = true;
        } else if (const int steps = UiStyle::wheelSteps()) {
            pan = std::clamp(pan + steps * (io.KeyShift ? 10 : 1), -64, 64);
            changed = true;
        }
    }
    return changed;
}

const char* const kFinePanTip = "Pan from -64 (left) to +64 (right): drag, mouse wheel (Shift: 10 at a time), double-click "
                                "for the centre, " UI_CTRL "+click to type. (Nice panning is on.)";

// A value cell that works like the unit's VALUE buttons: left click steps up, right click steps down (both repeat
// while held), the mouse wheel steps as well, and Shift makes steps of 10. `fill` (0-1) draws a level bar behind the
// value; negative for none. Returns the step to apply, 0 if none.
int stepperCell(const char* id, const char* text, float fill) {
    ImGui::PushID(id);
    const ImGuiIO& io = ImGui::GetIO();
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 size(std::max(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize()), ImGui::GetFrameHeight());
    ImGui::InvisibleButton("##value", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const int step = io.KeyShift ? 10 : 1;
    int delta = 0;
    if (ImGui::IsItemActive()) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left, true)) delta += step;
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right, true)) delta -= step;
    }
    if (hovered) delta += UiStyle::wheelSteps() * step;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const ImGuiCol background = ImGui::IsItemActive() ? ImGuiCol_FrameBgActive : hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg;
    drawList->AddRectFilled(min, max, ImGui::GetColorU32(background), style.FrameRounding);
    if (fill > 0.0f) {
        drawList->AddRectFilled(min, ImVec2(min.x + (max.x - min.x) * std::min(fill, 1.0f), max.y), ImGui::GetColorU32(ImGuiCol_PlotHistogram),
                                style.FrameRounding);
    }
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    drawList->AddText(ImVec2(min.x + std::max(0.0f, (max.x - min.x - textSize.x) * 0.5f), min.y + style.FramePadding.y),
                      ImGui::GetColorU32(ImGuiCol_Text), text);
    ImGui::PopID();
    return delta;
}

}  // namespace

App::App() = default;

App::~App() {
    shutdown();
}

void App::applyStyle() {
    UiStyle::apply();
}

void App::init(const AppOptions& options) {
    AppCore::init(options);
    // No sound (no ROMs, a ROM or audio error): the configuration window opens, where the ROMs and audio are set.
    showConfig_ = !status_.open || (options_.enableAudio && !audio_->isRunning());
}

double App::now() const {
    return ImGui::GetTime();
}

void App::onLogAdded(const std::string& /*line*/) {
    scrollLogToBottom_ = true;
}

// ---------------------------------------------------------------------------------------------
// Settings

void App::loadFrontEndSettings() {
    showMt32Presets_ = settings_.getBool("show_mt32_presets", false);
    showReverbTuning_ = settings_.getBool("show_reverb_tuning", false);
    lcdScheme_ = settings_.getString("lcd_scheme") == "custom" ? kCustomLcdScheme
                                                                : std::clamp(settings_.getInt("lcd_scheme", 0), 0, Lcd::schemeCount() - 1);
    std::array<std::array<float, 3>, 3> custom{};
    lcdCustomSet_ = true;
    for (size_t i = 0; i < 3; i++) lcdCustomSet_ = parseColorText(settings_.getString(kLcdColourKeys[i]), custom[i]) && lcdCustomSet_;
    lcdCustom_ = lcdCustomSet_ ? custom : presetLcdColors(0);

    keyboardChannel_ = std::clamp(settings_.getInt("keyboard_channel", 1), 1, 16) - 1;
    velocity_ = std::clamp(settings_.getInt("keyboard_velocity", 100), 1, 127);
    keyboardOctave_ = std::clamp(settings_.getInt("keyboard_octave", 4), 0, 7);
    showLog_ = settings_.getBool("show_log", false);
    showKeyboard_ = settings_.getBool("show_keyboard", !options_.plugin);  // A plugin window starts without it
    toneEditor_.setPianoShown(showKeyboard_);
    showPartials_ = settings_.getBool("show_partials", true);
    windowZoom_ = 100;
    for (int zoom : UiStyle::kZoomSteps) {
        if (zoom == settings_.getInt("window_zoom", 100)) windowZoom_ = zoom;
    }
    ToneEditor::Audition& audition = toneEditor_.audition();
    audition.key =std::clamp(settings_.getInt("tone_audition_key", 60), 24, 108);
    audition.velocity = std::clamp(settings_.getInt("tone_audition_velocity", 100), 1, 127);
    audition.lengthMs = std::clamp(settings_.getInt("tone_audition_length", 700), 50, 4000);
    audition.repeatMs = std::clamp(settings_.getInt("tone_audition_repeat", 1200), 200, 4000);
    audition.restrike = settings_.getBool("tone_restrike", true);
}

void App::saveFrontEndSettings() {
    settings_.set("show_mt32_presets", showMt32Presets_);
    settings_.set("show_reverb_tuning", showReverbTuning_);
    settings_.set("lcd_scheme", lcdScheme_ == kCustomLcdScheme ? std::string("custom") : std::to_string(lcdScheme_));
    if (lcdCustomSet_) {
        for (size_t i = 0; i < 3; i++) settings_.set(kLcdColourKeys[i], colorText(lcdCustom_[i]));
    }

    settings_.set("keyboard_channel", keyboardChannel_ + 1);
    settings_.set("keyboard_velocity", velocity_);
    settings_.set("keyboard_octave", keyboardOctave_);
    settings_.set("show_log", showLog_);
    settings_.set("show_keyboard", showKeyboard_);
    settings_.set("show_partials", showPartials_);
    if (!options_.plugin) settings_.set("window_zoom", windowZoom_);
    ToneEditor::Audition& audition = toneEditor_.audition();
    settings_.set("tone_audition_key", audition.key);
    settings_.set("tone_audition_velocity", audition.velocity);
    settings_.set("tone_audition_length", audition.lengthMs);
    settings_.set("tone_audition_repeat", audition.repeatMs);
    settings_.set("tone_restrike", audition.restrike);
}

// ---------------------------------------------------------------------------------------------
// Files

void App::openFileThen(Platform::FileKind kind, PathAction then) {
    if (dialog_ != nullptr) return;  // One at a time (its wait blocks the window anyway)
    dialog_ = Platform::startOpenFileDialog(kind);
    if (dialog_ != nullptr) {
        dialogThen_ = std::move(then);
        return;
    }
    std::filesystem::path path;
    if (Platform::openFileDialog(kind, path)) then(path);
}

void App::saveFileThen(Platform::FileKind kind, const std::string& defaultName, PathAction then) {
    if (dialog_ != nullptr) return;
    dialog_ = Platform::startSaveFileDialog(kind, defaultName);
    if (dialog_ != nullptr) {
        dialogThen_ = std::move(then);
        return;
    }
    std::filesystem::path path;
    if (Platform::saveFileDialog(kind, defaultName, path)) then(path);
}

void App::pickFolderThen(const std::filesystem::path& initialFolder, PathAction then) {
    if (dialog_ != nullptr) return;
    dialog_ = Platform::startPickFolderDialog(initialFolder);
    if (dialog_ != nullptr) {
        dialogThen_ = std::move(then);
        return;
    }
    std::filesystem::path path;
    if (Platform::pickFolderDialog(initialFolder, path)) then(path);
}

void App::drawDialogWait() {
    const char* const popup = "File dialog";
    std::filesystem::path chosen;
    const bool finished = dialog_ != nullptr && dialog_->done(chosen);
    if (dialog_ != nullptr && !finished && !ImGui::IsPopupOpen(popup)) ImGui::OpenPopup(popup);
    bool cancel = false;
    if (ImGui::BeginPopupModal(popup, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        if (dialog_ == nullptr || finished) {
            ImGui::CloseCurrentPopup();
        } else {
            ImGui::TextUnformatted("Waiting for the file dialog (it may be behind this window).");
            if (ImGui::Button("Cancel")) cancel = true;
        }
        ImGui::EndPopup();
    }
    if (cancel) {
        dialog_.reset();  // Closes it; the popup goes at the next frame
        dialogThen_ = nullptr;
    }
    if (finished) {
        dialog_.reset();
        const PathAction then = std::move(dialogThen_);
        dialogThen_ = nullptr;
        if (!chosen.empty() && then) then(chosen);
    }
}

void App::browseMidiFile() {
    openFileThen(Platform::FileKind::Midi, [this](const std::filesystem::path& path) { openMidiFile(path); });
}

void App::browseSysexFile() {
    openFileThen(Platform::FileKind::Sysex, [this](const std::filesystem::path& path) { loadSysexFile(path); });
}

void App::saveMemoryAs() {
    saveFileThen(Platform::FileKind::Sysex, "D-110 memory.syx", [this](const std::filesystem::path& path) {
        std::string error;
        if (engine_->saveSysexFile(path, DumpMemory, error)) {
            addLog("Saved memory to " + Platform::toUtf8(path.filename()));
        } else {
            addLog("SysEx: " + error);
        }
    });
}

void App::newCard() {
    saveFileThen(Platform::FileKind::Sysex, "Card.syx", [this](const std::filesystem::path& path) {
        saveCardFile();
        std::string error;
        if (!engine_->insertCard(std::filesystem::path(), error)) {
            addLog("Card: " + error);
            return;
        }
        cardFile_ = path;
        saveCardFile();
        addLog("New memory card " + Platform::toUtf8(path.filename()));
        cacheTime_ = -1.0;
        saveSettings();
    });
}

void App::insertCard() {
    openFileThen(Platform::FileKind::Sysex, [this](const std::filesystem::path& path) {
        saveCardFile();
        std::string error;
        if (!engine_->insertCard(path, error)) {
            addLog("Card: " + error);
            return;
        }
        cardFile_ = path;
        addLog("Inserted memory card " + Platform::toUtf8(path.filename()));
        cacheTime_ = -1.0;
        saveSettings();
    });
}

void App::ejectCard() {
    saveCardFile();
    engine_->ejectCard();
    addLog("Ejected memory card " + Platform::toUtf8(cardFile_.filename()));
    cardFile_.clear();
    cacheTime_ = -1.0;
    saveSettings();
}

// ---------------------------------------------------------------------------------------------
// UI

void App::frame() {
    update();
    // A file dialog that could not open (Linux without xdg-desktop-portal or zenity) says why in the status bar.
    const std::string dialogError = Platform::takeDialogError();
    if (!dialogError.empty()) addLog("File dialog: " + dialogError + ". Files can also be dropped on the window.");
    handleComputerKeyboard();
    toneTabWasShown_ = toneTabShown_;
    toneTabShown_ = false;
    performanceTabWasShown_ = performanceTabShown_;
    performanceTabShown_ = false;
    lcdPatternPlay_ = patternsTabShown_;  // The LCD shows the D-20's Pattern Play screen while the Patterns tab does
    patternsTabShown_ = false;
    keyboardDrawn_ = false;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("D110Emu", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar(2);

    drawMenuBar();

    const ImGuiStyle& style = ImGui::GetStyle();
    const float statusBarHeight = ImGui::GetTextLineHeight() + style.ItemSpacing.y;
    const float bodyHeight = std::max(ImGui::GetContentRegionAvail().y - statusBarHeight, 100.0f);

    // The unit: its display and the player, then the tabs. The settings are in the System tab and the configuration
    // window (File > Configuration...).
    ImGui::BeginChild("main", ImVec2(0.0f, bodyHeight), ImGuiChildFlags_Borders);
    drawLcdPanel();
    if (ImGui::BeginTabBar("views")) {
        auto tabFlags = [&](const char* name) {
            return requestedTab_ == name ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
        };
        if (ImGui::BeginTabItem("Play", nullptr, tabFlags("Play"))) {
            drawTabContent("play", true, [this] {
                drawPartsPanel();
                if (showPartials_) drawPartialsPanel();
            });
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Parts", nullptr, tabFlags("Parts"))) {
            drawTabContent("parts", false, [this] { drawPartsEditor(); });
            ImGui::EndTabItem();
        }
        if (status_.d110 && ImGui::BeginTabItem("Patches", nullptr, tabFlags("Patches"))) {
            drawTabContent("patches", false, [this] { drawPatchesTab(); });
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Tone", nullptr, tabFlags("Tone"))) {
            drawTabContent("tone", false, [this] { drawToneTab(); });
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Timbres", nullptr, tabFlags("Timbres"))) {
            drawTabContent("timbres", false, [this] { drawTimbresTab(); });
            ImGui::EndTabItem();
        }
        if (status_.d110 && ImGui::BeginTabItem("Performance", nullptr, tabFlags("Performance"))) {
            performanceTabShown_ = true;
            drawTabContent("performance", true, [this] { drawPerformanceTab(); });
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Rhythm", nullptr, tabFlags("Rhythm"))) {
            drawTabContent("rhythm", false, [this] { drawRhythmTab(); });
            ImGui::EndTabItem();
        }
        if (status_.d110 && ImGui::BeginTabItem("Patterns", nullptr, tabFlags("Patterns"))) {
            drawTabContent("patterns", false, [this] { drawPatternsTab(); });
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("System", nullptr, tabFlags("System"))) {
            drawTabContent("system", false, [this] { drawSystemTab(); });
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
        requestedTab_.clear();
    }
    ImGui::EndChild();

    drawStatusBar();
    drawAboutPopup();
    ImGui::End();

    // A note the mouse held on the keyboard ends when the keyboard goes (another tab, View > Keyboard).
    if (!keyboardDrawn_ && mouseNote_ >= 0) {
        sendShort(uint8_t(0x80 | mouseChannel_), uint8_t(mouseNote_), 0);
        mouseNote_ = -1;
    }
    // Leaving the Tone tab lets go of the notes its keyboard holds.
    if (toneTabWasShown_ && !toneTabShown_) {
        toneEditor_.releaseNotes(toneHost_);
        toneEditor_.audition().repeat = false;
    }

    if (showConfig_) drawConfigWindow();
    if (showLog_) drawLogPanel();
    if (showReverbTuning_) drawReverbTuningPanel();
    if (showMt32Presets_) drawMt32PresetsWindow();
    if (showLcdColourEditor_) drawLcdColourEditor();
    drawDialogWait();
    if (lcdCustomDirty_ && !ImGui::IsAnyItemActive()) {
        saveSettings();
        lcdCustomDirty_ = false;
    }
    if (reverbTestOffAt_ >= 0.0 && ImGui::GetTime() >= reverbTestOffAt_) {
        for (uint8_t key : {60, 64, 67}) sendShort(uint8_t(0x80 | reverbTestChannel_), key, 0);
        reverbTestOffAt_ = -1.0;
    }
    if (reverbSettingsDirty_ && !ImGui::IsAnyItemActive()) saveReverbSettingsFile();

    if (showDemo_) ImGui::ShowDemoWindow(&showDemo_);
}

void App::drawMenuBar() {
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, ImGuiInputFlags_RouteGlobal)) browseMidiFile();
    if (!ImGui::BeginMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open MIDI file...", UI_CTRL "+O")) browseMidiFile();
        ImGui::Separator();
        if (ImGui::MenuItem("Load SysEx file...")) browseSysexFile();
        if (ImGui::MenuItem("Save memory as SysEx...", nullptr, false, status_.open)) saveMemoryAs();
        UiStyle::setItemTooltip("Tones, timbres, patches, rhythm setup and system settings, as a bulk dump a real D-110 accepts.");
        ImGui::Separator();
        if (ImGui::BeginMenu("Memory card", status_.d110)) {
            if (ImGui::MenuItem("New card...")) newCard();
            if (ImGui::MenuItem("Insert card...")) insertCard();
            if (ImGui::MenuItem("Eject card", nullptr, false, status_.cardInserted)) ejectCard();
            ImGui::Separator();
            if (ImGui::MenuItem("Save memory to card", nullptr, false, status_.cardInserted)) {
                engine_->copyMemoryToCard();
                saveCardFile();
                cacheTime_ = -1.0;
            }
            UiStyle::setItemTooltip("Copies all internal tones, timbres and patches to the card, like the unit's Save to Card.");
            if (ImGui::MenuItem("Load memory from card", nullptr, false, status_.cardInserted)) {
                engine_->copyCardToMemory();
                cacheTime_ = -1.0;
            }
            UiStyle::setItemTooltip("Copies the card's tones, timbres and patches into internal memory, like the unit's Load from Card.");
            if (status_.cardInserted) ImGui::TextDisabled("%s", Platform::toUtf8(cardFile_.filename()).c_str());
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Initialize memory...", nullptr, false, status_.open)) openInitializeConfirm_ = true;
        ImGui::Separator();
        if (ImGui::MenuItem("Configuration...")) {
            showConfig_ = true;
            focusConfig_ = true;
        }
        UiStyle::setItemTooltip(options_.plugin ? "ROMs, the analog output stage and MIDI options. The sound, MIDI and system settings "
                                                "are in the System tab."
                                              : "ROMs, audio output and MIDI input. The sound, MIDI and system settings are in the "
                                                "System tab.");
        if (!options_.plugin) {  // A plugin goes when the host removes it
            ImGui::Separator();
            if (ImGui::MenuItem("Exit")) quitRequested_ = true;
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Synth")) {
        if (ImGui::MenuItem("Restart synth")) resetSynth();
        if (ImGui::MenuItem("All notes off")) engine_->allNotesOff();
        if (ImGui::MenuItem("Reset MIDI channels", nullptr, false, status_.open)) engine_->resetMidiChannels();
        ImGui::Separator();
        if (ImGui::MenuItem("D-110 channels (parts 1-8, rhythm 10)")) setChannels(kD110Channels, true);
        if (ImGui::MenuItem("MT-32 channels (parts 2-9, rhythm 10)")) setChannels(kMT32Channels, true);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Keyboard", nullptr, &showKeyboard_)) {
            toneEditor_.setPianoShown(showKeyboard_);
            saveSettings();
        }
        UiStyle::setItemTooltip("The on-screen keyboard under the Play and Performance tabs, and the Tone tab's audition keys. "
                              "The computer keyboard plays either way.");
        if (ImGui::MenuItem("Partials", nullptr, &showPartials_)) saveSettings();
        UiStyle::setItemTooltip("The Play tab's partial display: what each of the synth's partials is doing");
        if (ImGui::MenuItem("Log", nullptr, &showLog_)) saveSettings();
        if (ImGui::MenuItem("Reverb tuning", nullptr, &showReverbTuning_)) saveSettings();
        UiStyle::setItemTooltip("The D-series reverb's parameters, per type (D-110 ROMs)");
        if (ImGui::MenuItem("MT-32 presets", nullptr, &showMt32Presets_)) saveSettings();
        if (ImGui::BeginMenu("LCD colours")) {
            drawLcdColourItems();
            ImGui::EndMenu();
        }
        if (!options_.plugin) {
            if (ImGui::BeginMenu("Window size")) {
                for (int zoom : UiStyle::kZoomSteps) {
                    const std::string label = std::to_string(zoom) + "%";
                    if (ImGui::MenuItem(label.c_str(), nullptr, zoom == windowZoom_) && zoom != windowZoom_) {
                        windowZoom_ = zoom;  // The window host resizes the window and its text before the next frame
                        saveSettings();
                    }
                }
                ImGui::EndMenu();
            } else {
                UiStyle::setItemTooltip("The window and its text, on top of the display's scaling: 100%% is the size the display's "
                                        "scaling gives. D110Emu opens at this size next time.");
            }
        }
        if (options_.viewMenu) options_.viewMenu();
        ImGui::MenuItem("Dear ImGui demo", nullptr, &showDemo_);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About D110Emu")) openAbout_ = true;
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
}

void App::drawStatusBar() {
    // A problem the configuration window can solve (click for it), else the latest log line (click for the log).
    const std::string problem = setupProblem(" See File > Configuration.");
    if (!problem.empty()) {
        ImGui::TextColored(kErrorColor, "%s", problem.c_str());
        if (ImGui::IsItemClicked()) {
            showConfig_ = true;
            focusConfig_ = true;
        }
        UiStyle::setItemTooltip("Click for the configuration");
        return;
    }
    ImGui::TextDisabled("%s", log_.empty() ? "Ready" : log_.back().c_str());
    if (ImGui::IsItemClicked()) showLog_ = true;
    UiStyle::setItemTooltip("Click to show the log");
}

void App::drawConfigWindow() {
    // As wide as the old setup column's controls need, as tall as its contents (more MIDI ports, an error).
    const float width = ImGui::GetFontSize() * 34.0f;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, FLT_MAX));
    if (focusConfig_) {
        ImGui::SetNextWindowFocus();
        focusConfig_ = false;
    }
    const bool visible = ImGui::Begin("Configuration", &showConfig_, ImGuiWindowFlags_AlwaysAutoResize);
    if (visible) {
        drawRomPanel();
        drawAudioPanel();
        drawMidiPanel();
    }
    ImGui::End();
}

void App::drawRomPanel() {
    ImGui::SeparatorText("ROMs");

    std::string folder = Platform::toUtf8(romFolder_);
    ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 4.5f);
    if (ImGui::InputText("##folder", &folder, ImGuiInputTextFlags_EnterReturnsTrue)) {
        romFolder_ = Platform::fromUtf8(folder);
        scanRoms();
        restartSynth();
        saveSettings();
    }
    UiStyle::setItemTooltip("Folder with the ROM images. Type a path and press Enter, or use Browse.");
    ImGui::SameLine();
    if (ImGui::Button("Browse")) {
        pickFolderThen(romFolder_, [this](const std::filesystem::path& picked) {
            romFolder_ = picked;
            scanRoms();
            restartSynth();
            saveSettings();
        });
    }

    auto romCombo = [&](const char* label, bool control, int& index) {
        bool changed = false;
        const char* preview = index >= 0 ? roms_[index].description.c_str() : "(none)";
        ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 4.5f);
        if (ImGui::BeginCombo(label, preview)) {
            for (int i = 0; i < int(roms_.size()); i++) {
                if (roms_[i].isControl != control) continue;
                ImGui::PushID(i);
                const std::string item = roms_[i].description + "   " + roms_[i].fileName;
                if (ImGui::Selectable(item.c_str(), i == index) && i != index) {
                    index = i;
                    changed = true;
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        return changed;
    };
    const bool controlChanged = romCombo("Control", true, controlRom_);
    const bool pcmChanged = romCombo("PCM", false, pcmRom_);
    if (controlChanged || pcmChanged) {
        restartSynth();
        saveSettings();
    }

    if (roms_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
        ImGui::TextWrapped("No ROM images found. Put the D-110 Control and PCM ROMs (e.g. CONTROLromcombo.bin and "
                           "PCMromcombo.bin) in a folder named \"roms\" next to the %s, or choose their folder.",
                           !options_.plugin                 ? "program"
                           : options_.pluginFormat == "VST3" ? "plugin (inside D110Emu.vst3)"
                           : options_.pluginFormat == "AU"   ? "plugin (inside D110Emu.component)"
                                                             : "plugin (D110Emu.dll)");
        ImGui::PopStyleColor();
    } else if (!synthError_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
        ImGui::TextWrapped("%s", synthError_.c_str());
        ImGui::PopStyleColor();
    } else if (status_.open) {
        ImGui::TextColored(kOkColor, "Running");
        ImGui::SameLine();
        ImGui::TextDisabled("%s analog output, %u Hz", kAnalogModeNames[int(status_.analogMode) + 1], status_.outputSampleRate);
    }
    ImGui::Spacing();
}

void App::drawAudioDeviceSettings(float comboWidth, bool& restartAudio) {
    ImGui::SetNextItemWidth(comboWidth);
    if (ImGui::BeginCombo("Device", audioDevice_.empty() ? "System default" : audioDevice_.c_str())) {
        if (ImGui::IsWindowAppearing()) audioDevices_ = audio_->listDevices();
        if (ImGui::Selectable("System default", audioDevice_.empty()) && !audioDevice_.empty()) {
            audioDevice_.clear();
            restartAudio = true;
        }
        for (size_t i = 0; i < audioDevices_.size(); i++) {
            ImGui::PushID(int(i));
            if (ImGui::Selectable(audioDevices_[i].c_str(), audioDevices_[i] == audioDevice_) && audioDevices_[i] != audioDevice_) {
                audioDevice_ = audioDevices_[i];
                restartAudio = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    auto rateLabel = [](int rate) { return rate == 0 ? std::string("Device default") : std::to_string(rate) + " Hz"; };
    ImGui::SetNextItemWidth(comboWidth);
    if (ImGui::BeginCombo("Sample rate", rateLabel(sampleRate_).c_str())) {
        for (int rate : kSampleRates) {
            if (ImGui::Selectable(rateLabel(rate).c_str(), rate == sampleRate_) && rate != sampleRate_) {
                sampleRate_ = rate;
                restartAudio = true;
            }
        }
        ImGui::EndCombo();
    }

    const double rate = double(outputRate());
    auto bufferLabel = [rate](int frames) {
        char text[48];
        std::snprintf(text, sizeof(text), "%d frames (%.1f ms)", frames, 1000.0 * frames / rate);
        return std::string(text);
    };
    ImGui::SetNextItemWidth(comboWidth);
    if (ImGui::BeginCombo("Buffer", bufferLabel(bufferFrames_).c_str())) {
        for (int frames : kBufferSizes) {
            if (ImGui::Selectable(bufferLabel(frames).c_str(), frames == bufferFrames_) && frames != bufferFrames_) {
                bufferFrames_ = frames;
                restartAudio = true;
            }
        }
        ImGui::EndCombo();
    }
    helpMarker("Smaller buffers lower the latency but may crackle on a busy system.");

    const char* const layouts[] = {"Stereo", "7.1 surround"};
    ImGui::SetNextItemWidth(comboWidth);
    if (ImGui::BeginCombo("Channels", layouts[surround_ ? 1 : 0])) {
        for (int layout = 0; layout < 2; layout++) {
            if (ImGui::Selectable(layouts[layout], (layout == 1) == surround_) && (layout == 1) != surround_) {
                surround_ = layout == 1;
                restartAudio = true;
            }
        }
        ImGui::EndCombo();
    }
    helpMarker("7.1 surround gives the D-110's MULTI outputs speakers of their own: MULTI 1 and 2 play at the front left and "
               "right with the mix, MULTI 3 and 4 at the rear left and right, MULTI 5 and 6 at the side left and right (the "
               "centre and LFE stay silent), each on its own speaker or as stereo pairs (Multi, below). Parts and rhythm keys "
               "go to them with their Output (the Play, Parts and Rhythm tabs), dry, and MULTI 5 and 6 play even with reverb "
               "on. The device should be set to 7.1 speakers in the system's sound settings: on fewer speakers, the channels "
               "it lacks are mixed into the others.\n\nStereo: MULTI 1-6 are mixed in, dry and centred, and MULTI 5 and 6 are "
               "silent while reverb is on, as on the unit.");
    drawMultiOutputChoice(comboWidth);
}

void App::drawAudioPanel() {
    // The plugin's audio goes to the host at its rate: only the synth's own output stage is set here.
    ImGui::SeparatorText(options_.plugin ? "Output" : "Audio output");
    const float comboWidth = -ImGui::GetFontSize() * 7.0f;
    bool restartAudio = false;

    if (!options_.plugin) drawAudioDeviceSettings(comboWidth, restartAudio);

    bool restart = false;
    int analogIndex = int(analogMode_) + 1;
    ImGui::SetNextItemWidth(comboWidth);
    if (ImGui::Combo("Analog", &analogIndex, kAnalogModeNames, IM_ARRAYSIZE(kAnalogModeNames))) {
        analogMode_ = AnalogMode(analogIndex - 1);
        restart = true;
    }
    helpMarker("Emulation of the analog output stage. Auto picks the most accurate mode for the sample rate: "
               "Accurate (48 kHz) or Oversampled (96 kHz). Changing it restarts the synth.");
    ImGui::SetNextItemWidth(comboWidth);
    if (ImGui::Combo("Resampler", &resamplerQuality_, kQualityNames, IM_ARRAYSIZE(kQualityNames))) restart = true;
    if (options_.plugin) drawMultiOutputChoice(comboWidth);

    if (options_.plugin) {
        ImGui::TextDisabled("%u Hz, the host's sample rate", outputRate());
    } else if (audio_->isRunning()) {
        const double periodMs = 1000.0 * audio_->periodFrames() / std::max(1u, audio_->sampleRate());
        ImGui::TextDisabled("%s, %u Hz, %u-frame period (%.1f ms)", audio_->backendName().c_str(), audio_->sampleRate(),
                            audio_->periodFrames(), periodMs);
        if (surround_ && audio_->deviceSpeakers() != audio_->speakers()) {
            // A device set to fewer (or other) speakers: miniaudio mixes ours into its own.
            ImGui::PushStyleColor(ImGuiCol_Text, kWarningColor);
            ImGui::TextWrapped("The device plays %u channels (%s): the speakers it lacks are mixed into those it has. Set it to "
                               "7.1 speakers in the system's sound settings to hear each MULTI output on its own.",
                               audio_->deviceChannels(), audio_->deviceSpeakers().c_str());
            ImGui::PopStyleColor();
        } else if (surround_) {
            ImGui::TextDisabled("7.1: %s", audio_->deviceSpeakers().c_str());
        }
    } else if (options_.enableAudio) {
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
        ImGui::TextWrapped("Audio stopped. %s", audioError_.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();

    if (restartAudio) {
        startAudioAndSynth();
        saveSettings();
    } else if (restart) {
        restartSynth();
        saveSettings();
    }
}

void App::drawMultiOutputChoice(float comboWidth) {
    // The standalone: its own choice, at once, heard on its 7.1 speakers. The plugins: where the choice is fixed (the Audio
    // Unit), this instance's; otherwise the choice for instances added from now on.
    const bool standalone = !options_.plugin;
    const bool fixed = !standalone && (!options_.multiPairsForNew || !options_.setMultiPairsForNew);
    const bool pairs = standalone || fixed ? multiPairs_ : options_.multiPairsForNew();
    const char* const layouts[] = {"Six mono outputs", "Three stereo pairs"};
    ImGui::BeginDisabled(fixed);
    ImGui::SetNextItemWidth(comboWidth);
    if (ImGui::BeginCombo("Multi", layouts[pairs ? 1 : 0])) {
        for (int layout = 0; layout < 2; layout++) {
            if (!ImGui::Selectable(layouts[layout], (layout == 1) == pairs) || (layout == 1) == pairs) continue;
            if (standalone) {
                setMultiPairs(layout == 1);
            } else {
                options_.setMultiPairsForNew(layout == 1);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    const std::string pairsText = "Multi 1+2, 3+4 and 5+6, where a part or rhythm key on either output of a pair plays in "
                                  "stereo with its pan";
    if (standalone) {
        helpMarker(("How the 7.1 speakers play the D-110's MULTI 1-6. Six mono outputs: each on a speaker of its own, as the "
                    "unit's mono jacks: Multi 1 and 2 at the front left and right, 3 and 4 at the rear, 5 and 6 at the side. "
                    "Three stereo pairs: " + pairsText + "; Multi 1+2 at the front, 3+4 at the rear and 5+6 at the side. In "
                    "stereo (Channels), MULTI 1-6 play in the mix either way.").c_str());
        if (pairs && !surround_) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("Heard in 7.1 surround: in stereo, MULTI 1-6 play in the mix.");
            ImGui::PopStyleColor();
        }
    } else if (fixed) {
        helpMarker(("The Audio Unit gives the D-110's MULTI 1-6 as three stereo outputs, " + pairsText + " (Logic Pro cannot "
                    "load a plugin that has mono outputs beside stereo ones).").c_str());
    } else {
        helpMarker(("How the plugin gives the DAW the D-110's MULTI 1-6: six mono outputs, as the unit's jacks, or three "
                    "stereo outputs, " + pairsText + ". DAWs set up a plugin's outputs when they add it, so the choice applies "
                    "to instances added from now on, in the projects opened from now on too, on this computer; their Multi "
                    "tracks may need setting up again.").c_str());
    }
    if (!standalone && !fixed && pairs != multiPairs_) {
        ImGui::PushStyleColor(ImGuiCol_Text, kWarningColor);
        ImGui::TextWrapped("This instance keeps its %s until the DAW adds it again.", multiPairs_ ? "three stereo pairs" : "six mono outputs");
        ImGui::PopStyleColor();
    }
}

bool App::outputAssignCombo(const char* id, int& assign, bool compact) {
    const bool pairs = multiPairsInUse();
    bool changed = false;
    if (ImGui::BeginCombo(id, outputName(assign, compact), ImGuiComboFlags_HeightLarge)) {
        for (int a = 0; a < 8; a++) {
            if (pairs && a >= 2 && (a & 1) != 0) continue;  // MULTI 2, 4 and 6: the same stereo output as 1, 3 and 5
            const bool samePair = pairs && a >= 2 && assign >= 2 && (assign - 2) / 2 == (a - 2) / 2;
            if (ImGui::Selectable(outputName(a, false), samePair || a == assign) && !samePair && a != assign) {
                assign = a;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

void App::drawMidiPanel() {
    ImGui::SeparatorText(options_.plugin ? "MIDI" : "MIDI input");
    if (!options_.plugin) drawMidiPorts();  // The plugin's MIDI comes from the host
    if (!midiError_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
        ImGui::TextWrapped("%s", midiError_.c_str());
        ImGui::PopStyleColor();
    }
    if (!options_.plugin) {  // The host's MIDI plays at the sample it belongs to
        if (ImGui::Checkbox("Jitter-free timing", &engineOptions_.midiTimestamping)) setOptions();
        helpMarker("Plays each incoming event exactly one audio buffer after it arrives, instead of at the start of the "
                   "next buffer. Adds one buffer of latency but keeps rhythms steady.");
    }
    if (ImGui::Checkbox("MIDI cable speed", &engineOptions_.midiCableSpeed)) setOptions();
    helpMarker(options_.plugin ? "Spaces messages the way a MIDI cable delivers them to a real D-110 (about 0.8 ms per note), "
                                 "so the notes of a chord start one after another. Off: every message plays at the sample "
                                 "the host gives it."
                               : "Spaces messages the way a MIDI cable delivers them to a real D-110 (about 0.8 ms per note), "
                                 "so the notes of a chord start one after another. Off: every message plays as soon as it "
                                 "arrives, which suits USB and virtual ports and dense music.");
    ImGui::TextDisabled("Events received: %llu", static_cast<unsigned long long>(status_.midiEvents));
    if (status_.queueOverflows > 0) {
        ImGui::TextColored(kErrorColor, "Dropped (queue full): %llu", static_cast<unsigned long long>(status_.queueOverflows));
    }
}

void App::drawMidiPorts() {
    const double time = ImGui::GetTime();

    // Linux and macOS: the ALSA sequencer or CoreMIDI, which may not open (on Linux, a user without the audio group),
    // and D110Emu's own port.
    const std::string systemError = options_.enableMidiInput ? midi_->systemError() : std::string();
    const std::string ownPort = midi_->ownPortName();
    if (!systemError.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
        ImGui::TextWrapped("%s", systemError.c_str());
        ImGui::PopStyleColor();
    } else if (midiPorts_.empty() && !ownPort.empty()) {
        ImGui::TextWrapped("No MIDI inputs found. Programs can send to %s themselves (%s).", ownPort.c_str(),
                           midi_->ownPortHint().c_str());
    } else if (midiPorts_.empty()) {
        ImGui::TextWrapped("No MIDI input ports found. To play from another program (DOSBox, ScummVM, a DAW), "
                           "create a virtual port with loopMIDI and select it here.");
    }
    for (size_t i = 0; i < midiPorts_.size(); i++) {
        const std::string& name = midiPorts_[i];
        ImGui::PushID(int(i));
        bool enabled = midi_->isOpen(name);
        if (ImGui::Checkbox(name.c_str(), &enabled)) setMidiPortEnabled(name, enabled);
        const uint32_t count = midi_->messageCount(name);
        if (count != midiCounts_[name]) {
            midiCounts_[name] = count;
            midiActivity_[name] = time;
        }
        ImGui::SameLine();
        led(enabled && time - midiActivity_[name] < 0.15);
        ImGui::PopID();
    }
    for (const std::string& name : enabledMidiPorts_) {
        if (std::find(midiPorts_.begin(), midiPorts_.end(), name) == midiPorts_.end()) {
            ImGui::TextDisabled("%s (not connected)", name.c_str());
        }
    }
    if (ImGui::Button("Refresh ports")) refreshMidiPorts();
    if (!ownPort.empty() && !midiPorts_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("Programs can also send to %s", ownPort.c_str());
    }
}

void App::drawSystemTab() {
    // The sound on the left, MIDI and the system on the right; one below another in a narrow window.
    const int columns = ImGui::GetContentRegionAvail().x >= ImGui::GetFontSize() * 50.0f ? 2 : 1;
    if (!ImGui::BeginTable("system", columns, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) return;
    ImGui::TableNextColumn();
    drawSoundPanel();
    if (columns > 1) ImGui::TableNextColumn();
    drawMidiOptionsPanel();
    drawSystemPanel();
    ImGui::EndTable();
}

void App::drawMidiOptionsPanel() {
    ImGui::SeparatorText("MIDI");
    ImGui::BeginDisabled(!status_.open);
    if (ImGui::Button("Reset MIDI")) engine_->resetMidiChannels();
    ImGui::EndDisabled();
    UiStyle::setItemTooltip("Stops all sound and resets the controllers, pedal and pitch bend of every part, and what the "
                          "MIDI extensions set (the bend ranges go back to 2 semitones). Every part's level goes to 100 and "
                          "its pan to the centre, as a GM or GS reset does (with the D-110 ROMs). Timbres, channels and the "
                          "rhythm keys' own levels and pans stay as they are.");
    if (ImGui::Checkbox("MIDI extensions", &engineOptions_.midiExtensions)) setOptions();
    helpMarker("Understands more than the real unit, as a General MIDI or GS module does. The pitch bend range belongs to "
               "each MIDI channel (RPN 0, and CC 38 for cents; 2 semitones after a reset), not to its timbre, so program "
               "changes keep it. GS NRPNs and GM2 sound controllers change a part's sound, 64 (40H) meaning no change: "
               "filter cutoff (NRPN 01H 20H, CC 74) and resonance (01H 21H, CC 71), attack, decay and release times "
               "(01H 63H, 64H, 66H; CC 73, 75, 72) and vibrato rate and depth (01H 08H, 09H; CC 76, 77); Reset All "
               "Controllers keeps them. Portamento: CC 65 switches it on, so each note glides from the last one, CC 5 "
               "sets how long it takes (0 fast, 127 slow), and CC 84 names the key the next note glides from (a note "
               "still sounding there moves to the new key instead). RPN 1 and 2 set the fine tune and key shift, GS master "
               "tune (40 00 00) tunes the synth on top of its own master tune, and GM, GS and XG resets reset the MIDI "
               "channels and all of this, with every part's level at 100 and its pan centred, as Reset MIDI does. After "
               "such a reset, MT-32 SysEx no longer changes the part channels: GS files "
               "send it to an MT-32 beside the GS module, often to silence it.\n\nThe extensions are off in MT-32 "
               "translation, with the MT-32's own ROMs and in ROM Play, as that music was made for the real units. Without "
               "them, RPN 0 sets the timbre's bender range until the next program change, as on the unit.");
    if (engineOptions_.midiExtensions && status_.open && !status_.midiExtensions) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", !status_.d110 ? "Off with MT-32 ROMs" : status_.mt32Translation ? "Off while translating" : "Off in ROM Play");
    }
    bool mt32 = mt32Mode_;
    if (ImGui::Checkbox("MT-32 translation", &mt32)) setMt32Mode(mt32);
    helpMarker("Plays music made for the MT-32 (or CM-32L) as a translation box would: MIDI input, MIDI files and "
               "SysEx files are converted for the D-110. MT-32 waves become the D-110's samples of the same "
               "instruments at the MT-32's pitch (often the very same samples), pans are mirrored, and SysEx for the "
               "MT-32's unit (17) arrives whatever the unit number. The MT-32's presets play exactly with its control "
               "ROM (see below), otherwise D-110 presets of the same kind stand in. The CM-32L's sound effects have "
               "no D-110 counterpart.\n\nTurning it on sets up the MT-32's power-on state (parts 1-8 on channels "
               "2-9, rhythm on 10); turning it off brings back the D-110 tones, timbres, rhythm setup and parts as "
               "they were. Needs the D-110 ROMs.");
    ImGui::Indent();
    if (mt32PresetRom_ < 0) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("MT-32 presets: D-110 stand-ins");
        helpMarker("MT-32 presets and rhythm sounds become D-110 presets of the same kind, which often sound "
                   "different (Syn Brass 1 is the D-110's much richer Brass 1, for instance); Choose picks others. Put "
                   "an MT-32 or CM-32L control ROM into the ROM folder to play the MT-32's own presets instead.");
        ImGui::SameLine();
        if (ImGui::Button("Choose...")) {
            showMt32Presets_ = true;
            saveSettings();
        }
    } else {
        static const char* const modeNames[] = {"D-110 stand-ins", "Chosen one by one", "The MT-32's own"};
        int mode = int(mt32PresetMode_);  // StandIns, Hybrid, Exact
        ImGui::SetNextItemWidth(settingWidth(8.5f, 12.0f));  // Room for the help marker and Choose
        if (ImGui::Combo("##presetmode", &mode, modeNames, 3)) {
            mt32PresetMode_ = Mt32Translator::PresetMode(mode);
            engine_->setMt32PresetMode(mt32PresetMode_);
            cacheTime_ = -1.0;
            saveSettings();
        }
        helpMarker("What the MT-32's presets (A1-B64) and rhythm sounds (R1-R30) play while translating: D-110 presets "
                   "of the same kind, as on a real D-110; the MT-32's own, translated for the D-110 from its control ROM; "
                   "or a choice for each (Choose). The MT-32's own and chosen ones take the place of the D-110's preset "
                   "tones while translating (a11-b88 are A1-B64, r01-r63 are R1-R63) and the D-110's a and b move to "
                   "tone banks d and e.\n\nWhen not translating (or with stand-ins), d11-e88 are the MT-32's presets, "
                   "which any part can play: pick them as tones, or as the timbres P-D11-P-E88.");
        ImGui::SameLine();
        if (ImGui::Button("Choose...")) {
            showMt32Presets_ = true;
            saveSettings();
        }
        std::vector<int> choices;
        for (int i = 0; i < int(roms_.size()); i++) {
            const std::string family = romFamily(roms_[i]);
            if (roms_[i].isControl && (family == "mt32" || family == "cm32l")) choices.push_back(i);
        }
        ImGui::SetNextItemWidth(settingWidth(2.0f, 26.0f));
        const std::string current = roms_[mt32PresetRom_].description + " (" + roms_[mt32PresetRom_].fileName + ")";
        ImGui::BeginDisabled(choices.size() < 2);
        if (ImGui::BeginCombo("##mt32presetrom", current.c_str())) {
            for (int i : choices) {
                const std::string label = roms_[i].description + " (" + roms_[i].fileName + ")";
                if (ImGui::Selectable(label.c_str(), i == mt32PresetRom_) && i != mt32PresetRom_) {
                    mt32PresetRom_ = i;
                    useMt32PresetRom();
                    saveSettings();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        UiStyle::setItemTooltip("The control ROM the MT-32 presets come from.");
        if (!mt32PresetError_.empty()) ImGui::TextColored(kErrorColor, "%s", mt32PresetError_.c_str());
    }
    if (ImGui::Checkbox("Roomy toms", &mt32RoomyToms_)) {
        engine_->setMt32RoomyToms(mt32RoomyToms_);
        saveSettings();
    }
    helpMarker("The MT-32 plays all its toms with one sample, the one the D-110's TomTom2 set (r31-r33) uses, at the "
               "same pitches, so MT-32 toms play that set. On, they play the roomier TomTom1 set (r28-r30) instead, "
               "which suits some music better; the MT-32's own timbres then use its sample too.");
    ImGui::Unindent();
    ImGui::Spacing();
}

void App::drawSoundPanel() {
    ImGui::SeparatorText("Sound");
    const float width = settingWidth(7.0f);

    ImGui::BeginDisabled(!status_.open);
    // These live in the synth's memory (like on the real unit), so MIDI SysEx can change them too.
    if (!status_.d110) {
        // The D-110 has no master volume parameter, only the analog volume knob.
        int volume = status_.masterVolume;
        ImGui::SetNextItemWidth(width);
        if (ImGui::SliderInt("Master volume", &volume, 0, 100)) engine_->setMasterVolume(volume);
    }
    int masterTune = status_.masterTune;
    // The D-110's and D-10/D-20's factory setting is 440.0 Hz; the MT-32's (and MT-32 translation's) 442.0 Hz.
    const int defaultTune = status_.d110 && !status_.mt32Translation ? 0x40 : 0x4A;
    ImGui::SetNextItemWidth(width);
    if (ImGui::SliderInt("Master tune", &masterTune, 0, 127, masterTuneText(masterTune).c_str(), ImGuiSliderFlags_AlwaysClamp)) {
        engine_->setMasterTune(masterTune);
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) engine_->setMasterTune(defaultTune);
    UiStyle::setItemTooltip("The unit's master tune: A4 from 427.5 to 452.7 Hz. Right-click for the default, %s. It is kept in "
                          "the unit's memory, and SysEx can set it too.", masterTuneText(defaultTune).c_str());
    if (status_.gsMasterTune != 0) {
        const double cents = status_.gsMasterTune / 10.0;
        const double hz = 440.0 * std::exp2((status_.masterTune - 64.0) / 1536.0 + cents / 1200.0);
        ImGui::TextDisabled("GS master tune %+.1f cents on top: A4 at %.1f Hz", cents, hz);
        UiStyle::setItemTooltip("A GS master tune message (MIDI extensions) tunes the synth on top of the unit's own master tune "
                              "until a GS, GM or XG reset, Reset MIDI, or the next GS master tune.");
    }
    int reverbMode = std::min<int>(status_.reverbMode, status_.d110 ? 8 : 3);
    int reverbTime = status_.reverbTime + 1;
    int reverbLevel = status_.reverbLevel;
    bool reverbChanged = false;
    ImGui::SetNextItemWidth(width);
    if (status_.d110) {
        reverbChanged |= ImGui::Combo("Reverb type", &reverbMode, kD110ReverbNames, IM_ARRAYSIZE(kD110ReverbNames));
        UiStyle::setItemTooltip(engineOptions_.dSeriesReverb
                                  ? "The D-series reverb model plays all eight types (Reverb model below)."
                                  : "The MT-32 chip model has four reverbs: the rooms play its Room, the halls its Hall, the "
                                    "delays its Tap delay.");
    } else {
        reverbChanged |= ImGui::Combo("Reverb type", &reverbMode, kReverbModeNames, IM_ARRAYSIZE(kReverbModeNames));
    }
    ImGui::SetNextItemWidth(width);
    reverbChanged |= ImGui::SliderInt("Reverb time", &reverbTime, 1, 8);
    ImGui::SetNextItemWidth(width);
    reverbChanged |= ImGui::SliderInt("Reverb level", &reverbLevel, 0, 7);
    if (reverbChanged) engine_->setReverb(reverbMode, reverbTime - 1, reverbLevel);
    if (status_.d110) {
        int model = engineOptions_.dSeriesReverb ? 0 : 1;
        static const char* const kReverbModels[] = {"D-series", "MT-32 chip"};
        ImGui::SetNextItemWidth(width - ImGui::GetStyle().ItemSpacing.x - ImGui::CalcTextSize("Tune...").x -
                                2.0f * ImGui::GetStyle().FramePadding.x);
        if (ImGui::Combo("##reverbmodel", &model, kReverbModels, IM_ARRAYSIZE(kReverbModels))) {
            engineOptions_.dSeriesReverb = model == 0;
            setOptions();
        }
        UiStyle::setItemTooltip("D-series: all eight types of the D-110, D-10 and D-20, tunable per type (Tune...).\n"
                              "MT-32 chip: mt32emu's emulation of the MT-32 family's reverb chip, with four types.");
        ImGui::SameLine();
        if (ImGui::Button("Tune...")) showReverbTuning_ = true;
        ImGui::SameLine();
        ImGui::TextUnformatted("Reverb model");
    }
    ImGui::EndDisabled();

    ImGui::Separator();
    float outputGain = engineOptions_.outputGain * 100.0f;
    ImGui::SetNextItemWidth(width);
    if (ImGui::SliderFloat("Output gain", &outputGain, 0.0f, 400.0f, "%.0f%%")) {
        engineOptions_.outputGain = outputGain / 100.0f;
        setOptions();
    }
    float reverbGain = engineOptions_.reverbGain * 100.0f;
    ImGui::SetNextItemWidth(width);
    if (ImGui::SliderFloat("Reverb gain", &reverbGain, 0.0f, 400.0f, "%.0f%%")) {
        engineOptions_.reverbGain = reverbGain / 100.0f;
        setOptions();
    }
    if (ImGui::Checkbox("Reverb", &engineOptions_.reverbEnabled)) setOptions();
    ImGui::SameLine();
    if (ImGui::Checkbox("Lock reverb", &engineOptions_.reverbOverridden)) setOptions();
    UiStyle::setItemTooltip("Ignore reverb settings sent over MIDI (the reverb controls above are ignored too).");
    ImGui::SameLine();
    if (ImGui::Checkbox("Swap L/R", &engineOptions_.reversedStereo)) setOptions();

    if (ImGui::TreeNode("Emulation quirks")) {
        if (ImGui::Checkbox("Nice amp ramp", &engineOptions_.niceAmpRamp)) setOptions();
        UiStyle::setItemTooltip("Avoid sudden jumps in amplitude ramps (smoother, slightly less accurate).");
        if (ImGui::Checkbox("Nice panning", &engineOptions_.nicePanning)) setOptions();
        UiStyle::setItemTooltip("Finer panning than the real LA32 chip allows: all 15 panpot steps instead of 8. With the "
                              "D-110 ROMs, pan also becomes a slider from -64 to +64 for parts and rhythm keys, and "
                              "MIDI pan (CC 10) keeps its full resolution.");
        if (ImGui::Checkbox("Nice partial mixing", &engineOptions_.nicePartialMixing)) setOptions();
        UiStyle::setItemTooltip("Always mix partials in phase, so closely tuned partials never cancel out.");
        int dacMode = int(engineOptions_.dacInputMode);
        ImGui::SetNextItemWidth(width);
        if (ImGui::Combo("DAC input", &dacMode, kDacModeNames, IM_ARRAYSIZE(kDacModeNames))) {
            engineOptions_.dacInputMode = MT32Emu::DACInputMode(dacMode);
            setOptions();
        }
        ImGui::TreePop();
    }
}

void App::drawPlayerPanel() {
    const PlayerStatus& player = status_.player;
    const bool loaded = player.state != MidiPlayer::State::Empty;
    const bool playing = player.state == MidiPlayer::State::Playing;
    const float fontSize = ImGui::GetFontSize();

    if (ImGui::Button("Open MIDI...")) browseMidiFile();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (loaded) {
        ImGui::TextUnformatted(player.name.c_str());
    } else {
        ImGui::TextDisabled("Open or drop a .mid or .syx file");
    }

    ImGui::BeginDisabled(!loaded || !status_.open);
    if (ImGui::Button(playing ? "Pause" : "Play", ImVec2(fontSize * 3.5f, 0.0f))) {
        if (playing) {
            engine_->playerPause();
        } else {
            engine_->playerPlay();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop")) engine_->playerStop();
    ImGui::SameLine();
    bool loop = player.loop;
    if (ImGui::Checkbox("Loop", &loop)) engine_->playerSetLoop(loop);
    ImGui::EndDisabled();
    ImGui::SameLine();
    const float fraction = player.duration > 0.0 ? float(player.position / player.duration) : 0.0f;
    const std::string overlay = formatTime(player.position) + " / " + formatTime(player.duration);
    ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 0.0f), overlay.c_str());
    if (!playerError_.empty()) ImGui::TextColored(kErrorColor, "%s", playerError_.c_str());

    if (romSongs_.empty()) return;
    // ROM Play: the demo songs stored in the D-110 control ROM.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("ROM Play");
    ImGui::SameLine();
    auto songLabel = [&](int song) {
        return song < int(romSongs_.size()) ? std::to_string(song + 1) + ": " + romSongs_[size_t(song)].name : std::string("All songs (chain)");
    };
    // The song list takes what the Play and Export buttons leave (the LCD beside the strip is wider at some sizes).
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        const float buttons = ImGui::CalcTextSize("Play").x + ImGui::CalcTextSize("Export .mid").x + 4.0f * style.FramePadding.x +
                              2.0f * style.ItemSpacing.x;
        ImGui::SetNextItemWidth(std::clamp(ImGui::GetContentRegionAvail().x - buttons, fontSize * 5.0f, fontSize * 9.0f));
    }
    if (ImGui::BeginCombo("##romsong", songLabel(romSongChoice_).c_str())) {
        for (int song = 0; song <= int(romSongs_.size()); song++) {
            if (ImGui::Selectable(songLabel(song).c_str(), song == romSongChoice_)) romSongChoice_ = song;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!status_.open);
    if (ImGui::Button("Play##rom")) startRomPlay(romSongChoice_);
    ImGui::EndDisabled();
    UiStyle::setItemTooltip("Plays the song like the D-110's ROM Play mode; your parts are restored when it stops.");
    ImGui::SameLine();
    if (ImGui::Button("Export .mid")) exportRomSongs(romSongChoice_);
    UiStyle::setItemTooltip("Saves the song as a Standard MIDI File at the song's tempo, with the SysEx setup a D-110 needs; "
                          "\"All songs\" saves all of them into a folder.");
}

void App::drawPartsPanel() {
    const float fontSize = ImGui::GetFontSize();
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("parts", 11, flags)) return;
    ImGui::TableSetupColumn("Part", ImGuiTableColumnFlags_WidthFixed, fontSize * 2.3f);
    const float muteButton = ImGui::GetFrameHeight();
    ImGui::TableSetupColumn("M  S", ImGuiTableColumnFlags_WidthFixed, muteButton * 2.0f + ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TableSetupColumn("Ch", ImGuiTableColumnFlags_WidthFixed, fontSize * 3.4f);
    ImGui::TableSetupColumn(status_.d110 ? "Timbre" : "Patch", ImGuiTableColumnFlags_WidthFixed, fontSize * 9.0f);
    ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, fontSize * 2.6f);
    const bool finePan = engineOptions_.nicePanning && status_.d110;  // Pan sliders from -64 to +64
    ImGui::TableSetupColumn("Pan", ImGuiTableColumnFlags_WidthFixed, fontSize * (finePan ? 3.4f : 2.4f));
    ImGui::TableSetupColumn("Output", ImGuiTableColumnFlags_WidthFixed, fontSize * 6.2f);
    ImGui::TableSetupColumn("Vol", ImGuiTableColumnFlags_WidthFixed, fontSize * 2.6f);
    ImGui::TableSetupColumn("Exp", ImGuiTableColumnFlags_WidthFixed, fontSize * 2.6f);
    ImGui::TableSetupColumn("Ptl", ImGuiTableColumnFlags_WidthFixed, fontSize * 3.0f);
    ImGui::TableSetupColumn("Notes", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    const char* const stepHelp = "Left click: up, right click: down (hold to repeat); mouse wheel; Shift: steps of 10";

    for (int i : partOrder()) {
        const PartStatus& part = status_.parts[i];
        const bool rhythm = i == kRhythmPart;
        ImGui::TableNextRow();
        ImGui::PushID(i);
        ImGui::BeginDisabled(!status_.open);

        ImGui::TableNextColumn();
        led(part.active);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(partLabel(i).c_str());

        // Mute and solo: left click toggles; right click unmutes every part, or solos this one alone.
        ImGui::TableNextColumn();
        {
            bool changed = false;
            auto toggle = [&](const char* label, bool& on, const ImVec4& color, const char* help) {
                const bool lit = on;  // The click below changes `on`; pop what was pushed
                if (lit) {
                    ImGui::PushStyleColor(ImGuiCol_Button, color);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.05f, 0.05f, 0.05f, 1.0f));
                }
                if (ImGui::Button(label, ImVec2(muteButton, 0.0f))) {
                    on = !on;
                    changed = true;
                }
                if (lit) ImGui::PopStyleColor(2);
                UiStyle::setItemTooltip("%s", help);
                return ImGui::IsItemClicked(ImGuiMouseButton_Right);
            };
            if (toggle("M", partMuted_[size_t(i)], ImVec4(0.85f, 0.35f, 0.3f, 1.0f), "Mute: this part is not heard (right-click: unmute all)")) {
                partMuted_.fill(false);
                changed = true;
            }
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            if (toggle("S", partSolo_[size_t(i)], ImVec4(0.95f, 0.8f, 0.3f, 1.0f), "Solo: only soloed parts are heard (right-click: this part alone, or no solo)")) {
                const bool alone = partSolo_[size_t(i)] && std::count(partSolo_.begin(), partSolo_.end(), true) == 1;
                partSolo_.fill(false);
                partSolo_[size_t(i)] = !alone;
                changed = true;
            }
            if (changed) applyPartMutes();
        }

        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##channel", channelLabel(part.channel).c_str())) {
            for (uint8_t channel = 0; channel <= kChannelOff; channel++) {
                if (ImGui::Selectable(channelLabel(channel).c_str(), channel == part.channel) && channel != part.channel) {
                    std::array<uint8_t, kMaxPartCount> channels = channels_;
                    for (int p = 0; p < int(status_.partCount); p++) channels[p] = status_.parts[p].channel;
                    channels[i] = channel;
                    setChannels(channels, true);
                }
            }
            ImGui::EndCombo();
        }

        // Timbre (D-110) or patch (MT-32) with the tone it plays; click to pick another, like a program change.
        ImGui::TableNextColumn();
        if (rhythm) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Rhythm");
        } else {
            std::string name(part.name);
            name.erase(name.find_last_not_of(' ') + 1);
            std::string code;
            if (!status_.d110) {
                code = part.program != 0xFF ? std::to_string(part.program + 1) : std::string("--");
            } else {
                code = part.program != 0xFF ? timbreCode(partTimbre(part)).substr(2) : std::string(part.tone);
            }
            ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
            if (ImGui::Button((code + " " + name).c_str(), ImVec2(-FLT_MIN, 0.0f))) ImGui::OpenPopup("programs");
            ImGui::PopStyleVar();
            if (status_.d110) {
                UiStyle::setItemTooltip("Timbre %s, tone %s %s. Click to choose a timbre.", part.program != 0xFF ? timbreCode(partTimbre(part)).c_str() : "(none)",
                                      part.tone, name.c_str());
            }
            if (ImGui::BeginPopup("programs")) {
                ImGui::BeginChild("list", ImVec2(fontSize * 16.0f, fontSize * 22.0f));
                const int current = partTimbre(part);
                for (int t = 0; t < (status_.d110 ? 384 : 128); t++) {
                    if (status_.d110 && !timbreSelectable(t)) continue;
                    const std::string label = status_.d110 ? timbreLabel(t) : "Program " + std::to_string(t + 1);
                    if (ImGui::Selectable(label.c_str(), t == current)) {
                        engine_->setPartProgram(i, t);
                        ImGui::CloseCurrentPopup();
                    }
                    if (t == current && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY();
                }
                ImGui::EndChild();
                ImGui::EndPopup();
            }
        }

        ImGui::TableNextColumn();
        const int level = part.temp[TimbreTemp::OutputLevel];
        if (const int delta = stepperCell("level", std::to_string(level).c_str(), level / 100.0f)) {
            engine_->setPartParameter(i, TimbreTemp::OutputLevel, uint8_t(std::clamp(level + delta, 0, 100)));
        }
        UiStyle::setItemTooltip("Output level. %s", stepHelp);

        ImGui::TableNextColumn();
        if (!rhythm && finePan) {
            int pan = part.finePan;
            if (panSlider("##pan", pan, -FLT_MIN)) engine_->setPartFinePan(i, pan);
            UiStyle::setItemTooltip("%s", kFinePanTip);
        } else if (!rhythm) {
            // Shown and stepped left to right; the MT-32 stores pan the other way round.
            const int pan = part.temp[TimbreTemp::Panpot];
            const int shown = status_.d110 ? pan : 14 - pan;
            if (const int delta = stepperCell("pan", panLabel(shown).c_str(), -1.0f)) {
                const int moved = std::clamp(shown + delta, 0, 14);
                engine_->setPartParameter(i, TimbreTemp::Panpot, uint8_t(status_.d110 ? moved : 14 - moved));
            }
            UiStyle::setItemTooltip("Pan: left click moves right, right click moves left (hold to repeat); mouse wheel");
        }

        ImGui::TableNextColumn();
        if (!rhythm || options_.partOutputs) drawOutputCombo(int(i), "##output", -FLT_MIN, true);

        // MIDI volume and expression are controller states: stepping sends CC 7 / CC 11 on the part's channel.
        auto controller = [&](const char* id, int value, uint8_t cc, const char* help) {
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(part.channel >= 16);
            if (const int delta = stepperCell(id, std::to_string(value).c_str(), value / 100.0f)) {
                const int newValue = std::clamp(value + delta, 0, 100);
                sendShort(uint8_t(0xB0 | part.channel), cc, uint8_t((newValue * 127 + 99) / 100));
            }
            ImGui::EndDisabled();
            UiStyle::setItemTooltip("%s %s", help, stepHelp);
        };
        controller("volume", part.midiVolume, 7, "MIDI volume (CC 7).");
        controller("expression", part.expression, 11, "Expression (CC 11).");

        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        const bool overReserve = part.activePartials > part.reservedPartials;
        ImGui::TextColored(overReserve ? ImVec4(1.0f, 0.75f, 0.35f, 1.0f) : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%u/%u", part.activePartials,
                           part.reservedPartials);
        UiStyle::setItemTooltip("Partials playing / reserved for this part. Beyond its reserve, a part's notes can be taken "
                              "over by parts that are still within theirs.");

        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        std::vector<uint8_t> keys(part.keys, part.keys + std::min<uint32_t>(part.noteCount, kMaxPartials));
        std::sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        std::string notes;
        for (size_t k = 0; k < keys.size() && k < 6; k++) notes += (k ? " " : "") + noteName(keys[k]);
        if (keys.size() > 6) notes += " +" + std::to_string(keys.size() - 6);
        ImGui::TextUnformatted(notes.c_str());

        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void App::drawOutputCombo(int part, const char* id, float width, bool compact) {
    const PartStatus& status = status_.parts[size_t(part)];
    const bool rhythm = part == kRhythmPart;
    const bool own = options_.partOutputs && ((ownOutputs_ >> part) & 1) != 0;
    const int assign = std::min(int(status.temp[TimbreTemp::OutputAssign]), status_.d110 ? 7 : 1);
    const bool reverb = assign == 1;  // Mix + reverb (MT-32: reverb on)
    const std::string output = partOutputName(part);
    std::string preview;
    if (rhythm) {
        preview = own ? (compact ? "Own out" : "Own output") : (compact ? "Per key" : "Per key (the Rhythm tab)");
    } else if (own) {
        preview = compact ? (reverb ? "Own+Rev" : "Own out") : (reverb ? "Own output + reverb" : "Own output");
    } else {
        preview = status_.d110 ? outputName(assign, compact) : kMT32OutputNames[assign];
    }
    // A part on an output the host has switched off is not heard: shown in amber.
    const bool unheard = own ? hostPartOutputOff(part) : !rhythm && hostOutputOff(assign);
    ImGui::SetNextItemWidth(width);
    if (unheard) ImGui::PushStyleColor(ImGuiCol_Text, kWarningColor);
    const bool open = ImGui::BeginCombo(id, preview.c_str(), ImGuiComboFlags_HeightLarge);  // All the choices in view
    if (unheard) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        const std::string ownOutput = "the plugin's own output for this part (" + output + "), where it plays with its pan, dry or "
                                      "feeding the reverb, whose return stays in the mix";
        std::string tip;
        if (rhythm) {  // The plugins only
            tip = "Per key: each key plays where the Rhythm tab sends it: the mix (dry or with reverb)" +
                  (status_.d110 ? ", or " + multiChoicesText() + ", " + multiOutputsText() : std::string()) +
                  ". Own output: the whole rhythm part plays out of the plugin's " + output +
                  " output instead, with the keys' pans; keys with reverb still feed the reverb, whose return stays in the mix.";
        } else if (!status_.d110) {
            tip = options_.partOutputs ? "Reverb on or off, or " + ownOutput + "." : std::string("Reverb on or off.");
        } else if (!options_.partOutputs) {
            tip = "Mix (dry), Mix + reverb, or " + multiChoicesText() + ": " + multiOutputsText() + ".";
        } else {
            tip = "Mix (dry), Mix + reverb, " + multiChoicesText() + " (" + multiOutputsText() + "), or " + ownOutput + ".";
        }
        if (unheard) tip += "\n\nThe DAW has this output switched off, so the part is not heard: switch the plugin's outputs on in the DAW.";
        UiStyle::setTooltip("%s", tip.c_str());
    }
    if (!open) return;
    if (rhythm) {
        if (ImGui::Selectable("Per key (the Rhythm tab)", !own)) setOwnOutput(part, false);
        if (ImGui::Selectable(("Own output (" + output + ")").c_str(), own)) setOwnOutput(part, true);
    } else {
        const int assigns = status_.d110 ? 8 : 2;
        const bool pairs = status_.d110 && multiPairsInUse();
        for (int a = 0; a < assigns; a++) {
            if (pairs && a >= 2 && (a & 1) != 0) continue;  // MULTI 2, 4 and 6: the same stereo output as 1, 3 and 5
            const bool samePair = pairs && a >= 2 && assign >= 2 && (assign - 2) / 2 == (a - 2) / 2;
            if (ImGui::Selectable(status_.d110 ? outputName(a, false) : kMT32OutputNames[a], !own && (samePair || a == assign))) {
                if (!samePair) engine_->setPartParameter(part, TimbreTemp::OutputAssign, uint8_t(a));
                setOwnOutput(part, false);
            }
        }
        if (options_.partOutputs) {
            ImGui::Separator();
            if (ImGui::Selectable(("Own output (" + output + ")").c_str(), own && !reverb)) {
                if (reverb) engine_->setPartParameter(part, TimbreTemp::OutputAssign, 0);
                setOwnOutput(part, true);
            }
            if (ImGui::Selectable(("Own output (" + output + ") + reverb").c_str(), own && reverb)) {
                engine_->setPartParameter(part, TimbreTemp::OutputAssign, 1);
                setOwnOutput(part, true);
            }
        }
    }
    ImGui::EndCombo();
}

std::string App::multiOutputsText() const {
    if (options_.partOutputs && multiPairs_) {
        return "the plugin's stereo outputs Multi 1+2, 3+4 and 5+6, where either output of a pair plays with its pan, dry, "
               "even with reverb on";
    }
    if (options_.partOutputs) return "the plugin's Multi 1-6 outputs, mono and dry, even with reverb on";
    if (surround_ && multiPairs_) {
        return "in 7.1 surround (File > Configuration), stereo pairs of speakers, with their pan, dry, even with reverb on: "
               "Multi 1+2 at the front, 3+4 at the rear, 5+6 at the side";
    }
    if (surround_) {
        return "in 7.1 surround (File > Configuration), speakers of their own, dry, even with reverb on: Multi 1 and 2 front "
               "left and right, 3 and 4 rear left and right, 5 and 6 side left and right";
    }
    return "mixed in, dry and centred; Multi 5 and 6 are silent while reverb is on, as on the unit (7.1 surround, in File > "
           "Configuration, gives each its own speaker)";
}

std::string App::multiChoicesText() const {
    return multiPairsInUse() ? "Multi 1+2, 3+4 or 5+6" : "Multi 1-6";
}

bool App::hostOutputOff(int output) const {
    if (!options_.hostOutputs || output < 2 || output > 7) return false;
    return ((options_.hostOutputs() >> (16 + output - 2)) & 1) == 0;
}

bool App::hostPartOutputOff(int part) const {
    if (!options_.hostOutputs || part < 0 || part >= kMaxPartCount) return false;
    return ((options_.hostOutputs() >> part) & 1) == 0;
}

void App::setOwnOutput(int part, bool own) {
    if (!options_.partOutputs || part < 0 || part >= kMaxPartCount) return;
    const uint32_t mask = own ? ownOutputs_ | 1u << part : ownOutputs_ & ~(1u << part);
    if (mask == ownOutputs_) return;
    ownOutputs_ = mask;
    engine_->setPartOutputMask(ownOutputs_);
    saveSettings();
}

void App::drawPartialsPanel() {
    int active = 0;
    for (uint32_t i = 0; i < status_.partialCount; i++) {
        if (status_.partialStates[i] != MT32Emu::PartialState_INACTIVE) active++;
    }
    char title[64];
    std::snprintf(title, sizeof(title), "Partials: %d / %u", active, status_.partialCount);
    ImGui::SeparatorText(title);
    UiStyle::setItemTooltip("View > Partials hides this display");

    const float cell = ImGui::GetFontSize() * 0.95f;
    const float gap = std::max(2.0f, cell * 0.2f);
    const float available = ImGui::GetContentRegionAvail().x;
    const int count = int(status_.partialCount > 0 ? status_.partialCount : uint32_t(kDefaultPartials));
    const int perRow = std::clamp(int((available + gap) / (cell + gap)), 1, count);
    const int rows = (count + perRow - 1) / perRow;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    for (int i = 0; i < count; i++) {
        const int state = i < int(status_.partialCount) ? int(status_.partialStates[i]) : 0;
        const ImVec2 min(origin.x + (i % perRow) * (cell + gap), origin.y + (i / perRow) * (cell + gap));
        drawList->AddRectFilled(min, ImVec2(min.x + cell, min.y + cell), kPartialColors[std::clamp(state, 0, 3)], 2.0f);
    }
    ImGui::Dummy(ImVec2(perRow * (cell + gap), rows * (cell + gap)));

    const char* const labels[] = {"Attack", "Sustain", "Release"};
    for (int i = 0; i < 3; i++) {
        if (i > 0) ImGui::SameLine(0.0f, ImGui::GetFontSize());
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float size = ImGui::GetFontSize() * 0.7f;
        const float offset = (ImGui::GetTextLineHeight() - size) * 0.5f;
        drawList->AddRectFilled(ImVec2(pos.x, pos.y + offset), ImVec2(pos.x + size, pos.y + offset + size), kPartialColors[i + 1], 2.0f);
        ImGui::Dummy(ImVec2(size, ImGui::GetTextLineHeight()));
        ImGui::SameLine();
        ImGui::TextDisabled("%s", labels[i]);
    }
}

void App::drawTabContent(const char* id, bool keyboard, const std::function<void()>& content) {
    if (!keyboard || !showKeyboard_) {
        ImGui::BeginChild(id);  // Fills the rest of the window; scrolls when the content is taller
        content();
        ImGui::EndChild();
        return;
    }
    // The keys' natural height follows their width. When the content (as tall as it was at the last frame) leaves room,
    // they take it, up to that height, and the content does not scroll, as the Play tab has always been. When it does
    // not, the content scrolls above them and they keep their height, within half the tab.
    const float fontSize = ImGui::GetFontSize();
    int whiteCount = 0;
    for (int note = kKeyboardFirstNote; note <= kKeyboardLastNote; note++) {
        if (!Piano::isBlackKey(note)) whiteCount++;
    }
    const float whiteWidth = std::clamp(std::floor(ImGui::GetContentRegionAvail().x / float(whiteCount)), 8.0f, fontSize * 2.5f);
    const float natural = std::max(whiteWidth * 4.2f, fontSize * 4.0f);
    const float minimum = fontSize * 3.0f;
    const float area = ImGui::GetContentRegionAvail().y;
    if (keyboardControlsHeight_ <= 0.0f) {
        keyboardControlsHeight_ = ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing() * 2.0f;  // Until measured
    }
    const float room = area - keyboardContentHeights_[id] - keyboardControlsHeight_;
    const float pianoHeight = room >= minimum ? std::min(room, natural)
                                              : std::clamp(area * 0.5f - keyboardControlsHeight_, minimum, natural);
    ImGui::BeginChild(id, ImVec2(0.0f, -(keyboardControlsHeight_ + pianoHeight)));
    content();
    keyboardContentHeights_[id] = ImGui::GetCursorPosY();  // Scrolled or not
    ImGui::EndChild();
    const float top = ImGui::GetCursorPosY();
    drawKeyboardPanel(pianoHeight);
    keyboardControlsHeight_ = ImGui::GetCursorPosY() - top - pianoHeight;
}

void App::drawKeyboardPanel(float pianoHeight) {
    keyboardDrawn_ = true;
    ImGui::SeparatorText("Keyboard");
    const float fontSize = ImGui::GetFontSize();
    // In performance mode the Performance tab's keyboard plays the performance, as a D-20's own keyboard does.
    const bool playsPerformance = performanceTabShown_ && performanceMode_;
    const uint8_t playChannel = playsPerformance ? uint8_t(performanceChannel_) : uint8_t(keyboardChannel_);

    int channel = playChannel + 1;
    ImGui::SetNextItemWidth(fontSize * 5.5f);
    ImGui::BeginDisabled(playsPerformance);
    if (ImGui::InputInt("Channel", &channel) && !playsPerformance) {
        channel = std::clamp(channel, 1, 16);
        if (channel - 1 != keyboardChannel_) {
            // Release held notes on their channels first.
            for (int note = 0; note < 128; note++) {
                if (keyHeld_[note]) sendShort(uint8_t(0x80 | keyChannel_[size_t(note)]), uint8_t(note), 0);
                keyHeld_[note] = false;
            }
            keyboardChannel_ = channel - 1;
            saveSettings();
        }
    }
    ImGui::EndDisabled();
    if (playsPerformance) UiStyle::setItemTooltip("In performance mode the keyboard here plays the performance channel, set above.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fontSize * 7.0f);
    if (ImGui::SliderInt("Velocity", &velocity_, 1, 127)) saveSettings();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fontSize * 5.5f);
    if (ImGui::InputInt("##program", &program_)) program_ = std::clamp(program_, 1, 128);
    ImGui::SameLine();
    if (ImGui::Button("Program change")) sendShort(uint8_t(0xC0 | playChannel), uint8_t(program_ - 1), 0);
    if (playsPerformance) UiStyle::setItemTooltip("On the performance channel: selects performance A11-B88 (1-128).");
    ImGui::TextDisabled("Computer keys: %s to %s and %s to %s play from C%d; Page Up/Down change the octave.", Piano::keyName(0x2C).c_str(),
                        Piano::keyName(0x35).c_str(), Piano::keyName(0x10).c_str(), Piano::keyName(0x19).c_str(), keyboardOctave_);

    // Notes sounding on the parts that listen to the keyboard's channel.
    std::array<bool, 128> sounding = {};
    for (const PartStatus& part : status_.parts) {
        if (part.channel != playChannel) continue;
        for (uint32_t k = 0; k < part.noteCount && k < uint32_t(kMaxPartials); k++) sounding[part.keys[k] & 127] = true;
    }

    std::array<bool, 128> pressed = keyHeld_;
    if (mouseNote_ >= 0) pressed[size_t(mouseNote_)] = true;
    const int hitNote = Piano::draw("piano", kKeyboardFirstNote, kKeyboardLastNote, pianoHeight, fontSize * 2.5f, sounding, pressed);
    if (hitNote != mouseNote_ && status_.open) {
        if (mouseNote_ >= 0) sendShort(uint8_t(0x80 | mouseChannel_), uint8_t(mouseNote_), 0);
        if (hitNote >= 0) {
            mouseChannel_ = playChannel;
            sendShort(uint8_t(0x90 | playChannel), uint8_t(hitNote), uint8_t(velocity_));
        }
        mouseNote_ = hitNote;
    }
}

void App::onKey(int scancode, bool down) {
    if (scancode > 0 && scancode < int(scancodeDown_.size())) scancodeDown_[size_t(scancode)] = down;
}

void App::clearKeys() {
    scancodeDown_.fill(false);
}

void App::handleComputerKeyboard() {
    const ImGuiIO& io = ImGui::GetIO();
    std::array<bool, 128> wanted = {};
    if (status_.open && !io.WantTextInput && !io.KeyCtrl && !io.KeyAlt) {
        for (const Piano::Key& mapping : Piano::kKeys) {
            const int note = (keyboardOctave_ + 1) * 12 + mapping.semitone;
            if (note < 128 && scancodeDown_[mapping.scancode]) wanted[note] = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp, false)) keyboardOctave_ = std::min(keyboardOctave_ + 1, 7);
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown, false)) keyboardOctave_ = std::max(keyboardOctave_ - 1, 0);
    }
    const uint8_t channel = pianoChannel();
    for (int note = 0; note < 128; note++) {
        if (wanted[note] && !keyHeld_[note]) {
            keyChannel_[size_t(note)] = channel;
            sendShort(uint8_t(0x90 | channel), uint8_t(note), uint8_t(velocity_));
        }
        if (!wanted[note] && keyHeld_[note]) sendShort(uint8_t(0x80 | keyChannel_[size_t(note)]), uint8_t(note), 0);
        keyHeld_[note] = wanted[note];
    }
}

uint8_t App::pianoChannel() const {
    if (toneTabWasShown_ && tonePart_ >= 0 && tonePart_ < int(status_.partCount) && status_.parts[tonePart_].channel < 16) {
        return status_.parts[tonePart_].channel;
    }
    if (performanceTabWasShown_ && performanceMode_) return uint8_t(performanceChannel_);
    return uint8_t(keyboardChannel_);
}

void App::drawLogPanel() {
    const float fontSize = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(fontSize * 40.0f, fontSize * 16.0f), ImGuiCond_FirstUseEver);
    const bool visible = ImGui::Begin("Log", &showLog_);
    if (!showLog_) saveSettings();
    if (!visible) {
        ImGui::End();
        return;
    }
    if (ImGui::Button("Clear")) log_.clear();
    ImGui::BeginChild("lines", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    for (const std::string& line : log_) ImGui::TextUnformatted(line.c_str());
    if (scrollLogToBottom_) ImGui::SetScrollHereY(1.0f);
    scrollLogToBottom_ = false;
    ImGui::EndChild();
    ImGui::End();
}

// ---------------------------------------------------------------------------------------------
// The D-series reverb's tuning

void App::playReverbTest() {
    reverbTestChannel_ = pianoChannel();
    for (uint8_t key : {60, 64, 67}) sendShort(uint8_t(0x90 | reverbTestChannel_), key, 100);
    reverbTestOffAt_ = ImGui::GetTime() + 0.25;
}

namespace {

// Eight values in a row of vertical sliders with their labels below (1-8 or 0-7); true when one changed.
bool reverbTable(const char* id, float* values, float low, float high, const char* format, ImGuiSliderFlags flags, int firstLabel,
                 bool offAtBottom) {
    const float fontSize = ImGui::GetFontSize();
    const ImVec2 size(fontSize * 2.7f, fontSize * 5.5f);
    bool changed = false;
    ImGui::PushID(id);
    for (int i = 0; i < 8; i++) {
        if (i > 0) ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::BeginGroup();
        ImGui::PushID(i);
        const char* shown = offAtBottom && values[i] <= low + 0.01f ? "off" : format;
        changed |= ImGui::VSliderFloat("##value", size, &values[i], low, high, shown, flags | ImGuiSliderFlags_AlwaysClamp);
        const std::string label = std::to_string(firstLabel + i);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (size.x - ImGui::CalcTextSize(label.c_str()).x) * 0.5f);
        ImGui::TextDisabled("%s", label.c_str());
        ImGui::PopID();
        ImGui::EndGroup();
    }
    ImGui::PopID();
    return changed;
}

}  // namespace

void App::drawMt32PresetsWindow() {
    const float fontSize = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(fontSize * 40.0f, fontSize * 40.0f), ImGuiCond_FirstUseEver);
    const bool visible = ImGui::Begin("MT-32 presets", &showMt32Presets_);
    if (!showMt32Presets_) saveSettings();
    if (!visible) {
        ImGui::End();
        return;
    }
    const bool rom = mt32Presets_ != nullptr;
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("What the MT-32's presets and rhythm sounds play while MT-32 translation is on: a D-110 preset or "
                        "rhythm tone of the same kind (with a key shift where the D-110's is an octave away), or the "
                        "MT-32's own, translated from its control ROM.");
    ImGui::PopTextWrapPos();
    bool changed = false;
    if (rom) {
        int mode = int(mt32PresetMode_);  // StandIns, Hybrid, Exact
        bool modeChanged = ImGui::RadioButton("D-110 stand-ins", &mode, 0);
        ImGui::SameLine();
        modeChanged |= ImGui::RadioButton("Chosen one by one", &mode, 1);
        ImGui::SameLine();
        modeChanged |= ImGui::RadioButton("The MT-32's own", &mode, 2);
        if (modeChanged) {
            mt32PresetMode_ = Mt32Translator::PresetMode(mode);
            engine_->setMt32PresetMode(mt32PresetMode_);
            cacheTime_ = -1.0;
            saveSettings();
        }
        if (mt32PresetMode_ == Mt32Translator::PresetMode::Hybrid) {
            if (ImGui::SmallButton("All D-110's")) {
                for (Mt32Translator::PresetChoice& choice : mt32PresetChoices_) choice.exact = false;
                for (Mt32Translator::RhythmChoice& choice : mt32RhythmChoices_) choice.exact = false;
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("All MT-32's")) {
                for (Mt32Translator::PresetChoice& choice : mt32PresetChoices_) choice.exact = true;
                for (Mt32Translator::RhythmChoice& choice : mt32RhythmChoices_) choice.exact = true;
                changed = true;
            }
            ImGui::SameLine();
        }
    } else {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(kErrorColor, "No MT-32 control ROM: the D-110's stand-ins play. Put one in the ROM folder for the MT-32's own.");
        ImGui::PopTextWrapPos();
    }
    if (ImGui::SmallButton("All built in")) {
        Mt32Translator::parsePresetChoices(std::string(), mt32PresetChoices_);
        Mt32Translator::parseRhythmChoices(std::string(), mt32RhythmChoices_);
        changed = true;
    }
    UiStyle::setItemTooltip("Back to the choices that come with d110emu.");

    PresetChoiceTable::Context context;
    context.presets = mt32Presets_.get();
    context.mode = mt32PresetMode_;
    context.unitName = "D-110's";
    context.roomyToms = mt32RoomyToms_;
    if (ImGui::BeginTabBar("presettabs")) {
        if (ImGui::BeginTabItem("Presets")) {
            changed |= PresetChoiceTable::drawPresets(mt32PresetChoices_, context);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Rhythm")) {
            changed |= PresetChoiceTable::drawRhythm(mt32RhythmChoices_, context);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    if (changed) {
        engine_->setMt32PresetChoices(mt32PresetChoices_, mt32RhythmChoices_);
        cacheTime_ = -1.0;
        saveSettings();
    }
    ImGui::End();
}

void App::drawReverbTuningPanel() {
    const float fontSize = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(fontSize * 33.0f, fontSize * 46.0f), ImGuiCond_FirstUseEver);
    const bool visible = ImGui::Begin("Reverb tuning", &showReverbTuning_);
    if (!showReverbTuning_) saveSettings();
    if (!visible) {
        ImGui::End();
        return;
    }
    if (!status_.d110) {
        ImGui::TextWrapped("The D-series reverb is for the D-110 ROMs.");
        ImGui::End();
        return;
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("The D-110's reverb program is not known, so this is a model to tune by ear or to fit to recordings "
                        "of a real unit (reverb-kit/README.txt). Changes apply at once and are kept in d110emu-reverb.ini "
                        "%s.", options_.plugin ? ("in " + Platform::toUtf8(options_.reverbSettingsFile.parent_path())).c_str()
                                               : "next to the program");
    ImGui::PopTextWrapPos();
    int model = engineOptions_.dSeriesReverb ? 0 : 1;
    bool modelChanged = ImGui::RadioButton("D-series", &model, 0);
    ImGui::SameLine();
    modelChanged |= ImGui::RadioButton("MT-32 chip (not tunable)", &model, 1);
    if (modelChanged) {
        engineOptions_.dSeriesReverb = model == 0;
        setOptions();
    }

    // The type: the one the unit plays, or another.
    const int unitType = status_.reverbMode < 8 ? int(status_.reverbMode) : -1;
    const int type = reverbTuningType_ >= 0 ? reverbTuningType_ : std::max(unitType, 0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Type");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fontSize * 9.0f);
    if (ImGui::BeginCombo("##type", kD110ReverbNames[type])) {
        for (int t = 0; t < 8; t++) {
            if (ImGui::Selectable(kD110ReverbNames[t], t == type)) reverbTuningType_ = t == unitType ? -1 : t;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (type != unitType) {
        if (ImGui::Button("Play this type")) engine_->setReverb(type, status_.reverbTime, std::max<int>(status_.reverbLevel, 1));
        UiStyle::setItemTooltip("Sets the unit's reverb type to this one (System tab)");
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("playing at Reverb Time %d, Level %d", status_.reverbTime + 1, status_.reverbLevel);
    }
    if (ImGui::Button("Test chord")) playReverbTest();
    UiStyle::setItemTooltip("C, E and G for a quarter of a second on the keyboard channel. The part must have its output on "
                          "Mix + reverb; you can also play the keyboard.");
    if (!engineOptions_.dSeriesReverb) {
        ImGui::TextColored(kErrorColor, "The MT-32 chip model is playing: changes here are heard with D-series.");
    }

    const MT32Emu::DSeriesReverbSettings& defaults = MT32Emu::DSeriesReverbSettings::getDefaults();
    MT32Emu::DSeriesReverbType& t = reverbSettings_.types[type];
    bool changed = false;
    const float labelWidth = fontSize * 8.0f;
    auto slider = [&](const char* label, float* value, float low, float high, const char* format, ImGuiSliderFlags flags,
                      const char* help) {
        ImGui::SetNextItemWidth(-labelWidth);
        const bool moved = ImGui::SliderFloat(label, value, low, high, format, flags | ImGuiSliderFlags_AlwaysClamp);
        UiStyle::setItemTooltip("%s", help);
        changed |= moved;
    };

    ImGui::SeparatorText("Level");
    ImGui::TextUnformatted("Wet level per Reverb Level (dB)");
    UiStyle::setItemTooltip("The reverb's output for each Reverb Level. Level 0 is silent on the units.");
    changed |= reverbTable("wet", t.wetDb, -60.0f, 6.0f, "%.0f", 0, 0, true);
    slider("Send", &t.sendDb, -24.0f, 12.0f, "%.1f dB", 0, "What goes into the reverb (the parts with output Mix + reverb)");
    slider("Width", &t.width, 0.0f, 1.0f, "%.2f", 0, "0: the same in both channels; 1: as the model's taps make it");

    if (!t.delay) {
        ImGui::SeparatorText("Decay");
        ImGui::TextUnformatted("RT60 per Reverb Time (seconds, at 700 Hz)");
        UiStyle::setItemTooltip("How long the reverb takes to die away by 60 dB, for each Reverb Time");
        changed |= reverbTable("rt60", t.rt60, 0.05f, 10.0f, "%.2f", ImGuiSliderFlags_Logarithmic, 1, false);
        ImGui::SameLine();
        ImGui::BeginGroup();
        if (ImGui::Button("Longer")) {
            for (float& value : t.rt60) value *= 1.1f;
            changed = true;
        }
        if (ImGui::Button("Shorter")) {
            for (float& value : t.rt60) value /= 1.1f;
            changed = true;
        }
        ImGui::EndGroup();
        UiStyle::setItemTooltip("All eight by 10%%");
        slider("Damping", &t.dampingHz, 500.0f, 16000.0f, "%.0f Hz", ImGuiSliderFlags_Logarithmic,
               "Highs die away sooner than the RT60: a low-pass in the reverb's loops");
        ImGui::SeparatorText("Character");
        slider("Pre-delay", &t.preDelayMs, 0.0f, 150.0f, "%.1f ms", 0, "Silence before the reverb (its taps add some more)");
        slider("Bandwidth", &t.bandwidthHz, 500.0f, 16000.0f, "%.0f Hz", ImGuiSliderFlags_Logarithmic, "A low-pass on the way in");
        slider("Diffusion", &t.diffusion, 0.0f, 0.9f, "%.2f", 0, "How much the allpasses blur the echoes (0.5 on the MT-32's chip)");
        slider("Size", &t.size, 0.25f, 2.0f, "%.2f", 0, "Scales every delay of the structure: a bigger room, sparser echoes");
        if (ImGui::TreeNode("Structure")) {
            for (unsigned int i = 0; i < MT32Emu::DSeriesReverbType::ALLPASS_COUNT; i++) {
                const std::string label = "Allpass " + std::to_string(i + 1);
                slider(label.c_str(), &t.allpassMs[i], 0.1f, 50.0f, "%.1f ms", 0, "An allpass diffuser's delay");
            }
            for (unsigned int i = 0; i < MT32Emu::DSeriesReverbType::COMB_COUNT; i++) {
                const std::string label = "Comb " + std::to_string(i + 1);
                slider(label.c_str(), &t.combMs[i], 5.0f, 125.0f, "%.1f ms", 0, "A comb's loop: its echoes repeat at this interval");
            }
            ImGui::TreePop();
        }
    } else {
        ImGui::SeparatorText("Delay");
        ImGui::TextUnformatted("Left tap per Reverb Time (ms)");
        changed |= reverbTable("left", t.delayLMs, 1.0f, 1500.0f, "%.0f", ImGuiSliderFlags_Logarithmic, 1, false);
        ImGui::TextUnformatted("Right tap per Reverb Time (ms)");
        changed |= reverbTable("right", t.delayRMs, 1.0f, 1500.0f, "%.0f", ImGuiSliderFlags_Logarithmic, 1, false);
        slider("Feedback", &t.feedback, 0.0f, 0.95f, "%.2f", 0, "How much of the right tap goes round again");
        slider("Damping", &t.dampingHz, 500.0f, 16000.0f, "%.0f Hz", ImGuiSliderFlags_Logarithmic, "A low-pass on each repeat");
        slider("Bandwidth", &t.bandwidthHz, 500.0f, 16000.0f, "%.0f Hz", ImGuiSliderFlags_Logarithmic, "A low-pass on the way in");
    }

    ImGui::Separator();
    if (ImGui::Button("Reset this type")) {
        t = defaults.types[type];
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset all")) {
        reverbSettings_ = defaults;
        changed = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(options_.reverbSettingsFile.empty());
    if (ImGui::Button("Reload the file")) loadReverbSettingsFile();
    UiStyle::setItemTooltip("Reads d110emu-reverb.ini again, e.g. after replacing it with fitted values");
    ImGui::EndDisabled();
    if (changed) {
        reverbSettings_.clamp();
        engine_->setDSeriesReverbSettings(reverbSettings_);
        reverbSettingsDirty_ = true;
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------------------------
// D-110 panel: LCD, ROM Play, part/patch/rhythm editing

void App::drawLcdPanel() {
    const int dotPitch = std::max(3, int(std::lround(ImGui::GetFontSize() * 0.25f)));
    ImGui::BeginGroup();
    if (Lcd::draw("##lcd", lcdText(), dotPitch, lcdColors())) dismissLcdMessage();
    UiStyle::setItemTooltip("Click to return the display to its normal reading; right-click for its colours");
    if (ImGui::BeginPopupContextItem("lcdcolours")) {
        drawLcdColourItems();
        ImGui::EndPopup();
    }
    ImGui::BeginDisabled(!status_.open);
    bool viewChanged = false;
    if (status_.performanceMode && romPlaying_ < 0) {
        // The D-20's DISPLAY buttons: either one switches between the patch and its upper and lower tones.
        for (const ImGuiDir direction : {ImGuiDir_Up, ImGuiDir_Down}) {
            if (direction == ImGuiDir_Down) ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            if (ImGui::ArrowButton(direction == ImGuiDir_Up ? "##lcdup" : "##lcddown", direction)) {
                lcdToneView_ = !lcdToneView_;
                viewChanged = true;
            }
            UiStyle::setItemTooltip("DISPLAY: the patch, or its upper and lower tones");
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(lcdToneView_ ? "Upper and lower tones" : "Performance patch");
    } else {
        const std::vector<int> order = partOrder();
        const int position = int(std::find(order.begin(), order.end(), lcdPart_) - order.begin()) % int(order.size());
        if (ImGui::ArrowButton("##lcdprev", ImGuiDir_Left)) {
            lcdPart_ = order[size_t((position + int(order.size()) - 1) % int(order.size()))];
            viewChanged = true;
        }
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        if (ImGui::ArrowButton("##lcdnext", ImGuiDir_Right)) {
            lcdPart_ = order[size_t((position + 1) % int(order.size()))];
            viewChanged = true;
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(("Part " + partLabel(lcdPart_)).c_str());
        if (status_.d110) {
            ImGui::SameLine();
            viewChanged |= ImGui::Checkbox("Patch view", &lcdPatchView_);
        }
    }
    if (status_.mt32Translation) {
        ImGui::SameLine();
        ImGui::TextColored(kOkColor, "MT-32");
        UiStyle::setItemTooltip("MT-32 translation is on (System tab)");
    }
    if (viewChanged) lcdViewChanged();
    ImGui::EndDisabled();
    ImGui::EndGroup();

    ImGui::SameLine();
    ImGui::BeginGroup();
    drawPlayerPanel();
    ImGui::EndGroup();
    ImGui::Spacing();
}

Lcd::Colors App::lcdColors() const {
    if (lcdScheme_ != kCustomLcdScheme) return Lcd::schemeColors(lcdScheme_);
    auto pack = [](const std::array<float, 3>& rgb) { return uint32_t(ImGui::ColorConvertFloat4ToU32(ImVec4(rgb[0], rgb[1], rgb[2], 1.0f))); };
    return {pack(lcdCustom_[0]), pack(lcdCustom_[1]), pack(lcdCustom_[2])};
}

void App::drawLcdColourItems() {
    for (int scheme = 0; scheme < Lcd::schemeCount(); scheme++) {
        if (ImGui::MenuItem(Lcd::schemeName(scheme), nullptr, scheme == lcdScheme_)) {
            lcdScheme_ = scheme;
            saveSettings();
        }
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Custom", nullptr, lcdScheme_ == kCustomLcdScheme)) {
        useCustomLcdColours();
        saveSettings();
    }
    UiStyle::setItemTooltip("Your own colours, set in Edit custom colours...");
    if (ImGui::MenuItem("Edit custom colours...", nullptr, showLcdColourEditor_)) showLcdColourEditor(true);
}

void App::useCustomLcdColours() {
    if (!lcdCustomSet_ && lcdScheme_ != kCustomLcdScheme) lcdCustom_ = presetLcdColors(lcdScheme_);
    lcdCustomSet_ = true;
    lcdScheme_ = kCustomLcdScheme;
}

void App::showLcdColourEditor(bool on) {
    if (!on) {
        showLcdColourEditor_ = false;
        return;
    }
    useCustomLcdColours();
    lcdCustomOpened_ = lcdCustom_;
    showLcdColourEditor_ = true;
    focusLcdColourEditor_ = true;
    saveSettings();
}

void App::drawLcdColourEditor() {
    // At the top right, clear of the display, which shows every change as it is made.
    const float fontSize = ImGui::GetFontSize();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - fontSize, viewport->WorkPos.y + fontSize * 2.5f),
                            ImGuiCond_FirstUseEver, ImVec2(1.0f, 0.0f));
    if (focusLcdColourEditor_) {
        ImGui::SetNextWindowFocus();
        focusLcdColourEditor_ = false;
    }
    const bool visible = ImGui::Begin("Custom LCD colours", &showLcdColourEditor_, ImGuiWindowFlags_AlwaysAutoResize);
    if (visible) {
        for (int part = 0; part < 3; part++) {
            if (part > 0) ImGui::SameLine();
            ImGui::RadioButton(kLcdColourParts[part], &lcdColourPart_, part);
        }
        ImGui::PushID(lcdColourPart_);  // A picker of its own per colour (it keeps the hue of greys and blacks)
        ImGui::SetNextItemWidth(fontSize * 14.0f);
        if (ImGui::ColorPicker3("##colour", lcdCustom_[size_t(lcdColourPart_)].data(),
                                ImGuiColorEditFlags_PickerHueBar | ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_DisplayHex)) {
            lcdScheme_ = kCustomLcdScheme;
            lcdCustomDirty_ = true;
        }
        ImGui::PopID();
        const char* const startFrom = "Start from a preset";
        ImGui::SetNextItemWidth(ImGui::CalcTextSize(startFrom).x + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f);
        if (ImGui::BeginCombo("##startfrom", startFrom)) {
            for (int scheme = 0; scheme < Lcd::schemeCount(); scheme++) {
                if (ImGui::Selectable(Lcd::schemeName(scheme))) {
                    lcdCustom_ = presetLcdColors(scheme);
                    lcdScheme_ = kCustomLcdScheme;
                    lcdCustomDirty_ = true;
                }
            }
            ImGui::EndCombo();
        }
        UiStyle::setItemTooltip("Copies a preset's three colours, to change from there");
        ImGui::SameLine();
        ImGui::BeginDisabled(lcdCustom_ == lcdCustomOpened_);
        if (ImGui::Button("Undo changes")) {
            lcdCustom_ = lcdCustomOpened_;
            lcdCustomDirty_ = true;
        }
        ImGui::EndDisabled();
        UiStyle::setItemTooltip("Back to the colours from when this window opened");
    }
    ImGui::End();
}

void App::selectTab(const std::string& name) {
    requestedTab_ = name;
}

void App::exportRomSongs(int song) {
    if (romSongs_.empty()) return;
    auto fileName = [this](size_t index) {
        char name[64];
        std::snprintf(name, sizeof(name), "%02d %s.mid", int(index + 1), romSongs_[index].name.c_str());
        return std::string(name);
    };
    if (song < int(romSongs_.size())) {
        saveFileThen(Platform::FileKind::Midi, fileName(size_t(song)), [this, song](const std::filesystem::path& path) {
            if (song >= int(romSongs_.size())) return;
            std::string error;
            if (saveRomSongSmf(path, romSongs_[size_t(song)], uint8_t(engineOptions_.unitNumber - 1), error)) {
                addLog("Exported " + Platform::toUtf8(path.filename()));
            } else {
                addLog("Export: " + error);
            }
        });
        return;
    }
    pickFolderThen(lastMidiFolder_, [this, fileName](const std::filesystem::path& folder) {
        std::string error;
        for (size_t i = 0; i < romSongs_.size(); i++) {
            if (!saveRomSongSmf(folder / Platform::fromUtf8(fileName(i)), romSongs_[i], uint8_t(engineOptions_.unitNumber - 1), error)) {
                addLog("Export: " + error);
                return;
            }
        }
        addLog("Exported " + std::to_string(romSongs_.size()) + " ROM Play songs to " + Platform::toUtf8(folder));
    });
}

bool App::drawTonePicker(const char* id, uint8_t& group, uint8_t& number, const char* currentName, bool withCard, bool withAlt) {
    auto toneName = [&](int g, int n) {
        const size_t index = size_t(g * 64 + n);
        return index < toneNames_.size() ? toneNames_[index] : std::string();
    };
    bool changed = false;
    ImGui::PushID(id);
    // The part's own tone name: its tone may have been replaced (a ROM Play demo tone, an editor's SysEx).
    std::string name = currentName != nullptr ? std::string(currentName) : toneName(group, number & 63);
    name.erase(name.find_last_not_of(' ') + 1);
    const std::string current = toneCode(group, number) + " " + name;
    if (ImGui::Button(current.c_str(), ImVec2(ImGui::GetFontSize() * 9.0f, 0.0f))) ImGui::OpenPopup("tones");
    if (ImGui::BeginPopup("tones")) {
        // While translating with the MT-32's presets, a, b and r hold them, and d and e the D-110's a and b.
        const bool swapped = status_.mt32PresetBanks;
        const char* const groupTitles[kToneGroupCount] = {swapped ? "a (MT-32 A)" : "a (preset)", swapped ? "b (MT-32 B)" : "b (preset)",
                                                          "i (internal)", swapped ? "r (MT-32 R)" : "r (rhythm)", "c (card)",
                                                          swapped ? "d (D-110 a)" : "d (MT-32 A)", swapped ? "e (D-110 b)" : "e (MT-32 B)"};
        if (ImGui::BeginTabBar("groups")) {
            for (int g = 0; g < kToneGroupCount; g++) {
                if (g == kCardGroup && !(withCard && status_.cardInserted)) continue;
                if (g >= kAltGroup && !(withAlt && status_.altTones)) continue;
                const ImGuiTabItemFlags flags = ImGui::IsWindowAppearing() && g == group ? ImGuiTabItemFlags_SetSelected : 0;
                if (!ImGui::BeginTabItem(groupTitles[g], nullptr, flags)) continue;
                if (ImGui::BeginTable("tonegrid", 4, ImGuiTableFlags_SizingFixedFit)) {
                    for (int n = 0; n < 64; n++) {
                        ImGui::TableNextColumn();
                        const std::string label = toneCode(g, n) + " " + toneName(g, n);
                        if (ImGui::Selectable(label.c_str(), g == group && n == number, 0, ImVec2(ImGui::GetFontSize() * 8.0f, 0.0f))) {
                            group = uint8_t(g);
                            number = uint8_t(n);
                            changed = true;
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
    ImGui::PopID();
    return changed;
}

void App::drawPartsEditor() {
    if (!status_.open) {
        ImGui::TextDisabled("The synth is not running.");
        return;
    }
    const float fontSize = ImGui::GetFontSize();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Part");
    if (editPart_ >= int(status_.partCount)) editPart_ = 0;
    for (int i : partOrder()) {
        ImGui::SameLine();
        if (ImGui::RadioButton(partLabel(i).c_str(), editPart_ == i)) editPart_ = i;
    }
    ImGui::Separator();

    const PartStatus& part = status_.parts[editPart_];
    const float width = fontSize * 16.0f;
    auto setParameter = [&](uint8_t offset, int value) { engine_->setPartParameter(editPart_, offset, uint8_t(value)); };

    if (editPart_ != kRhythmPart) {
        // Timbre memory A11-B88: a timbre is a tone plus its key shift, tuning, bender, assign mode and output.
        const int currentTimbre = partTimbre(part);
        std::string timbrePreview = currentTimbre >= 0 ? timbreCode(currentTimbre) : std::string("(none)");
        ImGui::SetNextItemWidth(width);
        if (ImGui::BeginCombo("Timbre", timbrePreview.c_str(), ImGuiComboFlags_HeightLarge)) {
            for (int t = 0; t < 384 && timbreMemory_.size() >= 1024; t++) {
                if (!timbreSelectable(t)) continue;
                if (ImGui::Selectable(timbreLabel(t).c_str(), t == currentTimbre)) engine_->setPartProgram(editPart_, t);
            }
            ImGui::EndCombo();
        }
        UiStyle::setItemTooltip("Loads a timbre from memory (or the card), like a program change on the part's channel. "
                              "P-D11-P-E88 play tone banks d and e (the MT-32's presets) with a new timbre's settings.");

        // Group i/c plays the card's tones when the part's card flag (offset 07H) is set, a/b tone banks d/e with the alt flag.
        uint8_t group = uint8_t(toneGroupOf(part.temp, false));
        uint8_t number = part.temp[TimbreTemp::ToneNumber] & 63;
        if (drawTonePicker("tone", group, number, part.name, true, true)) {
            uint8_t timbre[8];
            std::copy(part.temp, part.temp + 8, timbre);
            setToneGroup(timbre, group);
            timbre[1] = number;
            engine_->writePartTemp(editPart_, TimbreTemp::ToneGroup, timbre, 8);
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("Tone");

        int level = part.temp[TimbreTemp::OutputLevel];
        ImGui::SetNextItemWidth(width);
        if (ImGui::SliderInt("Level", &level, 0, 100)) setParameter(TimbreTemp::OutputLevel, level);

        int pan = part.temp[TimbreTemp::Panpot];
        if (engineOptions_.nicePanning && status_.d110) {
            int fine = part.finePan;
            if (panSlider("Pan", fine, width)) engine_->setPartFinePan(editPart_, fine);
            UiStyle::setItemTooltip("%s", kFinePanTip);
        } else {
            const std::string panText = panLabel(status_.d110 ? pan : 14 - pan);
            ImGui::SetNextItemWidth(width);
            if (ImGui::SliderInt("Pan", &pan, 0, 14, panText.c_str())) setParameter(TimbreTemp::Panpot, pan);
        }

        drawOutputCombo(editPart_, "Output", width, false);

        int keyShift = int(part.temp[TimbreTemp::KeyShift]) - 24;
        ImGui::SetNextItemWidth(width);
        if (ImGui::SliderInt("Key shift", &keyShift, -24, 24, "%+d")) setParameter(TimbreTemp::KeyShift, keyShift + 24);
        int fineTune = int(part.temp[TimbreTemp::FineTune]) - 50;
        ImGui::SetNextItemWidth(width);
        if (ImGui::SliderInt("Fine tune", &fineTune, -50, 50, "%+d")) setParameter(TimbreTemp::FineTune, fineTune + 50);
        int bender = part.temp[TimbreTemp::BenderRange];
        ImGui::SetNextItemWidth(width);
        if (ImGui::SliderInt("Bender range", &bender, 0, 24)) setParameter(TimbreTemp::BenderRange, bender);
        if (status_.midiExtensions) {
            UiStyle::setItemTooltip("The timbre's bender range. With the MIDI extensions a part bends by its MIDI channel's range "
                                  "instead, as General MIDI modules do: RPN 0 sets it, program changes keep it, and a GM, GS or XG "
                                  "reset (or Reset MIDI) makes it 2 semitones. The timbre's range plays with the extensions off, "
                                  "and so in MT-32 translation and in ROM Play.");
            const uint32_t cents = part.bendRangeCents;
            char text[64];
            if (cents % 100 == 0) {
                std::snprintf(text, sizeof(text), "Bends \xC2\xB1%u semitone%s (the channel's, RPN 0)", cents / 100, cents == 100 ? "" : "s");
            } else {
                std::snprintf(text, sizeof(text), "Bends \xC2\xB1%u.%02u semitones (the channel's, RPN 0)", cents / 100, cents % 100);
            }
            ImGui::TextDisabled("%s", text);
        }
        int assign = std::min<int>(part.temp[TimbreTemp::AssignMode], 3);
        ImGui::SetNextItemWidth(width);
        if (ImGui::Combo("Assign mode", &assign, kAssignNames, IM_ARRAYSIZE(kAssignNames))) setParameter(TimbreTemp::AssignMode, assign);

        if (status_.d110) {
            int lower = part.temp[TimbreTemp::KeyRangeLower];
            int upper = part.temp[TimbreTemp::KeyRangeUpper];
            const std::string lowerText = noteName(lower);
            const std::string upperText = noteName(upper);
            ImGui::SetNextItemWidth(width);
            if (ImGui::SliderInt("Lowest key", &lower, 0, 127, lowerText.c_str())) setParameter(TimbreTemp::KeyRangeLower, lower);
            ImGui::SetNextItemWidth(width);
            if (ImGui::SliderInt("Highest key", &upper, 0, 127, upperText.c_str())) setParameter(TimbreTemp::KeyRangeUpper, upper);

            ImGui::Spacing();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Write timbre to");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(fontSize * 5.0f);
            if (writeTimbreTarget_ >= timbreCount()) writeTimbreTarget_ = 0;
            if (ImGui::BeginCombo("##timbretarget", timbreCode(writeTimbreTarget_).c_str(), ImGuiComboFlags_HeightLarge)) {
                for (int t = 0; t < timbreCount(); t++) {
                    if (ImGui::Selectable(timbreCode(t).c_str(), t == writeTimbreTarget_)) writeTimbreTarget_ = t;
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button("Write##timbre")) {
                const bool toCard = writeTimbreTarget_ >= 128;
                if (editPart_ < kRhythmPart) {
                    const uint8_t request[2] = {uint8_t(writeTimbreTarget_ & 127), uint8_t(toCard ? 1 : 0)};  // Number, internal/card
                    engine_->writeData(addressPlus(kTimbreWriteAddress, uint32_t(editPart_) * 2), request, 2);
                } else {
                    // Parts 9-15 have no write request: copy the part's 8 timbre bytes into timbre memory.
                    uint8_t timbre[8];
                    std::copy(part.temp, part.temp + 8, timbre);
                    timbre[7] = 0;
                    engine_->writeData(addressPlus(toCard ? kCardTimbreAddress : kTimbreMemoryAddress, uint32_t(writeTimbreTarget_ & 127) * 8), timbre, 8);
                }
                cacheTime_ = -1.0;
            }
            UiStyle::setItemTooltip("Stores the tone and the settings above (except level, pan and key range) in timbre memory.");
        }
    } else {
        int level = part.temp[TimbreTemp::OutputLevel];
        ImGui::SetNextItemWidth(width);
        if (ImGui::SliderInt("Level", &level, 0, 100)) setParameter(TimbreTemp::OutputLevel, level);
        if (options_.partOutputs) drawOutputCombo(kRhythmPart, "Output", width, false);
        ImGui::TextDisabled("Tones, levels, pans and outputs per key are on the Rhythm tab.");
    }

    ImGui::Separator();
    int reserve = part.reserve;
    int others = 0;
    for (int i = 0; i < int(status_.partCount); i++) {
        if (i != editPart_) others += status_.parts[i].reserve;
    }
    const bool sixteen = status_.partCount > uint32_t(kBasePartCount);
    ImGui::SetNextItemWidth(width);
    if (ImGui::SliderInt("Partial reserve", &reserve, 0, sixteen ? 32 : std::max(0, 32 - others))) {
        uint8_t reserves[kMaxPartCount] = {};
        for (int i = 0; i < int(status_.partCount); i++) reserves[i] = status_.parts[i].reserve;
        reserves[editPart_] = uint8_t(reserve);
        if (editPart_ < kBasePartCount) {
            engine_->writeData(kReserveAddress, reserves, kBasePartCount);  // Sent as a package of all 9 parts, as required
        } else {
            engine_->writeData(kExtraReserveAddress, reserves + kBasePartCount, kMaxPartCount - kBasePartCount);
        }
    }
    UiStyle::setItemTooltip(sixteen ? "Partials kept for this part. In 16-part mode the reserves share out all partials in "
                                    "proportion, whatever they add up to. When there are enough partials, every part "
                                    "keeps at least 4, even with a reserve of 0."
                                  : "Partials kept for this part; all parts together can reserve at most 32. With more "
                                    "partials (System > Partials) each part keeps the same share of the larger pool, "
                                    "and at least 4 partials even with a reserve of 0.");
    if (partialCount_ != kDefaultPartials || sixteen) {
        ImGui::SameLine();
        ImGui::TextDisabled("= %u of %d", part.reservedPartials, partialCount_);
    }
    ImGui::TextDisabled("MIDI channel %s, volume %d, expression %d", channelLabel(part.channel).c_str(), part.midiVolume, part.expression);
}

void App::drawPatchesTab() {
    const float fontSize = ImGui::GetFontSize();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Current patch %s", patchCode(status_.currentPatch).c_str());
    ImGui::SameLine();
    if (!patchNameActive_) {
        patchNameEdit_ = status_.patchName;  // Follow the synth unless the user is typing
        patchNameEdit_.erase(patchNameEdit_.find_last_not_of(' ') + 1);
    }
    ImGui::SetNextItemWidth(fontSize * 7.0f);
    const bool entered = ImGui::InputText("Name", &patchNameEdit_, ImGuiInputTextFlags_EnterReturnsTrue);
    patchNameActive_ = ImGui::IsItemActive();
    if (entered) {
        std::string padded10 = padded(patchNameEdit_, 10);
        std::replace_if(padded10.begin(), padded10.end(), [](char c) { return uint8_t(c) < 32 || uint8_t(c) > 126; }, ' ');
        engine_->writeData(kPatchNameAddress, reinterpret_cast<const uint8_t*>(padded10.data()), 10);
    }
    UiStyle::setItemTooltip("Press Enter to rename the current patch (then write it to keep the name).");
    ImGui::SameLine();
    ImGui::TextUnformatted("  Write to");
    ImGui::SameLine();
    // Patches 0-63 are internal (I-11..I-88), 64-127 on the memory card (C-11..C-88).
    const int patchCount = status_.cardInserted ? 128 : 64;
    auto patchName = [&](int patch) {
        const std::vector<uint8_t>& memory = patch < 64 ? patchMemory_ : cardPatchMemory_;
        const size_t entry = size_t(patch & 63) * 128;
        return memory.size() >= entry + 128 ? std::string(reinterpret_cast<const char*>(&memory[entry]), 10) : std::string();
    };
    auto patchLabel = [&](int patch) { return patchCode(patch) + " " + patchName(patch); };
    if (writePatchTarget_ >= patchCount) writePatchTarget_ = 0;
    ImGui::SetNextItemWidth(fontSize * 9.0f);
    if (ImGui::BeginCombo("##patchtarget", patchLabel(writePatchTarget_).c_str(), ImGuiComboFlags_HeightLarge)) {
        for (int patch = 0; patch < patchCount; patch++) {
            if (ImGui::Selectable(patchLabel(patch).c_str(), patch == writePatchTarget_)) writePatchTarget_ = patch;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Write##patch")) {
        const uint8_t request[2] = {uint8_t(writePatchTarget_ & 63), uint8_t(writePatchTarget_ >= 64 ? 1 : 0)};  // Number, internal/card
        engine_->writeData(kPatchWriteAddress, request, 2);
        cacheTime_ = -1.0;
    }
    UiStyle::setItemTooltip("Stores all parts, channels, reserves and reverb as this patch.");

    ImGui::TextDisabled("Click a patch to recall it (like a program change on the control channel).");
    if (patchMemory_.size() < size_t(64 * 128)) return;
    for (int bank = 0; bank < patchCount / 64; bank++) {
        const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchSame;
        if (!ImGui::BeginTable(bank == 0 ? "patches" : "cardpatches", 9, flags)) continue;
        ImGui::TableSetupColumn(bank == 0 ? "I" : "C", ImGuiTableColumnFlags_WidthFixed, fontSize * 1.5f);
        for (int number = 1; number <= 8; number++) ImGui::TableSetupColumn(std::to_string(number).c_str());
        ImGui::TableHeadersRow();
        for (int group = 0; group < 8; group++) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%d", group + 1);
            for (int number = 0; number < 8; number++) {
                ImGui::TableNextColumn();
                const int patch = bank * 64 + group * 8 + number;
                std::string label = patchName(patch);
                label.erase(label.find_last_not_of(std::string(" \0", 2)) + 1);
                ImGui::PushID(patch);
                if (ImGui::Selectable(label.empty() || label[0] == 0 ? "-" : label.c_str(), patch == status_.currentPatch)) {
                    engine_->recallPatch(patch);
                    writePatchTarget_ = patch;
                }
                UiStyle::setItemTooltip("%s %s", patchCode(patch).c_str(), label.c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
}

void App::drawPerformanceTab() {
    const float fontSize = ImGui::GetFontSize();
    bool enabled = performanceMode_;
    if (ImGui::Checkbox("Performance mode", &enabled)) setPerformanceMode(enabled);
    UiStyle::setItemTooltip("Plays one performance patch on one MIDI channel, like a D-20: part 1 takes the upper tone and part 2 "
                          "the lower one, whole, dual or split. Meanwhile the other parts are off (the rhythm part stays); "
                          "leaving the mode brings them back.");
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("  Channel");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fontSize * 3.5f);
    if (ImGui::BeginCombo("##perfchannel", std::to_string(performanceChannel_ + 1).c_str())) {
        for (int channel = 0; channel < 16; channel++) {
            if (ImGui::Selectable(std::to_string(channel + 1).c_str(), channel == performanceChannel_)) {
                performanceChannel_ = channel;
                if (performanceMode_) engine_->setPerformanceMode(true, performanceChannel_);
                saveSettings();
            }
        }
        ImGui::EndCombo();
    }
    UiStyle::setItemTooltip("MIDI channel of the performance; program changes on it select performance patches.");

    const uint8_t* temp = status_.performanceTemp;
    auto write = [&](uint8_t offset, int value) {
        const uint8_t byte = uint8_t(value);
        engine_->writeData(kPerformanceTempAddress + offset, &byte, 1);
    };
    ImGui::SeparatorText(("Performance " + performanceCode(status_.currentPerformance)).c_str());

    if (!performanceNameActive_) {
        performanceNameEdit_.assign(reinterpret_cast<const char*>(&temp[0x15]), 16);
        performanceNameEdit_.erase(performanceNameEdit_.find_last_not_of(' ') + 1);
    }
    ImGui::SetNextItemWidth(fontSize * 10.0f);
    const bool entered = ImGui::InputText("Name##perf", &performanceNameEdit_, ImGuiInputTextFlags_EnterReturnsTrue);
    performanceNameActive_ = ImGui::IsItemActive();
    if (entered) {
        std::string name16 = padded(performanceNameEdit_, 16);
        std::replace_if(name16.begin(), name16.end(), [](char c) { return uint8_t(c) < 32 || uint8_t(c) > 126; }, ' ');
        engine_->writeData(kPerformanceTempAddress + 0x15, reinterpret_cast<const uint8_t*>(name16.data()), 16);
    }
    UiStyle::setItemTooltip("Press Enter to rename the performance (then write it to keep the name).");
    ImGui::SameLine();
    ImGui::TextUnformatted("  Write to");
    ImGui::SameLine();
    auto patchLabel = [&](int patch) {
        std::string label = performanceCode(patch);
        if (performanceMemory_.size() >= size_t(128 * 38)) {
            std::string name(reinterpret_cast<const char*>(&performanceMemory_[size_t(patch) * 38 + 0x15]), 16);
            name.erase(name.find_last_not_of(std::string(" \0", 2)) + 1);
            label += " " + (name.empty() || name[0] == 0 ? std::string("(empty)") : name);
        }
        return label;
    };
    ImGui::SetNextItemWidth(fontSize * 11.0f);
    if (ImGui::BeginCombo("##perftarget", patchLabel(writePerformanceTarget_).c_str(), ImGuiComboFlags_HeightLarge)) {
        for (int patch = 0; patch < 128; patch++) {
            if (ImGui::Selectable(patchLabel(patch).c_str(), patch == writePerformanceTarget_)) writePerformanceTarget_ = patch;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Write##perf")) {
        engine_->writeData(addressPlus(kPerformanceMemoryAddress, uint32_t(writePerformanceTarget_) * 38), temp, 38);
        cacheTime_ = -1.0;
    }
    UiStyle::setItemTooltip("Stores the performance in patch memory.");

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Key mode");
    static const char* const kKeyModes[] = {"Whole", "Dual", "Split"};
    for (int mode = 0; mode < 3; mode++) {
        ImGui::SameLine();
        if (ImGui::RadioButton(kKeyModes[mode], temp[0x00] == mode)) write(0x00, mode);
    }
    UiStyle::setItemTooltip("Whole: the upper tone alone. Dual: both tones layered. Split: the lower tone below the split point.");
    if (temp[0x00] == 2) {
        ImGui::SameLine();
        int split = std::min<int>(temp[0x01], 61);
        ImGui::SetNextItemWidth(fontSize * 7.0f);
        if (ImGui::SliderInt("Split point", &split, 0, 61, noteName(36 + split).c_str())) write(0x01, split);
    }
    const float width = fontSize * 10.0f;
    int balance = temp[0x13];
    ImGui::SetNextItemWidth(width);
    if (ImGui::SliderInt("U/L balance", &balance, 0, 100)) write(0x13, balance);
    UiStyle::setItemTooltip("0: lower tone only, 50: both at the patch level, 100: upper tone only.");
    ImGui::SameLine();
    int level = temp[0x14];
    ImGui::SetNextItemWidth(width * 0.6f);
    if (ImGui::SliderInt("Level##perf", &level, 0, 100)) write(0x14, level);
    int reverbMode = std::min<int>(temp[0x10], 8);
    ImGui::SetNextItemWidth(width);
    if (ImGui::Combo("Reverb##perf", &reverbMode, kD110ReverbNames, IM_ARRAYSIZE(kD110ReverbNames))) write(0x10, reverbMode);
    ImGui::SameLine();
    int reverbTime = temp[0x11] + 1;
    ImGui::SetNextItemWidth(width * 0.4f);
    if (ImGui::SliderInt("Time##perf", &reverbTime, 1, 8)) write(0x11, reverbTime - 1);
    ImGui::SameLine();
    int reverbLevel = temp[0x12];
    ImGui::SetNextItemWidth(width * 0.4f);
    if (ImGui::SliderInt("Level##perfreverb", &reverbLevel, 0, 7)) write(0x12, reverbLevel);

    // Upper tone (part 1) and lower tone (part 2): offsets of each parameter.
    struct ToneOffsets {
        uint8_t group, keyShift, fineTune, bender, assign, reverb;
    };
    const ToneOffsets tones[2] = {{0x04, 0x07, 0x09, 0x0B, 0x0D, 0x0F}, {0x02, 0x06, 0x08, 0x0A, 0x0C, 0x0E}};
    if (ImGui::BeginTable("perftones", 3, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, fontSize * 5.0f);
        ImGui::TableSetupColumn("Upper tone (part 1)");
        ImGui::TableSetupColumn("Lower tone (part 2)");
        ImGui::TableHeadersRow();
        auto row = [&](const char* label, auto&& cell) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            for (int t = 0; t < 2; t++) {
                ImGui::TableNextColumn();
                ImGui::PushID(t);
                ImGui::BeginDisabled(t == 1 && temp[0x00] == 0);  // Whole mode: the lower tone does not sound
                ImGui::SetNextItemWidth(-FLT_MIN);
                cell(tones[t]);
                ImGui::EndDisabled();
                ImGui::PopID();
            }
        };
        row("Tone", [&](const ToneOffsets& o) {
            uint8_t group = temp[o.group] & 3;
            uint8_t number = temp[o.group + 1] & 63;
            if (drawTonePicker("tone", group, number)) {
                const uint8_t tone[2] = {group, number};
                engine_->writeData(kPerformanceTempAddress + o.group, tone, 2);
            }
        });
        row("Key shift", [&](const ToneOffsets& o) {
            int shift = int(temp[o.keyShift]) - 24;
            if (ImGui::SliderInt("##shift", &shift, -24, 24, "%+d")) write(o.keyShift, shift + 24);
        });
        row("Fine tune", [&](const ToneOffsets& o) {
            int fine = int(temp[o.fineTune]) - 50;
            if (ImGui::SliderInt("##fine", &fine, -50, 50, "%+d")) write(o.fineTune, fine + 50);
        });
        row("Bender", [&](const ToneOffsets& o) {
            int bender = temp[o.bender];
            if (ImGui::SliderInt("##bender", &bender, 0, 24)) write(o.bender, bender);
        });
        row("Assign", [&](const ToneOffsets& o) {
            int assign = std::min<int>(temp[o.assign], 3);
            if (ImGui::Combo("##assign", &assign, kAssignNames, IM_ARRAYSIZE(kAssignNames))) write(o.assign, assign);
        });
        row("Reverb", [&](const ToneOffsets& o) {
            bool reverb = temp[o.reverb] != 0;
            if (ImGui::Checkbox("##reverb", &reverb)) write(o.reverb, reverb ? 1 : 0);
        });
        ImGui::EndTable();
    }

    ImGui::TextDisabled("Click a performance to play it (like a program change on the performance channel).");
    if (performanceMemory_.size() < size_t(128 * 38)) return;
    for (int bank = 0; bank < 2; bank++) {
        const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchSame;
        if (!ImGui::BeginTable(bank == 0 ? "perfA" : "perfB", 9, flags)) continue;
        ImGui::TableSetupColumn(bank == 0 ? "A" : "B", ImGuiTableColumnFlags_WidthFixed, fontSize * 1.5f);
        for (int number = 1; number <= 8; number++) ImGui::TableSetupColumn(std::to_string(number).c_str());
        ImGui::TableHeadersRow();
        for (int group = 0; group < 8; group++) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%d", group + 1);
            for (int number = 0; number < 8; number++) {
                ImGui::TableNextColumn();
                const int patch = bank * 64 + group * 8 + number;
                std::string name(reinterpret_cast<const char*>(&performanceMemory_[size_t(patch) * 38 + 0x15]), 16);
                name.erase(name.find_last_not_of(std::string(" \0", 2)) + 1);
                ImGui::PushID(patch);
                if (ImGui::Selectable(name.empty() || name[0] == 0 ? "-" : name.c_str(), patch == status_.currentPerformance)) {
                    engine_->recallPerformance(patch);
                    writePerformanceTarget_ = patch;
                }
                UiStyle::setItemTooltip("%s %s", performanceCode(patch).c_str(), name.c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
}

void App::drawRhythmTab() {
    if (!status_.open || rhythmSetup_.size() < 85 * 4) {
        ImGui::TextDisabled("The synth is not running.");
        return;
    }
    const float fontSize = ImGui::GetFontSize();
    auto rhythmToneName = [&](int value) {
        // 0-63 internal tones i01-i64, 64-127 rhythm tones r01-r64
        const int group = value < 64 ? 2 : 3;
        const int number = value & 63;
        const size_t index = size_t(group * 64 + number);
        char code[8];
        std::snprintf(code, sizeof(code), "%c%02d", group == 2 ? 'i' : 'r', number + 1);
        return std::string(code) + " " + (index < toneNames_.size() ? toneNames_[index] : "");
    };
    std::array<bool, 128> sounding = {};
    const PartStatus& rhythm = status_.parts[kRhythmPart];
    for (uint32_t k = 0; k < rhythm.noteCount && k < uint32_t(kMaxPartials); k++) sounding[rhythm.keys[k] & 127] = true;

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("rhythm", 5, flags, ImVec2(0.0f, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, fontSize * 3.5f);
    ImGui::TableSetupColumn("Tone", ImGuiTableColumnFlags_WidthFixed, fontSize * 10.5f);
    ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, fontSize * 6.5f);
    ImGui::TableSetupColumn("Pan", ImGuiTableColumnFlags_WidthFixed, fontSize * 6.5f);
    ImGui::TableSetupColumn("Output", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (int key = 0; key < 85; key++) {
        const uint8_t* entry = &rhythmSetup_[size_t(key) * 4];
        auto write = [&](uint32_t field, int value) {
            const uint8_t byte = uint8_t(value);
            engine_->writeData(addressPlus(kRhythmSetupAddress, uint32_t(key) * 4 + field), &byte, 1);
            rhythmSetup_[size_t(key) * 4 + field] = byte;  // Show the change before the next refresh
        };
        ImGui::TableNextRow();
        ImGui::PushID(key);
        ImGui::TableNextColumn();
        led(sounding[size_t(key + 24)]);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(noteName(key + 24).c_str());

        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##tone", rhythmToneName(entry[0] & 127).c_str(), ImGuiComboFlags_HeightLarge)) {
            for (int value = 0; value < 128; value++) {
                if (ImGui::Selectable(rhythmToneName(value).c_str(), value == entry[0])) write(0, value);
            }
            ImGui::EndCombo();
        }
        ImGui::TableNextColumn();
        int level = entry[1];
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderInt("##level", &level, 0, 100)) write(1, level);
        ImGui::TableNextColumn();
        int pan = std::min<int>(entry[2], 14);
        if (engineOptions_.nicePanning && status_.d110 && rhythmFinePan_.size() >= 85 * 2) {
            uint8_t* fineEntry = &rhythmFinePan_[size_t(key) * 2];
            const int value = fineEntry[0] * 128 + fineEntry[1];
            int fine = value >= 1 && value <= 129 ? value - 65 : panpotToFinePan(pan);
            if (panSlider("##pan", fine, -FLT_MIN)) {
                engine_->setRhythmFinePan(key + 24, fine);
                // Show the change before the next refresh, the panpot at its nearest step as the synth sets it
                fineEntry[0] = uint8_t((fine + 65) >> 7);
                fineEntry[1] = uint8_t((fine + 65) & 0x7F);
                rhythmSetup_[size_t(key) * 4 + 2] = uint8_t(7 + (fine >= 0 ? (fine * 7 + 32) / 64 : -((-fine * 7 + 32) / 64)));
            }
            UiStyle::setItemTooltip("%s", kFinePanTip);
        } else {
            const std::string panText = panLabel(status_.d110 ? pan : 14 - pan);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::SliderInt("##pan", &pan, 0, 14, panText.c_str())) {
                write(2, pan);
                if (rhythmFinePan_.size() >= 85 * 2) std::fill_n(&rhythmFinePan_[size_t(key) * 2], 2, uint8_t(0));  // A panpot drops the fine pan
            }
        }
        ImGui::TableNextColumn();
        int output = entry[3];
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (status_.d110) {
            output = std::min(output, 7);
            const bool unheard = !((ownOutputs_ >> kRhythmPart) & 1) && hostOutputOff(output);
            if (unheard) ImGui::PushStyleColor(ImGuiCol_Text, kWarningColor);
            if (outputAssignCombo("##output", output, false)) write(3, output);
            if (unheard) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
                std::string tip = "Mix (dry), Mix + reverb, or " + multiChoicesText() + ": " + multiOutputsText() + ".";
                if (options_.partOutputs) {
                    tip += " When the rhythm part plays out of its own output (its Output in the Play or Parts tab), every key "
                           "plays there, and Mix + reverb still feeds the reverb.";
                }
                if (unheard) tip += "\n\nThe DAW has this output switched off, so the key is not heard: switch the plugin's outputs on in the DAW.";
                UiStyle::setTooltip("%s", tip.c_str());
            }
        } else {
            output = std::min(output, 1);
            if (ImGui::Combo("##output", &output, kMT32OutputNames, IM_ARRAYSIZE(kMT32OutputNames))) write(3, output);
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void App::exportD20(int what) {
    const bool track = what >= kD20PatternCount;
    const std::string name = track ? std::string("D-20 rhythm track.mid") : d20PatternName(what) + ".mid";
    saveFileThen(Platform::FileKind::Midi, name, [this, what, track](const std::filesystem::path& path) {
        const uint8_t channel = status_.parts[kRhythmPart].channel < 16 ? status_.parts[kRhythmPart].channel : 9;
        std::unique_ptr<SmfFile> smf = track ? d20TrackToSmf(d20Rhythm_, channel, d20Tempo_) : d20PatternToSmf(d20Rhythm_, what, channel, d20Tempo_);
        std::string error;
        // 96 ticks per quarter note: 4 per step of the D-20's grid.
        if (saveSmfFile(path, *smf, 96, uint32_t(std::lround(60000000.0 / d20Tempo_)), error)) {
            addLog("Exported " + Platform::toUtf8(path.filename()));
        } else {
            addLog("Export: " + error);
        }
    });
}

void App::loadD20Presets() {
    openFileThen(Platform::FileKind::Sysex, [this](const std::filesystem::path& path) {
        std::string error;
        if (!engine_->loadD20PresetPatterns(path, error)) addLog("Patterns: " + error);
        cacheTime_ = -1.0;
    });
}

void App::drawPatternsTab() {
    patternsTabShown_ = true;
    const float fontSize = ImGui::GetFontSize();
    const ImGuiStyle& style = ImGui::GetStyle();
    const uint8_t rhythmChannel = status_.parts[kRhythmPart].channel;
    const double position = status_.player.position;  // Seconds at kD20BaseTempo
    // What plays. The transport and the pattern grid start and stop it within the frame (stopD20() empties the bar
    // starts), so it is read again after them.
    bool active = false;
    bool trackPlaying = false;
    bool patternPlaying = false;  // A pattern repeats: a click on another one queues it
    int trackBar = -1;
    int playingPattern = -1;
    auto readPlaying = [&] {
        active = d20Playing_ >= 0 && (status_.player.state == MidiPlayer::State::Playing || status_.player.state == MidiPlayer::State::Paused);
        trackPlaying = active && d20Playing_ == kD20PatternCount && !d20BarStarts_.empty();
        trackBar = -1;
        if (trackPlaying) {
            trackBar = int(std::upper_bound(d20BarStarts_.begin(), d20BarStarts_.end(), position) - d20BarStarts_.begin()) - 1;
            trackBar = std::clamp(trackBar, 0, int(d20BarStarts_.size()) - 1);
        }
        const int trackEntry = trackBar >= 0 && trackBar < int(d20Rhythm_.track.size()) ? d20Rhythm_.track[size_t(trackBar)] : -1;
        playingPattern = trackPlaying ? (trackEntry >= 0 && trackEntry < kD20PatternCount ? trackEntry : -1)
                                      : (active && d20Playing_ < kD20PatternCount ? d20Playing_ : -1);
        patternPlaying = active && d20Playing_ < kD20PatternCount;
    };
    readPlaying();

    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("The D-20's rhythm machine: its patterns and rhythm track, from D-20 dumps (File > Load SysEx file). "
                        "They play on the rhythm part with the current rhythm setup.");
    ImGui::PopTextWrapPos();

    // Transport
    ImGui::BeginDisabled(!status_.open || rhythmChannel >= 16);
    if (ImGui::Button("Play pattern")) playD20(d20Selected_);
    UiStyle::setItemTooltip("Plays the selected pattern over and over (or double-click a pattern). While a pattern plays, a click "
                          "on another one plays that from the next bar.");
    ImGui::SameLine();
    ImGui::BeginDisabled(d20Rhythm_.track.empty());
    if (ImGui::Button("Play track")) playD20(kD20PatternCount);
    ImGui::EndDisabled();
    UiStyle::setItemTooltip("Plays the rhythm track: its bars one after another, as the D-20 does");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!active);
    if (ImGui::Button("Stop")) stopD20();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fontSize * 9.0f);
    if (ImGui::SliderInt("Tempo", &d20Tempo_, 20, 250, "%d BPM")) {
        if (active) engine_->playerSetSpeed(d20Tempo_ / kD20BaseTempo);
        saveSettings();
    }
    UiStyle::setItemTooltip("D-20 dumps hold no tempo. A change applies at once.");
    if (d20Queued_ >= 0) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kQueuedColor, "Next: %s", d20PatternName(d20Queued_).c_str());
        UiStyle::setItemTooltip("Plays from the next bar's first beat. Click the playing pattern to stay on it.");
    }
    if (rhythmChannel >= 16) ImGui::TextColored(kErrorColor, "The rhythm part has no MIDI channel (Play tab).");
    readPlaying();

    // Pattern grid: banks 1-4 are the presets, 5-8 the programmable patterns.
    auto rhythmKeyLabel = [&](int key) {
        std::string label = noteName(key);
        const size_t entry = size_t(key - 24) * 4;
        if (entry < rhythmSetup_.size()) {
            const int value = rhythmSetup_[entry] & 127;
            const size_t index = size_t((value < 64 ? 2 : 3) * 64 + (value & 63));
            if (index < toneNames_.size()) label += " " + toneNames_[index];
        }
        return label;
    };
    // "P-11 8Beat 1": the presets by the names the D-20 shows.
    auto patternTitle = [](int p) { return d20PatternName(p) + (p < kD20PresetPatterns ? std::string(" ") + kD20PresetPatternNames[p] : std::string()); };
    const ImVec2 cell(fontSize * 3.3f, ImGui::GetFrameHeight());
    for (int bank = 0; bank < 8; bank++) {
        if (bank == 0) ImGui::SeparatorText("Preset patterns (the D-20's ROM)");
        if (bank == 4) ImGui::SeparatorText("Programmable patterns");
        for (int number = 0; number < 8; number++) {
            const int p = bank * 8 + number;
            const D20Pattern& pattern = d20Rhythm_.patterns[size_t(p)];
            if (number > 0) ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
            int colors = 0;
            if (p == playingPattern) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.52f, 0.28f, 1.0f));
                colors++;
            } else if (p == d20Queued_) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.43f, 0.12f, 1.0f));
                colors++;
            } else if (p == d20Selected_) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                colors++;
            } else if (!pattern.present) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
                colors++;
            }
            if (!pattern.present) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                colors++;
            }
            ImGui::PushID(p);
            if (ImGui::Button(d20PatternName(p).c_str(), cell)) {
                d20Selected_ = p;
                if (patternPlaying) queueD20(p);  // From the next bar, as on the D-20
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && rhythmChannel < 16 && !patternPlaying) playD20(p);
            if (pattern.present) {
                UiStyle::setItemTooltip("%s: %d/4, %zu notes (%s)", patternTitle(p).c_str(), pattern.beats, pattern.notes.size(),
                                      patternPlaying ? "click to play it from the next bar" : "double-click to play");
            } else if (p < kD20PresetPatterns) {
                UiStyle::setItemTooltip("%s is a preset pattern: presets are in the D-20's ROM, so dumps never hold them. Put \"%s\" "
                                      "(recorded from a D-20 with PatternCapture) in the ROM folder, or see Load presets below.",
                                      patternTitle(p).c_str(), kD20PresetFile);
            } else {
                UiStyle::setItemTooltip("%s is empty", d20PatternName(p).c_str());
            }
            ImGui::PopID();
            ImGui::PopStyleColor(colors);
        }
    }

    readPlaying();

    // The selected pattern (or the one playing) on its step grid: 24 steps per quarter note.
    const int shown = playingPattern >= 0 ? playingPattern : d20Selected_;
    const D20Pattern& pattern = d20Rhythm_.patterns[size_t(shown)];
    char title[64];
    if (pattern.present) {
        std::snprintf(title, sizeof(title), "%s  %d/4, %zu notes", patternTitle(shown).c_str(), pattern.beats, pattern.notes.size());
    } else {
        std::snprintf(title, sizeof(title), "%s  (not loaded)", patternTitle(shown).c_str());
    }
    ImGui::SeparatorText(title);
    std::vector<int> keys;
    for (const D20Pattern::Note& note : pattern.notes) keys.push_back(note.key);
    std::sort(keys.begin(), keys.end(), std::greater<int>());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    if (keys.empty()) {
        ImGui::TextDisabled("No notes");
    } else {
        const float row = std::floor(fontSize * 1.15f);
        const float labelWidth = fontSize * 9.5f;
        const float width = std::max(ImGui::GetContentRegionAvail().x - labelWidth, fontSize * 8.0f);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float height = row * float(keys.size());
        ImGui::Dummy(ImVec2(labelWidth + width, height));
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const int steps = pattern.beats * kD20StepsPerQuarter;
        const float stepWidth = width / float(steps);
        const float gridX = origin.x + labelWidth;
        draw->AddRectFilled(ImVec2(gridX, origin.y), ImVec2(gridX + width, origin.y + height), ImGui::GetColorU32(ImGuiCol_FrameBg));
        for (int step = 0; step <= steps; step += kD20StepsPerQuarter / 4) {
            const float x = gridX + float(step) * stepWidth;
            const bool beat = step % kD20StepsPerQuarter == 0;
            draw->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + height), ImGui::GetColorU32(ImGuiCol_Border, beat ? 1.0f : 0.35f));
        }
        for (size_t k = 0; k < keys.size(); k++) {
            const std::string label = rhythmKeyLabel(keys[k]);
            draw->AddText(ImVec2(origin.x, origin.y + float(k) * row + (row - fontSize) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
        }
        for (const D20Pattern::Note& note : pattern.notes) {
            const size_t k = size_t(std::find(keys.begin(), keys.end(), note.key) - keys.begin());
            const float x = gridX + float(note.step) * stepWidth;
            const float y = origin.y + float(k) * row;
            const float alpha = 0.35f + 0.65f * float(note.velocity) / 127.0f;
            draw->AddRectFilled(ImVec2(x + 1.0f, y + 2.0f), ImVec2(x + std::max(stepWidth * 3.0f, 3.0f), y + row - 2.0f),
                                ImGui::GetColorU32(ImVec4(0.35f, 0.65f, 1.0f, alpha)), 2.0f);
        }
        if (shown == playingPattern) {
            const double barSeconds = pattern.beats * 60.0 / kD20BaseTempo;
            const bool inTrack = trackPlaying && trackBar >= 0 && size_t(trackBar) < d20BarStarts_.size();
            const double inBar = inTrack ? position - d20BarStarts_[size_t(trackBar)] : std::fmod(position, barSeconds);
            const float x = gridX + float(inBar / barSeconds) * width;
            draw->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + height), ImGui::GetColorU32(kOkColor), 2.0f);
        }
    }

    // The rhythm track, bar by bar.
    ImGui::SeparatorText("Rhythm track");
    if (ImGui::SmallButton("Factory track...")) ImGui::OpenPopup("factorytrack");
    UiStyle::setItemTooltip("The track a D-20 comes with: its preset patterns P-11-P-48 in order, each played twice.");
    if (ImGui::BeginPopup("factorytrack")) {
        ImGui::TextUnformatted("Replace the rhythm track with the D-20's factory one\n(the preset patterns P-11-P-48 in order, each twice)?");
        if (ImGui::Button("Replace")) {
            if (trackPlaying) stopD20();
            engine_->resetD20RhythmTrack();
            cacheTime_ = -1.0;  // Read the track again
            addLog("Patterns: the rhythm track is the D-20's factory one again");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    readPlaying();
    ImGui::SameLine();
    if (d20Rhythm_.track.empty()) {
        ImGui::TextDisabled("Empty: a D-20 dump brings its rhythm track along.");
    } else {
        int beats = 0;
        for (int entry : d20Rhythm_.track) beats += d20BarBeats(d20Rhythm_, entry);
        const int seconds = int(std::lround(beats * 60.0 / d20Tempo_));
        ImGui::Text("%zu bars, %d:%02d at %d BPM", d20Rhythm_.track.size(), seconds / 60, seconds % 60, d20Tempo_);
        if (trackPlaying) {
            const int entry = trackBar < int(d20Rhythm_.track.size()) ? d20Rhythm_.track[size_t(trackBar)] : kD20TrackBlank + 3;
            ImGui::SameLine();
            ImGui::TextColored(kOkColor, "  bar %d: %s", trackBar + 1, entry < kD20PatternCount ? patternTitle(entry).c_str() : "a blank bar");
        }
        const float cellWidth = fontSize * 2.4f;
        const int perRow = std::max(1, int((ImGui::GetContentRegionAvail().x + style.ItemInnerSpacing.x) / (cellWidth + style.ItemInnerSpacing.x)));
        const int rows = (int(d20Rhythm_.track.size()) + perRow - 1) / perRow;
        const float rowHeight = ImGui::GetFrameHeight() + style.ItemSpacing.y;
        ImGui::BeginChild("track", ImVec2(0.0f, std::min(rows, 8) * rowHeight + style.WindowPadding.y));
        for (int bar = 0; bar < int(d20Rhythm_.track.size()); bar++) {
            const int entry = d20Rhythm_.track[size_t(bar)];
            if (bar % perRow != 0) ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
            const bool missing = entry < kD20PatternCount && !d20Rhythm_.patterns[size_t(entry)].present;
            int colors = 0;
            if (bar == trackBar) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.52f, 0.28f, 1.0f));
                colors++;
            } else if (entry >= kD20PatternCount || missing) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
                colors++;
            }
            if (missing) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                colors++;
            }
            char label[32];
            if (entry < kD20PatternCount) {
                std::snprintf(label, sizeof(label), "%d%d##%d", entry / 8 + 1, entry % 8 + 1, bar);
            } else {
                std::snprintf(label, sizeof(label), "%d/4##%d", ((entry - kD20TrackBlank) & 7) + 1, bar);  // A blank bar
            }
            if (ImGui::Button(label, ImVec2(cellWidth, 0.0f)) && entry < kD20PatternCount) d20Selected_ = entry;
            if (bar == trackBar && d20ScrolledBar_ != bar) {
                ImGui::SetScrollHereY(0.5f);
                d20ScrolledBar_ = bar;
            }
            const std::string name = entry < kD20PatternCount ? patternTitle(entry) : d20TrackEntryName(entry);
            if (missing) {
                UiStyle::setItemTooltip("Bar %d: %s, a preset pattern that is not loaded (a 4/4 rest)", bar + 1, name.c_str());
            } else if (entry >= kD20PatternCount) {
                UiStyle::setItemTooltip("Bar %d: a blank bar of %d/4 (the D-20 counts it, and plays nothing)", bar + 1, d20BarBeats(d20Rhythm_, entry));
            } else {
                UiStyle::setItemTooltip("Bar %d: %s (%d/4)", bar + 1, name.c_str(), d20BarBeats(d20Rhythm_, entry));
            }
            ImGui::PopStyleColor(colors);
        }
        ImGui::EndChild();
    }

    ImGui::SeparatorText("Files");
    if (ImGui::Button("Load presets P-11-P-48...")) loadD20Presets();
    UiStyle::setItemTooltip("The D-20's preset patterns are in its ROM, so no dump holds them. d110emu loads them at start from "
                          "\"D-20 preset patterns.syx\" in the ROM folder (PatternCapture records it from a D-20). Another "
                          "dump whose P-51-P-88 hold them can be loaded here, until the next start: its P-51-P-88 become P-11-P-48.");
    ImGui::SameLine();
    if (ImGui::Button("Export pattern .mid")) exportD20(d20Selected_);
    UiStyle::setItemTooltip("The selected pattern as a Standard MIDI File on the rhythm part's channel, at the tempo above");
    ImGui::SameLine();
    ImGui::BeginDisabled(d20Rhythm_.track.empty());
    if (ImGui::Button("Export track .mid")) exportD20(kD20PatternCount);
    ImGui::EndDisabled();
    UiStyle::setItemTooltip("The rhythm track as a Standard MIDI File on the rhythm part's channel, at the tempo above");
}

void App::drawSystemPanel() {
    ImGui::SeparatorText("System");
    const float width = settingWidth(7.0f);
    ImGui::BeginDisabled(!status_.d110);
    auto channelName = [](int channel) { return channel < 16 ? std::to_string(channel + 1) : std::string("Off"); };
    ImGui::SetNextItemWidth(width);
    if (ImGui::BeginCombo("Control channel", channelName(engineOptions_.controlChannel).c_str())) {
        for (int channel = 0; channel <= 16; channel++) {
            if (ImGui::Selectable(channelName(channel).c_str(), channel == engineOptions_.controlChannel)) {
                engineOptions_.controlChannel = uint8_t(channel);
                setOptions();
            }
        }
        ImGui::EndCombo();
    }
    helpMarker("Program changes on this MIDI channel select patches (I-11 to I-88) instead of timbres.");
    int unit = engineOptions_.unitNumber;
    ImGui::SetNextItemWidth(width);
    if (ImGui::InputInt("Unit number", &unit)) {
        engineOptions_.unitNumber = uint8_t(std::clamp(unit, 17, 32));
        setOptions();
    }
    helpMarker("SysEx device number (17 = device ID 10H). Messages for other unit numbers are ignored.");
    ImGui::EndDisabled();

    ImGui::SetNextItemWidth(width);
    const std::string preview = std::to_string(partialCount_) + (partialCount_ == kDefaultPartials ? " (original)" : "");
    if (ImGui::BeginCombo("Partials", preview.c_str())) {
        for (int count : kPartialCounts) {
            const std::string label = std::to_string(count) + (count == kDefaultPartials ? " (original)" : "");
            if (ImGui::Selectable(label.c_str(), count == partialCount_) && count != partialCount_) {
                partialCount_ = count;
                restartSynth();
                saveSettings();
            }
        }
        ImGui::EndCombo();
    }
    helpMarker("Polyphony: the D-110 has 32 partials, and a tone uses 1 to 4 of them per note. More partials allow "
               "more simultaneous notes than the real unit. Changing it restarts the synth (memory is kept).");

    ImGui::BeginDisabled(!status_.d110 && !sixteenParts_);
    if (ImGui::Checkbox("16 parts", &sixteenParts_)) {
        restartSynth();
        saveSettings();
    }
    ImGui::EndDisabled();
    helpMarker("15 melodic parts and rhythm, like a 16-channel sound module: parts 9-15 start on MIDI channels 9 and "
               "11-16, so every channel has a part and channel 10 stays with the rhythm part. Use more partials (512) "
               "to go with them. Patches store parts 1-8 only. Changing it restarts the synth (memory is kept).");

    ImGui::Spacing();
    if (ImGui::Button("Restart synth")) resetSynth();
    UiStyle::setItemTooltip("Power-cycles the emulated synth: sounding notes stop and the MIDI file rewinds. With the D-110 "
                          "ROMs its memory and parts are kept, with every part's level at 100 and its pan centred, as "
                          "Reset MIDI leaves them; with MT-32 ROMs all SysEx changes are lost.");
}

void App::drawAboutPopup() {
    if (openInitializeConfirm_) {
        ImGui::OpenPopup("Initialize memory?");
        openInitializeConfirm_ = false;
    }
    if (ImGui::BeginPopupModal("Initialize memory?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("All tones, timbres, patches and settings in the synth's memory will be reset.");
        ImGui::TextDisabled("Save them first with File > Save memory as SysEx if you want to keep them.");
        if (ImGui::Button("Initialize")) {
            initializeMemory();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (openAbout_) {
        ImGui::OpenPopup("About D110Emu");
        openAbout_ = false;
    }
    if (!ImGui::BeginPopupModal("About D110Emu", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    if (options_.plugin) {
        ImGui::Text("D110Emu - Roland D-110 emulator (%s plugin)", options_.pluginFormat.c_str());
    } else {
        ImGui::TextUnformatted("D110Emu - Roland D-110 emulator");
    }
    ImGui::Separator();
    ImGui::Text("Emulation: Munt mt32emu %s (LGPL 2.1+) with D-110 ROM support", MT32Emu::Synth::getLibraryVersionString());
    ImGui::Text("Interface: Dear ImGui %s (MIT)", ImGui::GetVersion());
    if (!options_.aboutExtra.empty()) ImGui::TextUnformatted(options_.aboutExtra.c_str());
    // Where the settings and the memory are (the plugins: their computer-wide settings), which a Mac and Linux hide.
    const std::filesystem::path files = (options_.settingsFile.empty() ? options_.reverbSettingsFile : options_.settingsFile).parent_path();
    if (!files.empty()) ImGui::Text("Files: %s", Platform::toUtf8(files).c_str());
    if (options_.plugin && options_.pluginFormat == "VST3") {
        ImGui::TextUnformatted("Plugin interface: VST 3, Steinberg's VST 3 SDK interfaces (MIT)");
        ImGui::TextUnformatted("VST is a trademark of Steinberg Media Technologies GmbH");
    } else if (options_.plugin && options_.pluginFormat == "AU") {
        ImGui::TextUnformatted("Plugin interface: Audio Unit (v2), Apple's AudioUnitSDK (Apache 2.0)");
    } else if (options_.plugin) {
        ImGui::TextUnformatted("Plugin interface: VST 2.4 (VST is a trademark of Steinberg Media Technologies GmbH)");
    } else {
        ImGui::TextUnformatted("Audio: miniaudio (public domain / MIT-0)");
    }
    ImGui::Spacing();
    if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// ---------------------------------------------------------------------------------------------
// Tone editor (Tone tab) and timbre memory (Timbres tab)

void App::ToneHost::writeTone(int offset, const uint8_t* data, int length) {
    if (app.tonePart_ < 0 || length <= 0) return;
    app.engine_->writeData(RolandSysex::unpack(Tone::toneTempAddress(app.tonePart_) + uint32_t(offset)), data, size_t(length));
    app.toneWriteTime_ = ImGui::GetTime();
}

void App::ToneHost::noteOn(int key, int velocity) {
    if (channel < 16) app.sendShort(uint8_t(0x90 | channel), uint8_t(key & 127), uint8_t(std::clamp(velocity, 1, 127)));
}

void App::ToneHost::noteOff(int key) {
    if (channel < 16) app.sendShort(uint8_t(0x80 | channel), uint8_t(key & 127), 0);
}

void App::ToneHost::restrike() {
    // Notes held on the computer keyboard play again, so a change is heard at once.
    for (int note = 0; note < 128; note++) {
        if (!app.keyHeld_[note]) continue;
        const uint8_t noteChannel = app.keyChannel_[size_t(note)];
        app.sendShort(uint8_t(0x80 | noteChannel), uint8_t(note), 0);
        app.sendShort(uint8_t(0x90 | noteChannel), uint8_t(note), uint8_t(app.velocity_));
    }
}

void App::ToneHost::soundingNotes(std::array<bool, 128>& notes) {
    if (app.tonePart_ < 0 || app.tonePart_ >= int(app.status_.partCount)) return;
    const PartStatus& part = app.status_.parts[app.tonePart_];
    for (uint32_t k = 0; k < part.noteCount && k < uint32_t(kMaxPartials); k++) notes[part.keys[k] & 127] = true;
}

void App::syncToneEditor() {
    Tone::Data synthTone;
    engine_->readMemory(RolandSysex::unpack(Tone::toneTempAddress(editPart_)), Tone::kSize, synthTone.data());
    toneEditor_.setModel(status_.d110 ? Tone::Model::D110 : Tone::Model::MT32);
    const uint8_t channel = status_.parts[editPart_].channel;
    if (tonePart_ != editPart_) {
        toneEditor_.releaseNotes(toneHost_);  // On the old part's channel
        tonePart_ = editPart_;
        toneEditor_.loadTone(synthTone);
        toneEditor_.clearHistory();
        toneWriteTime_ = -1.0;
    } else if (synthTone != toneEditor_.sentTone() && ImGui::GetTime() - toneWriteTime_ > 0.4) {
        // Another tone was selected, or SysEx from outside changed it (the editor's own writes may take a moment).
        toneEditor_.loadTone(synthTone);
    }
    if (channel != toneHost_.channel) {
        toneEditor_.releaseNotes(toneHost_);
        toneHost_.channel = channel;
    }
}

void App::drawToneTab() {
    toneTabShown_ = true;
    if (!status_.open) {
        ImGui::TextDisabled("The synth is not running.");
        return;
    }
    const float fontSize = ImGui::GetFontSize();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Part");
    if (editPart_ >= int(status_.partCount)) editPart_ = 0;
    for (int i : partOrder()) {
        if (i == kRhythmPart) continue;
        ImGui::SameLine();
        if (ImGui::RadioButton(partLabel(i).c_str(), editPart_ == i)) editPart_ = i;
    }
    if (editPart_ == kRhythmPart) {
        ImGui::TextDisabled("The rhythm part plays rhythm tones by key. Choose a part to edit its tone.");
        if (tonePart_ >= 0) toneEditor_.releaseNotes(toneHost_);
        tonePart_ = -1;
        return;
    }
    syncToneEditor();
    const PartStatus& part = status_.parts[editPart_];
    ImGui::SameLine(0.0f, fontSize);
    if (part.channel < 16) {
        ImGui::TextDisabled("channel %d", part.channel + 1);
        UiStyle::setItemTooltip("The editor's keyboard, the computer keys and your MIDI keyboard on this channel play the part.");
    } else {
        ImGui::TextColored(kErrorColor, "no MIDI channel");
        UiStyle::setItemTooltip("Give the part a channel (Play tab) to hear the editor's keyboard.");
    }

    // The part's tone: choosing another loads it into the part (and the editor), as on the unit.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Tone");
    ImGui::SameLine();
    uint8_t group = uint8_t(toneGroupOf(part.temp, false));
    uint8_t number = part.temp[TimbreTemp::ToneNumber] & 63;
    if (drawTonePicker("tone", group, number, part.name, true, true)) {
        uint8_t timbre[8];
        std::copy(part.temp, part.temp + 8, timbre);
        setToneGroup(timbre, group);
        timbre[1] = number;
        engine_->writePartTemp(editPart_, TimbreTemp::ToneGroup, timbre, 8);
        toneWriteTime_ = -1.0;  // Take the new tone in as soon as the part has it
    }
    UiStyle::setItemTooltip("Loads a tone into the part. Edits not written to memory can be brought back with Undo.");

    // Writing, like the unit's tone write: tone memory i11-i88, or the card's c11-c88.
    ImGui::SameLine(0.0f, fontSize * 1.5f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Write to");
    ImGui::SameLine();
    const int targets = status_.cardInserted && status_.d110 ? 128 : 64;
    if (writeToneTarget_ >= targets) writeToneTarget_ = 0;
    auto targetLabel = [&](int target) {
        const size_t index = target < 64 ? size_t(128 + target) : size_t(256 + (target & 63));
        std::string code = Tone::slotName(target & 63);
        if (target >= 64) code[0] = 'c';
        return code + " " + (index < toneNames_.size() ? toneNames_[index] : std::string());
    };
    ImGui::SetNextItemWidth(fontSize * 8.0f);
    if (ImGui::BeginCombo("##tonetarget", targetLabel(writeToneTarget_).c_str(), ImGuiComboFlags_HeightLarge)) {
        for (int target = 0; target < targets; target++) {
            if (ImGui::Selectable(targetLabel(target).c_str(), target == writeToneTarget_)) writeToneTarget_ = target;
            if (target == writeToneTarget_ && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Write##tone")) writeEditedTone();
    UiStyle::setItemTooltip("Stores the edited tone in this slot of tone memory and points the part at it, like the unit's "
                          "tone write. The tone that was there is replaced.");
    ImGui::SameLine(0.0f, fontSize);
    if (ImGui::Button("Load...")) loadToneFile();
    UiStyle::setItemTooltip("Loads a tone from a SysEx file (a tone, a bulk dump, MT-32 timbres) into the part");
    ImGui::SameLine();
    if (ImGui::Button("Save...")) saveToneFile();
    UiStyle::setItemTooltip("Saves the edited tone as a SysEx file (tone temporary area of part 1, any LA synth loads it)");
    drawToneFilePicker();

    ImGui::Separator();
    toneEditor_.draw(toneHost_, ImGui::GetTime());
}

void App::writeEditedTone() {
    if (tonePart_ < 0 || tonePart_ >= int(status_.partCount) || tonePart_ == kRhythmPart) return;
    toneEditor_.settle(toneHost_);
    const bool toCard = writeToneTarget_ >= 64;
    const int slot = writeToneTarget_ & 63;
    if (tonePart_ < kRhythmPart && status_.d110) {
        const uint8_t request[2] = {uint8_t(slot), uint8_t(toCard ? 1 : 0)};  // Tone number, internal/card
        engine_->writeData(0x400000 + uint32_t(tonePart_) * 2, request, 2);
    } else {
        // Parts 9-15 and the MT-32 have no write request: store the tone and select it on the part.
        const Tone::Data& tone = toneEditor_.tone();
        engine_->writeData(RolandSysex::unpack(toCard ? Tone::cardToneAddress(slot) : Tone::toneMemoryAddress(slot)), tone.data(), Tone::kSize);
        uint8_t timbre[8];
        std::copy(status_.parts[tonePart_].temp, status_.parts[tonePart_].temp + 8, timbre);
        timbre[0] = 2;
        timbre[1] = uint8_t(slot);
        timbre[7] = toCard ? 1 : 0;
        engine_->writePartTemp(tonePart_, TimbreTemp::ToneGroup, timbre, 8);
    }
    toneEditor_.markOriginal();
    toneWriteTime_ = ImGui::GetTime();
    cacheTime_ = -1.0;
    std::string code = Tone::slotName(slot);
    if (toCard) code[0] = 'c';
    addLog("Tone \"" + Tone::name(toneEditor_.tone()) + "\" written to " + code);
}

void App::loadToneFile() {
    openFileThen(Platform::FileKind::MidiOrSysex, [this](const std::filesystem::path& path) { loadToneFile(path); });
}

void App::loadToneFile(const std::filesystem::path& path) {
    const std::vector<uint8_t> data = readBinaryFile(path);
    toneFileTones_ = Tone::findTones(data.data(), data.size());
    toneFileName_ = Platform::toUtf8(path.filename());
    if (toneFileTones_.empty()) {
        addLog("No complete tones in " + toneFileName_);
        return;
    }
    // MT-32 data (a game's timbres, often a .dat handshake transfer) is offered with its waves translated.
    std::string extension = Platform::toUtf8(path.extension());
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    toneFileMt32_ = status_.d110 && (extension == ".dat" || (mt32Mode_ && !Mt32Translator::isDSeriesData(data.data(), data.size())));
    openToneFilePicker_ = true;
}

void App::drawToneFilePicker() {
    if (openToneFilePicker_) {
        ImGui::OpenPopup("tonefile");
        openToneFilePicker_ = false;
    }
    if (!ImGui::BeginPopup("tonefile")) return;
    const float fontSize = ImGui::GetFontSize();
    ImGui::Text("Tones in %s", toneFileName_.c_str());
    if (status_.d110) {
        ImGui::Checkbox("MT-32 timbres: translate the waves", &toneFileMt32_);
        UiStyle::setItemTooltip("The MT-32's waves are other samples than the D-110's: this picks the D-110's closest wave "
                              "for each PCM partial and corrects its pitch, as MT-32 translation does.");
    }
    ImGui::BeginChild("tones", ImVec2(fontSize * 18.0f, std::min(fontSize * 20.0f, fontSize * (1.6f * float(toneFileTones_.size()) + 1.0f))));
    for (size_t i = 0; i < toneFileTones_.size(); i++) {
        const Tone::FoundTone& found = toneFileTones_[i];
        const std::string label = found.where + "  " + Tone::name(found.data) + "##" + std::to_string(i);
        if (ImGui::Selectable(label.c_str())) {
            Tone::Data tone = found.data;
            if (toneFileMt32_ && status_.d110) {
                Mt32Translator translator;
                translator.setRoomyToms(mt32RoomyToms_);
                translator.convertTone(found.data.data(), tone.data());
            }
            toneEditor_.replaceTone(toneHost_, tone);
            addLog("Loaded tone \"" + Tone::name(tone) + "\" (" + found.where + " of " + toneFileName_ + ") into part " + partLabel(tonePart_));
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndChild();
    ImGui::TextDisabled("It replaces the part's tone (Undo brings it back).");
    ImGui::EndPopup();
}

void App::saveToneFile() {
    std::string name = Tone::name(toneEditor_.tone());
    for (char& c : name) {
        if (std::strchr("\\/:*?\"<>|", c) != nullptr) c = '_';
    }
    saveFileThen(Platform::FileKind::Sysex, (name.empty() ? std::string("Tone") : name) + ".syx", [this](const std::filesystem::path& path) {
        const Tone::Data& tone = toneEditor_.tone();
        const std::vector<uint8_t> message = RolandSysex::dataSet(0x10, Tone::toneTempAddress(0), tone.data(), Tone::kSize);
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(message.data()), std::streamsize(message.size()));
        if (!out) {
            addLog("Cannot write " + Platform::toUtf8(path));
            return;
        }
        toneEditor_.markOriginal();
        addLog("Saved tone \"" + Tone::name(tone) + "\" to " + Platform::toUtf8(path.filename()));
    });
}

void App::writeTimbreBytes(int timbre, int offset, const uint8_t* data, int length) {
    const bool card = timbre >= 128;
    std::vector<uint8_t>& memory = card ? cardTimbreMemory_ : timbreMemory_;
    const size_t entry = size_t(timbre & 127) * 8;
    if (memory.size() < entry + 8) return;
    std::copy(data, data + length, memory.begin() + long(entry + size_t(offset)));
    engine_->writeData(addressPlus(card ? kCardTimbreAddress : kTimbreMemoryAddress, uint32_t(entry + size_t(offset))), data, size_t(length));
    // Parts that play this timbre take the change at once: their timbre temporary areas get the same bytes (a tone
    // change reloads the tone, as a program change would).
    for (int part = 0; part < int(status_.partCount); part++) {
        if (part == kRhythmPart || partTimbre(status_.parts[part]) != timbre) continue;
        engine_->writePartTemp(part, uint8_t(offset), data, size_t(length));
    }
    cacheTime_ = ImGui::GetTime();  // The copy above is current; read the synth again later
}

void App::drawTimbresTab() {
    if (!status_.open || timbreMemory_.size() < 128 * 8) {
        ImGui::TextDisabled("The synth is not running.");
        return;
    }
    const float fontSize = ImGui::GetFontSize();
    if (status_.cardInserted && status_.d110) {
        ImGui::RadioButton("Internal", &timbreBank_, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Card", &timbreBank_, 1);
        ImGui::SameLine(0.0f, fontSize);
    } else {
        timbreBank_ = 0;
    }
    const std::vector<uint8_t>& memory = timbreBank_ == 1 ? cardTimbreMemory_ : timbreMemory_;
    if (memory.size() < 128 * 8) return;
    const bool auditionPart = editPart_ != kRhythmPart && editPart_ < int(status_.partCount);
    ImGui::AlignTextToFramePadding();
    if (auditionPart) {
        ImGui::TextDisabled("Click a timbre's number to play it on part %s. Parts that play a timbre hear its changes at once.",
                            partLabel(editPart_).c_str());
    } else {
        ImGui::TextDisabled("Choose a part 1-8 on the Parts tab to play timbres on.");
    }

    const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("timbres", 7, flags, ImVec2(0.0f, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Timbre", ImGuiTableColumnFlags_WidthFixed, fontSize * 4.0f);
    ImGui::TableSetupColumn("Tone", ImGuiTableColumnFlags_WidthFixed, fontSize * 9.3f);
    ImGui::TableSetupColumn("Key shift", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Fine tune", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Bender", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Assign mode", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Output", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    const int playing = auditionPart ? partTimbre(status_.parts[editPart_]) : -1;
    for (int t = 0; t < 128; t++) {
        const int timbre = timbreBank_ * 128 + t;
        const uint8_t* entry = &memory[size_t(t) * 8];
        ImGui::TableNextRow();
        ImGui::PushID(timbre);

        ImGui::TableNextColumn();
        if (ImGui::Selectable(timbreCode(timbre).c_str(), timbre == playing, 0, ImVec2(0.0f, ImGui::GetFrameHeight())) && auditionPart) {
            engine_->setPartProgram(editPart_, timbre);
        }
        if (ImGui::BeginPopupContextItem("timbre")) {
            if (ImGui::MenuItem("Copy")) {
                std::copy(entry, entry + 8, timbreClipboard_.begin());
                timbreClipboardFull_ = true;
            }
            if (ImGui::MenuItem("Paste", nullptr, false, timbreClipboardFull_)) {
                uint8_t bytes[7];
                std::copy(timbreClipboard_.begin(), timbreClipboard_.begin() + 7, bytes);
                writeTimbreBytes(timbre, 0, bytes, 7);
            }
            ImGui::EndPopup();
        }

        // Group i of a card timbre plays the card's tones (c); a and b with the alt flag (07H) tone banks d and e.
        ImGui::TableNextColumn();
        const bool cardTimbre = timbreBank_ == 1;
        uint8_t group = uint8_t(toneGroupOf(entry, cardTimbre));
        uint8_t number = uint8_t(entry[1] & 63);
        if (drawTonePicker("tone", group, number, nullptr, cardTimbre, true)) {
            uint8_t bytes[8];
            std::copy(entry, entry + 8, bytes);
            setToneGroup(bytes, group);
            bytes[1] = number;
            if (cardTimbre && group == kCardGroup) bytes[7] = 0;  // A card timbre's group i is always the card's
            const uint8_t flag = bytes[7];
            const bool flagChanged = flag != entry[7];
            writeTimbreBytes(timbre, 0, bytes, 2);
            if (flagChanged) writeTimbreBytes(timbre, 7, &flag, 1);
        }

        auto slider = [&](const char* id, int offset, int low, int high, int shift, const char* format) {
            ImGui::TableNextColumn();
            int value = int(entry[offset]) - shift;
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::SliderInt(id, &value, low, high, format, ImGuiSliderFlags_AlwaysClamp)) {
                const uint8_t byte = uint8_t(value + shift);
                writeTimbreBytes(timbre, offset, &byte, 1);
            }
        };
        slider("##keyshift", TimbreTemp::KeyShift, -24, 24, 24, "%+d");
        slider("##finetune", TimbreTemp::FineTune, -50, 50, 50, "%+d");
        slider("##bender", TimbreTemp::BenderRange, 0, 24, 0, "%d");

        ImGui::TableNextColumn();
        int assign = std::min<int>(entry[TimbreTemp::AssignMode], 3);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##assign", &assign, kAssignNames, IM_ARRAYSIZE(kAssignNames))) {
            const uint8_t byte = uint8_t(assign);
            writeTimbreBytes(timbre, TimbreTemp::AssignMode, &byte, 1);
        }

        ImGui::TableNextColumn();
        int output = std::min<int>(entry[TimbreTemp::OutputAssign], status_.d110 ? 7 : 1);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (status_.d110) {
            if (outputAssignCombo("##output", output, true)) {
                const uint8_t byte = uint8_t(output);
                writeTimbreBytes(timbre, TimbreTemp::OutputAssign, &byte, 1);
            }
        } else if (ImGui::Combo("##output", &output, kMT32OutputNames, IM_ARRAYSIZE(kMT32OutputNames))) {
            const uint8_t byte = uint8_t(output);
            writeTimbreBytes(timbre, TimbreTemp::OutputAssign, &byte, 1);
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}
