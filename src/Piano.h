#pragma once

#include <array>
#include <cstdint>
#include <string>

// The on-screen piano and the computer-keyboard layout shared by D110Emu and the tone editors.
namespace Piano {

// Tracker-style computer keyboard: the bottom letter row plays the lower octave, the top row the one above. Keys are
// physical positions (PC scancodes), so QWERTZ and AZERTY keyboards get the same layout as QWERTY.
struct Key {
    uint8_t scancode;
    int semitone;
    const char* usName;  // Key cap on a US keyboard, for platforms without key names
};
constexpr int kKeyCount = 34;
extern const Key kKeys[kKeyCount];

// The key cap of a piano key in the user's keyboard layout.
std::string keyName(uint8_t scancode);
bool isBlackKey(int note);

// Draws keys firstNote..lastNote across the available width (white keys at most `maxWhiteWidth` wide), `height`
// tall. `lit` marks sounding notes, `pressed` notes held by the player. Returns the note under the left mouse button
// while it is held on the keyboard, -1 otherwise.
int draw(const char* id, int firstNote, int lastNote, float height, float maxWhiteWidth, const std::array<bool, 128>& lit,
         const std::array<bool, 128>& pressed);

}  // namespace Piano
