#pragma once

#include <memory>
#include <string>
#include <vector>

#include "TextScreen.h"

namespace Tui {

// The terminal the program runs in (TerminalPosix.cpp: Linux and macOS terminals, the Linux console; TerminalWin32.cpp:
// Windows 10's console and Windows Terminal).
class Terminal {
public:
    Terminal();
    ~Terminal();  // Gives the terminal back if it is still taken
    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    // Takes the terminal over: keys come unbuffered and unechoed, the program draws on the alternate screen (what was on
    // the screen comes back afterwards), the cursor is hidden. False, with the reason, when the input or output is not
    // a terminal. A crash still gives the terminal back.
    bool open(std::string& error);
    void close();
    // What it shows, as the environment (TERM, COLORTERM, NO_COLOR, the locale) tells.
    Capabilities capabilities() const;
    void size(int& columns, int& rows) const;
    // Waits up to timeoutMs for keys, and adds those that came to `keys`.
    void readKeys(int timeoutMs, std::vector<Key>& keys);
    void write(const std::string& bytes);
    // The terminal went away (closed, the connection dropped): nothing more comes from it.
    bool lost() const;

    struct State;

private:
    std::unique_ptr<State> state_;
};

}  // namespace Tui
