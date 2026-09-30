#pragma once

#include "imgui.h"

// The look shared by D110Emu and MT32Translator.
namespace UiStyle {

void apply();                          // Dear ImGui style and colours
// The apps' font at their 16-pixel base size, into the current context: Segoe UI on Windows; Helvetica on macOS, else
// Arial; on Linux DejaVu Sans, else Noto Sans or Liberation Sans; else Dear ImGui's own.
void loadFonts();
void helpMarker(const char* text);     // A "(?)" after the last item, with `text` as its tooltip
// ImGui::SetItemTooltip and ImGui::SetTooltip with the text wrapped at a readable width: ImGui's tooltips never wrap by
// themselves, so long ones ran off the window.
void setItemTooltip(const char* fmt, ...) IM_FMTARGS(1);
void setTooltip(const char* fmt, ...) IM_FMTARGS(1);
// Whole steps the mouse wheel turned over the last item, which then takes the wheel from its window (which would scroll
// instead); 0 when the item is not hovered. Fine-grained wheels and trackpads (a Mac's) send fractions of a step, which
// add up; on macOS, Shift+wheel comes as the horizontal wheel and counts too.
int wheelSteps();

// The key Dear ImGui's Ctrl shortcuts and Ctrl+click take, as the keyboard names it, for the texts that name it: Cmd on
// macOS, where Dear ImGui swaps Cmd and Ctrl (and a Ctrl+click is a right-click), else Ctrl.
#ifdef __APPLE__
#define UI_CTRL "Cmd"
#else
#define UI_CTRL "Ctrl"
#endif

// View > Window size, in percent of the size the display's scaling gives (the standalone's window and the plugins').
inline constexpr int kZoomSteps[] = {75, 100, 125, 150, 200};

const ImVec4 kErrorColor(1.0f, 0.45f, 0.4f, 1.0f);
const ImVec4 kOkColor(0.45f, 0.85f, 0.5f, 1.0f);

}  // namespace UiStyle
