#pragma once

#include <cstdint>
#include <string>

// A 16x2 character LCD like the D-110's (HD44780-style, 5x8 dot characters).
namespace Lcd {

constexpr int kColumns = 16;
constexpr int kRows = 2;

// A display's colours, as Dear ImGui's packed colours (IM_COL32).
struct Colors {
    uint32_t background;  // The glass
    uint32_t dotOff;      // Unlit dots, a shade off the glass
    uint32_t dotOn;       // Lit dots
};

// Preset colour schemes: the D-110's yellow-green, the D-10/D-20's bright yellow-green characters on black, and others.
int schemeCount();
const char* schemeName(int scheme);
Colors schemeColors(int scheme);

// Draws the display at the cursor. `text` holds up to 32 characters, row 1 then row 2; characters with the
// high bit set are drawn inverted (lit cell, dark glyph), which the play mode uses for active part numbers.
// `dotPitch` is the distance between dots in whole pixels (a dot and the gap after it), so that every dot, lit or
// not, lands on the same pixel grid. Returns true when the display was clicked.
bool draw(const char* id, const std::string& text, int dotPitch, const Colors& colors);

}  // namespace Lcd
