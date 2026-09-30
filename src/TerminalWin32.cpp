// The terminal on Windows: the console (conhost or Windows Terminal) with VT sequences, which Windows 10 1511 and later
// understand, and its key events.

#include "Terminal.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#ifndef DISABLE_NEWLINE_AUTO_RETURN
#define DISABLE_NEWLINE_AUTO_RETURN 0x0008
#endif

namespace Tui {

namespace {

// What gives the console back, reachable from the crash handler and atexit().
HANDLE gInput = INVALID_HANDLE_VALUE;
HANDLE gOutput = INVALID_HANDLE_VALUE;
DWORD gInputMode = 0;
DWORD gOutputMode = 0;
UINT gOutputCodePage = 0;
volatile LONG gTaken = 0;

void giveBack() {
    if (InterlockedExchange(&gTaken, 0) == 0) return;
    const char* leave = Encoder::leaveSequence();
    DWORD written = 0;
    WriteFile(gOutput, leave, DWORD(strlen(leave)), &written, nullptr);
    SetConsoleMode(gInput, gInputMode);
    SetConsoleMode(gOutput, gOutputMode);
    SetConsoleOutputCP(gOutputCodePage);
}

LONG WINAPI onCrash(EXCEPTION_POINTERS* /*exception*/) {
    giveBack();
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

struct Terminal::State {
    bool open = false;
    bool lost = false;
    wchar_t highSurrogate = 0;
    LPTOP_LEVEL_EXCEPTION_FILTER previousFilter = nullptr;
};

Terminal::Terminal() : state_(new State) {}

Terminal::~Terminal() {
    close();
}

bool Terminal::open(std::string& error) {
    if (state_->open) return true;
    gInput = GetStdHandle(STD_INPUT_HANDLE);
    gOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!GetConsoleMode(gInput, &gInputMode) || !GetConsoleMode(gOutput, &gOutputMode)) {
        error = "The input or output is not a console";
        return false;
    }
    if (!SetConsoleMode(gOutput, gOutputMode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN)) {
        error = "This console cannot show the interface (it needs Windows 10 or later)";
        return false;
    }
    // Key events as they come: no line editing, echo or Ctrl+C handling, and no Quick Edit (its selection stops output).
    SetConsoleMode(gInput, ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS);
    gOutputCodePage = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
    gTaken = 1;
    static bool atExitSet = false;
    if (!atExitSet) {
        std::atexit(giveBack);
        atExitSet = true;
    }
    state_->previousFilter = SetUnhandledExceptionFilter(onCrash);
    state_->open = true;
    state_->lost = false;
    write(Encoder::enterSequence());
    return true;
}

void Terminal::close() {
    if (!state_->open) return;
    state_->open = false;
    giveBack();
    SetUnhandledExceptionFilter(state_->previousFilter);
}

Capabilities Terminal::capabilities() const {
    Capabilities caps;
    caps.glyphs = Capabilities::Glyphs::Unicode;
    const char* noColor = std::getenv("NO_COLOR");
    caps.colors = noColor != nullptr && *noColor != '\0' ? Capabilities::Colors::None : Capabilities::Colors::TrueColor;
    return caps;
}

void Terminal::size(int& columns, int& rows) const {
    CONSOLE_SCREEN_BUFFER_INFO info = {};
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
        columns = info.srWindow.Right - info.srWindow.Left + 1;
        rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    } else {
        columns = 80;
        rows = 24;
    }
}

void Terminal::readKeys(int timeoutMs, std::vector<Key>& keys) {
    State& state = *state_;
    if (WaitForSingleObject(gInput, DWORD(timeoutMs < 0 ? 0 : timeoutMs)) != WAIT_OBJECT_0) return;
    INPUT_RECORD records[64];
    DWORD count = 0;
    if (!ReadConsoleInputW(gInput, records, 64, &count)) {
        state.lost = true;
        return;
    }
    for (DWORD i = 0; i < count; i++) {
        if (records[i].EventType != KEY_EVENT || !records[i].Event.KeyEvent.bKeyDown) continue;
        const KEY_EVENT_RECORD& event = records[i].Event.KeyEvent;
        const bool ctrl = (event.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
        const bool alt = (event.dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;
        const bool shift = (event.dwControlKeyState & SHIFT_PRESSED) != 0;
        Key key;
        switch (event.wVirtualKeyCode) {
        case VK_UP: key.code = Key::Up; break;
        case VK_DOWN: key.code = Key::Down; break;
        case VK_LEFT: key.code = Key::Left; break;
        case VK_RIGHT: key.code = Key::Right; break;
        case VK_HOME: key.code = Key::Home; break;
        case VK_END: key.code = Key::End; break;
        case VK_PRIOR: key.code = Key::PageUp; break;
        case VK_NEXT: key.code = Key::PageDown; break;
        case VK_INSERT: key.code = Key::Insert; break;
        case VK_DELETE: key.code = Key::Delete; break;
        case VK_RETURN: key.code = Key::Enter; break;
        case VK_ESCAPE: key.code = Key::Escape; break;
        case VK_BACK: key.code = Key::Backspace; break;
        case VK_TAB: key.code = shift ? Key::BackTab : Key::Tab; break;
        default:
            if (event.wVirtualKeyCode >= VK_F1 && event.wVirtualKeyCode <= VK_F12) {
                key.code = Key::Code(Key::F1 + (event.wVirtualKeyCode - VK_F1));
                break;
            }
            {
                const wchar_t c = event.uChar.UnicodeChar;
                if (c == 0) break;  // A modifier alone
                if (c >= 0xD800 && c <= 0xDBFF) {
                    state.highSurrogate = c;
                    break;
                }
                char32_t ch = c;
                if (c >= 0xDC00 && c <= 0xDFFF && state.highSurrogate != 0) {
                    ch = 0x10000 + ((char32_t(state.highSurrogate) - 0xD800) << 10) + (char32_t(c) - 0xDC00);
                }
                state.highSurrogate = 0;
                key.code = Key::Char;
                if (ctrl && !alt && ch >= 1 && ch <= 26) {
                    key.ch = char32_t('a' + ch - 1);
                    key.ctrl = true;
                } else {
                    key.ch = ch;
                    // AltGr (Ctrl+Alt) makes characters on European layouts: they are characters, not shortcuts.
                    key.alt = alt && !ctrl;
                }
            }
            break;
        }
        if (key.code == Key::None) continue;
        if (key.code != Key::Char) {
            key.ctrl = ctrl;
            key.alt = alt;
            key.shift = shift;
        }
        for (WORD repeat = 0; repeat < std::max<WORD>(event.wRepeatCount, 1); repeat++) keys.push_back(key);
    }
}

void Terminal::write(const std::string& bytes) {
    size_t done = 0;
    while (done < bytes.size()) {
        DWORD written = 0;
        if (!WriteFile(gOutput, bytes.data() + done, DWORD(bytes.size() - done), &written, nullptr) || written == 0) {
            state_->lost = true;
            return;
        }
        done += written;
    }
}

bool Terminal::lost() const {
    return state_->lost;
}

}  // namespace Tui
