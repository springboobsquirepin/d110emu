#include "TextScreen.h"

#include <algorithm>
#include <cstdlib>

namespace Tui {

char32_t decodeUtf8(const std::string& text, size_t& i) {
    const uint8_t lead = uint8_t(text[i++]);
    if (lead < 0x80) return lead;
    int extra = lead >= 0xF0 && lead < 0xF8 ? 3 : lead >= 0xE0 ? 2 : lead >= 0xC0 ? 1 : -1;
    if (extra < 0) return U'�';
    char32_t ch = lead & (0x3F >> extra);
    for (; extra > 0; extra--) {
        if (i >= text.size() || (uint8_t(text[i]) & 0xC0) != 0x80) return U'�';
        ch = (ch << 6) | (uint8_t(text[i++]) & 0x3F);
    }
    return ch;
}

void appendUtf8(std::string& out, char32_t ch) {
    if (ch < 0x80) {
        out += char(ch);
    } else if (ch < 0x800) {
        out += char(0xC0 | (ch >> 6));
        out += char(0x80 | (ch & 0x3F));
    } else if (ch < 0x10000) {
        out += char(0xE0 | (ch >> 12));
        out += char(0x80 | ((ch >> 6) & 0x3F));
        out += char(0x80 | (ch & 0x3F));
    } else {
        out += char(0xF0 | (ch >> 18));
        out += char(0x80 | ((ch >> 12) & 0x3F));
        out += char(0x80 | ((ch >> 6) & 0x3F));
        out += char(0x80 | (ch & 0x3F));
    }
}

int textWidth(const std::string& utf8) {
    int width = 0;
    for (size_t i = 0; i < utf8.size();) {
        decodeUtf8(utf8, i);
        width++;
    }
    return width;
}

std::string fitText(const std::string& utf8, int width) {
    std::string out;
    int used = 0;
    for (size_t i = 0; i < utf8.size() && used < width; used++) {
        const size_t start = i;
        decodeUtf8(utf8, i);
        out.append(utf8, start, i - start);
    }
    out.append(size_t(std::max(0, width - used)), ' ');
    return out;
}

// ---------------------------------------------------------------------------------------------
// Screen

void Screen::resize(int columns, int rows) {
    columns_ = std::max(columns, 0);
    rows_ = std::max(rows, 0);
    cells_.assign(size_t(columns_) * size_t(rows_), Cell());
}

void Screen::clear(const Style& style) {
    Cell blank;
    blank.style = style;
    std::fill(cells_.begin(), cells_.end(), blank);
}

void Screen::set(int x, int y, char32_t ch, const Style& style) {
    if (x < 0 || y < 0 || x >= columns_ || y >= rows_) return;
    Cell& cell = cells_[size_t(y) * size_t(columns_) + size_t(x)];
    cell.ch = ch;
    cell.style = style;
}

void Screen::setStyle(int x, int y, const Style& style) {
    if (x < 0 || y < 0 || x >= columns_ || y >= rows_) return;
    cells_[size_t(y) * size_t(columns_) + size_t(x)].style = style;
}

int Screen::text(int x, int y, const std::string& utf8, const Style& style, int maxWidth) {
    int written = 0;
    for (size_t i = 0; i < utf8.size();) {
        if (maxWidth >= 0 && written >= maxWidth) break;
        const char32_t ch = decodeUtf8(utf8, i);
        set(x + written, y, ch, style);
        written++;
    }
    return written;
}

void Screen::fill(int x, int y, int width, int height, char32_t ch, const Style& style) {
    for (int row = y; row < y + height; row++) {
        for (int column = x; column < x + width; column++) set(column, row, ch, style);
    }
}

void Screen::box(int x, int y, int width, int height, const Style& style, const std::string& title) {
    if (width < 2 || height < 2) return;
    for (int column = x + 1; column < x + width - 1; column++) {
        set(column, y, U'─', style);
        set(column, y + height - 1, U'─', style);
    }
    for (int row = y + 1; row < y + height - 1; row++) {
        set(x, row, U'│', style);
        set(x + width - 1, row, U'│', style);
    }
    set(x, y, U'┌', style);
    set(x + width - 1, y, U'┐', style);
    set(x, y + height - 1, U'└', style);
    set(x + width - 1, y + height - 1, U'┘', style);
    if (!title.empty() && width > 6) text(x + 2, y, " " + title + " ", style, width - 4);
}

std::string Screen::plainText() const {
    std::string out;
    for (int y = 0; y < rows_; y++) {
        std::string line;
        for (int x = 0; x < columns_; x++) appendUtf8(line, at(x, y).ch);
        line.erase(line.find_last_not_of(' ') + 1);
        out += line + "\n";
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Encoder

namespace {

bool isWide(char32_t ch) {
    return (ch >= 0x1100 && ch <= 0x115F) || (ch >= 0x2E80 && ch <= 0xA4CF) || (ch >= 0xAC00 && ch <= 0xD7A3) ||
           (ch >= 0xF900 && ch <= 0xFAFF) || (ch >= 0xFE30 && ch <= 0xFE4F) || (ch >= 0xFF00 && ch <= 0xFF60) ||
           (ch >= 0xFFE0 && ch <= 0xFFE6) || (ch >= 0x1F300 && ch <= 0x1FAFF) || (ch >= 0x20000 && ch <= 0x3FFFD) ||
           (ch >= 0x0300 && ch <= 0x036F);  // Combining marks take no column: not shown either
}

// The xterm 256-colour palette's nearest entry: the 6x6x6 cube or the grey ramp.
int nearest256(int r, int g, int b) {
    static const int levels[6] = {0, 95, 135, 175, 215, 255};
    auto level = [](int value) {
        int best = 0;
        for (int i = 1; i < 6; i++) {
            if (std::abs(levels[i] - value) < std::abs(levels[best] - value)) best = i;
        }
        return best;
    };
    const int ri = level(r), gi = level(g), bi = level(b);
    const int cubeDistance = (levels[ri] - r) * (levels[ri] - r) + (levels[gi] - g) * (levels[gi] - g) + (levels[bi] - b) * (levels[bi] - b);
    const int grey = std::clamp((r + g + b) / 3, 8, 238);
    const int greyIndex = std::clamp((grey - 8 + 5) / 10, 0, 23);
    const int greyValue = 8 + greyIndex * 10;
    const int greyDistance = (greyValue - r) * (greyValue - r) + (greyValue - g) * (greyValue - g) + (greyValue - b) * (greyValue - b);
    return greyDistance < cubeDistance ? 232 + greyIndex : 16 + 36 * ri + 6 * gi + bi;
}

}  // namespace

const char* Encoder::enterSequence() {
    return "\x1b[?1049h\x1b[?25l\x1b[?7l\x1b[0m\x1b[H\x1b[2J";
}

const char* Encoder::leaveSequence() {
    return "\x1b[0m\x1b[?7h\x1b[?25h\x1b[?1049l";
}

char32_t Encoder::glyph(char32_t ch) const {
    if (ch < 0x80) return ch < 0x20 || ch == 0x7F ? U' ' : ch;
    if (isWide(ch)) return U'?';
    if (caps_.glyphs == Capabilities::Glyphs::Unicode) return ch;
    const bool console = caps_.glyphs == Capabilities::Glyphs::Console;
    switch (ch) {
    case U'─': case U'━': return console ? U'─' : U'-';
    case U'│': case U'┃': return console ? U'│' : U'|';
    case U'┌': case U'╭': return console ? U'┌' : U'+';
    case U'┐': case U'╮': return console ? U'┐' : U'+';
    case U'└': case U'╰': return console ? U'└' : U'+';
    case U'┘': case U'╯': return console ? U'┘' : U'+';
    case U'├': case U'┤': case U'┬': case U'┴': case U'┼': return console ? ch : U'+';
    case U'█': case U'▉': case U'▊': case U'▋': return console ? U'█' : U'#';
    case U'▌': case U'▍': return console ? U'▌' : U'=';
    case U'▎': case U'▏': return console ? U'▌' : U'-';
    case U'▐': case U'▀': case U'▄': return console ? ch : U'#';
    case U'░': return console ? ch : U'.';
    case U'▒': return console ? ch : U':';
    case U'▓': case U'■': return console ? ch : U'#';
    case U'●': case U'•': return U'*';
    case U'○': return U'o';
    case U'▶': case U'►': case U'▸': return U'>';
    case U'◀': case U'◄': case U'◂': return U'<';
    case U'▲': case U'↑': return U'^';
    case U'▼': case U'↓': return U'v';
    case U'←': return U'<';
    case U'→': return U'>';
    case U'·': return console ? ch : U'.';
    case U'…': return U'.';
    case U'—': case U'–': return U'-';
    case U'×': return console ? ch : U'x';
    case U'¥': return console ? ch : U'Y';
    default: break;
    }
    // The Linux console's fonts have Latin-1; anything else shows as a question mark.
    return console && ch < 0x100 ? ch : U'?';
}

void Encoder::appendColor(std::string& out, const Color& color, bool background) const {
    if (caps_.colors == Capabilities::Colors::None || color.kind == Color::Default) return;
    if (color.kind == Color::Rgb && caps_.colors == Capabilities::Colors::TrueColor) {
        out += background ? ";48;2;" : ";38;2;";
        out += std::to_string(color.r) + ";" + std::to_string(color.g) + ";" + std::to_string(color.b);
        return;
    }
    if (color.kind == Color::Rgb && caps_.colors == Capabilities::Colors::Ansi256) {
        out += (background ? ";48;5;" : ";38;5;") + std::to_string(nearest256(color.r, color.g, color.b));
        return;
    }
    const int index = color.ansi & 15;
    if (index < 8) {
        out += ";" + std::to_string((background ? 40 : 30) + index);
    } else if (caps_.boldBright) {
        out += background ? ";" + std::to_string(40 + index - 8) : ";1;" + std::to_string(30 + index - 8);
    } else {
        out += ";" + std::to_string((background ? 100 : 90) + index - 8);
    }
}

void Encoder::appendStyle(std::string& out, const Style& style) const {
    out += "\x1b[0";
    if (style.attributes & Bold) out += ";1";
    if (style.attributes & Dim) out += ";2";
    if (style.attributes & Underline) out += ";4";
    if (style.attributes & Reverse) out += ";7";
    appendColor(out, style.fg, false);
    appendColor(out, style.bg, true);
    out += "m";
}

std::string Encoder::frame(const Screen& screen) {
    std::string out;
    const bool full = previous_.columns() != screen.columns() || previous_.rows() != screen.rows();
    Style current;  // After a reset (\x1b[0m): the terminal's own colours
    if (full) out += "\x1b[0m\x1b[H\x1b[2J";
    bool styleKnown = full;
    int cursorX = -1;
    int cursorY = -1;
    for (int y = 0; y < screen.rows(); y++) {
        for (int x = 0; x < screen.columns(); x++) {
            const Cell& cell = screen.at(x, y);
            if (full ? cell == Cell() : cell == previous_.at(x, y)) continue;
            if (x != cursorX || y != cursorY) out += "\x1b[" + std::to_string(y + 1) + ";" + std::to_string(x + 1) + "H";
            if (!styleKnown || cell.style != current) {
                appendStyle(out, cell.style);
                current = cell.style;
                styleKnown = true;
            }
            appendUtf8(out, glyph(cell.ch));
            cursorX = x + 1;
            cursorY = y;
        }
    }
    if (styleKnown && current != Style()) out += "\x1b[0m";
    previous_ = screen;
    return out;
}

// ---------------------------------------------------------------------------------------------
// KeyParser

void KeyParser::feed(const char* bytes, size_t count, std::vector<Key>& keys) {
    buffer_.append(bytes, count);
    while (!buffer_.empty()) {
        const size_t used = parseOne(keys, false);
        if (used == 0) break;
        buffer_.erase(0, used);
    }
}

void KeyParser::flush(std::vector<Key>& keys) {
    while (!buffer_.empty()) {
        const size_t used = parseOne(keys, true);
        buffer_.erase(0, std::max<size_t>(used, 1));
    }
}

size_t KeyParser::parseOne(std::vector<Key>& keys, bool final) {
    const uint8_t first = uint8_t(buffer_[0]);
    if (first == 0x1B) {
        if (buffer_.size() == 1) {
            if (!final) return 0;
            keys.push_back(Key::of(Key::Escape));
            return 1;
        }
        const char second = buffer_[1];
        if (second == '[') {
            // CSI: parameters (0x30-0x3F), intermediates (0x20-0x2F), then a final byte (0x40-0x7E). The Linux
            // console's F1-F5 are "ESC [ [ A" to "E".
            if (buffer_.size() >= 3 && buffer_[2] == '[') {
                if (buffer_.size() < 4) return final ? 1 : 0;
                const char code = buffer_[3];
                if (code >= 'A' && code <= 'E') keys.push_back(Key::of(Key::Code(Key::F1 + (code - 'A'))));
                return 4;
            }
            size_t end = 2;
            while (end < buffer_.size() && uint8_t(buffer_[end]) >= 0x20 && uint8_t(buffer_[end]) <= 0x3F) end++;
            if (end >= buffer_.size()) {
                if (!final) return 0;
                keys.push_back(Key::of(Key::Escape));  // Not a sequence after all
                return 1;
            }
            const char code = buffer_[end];
            std::vector<int> params;
            {
                int value = -1;
                for (size_t i = 2; i < end; i++) {
                    const char c = buffer_[i];
                    if (c >= '0' && c <= '9') {
                        value = (value < 0 ? 0 : value * 10) + (c - '0');
                    } else if (c == ';') {
                        params.push_back(value);
                        value = -1;
                    }
                }
                params.push_back(value);
            }
            Key key;
            switch (code) {
            case 'A': key.code = Key::Up; break;
            case 'B': key.code = Key::Down; break;
            case 'C': key.code = Key::Right; break;
            case 'D': key.code = Key::Left; break;
            case 'H': key.code = Key::Home; break;
            case 'F': key.code = Key::End; break;
            case 'Z': key.code = Key::BackTab; break;
            case 'P': key.code = Key::F1; break;
            case 'Q': key.code = Key::F2; break;
            case 'R': key.code = Key::F3; break;
            case 'S': key.code = Key::F4; break;
            case '~':
                switch (params.empty() ? -1 : params[0]) {
                case 1: case 7: key.code = Key::Home; break;
                case 2: key.code = Key::Insert; break;
                case 3: key.code = Key::Delete; break;
                case 4: case 8: key.code = Key::End; break;
                case 5: key.code = Key::PageUp; break;
                case 6: key.code = Key::PageDown; break;
                case 11: case 12: case 13: case 14: case 15: key.code = Key::Code(Key::F1 + params[0] - 11); break;
                case 17: case 18: case 19: case 20: case 21: key.code = Key::Code(Key::F6 + params[0] - 17); break;
                case 23: case 24: key.code = Key::Code(Key::F11 + params[0] - 23); break;
                default: break;
                }
                break;
            default:
                break;  // Focus, mouse and other reports: not keys
            }
            if (key.code != Key::None) {
                // xterm's modifiers: 1 + (shift 1, alt 2, ctrl 4) as the second parameter.
                const int modifiers = params.size() >= 2 && params[1] > 1 ? params[1] - 1 : 0;
                key.shift = (modifiers & 1) != 0;
                key.alt = (modifiers & 2) != 0;
                key.ctrl = (modifiers & 4) != 0;
                keys.push_back(key);
            }
            return end + 1;
        }
        if (second == 'O') {
            // SS3: cursor keys in application mode, F1-F4.
            if (buffer_.size() < 3) {
                if (!final) return 0;
                Key key = Key::character(U'O');
                key.alt = true;
                keys.push_back(key);
                return 2;
            }
            Key key;
            switch (buffer_[2]) {
            case 'A': key.code = Key::Up; break;
            case 'B': key.code = Key::Down; break;
            case 'C': key.code = Key::Right; break;
            case 'D': key.code = Key::Left; break;
            case 'H': key.code = Key::Home; break;
            case 'F': key.code = Key::End; break;
            case 'M': key.code = Key::Enter; break;
            case 'P': key.code = Key::F1; break;
            case 'Q': key.code = Key::F2; break;
            case 'R': key.code = Key::F3; break;
            case 'S': key.code = Key::F4; break;
            default: break;
            }
            if (key.code != Key::None) keys.push_back(key);
            return 3;
        }
        if (uint8_t(second) == 0x1B) {
            keys.push_back(Key::of(Key::Escape));
            return 1;
        }
        // Alt with a key: Escape, then the key.
        std::vector<Key> inner;
        const std::string rest = buffer_.substr(1);
        KeyParser nested;
        nested.buffer_ = rest;
        const size_t used = nested.parseOne(inner, final);
        if (used == 0) return 0;
        for (Key& key : inner) {
            key.alt = true;
            keys.push_back(key);
        }
        return 1 + used;
    }
    if (first == '\r' || first == '\n') {
        keys.push_back(Key::of(Key::Enter));
        return first == '\r' && buffer_.size() >= 2 && buffer_[1] == '\n' ? 2 : 1;
    }
    if (first == '\t') {
        keys.push_back(Key::of(Key::Tab));
        return 1;
    }
    if (first == 0x7F || first == 0x08) {
        keys.push_back(Key::of(Key::Backspace));
        return 1;
    }
    if (first < 0x20) {
        if (first >= 1 && first <= 26) {
            Key key = Key::character(char32_t('a' + first - 1));
            key.ctrl = true;
            keys.push_back(key);
        }
        return 1;
    }
    if (first < 0x80) {
        keys.push_back(Key::character(first));
        return 1;
    }
    const size_t length = first >= 0xF0 ? 4 : first >= 0xE0 ? 3 : first >= 0xC0 ? 2 : 1;
    if (buffer_.size() < length) {
        if (!final) return 0;
        return buffer_.size();  // A broken character: dropped
    }
    size_t i = 0;
    const std::string sequence = buffer_.substr(0, length);
    keys.push_back(Key::character(decodeUtf8(sequence, i)));
    return length;
}

}  // namespace Tui
