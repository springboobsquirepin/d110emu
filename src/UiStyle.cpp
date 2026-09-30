#include "UiStyle.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdarg>
#include <filesystem>
#include <string>

#include "imgui_internal.h"  // BeginTooltipEx, as SetTooltip calls it

namespace UiStyle {

namespace {

void tooltipV(const char* fmt, va_list args) {
    if (!ImGui::BeginTooltipEx(ImGuiTooltipFlags_OverridePrevious, ImGuiWindowFlags_None)) return;
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36.0f);
    ImGui::TextV(fmt, args);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

}  // namespace

void loadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::GetStyle().FontSizeBase = 16.0f;
    std::error_code ec;
#ifdef _WIN32
    wchar_t windowsDir[MAX_PATH] = {};
    GetWindowsDirectoryW(windowsDir, MAX_PATH);
    const std::filesystem::path segoe = std::filesystem::path(windowsDir) / "Fonts" / "segoeui.ttf";
    if (std::filesystem::exists(segoe, ec) && io.Fonts->AddFontFromFileTTF(segoe.u8string().c_str()) != nullptr) return;
#elif defined(__APPLE__)
    // Not San Francisco, macOS's own: it is a variable font, and Dear ImGui's font reader (stb_truetype) takes no font
    // variations.
    static const char* const kFonts[] = {
        "/System/Library/Fonts/Helvetica.ttc",  // Its first font, which Dear ImGui takes, is Helvetica Regular
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/Library/Fonts/Arial.ttf",
    };
    for (const char* path : kFonts) {
        if (std::filesystem::exists(path, ec) && io.Fonts->AddFontFromFileTTF(path) != nullptr) return;
    }
#else
    static const char* const kFonts[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",    // Debian, Raspberry Pi OS, Ubuntu
        "/usr/share/fonts/TTF/DejaVuSans.ttf",                // Arch
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",  // Fedora
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
    };
    for (const char* path : kFonts) {
        if (std::filesystem::exists(path, ec) && io.Fonts->AddFontFromFileTTF(path) != nullptr) return;
    }
#endif
    io.Fonts->AddFontDefault();
}

int wheelSteps() {
    static float carried = 0.0f;  // What the wheel turned short of a whole step, over `carriedItem`
    static ImGuiID carriedItem = 0;
    const ImGuiIO& io = ImGui::GetIO();
    if (!ImGui::IsItemHovered() || !ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY)) return 0;
    float delta = io.MouseWheel;
#ifdef __APPLE__
    if (io.KeyShift && ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelX) && delta == 0.0f) delta = io.MouseWheelH;
#endif
    if (delta == 0.0f) return 0;
    const ImGuiID item = ImGui::GetItemID();
    if (item != carriedItem || (carried > 0.0f) != (delta > 0.0f)) carried = 0.0f;
    carriedItem = item;
    carried += delta;
    const int steps = int(carried);  // Toward zero: what is left carries over
    carried -= float(steps);
    return steps;
}

void apply() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.PopupRounding = 4.0f;
    style.ScrollbarRounding = 6.0f;
    style.FramePadding = ImVec2(6.0f, 4.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.09f, 0.10f, 1.0f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.11f, 0.11f, 0.12f, 1.0f);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.13f, 0.13f, 0.14f, 1.0f);
    colors[ImGuiCol_Header] = ImVec4(0.22f, 0.24f, 0.28f, 1.0f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.28f, 0.32f, 0.38f, 1.0f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.32f, 0.37f, 0.45f, 1.0f);
}

void helpMarker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    setItemTooltip("%s", text);
}

void setItemTooltip(const char* fmt, ...) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) return;
    va_list args;
    va_start(args, fmt);
    tooltipV(fmt, args);
    va_end(args);
}

void setTooltip(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    tooltipV(fmt, args);
    va_end(args);
}

}  // namespace UiStyle
