#include "Piano.h"

#include <algorithm>
#include <cmath>

#include "Platform.h"
#include "imgui.h"

namespace Piano {

const Key kKeys[kKeyCount] = {
    {0x2C, 0, "Z"},  {0x1F, 1, "S"},  {0x2D, 2, "X"},  {0x20, 3, "D"},  {0x2E, 4, "C"},  {0x2F, 5, "V"},
    {0x22, 6, "G"},  {0x30, 7, "B"},  {0x23, 8, "H"},  {0x31, 9, "N"},  {0x24, 10, "J"}, {0x32, 11, "M"},
    {0x33, 12, ","}, {0x26, 13, "L"}, {0x34, 14, "."}, {0x27, 15, ";"}, {0x35, 16, "/"},
    {0x10, 12, "Q"}, {0x03, 13, "2"}, {0x11, 14, "W"}, {0x04, 15, "3"}, {0x12, 16, "E"}, {0x13, 17, "R"},
    {0x06, 18, "5"}, {0x14, 19, "T"}, {0x07, 20, "6"}, {0x15, 21, "Y"}, {0x08, 22, "7"}, {0x16, 23, "U"},
    {0x17, 24, "I"}, {0x0A, 25, "9"}, {0x18, 26, "O"}, {0x0B, 27, "0"}, {0x19, 28, "P"},
};

std::string keyName(uint8_t scancode) {
    std::string name = Platform::keyName(scancode);
    if (!name.empty()) return name;
    for (const Key& key : kKeys) {
        if (key.scancode == scancode) return key.usName;
    }
    return "?";
}

bool isBlackKey(int note) {
    const int semitone = note % 12;
    return semitone == 1 || semitone == 3 || semitone == 6 || semitone == 8 || semitone == 10;
}

int draw(const char* id, int firstNote, int lastNote, float height, float maxWhiteWidth, const std::array<bool, 128>& lit,
         const std::array<bool, 128>& pressed) {
    firstNote = std::clamp(firstNote, 0, 127);
    lastNote = std::clamp(lastNote, firstNote, 127);
    int whiteCount = 0;
    for (int note = firstNote; note <= lastNote; note++) {
        if (!isBlackKey(note)) whiteCount++;
    }
    const float available = ImGui::GetContentRegionAvail().x;
    const float whiteWidth = std::clamp(std::floor(available / float(std::max(whiteCount, 1))), 6.0f, maxWhiteWidth);
    const float blackWidth = whiteWidth * 0.6f;
    const float blackHeight = height * 0.62f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    // Left edge of every key.
    float keyX[128] = {};
    int whiteIndex = 0;
    for (int note = firstNote; note <= lastNote; note++) {
        if (isBlackKey(note)) {
            keyX[note] = origin.x + float(whiteIndex) * whiteWidth - blackWidth * 0.5f;
        } else {
            keyX[note] = origin.x + float(whiteIndex) * whiteWidth;
            whiteIndex++;
        }
    }

    ImGui::InvisibleButton(id, ImVec2(whiteWidth * float(whiteCount), height));
    int hitNote = -1;
    if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const ImVec2 mouse = ImGui::GetMousePos();
        for (int note = firstNote; note <= lastNote && hitNote < 0; note++) {
            if (isBlackKey(note) && mouse.x >= keyX[note] && mouse.x < keyX[note] + blackWidth && mouse.y < origin.y + blackHeight) hitNote = note;
        }
        for (int note = firstNote; note <= lastNote && hitNote < 0; note++) {
            if (!isBlackKey(note) && mouse.x >= keyX[note] && mouse.x < keyX[note] + whiteWidth) hitNote = note;
        }
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImU32 litColor = IM_COL32(110, 170, 250, 255);
    const ImU32 pressedColor = IM_COL32(70, 130, 230, 255);
    auto keyColor = [&](int note, ImU32 normal) {
        if (note == hitNote || pressed[size_t(note)]) return pressedColor;
        return lit[size_t(note)] ? litColor : normal;
    };
    for (int note = firstNote; note <= lastNote; note++) {
        if (isBlackKey(note)) continue;
        const ImVec2 min(keyX[note], origin.y);
        const ImVec2 max(keyX[note] + whiteWidth - 1.0f, origin.y + height);
        drawList->AddRectFilled(min, max, keyColor(note, IM_COL32(235, 235, 235, 255)), 2.0f);
        if (note % 12 == 0) {
            const std::string label = "C" + std::to_string(note / 12 - 1);
            const ImVec2 size = ImGui::CalcTextSize(label.c_str());
            if (size.x < whiteWidth) {
                drawList->AddText(ImVec2(min.x + (whiteWidth - size.x) * 0.5f, max.y - size.y - 2.0f), IM_COL32(90, 90, 90, 255), label.c_str());
            }
        }
    }
    for (int note = firstNote; note <= lastNote; note++) {
        if (!isBlackKey(note)) continue;
        drawList->AddRectFilled(ImVec2(keyX[note], origin.y), ImVec2(keyX[note] + blackWidth, origin.y + blackHeight),
                                keyColor(note, IM_COL32(25, 25, 28, 255)), 2.0f);
    }
    return hitNote;
}

}  // namespace Piano
