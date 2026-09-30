#include "PresetChoiceTable.h"

#include <algorithm>
#include <cfloat>

#include "PresetNames.h"
#include "imgui.h"

namespace PresetChoiceTable {

namespace {

constexpr ImGuiTableFlags kTableFlags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                        ImGuiTableFlags_SizingFixedFit;

// The Plays column's items: the stand-in, then the MT-32's own (a zero-separated list for ImGui::Combo).
std::string playsItems(const Context& context) {
    return std::string(context.unitName) + '\0' + "MT-32's" + '\0';
}

std::string rhythmToneLabel(int tone) {
    return tone == Mt32Translator::kRhythmOff ? std::string("Off") : toneLabel(192 + tone);
}

}  // namespace

std::string toneLabel(int tone) {
    return dSeriesToneLabel(tone);
}

bool drawPresets(Mt32Translator::PresetChoices& choices, const Context& context) {
    const bool hybrid = context.mode == Mt32Translator::PresetMode::Hybrid && context.presets != nullptr;
    const bool exactAll = context.mode == Mt32Translator::PresetMode::Exact && context.presets != nullptr;
    if (!ImGui::BeginTable("presets", 5, kTableFlags, ImVec2(0.0f, 0.0f))) return false;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("MT-32 preset");
    ImGui::TableSetupColumn("Plays");
    ImGui::TableSetupColumn((std::string(context.unitName) + " preset").c_str(), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Shift");
    ImGui::TableSetupColumn("");
    ImGui::TableHeadersRow();
    const std::string plays = playsItems(context);
    bool changed = false;
    ImGuiListClipper clipper;
    clipper.Begin(128);
    while (clipper.Step()) {
        for (int timbre = clipper.DisplayStart; timbre < clipper.DisplayEnd; timbre++) {
            Mt32Translator::PresetChoice& choice = choices[size_t(timbre)];
            const bool isDefault = choice == Mt32Translator::defaultPresetChoice(timbre);
            ImGui::PushID(timbre);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const std::string name = context.presets ? Mt32Presets::name(context.presets->melodic[size_t(timbre)]) : std::string(kMt32PresetNames[timbre]);
            ImGui::Text("%c%-2d %s", timbre < 64 ? 'A' : 'B', timbre % 64 + 1, name.c_str());
            ImGui::TableNextColumn();
            const bool exact = exactAll || (hybrid && choice.exact);
            ImGui::BeginDisabled(!hybrid);
            int playsIndex = exact ? 1 : 0;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
            if (ImGui::Combo("##plays", &playsIndex, plays.c_str())) {
                choice.exact = playsIndex == 1;
                changed = true;
            }
            ImGui::EndDisabled();
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(exact);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##tone", toneLabel(choice.tone).c_str(), ImGuiComboFlags_HeightLarge)) {
                for (int tone = 0; tone < 256; tone++) {
                    if (tone >= 128 && tone < 192) continue;
                    if (ImGui::Selectable(toneLabel(tone).c_str(), tone == choice.tone)) {
                        choice.tone = uint8_t(tone);
                        changed = true;
                    }
                    if (tone == choice.tone) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::TableNextColumn();
            static const int8_t shifts[] = {-24, -12, 0, 12, 24};
            static const char* const shiftNames[] = {"-2 oct", "-1 oct", "0", "+1 oct", "+2 oct"};
            int shift = 2;
            for (int i = 0; i < 5; i++) {
                if (shifts[i] == choice.keyShift) shift = i;
            }
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.0f);
            if (ImGui::Combo("##shift", &shift, shiftNames, 5)) {
                choice.keyShift = shifts[shift];
                changed = true;
            }
            ImGui::EndDisabled();
            ImGui::TableNextColumn();
            if (!isDefault) {
                if (ImGui::SmallButton("Built in")) {
                    choice = Mt32Translator::defaultPresetChoice(timbre);
                    changed = true;
                }
            }
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
    return changed;
}

bool drawRhythm(Mt32Translator::RhythmChoices& choices, const Context& context) {
    const bool hybrid = context.mode == Mt32Translator::PresetMode::Hybrid && context.presets != nullptr;
    const bool exactAll = context.mode == Mt32Translator::PresetMode::Exact && context.presets != nullptr;
    // The MT-32 has R1-R30; the CM-32L's sound effects follow.
    const int ownCount = context.presets ? int(std::min<size_t>(context.presets->rhythm.size(), 63)) : 0;
    const int count = std::max(ownCount, 30);
    if (!ImGui::BeginTable("rhythm", 4, kTableFlags, ImVec2(0.0f, 0.0f))) return false;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("MT-32 rhythm");
    ImGui::TableSetupColumn("Plays");
    ImGui::TableSetupColumn((std::string(context.unitName) + " rhythm tone").c_str(), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("");
    ImGui::TableHeadersRow();
    const std::string plays = playsItems(context);
    bool changed = false;
    for (int timbre = 0; timbre < count; timbre++) {
        Mt32Translator::RhythmChoice& choice = choices[size_t(timbre)];
        const bool own = timbre < ownCount;
        const int builtIn = Mt32Translator::rhythmTone(timbre, context.roomyToms);
        ImGui::PushID(timbre);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const std::string name = own ? Mt32Presets::name(context.presets->rhythm[size_t(timbre)]) : std::string(kMt32RhythmNames[timbre]);
        ImGui::Text("R%-2d %s", timbre + 1, name.c_str());
        ImGui::TableNextColumn();
        const bool exact = own && (exactAll || (hybrid && choice.exact));
        ImGui::BeginDisabled(!hybrid || !own);
        int playsIndex = exact ? 1 : 0;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
        if (ImGui::Combo("##plays", &playsIndex, plays.c_str())) {
            choice.exact = playsIndex == 1;
            changed = true;
        }
        ImGui::EndDisabled();
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(exact);
        const int current = choice.tone >= 0 ? choice.tone : builtIn;
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##tone", rhythmToneLabel(current).c_str(), ImGuiComboFlags_HeightLarge)) {
            for (int tone = 0; tone <= Mt32Translator::kRhythmOff; tone++) {
                if (ImGui::Selectable(rhythmToneLabel(tone).c_str(), tone == current)) {
                    choice.tone = int8_t(tone == builtIn ? -1 : tone);  // The built-in one follows the tom setting
                    changed = true;
                }
                if (tone == current) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        ImGui::TableNextColumn();
        if (!(choice == Mt32Translator::RhythmChoice())) {
            if (ImGui::SmallButton("Built in")) {
                choice = Mt32Translator::RhythmChoice();
                changed = true;
            }
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
    return changed;
}

}  // namespace PresetChoiceTable
