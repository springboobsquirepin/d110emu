#include "Lcd.h"

// Only for IM_COL32, in which the colours are written: nothing of Dear ImGui is called here, so the terminal interface
// has the schemes without it.
#include "imgui.h"

namespace Lcd {

namespace {

struct Scheme {
    const char* name;
    ImU32 background;
    ImU32 dotOff;  // Unlit dots, a shade off the background
    ImU32 dotOn;
};

const Scheme kSchemes[] = {
    {"D-110 (lime green)", IM_COL32(79, 139, 21, 255), IM_COL32(68, 120, 18, 255), IM_COL32(24, 36, 12, 255)},
    {"D-10/D-20 (dark green)", IM_COL32(0, 0, 0, 255), IM_COL32(94, 93, 0, 255), IM_COL32(202, 200, 0, 255)},  // The user's colours for their D-20
    {"Yellow-green", IM_COL32(126, 158, 34, 255), IM_COL32(114, 144, 30, 255), IM_COL32(24, 36, 12, 255)},
    {"Blue", IM_COL32(30, 58, 168, 255), IM_COL32(40, 70, 182, 255), IM_COL32(226, 236, 255, 255)},
    {"Amber", IM_COL32(38, 20, 2, 255), IM_COL32(52, 30, 6, 255), IM_COL32(255, 178, 48, 255)},
    {"Grey (unlit)", IM_COL32(168, 174, 158, 255), IM_COL32(156, 162, 146, 255), IM_COL32(38, 42, 38, 255)},
};
constexpr int kSchemeCount = int(sizeof(kSchemes) / sizeof(kSchemes[0]));

}  // namespace

int schemeCount() {
    return kSchemeCount;
}

const char* schemeName(int scheme) {
    return kSchemes[scheme >= 0 && scheme < kSchemeCount ? scheme : 0].name;
}

Colors schemeColors(int scheme) {
    const Scheme& preset = kSchemes[scheme >= 0 && scheme < kSchemeCount ? scheme : 0];
    return {preset.background, preset.dotOff, preset.dotOn};
}

}  // namespace Lcd
