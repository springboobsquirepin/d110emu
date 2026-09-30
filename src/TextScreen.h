#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// The terminal interface's drawing and input, without the platform: a screen of text cells, how a frame of it reaches
// the terminal as VT escape sequences (xterm-like terminals, the Linux console, Windows 10's console; only the cells that
// changed), and the keys that the bytes a terminal sends stand for. Terminal.h is the platform's side.
namespace Tui {

// A colour: the terminal's own (Default), one of the 16 ANSI colours, or RGB (with the ANSI colour to use where the
// terminal has only those).
struct Color {
    enum Kind : uint8_t { Default, Ansi, Rgb };
    Kind kind = Default;
    uint8_t ansi = 0;  // 0-7 normal, 8-15 bright
    uint8_t r = 0, g = 0, b = 0;

    static Color ansiColor(int index) {
        Color color;
        color.kind = Ansi;
        color.ansi = uint8_t(index & 15);
        return color;
    }
    static Color rgb(int red, int green, int blue, int fallback) {
        Color color;
        color.kind = Rgb;
        color.ansi = uint8_t(fallback & 15);
        color.r = uint8_t(red);
        color.g = uint8_t(green);
        color.b = uint8_t(blue);
        return color;
    }
    bool operator==(const Color& other) const {
        return kind == other.kind && ansi == other.ansi && r == other.r && g == other.g && b == other.b;
    }
    bool operator!=(const Color& other) const { return !(*this == other); }
};

enum Attribute : uint8_t { Bold = 1, Dim = 2, Reverse = 4, Underline = 8 };

struct Style {
    Color fg;
    Color bg;
    uint8_t attributes = 0;
    bool operator==(const Style& other) const { return fg == other.fg && bg == other.bg && attributes == other.attributes; }
    bool operator!=(const Style& other) const { return !(*this == other); }
};

struct Cell {
    char32_t ch = U' ';
    Style style;
    bool operator==(const Cell& other) const { return ch == other.ch && style == other.style; }
    bool operator!=(const Cell& other) const { return !(*this == other); }
};

// What a terminal shows. Glyphs: Unicode (box lines, blocks, symbols), Console (the Linux console's fonts: lines and
// blocks, not every symbol) or Ascii.
struct Capabilities {
    enum class Colors { None, Ansi16, Ansi256, TrueColor };
    enum class Glyphs { Unicode, Console, Ascii };
    Colors colors = Colors::Ansi16;
    Glyphs glyphs = Glyphs::Unicode;
    bool boldBright = false;  // Bright foregrounds as bold (the Linux console); bright backgrounds as normal ones
};

// Decodes one UTF-8 character at `text[i]`, advancing i; invalid bytes come out as U+FFFD.
char32_t decodeUtf8(const std::string& text, size_t& i);
void appendUtf8(std::string& out, char32_t ch);

class Screen {
public:
    void resize(int columns, int rows);  // And clears
    int columns() const { return columns_; }
    int rows() const { return rows_; }
    void clear(const Style& style = Style());
    const Cell& at(int x, int y) const { return cells_[size_t(y) * size_t(columns_) + size_t(x)]; }
    // Outside the screen: ignored.
    void set(int x, int y, char32_t ch, const Style& style);
    void setStyle(int x, int y, const Style& style);
    // UTF-8 text from (x, y) on, cut at maxWidth columns (and the screen's edge); returns the columns written.
    int text(int x, int y, const std::string& utf8, const Style& style, int maxWidth = -1);
    void fill(int x, int y, int width, int height, char32_t ch, const Style& style);
    // A frame of line characters around the rectangle, with a title in its top line.
    void box(int x, int y, int width, int height, const Style& style, const std::string& title = std::string());
    // The screen as text, one line per row (tests).
    std::string plainText() const;

private:
    int columns_ = 0;
    int rows_ = 0;
    std::vector<Cell> cells_;
};

// Columns a UTF-8 string takes (one per character).
int textWidth(const std::string& utf8);
// Cut or filled with spaces to `width` columns.
std::string fitText(const std::string& utf8, int width);

// Turns screens into what a terminal draws: the whole screen the first time (after a resize, or reset()), then only the
// cells that changed. Glyphs the terminal lacks become the nearest it has.
class Encoder {
public:
    explicit Encoder(const Capabilities& capabilities) : caps_(capabilities) {}
    std::string frame(const Screen& screen);
    void reset() { previous_ = Screen(); }
    // Enters and leaves the interface: the alternate screen, no cursor, no line wrap at the right edge.
    static const char* enterSequence();
    static const char* leaveSequence();
    char32_t glyph(char32_t ch) const;  // What the terminal shows for `ch`

private:
    void appendStyle(std::string& out, const Style& style) const;
    void appendColor(std::string& out, const Color& color, bool background) const;

    Capabilities caps_;
    Screen previous_;
};

struct Key {
    enum Code : uint8_t {
        None, Char, Enter, Escape, Tab, BackTab, Backspace, Delete, Insert,
        Up, Down, Left, Right, Home, End, PageUp, PageDown,
        F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    };
    Code code = None;
    char32_t ch = 0;  // Char: the character; with ctrl, the letter in lower case (Ctrl+C: 'c')
    bool ctrl = false;
    bool alt = false;
    bool shift = false;

    static Key of(Code code) {
        Key key;
        key.code = code;
        return key;
    }
    static Key character(char32_t ch) {
        Key key;
        key.code = Char;
        key.ch = ch;
        return key;
    }
    bool is(char32_t lower) const { return code == Char && !ctrl && !alt && (ch == lower || ch == lower - 32); }  // 'q' or 'Q'
};

// The bytes a POSIX terminal sends for keys (UTF-8, control characters, VT sequences: xterm's, the Linux console's)
// into keys. An Escape key alone can only be told from the start of a sequence when nothing follows it soon: flush()
// takes what waits once the input pauses.
class KeyParser {
public:
    void feed(const char* bytes, size_t count, std::vector<Key>& keys);
    bool pending() const { return !buffer_.empty(); }
    void flush(std::vector<Key>& keys);

private:
    // Parses buffer_ from its start; returns bytes used, 0 when a sequence is incomplete.
    size_t parseOne(std::vector<Key>& keys, bool final);
    std::string buffer_;
};

}  // namespace Tui
