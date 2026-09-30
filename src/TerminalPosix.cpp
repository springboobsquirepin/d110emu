// The terminal on Linux and macOS: termios for unbuffered keys, VT sequences for the rest.

#include "Terminal.h"

#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace Tui {

namespace {

// What gives the terminal back, reachable from signal handlers and atexit().
termios gSaved;
volatile sig_atomic_t gTaken = 0;
const int kFatalSignals[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT};

void giveBack() {
    if (!gTaken) return;
    gTaken = 0;
    const char* leave = Encoder::leaveSequence();
    size_t length = 0;
    while (leave[length] != '\0') length++;
    while (::write(STDOUT_FILENO, leave, length) < 0 && errno == EINTR) {
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &gSaved);
}

void onFatalSignal(int signal) {
    giveBack();
    ::signal(signal, SIG_DFL);
    raise(signal);
}

// Waits up to timeoutMs for the terminal to have input (or to take output); > 0 when it has, 0 when the time ran out.
// select(), not poll(): macOS's poll() takes no terminal devices (it answers POLLNVAL at once).
int waitFor(int fd, bool output, int timeoutMs) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    timeval timeout = {};
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;
    return select(fd + 1, output ? nullptr : &set, output ? &set : nullptr, nullptr, &timeout);
}

std::string lowerEnv(const char* name) {
    const char* value = std::getenv(name);
    std::string text = value != nullptr ? value : "";
    std::transform(text.begin(), text.end(), text.begin(), [](char c) { return char(std::tolower(uint8_t(c))); });
    return text;
}

}  // namespace

struct Terminal::State {
    KeyParser parser;
    bool open = false;
    bool lost = false;
    struct sigaction previous[sizeof(kFatalSignals) / sizeof(kFatalSignals[0])];
};

Terminal::Terminal() : state_(new State) {}

Terminal::~Terminal() {
    close();
}

bool Terminal::open(std::string& error) {
    if (state_->open) return true;
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        error = "The input or output is not a terminal";
        return false;
    }
    if (tcgetattr(STDIN_FILENO, &gSaved) != 0) {
        error = std::string("Cannot read the terminal's settings: ") + std::strerror(errno);
        return false;
    }
    termios raw = gSaved;
    raw.c_iflag &= tcflag_t(~(BRKINT | ICRNL | INPCK | ISTRIP | IXON));
    raw.c_cflag |= CS8;
    raw.c_lflag &= tcflag_t(~(ECHO | ICANON | IEXTEN | ISIG));  // Ctrl+C comes as a key
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
        error = std::string("Cannot set up the terminal: ") + std::strerror(errno);
        return false;
    }
    gTaken = 1;
    static bool atExitSet = false;
    if (!atExitSet) {
        std::atexit(giveBack);
        atExitSet = true;
    }
    for (size_t i = 0; i < sizeof(kFatalSignals) / sizeof(kFatalSignals[0]); i++) {
        struct sigaction action = {};
        action.sa_handler = onFatalSignal;
        sigemptyset(&action.sa_mask);
        sigaction(kFatalSignals[i], &action, &state_->previous[i]);
    }
    state_->open = true;
    state_->lost = false;
    write(Encoder::enterSequence());
    return true;
}

void Terminal::close() {
    if (!state_->open) return;
    state_->open = false;
    giveBack();
    for (size_t i = 0; i < sizeof(kFatalSignals) / sizeof(kFatalSignals[0]); i++) sigaction(kFatalSignals[i], &state_->previous[i], nullptr);
}

Capabilities Terminal::capabilities() const {
    Capabilities caps;
    const std::string term = lowerEnv("TERM");
    const std::string colorTerm = lowerEnv("COLORTERM");
    // The locale's character set: the first of LC_ALL, LC_CTYPE and LANG that is set.
    std::string locale = lowerEnv("LC_ALL");
    if (locale.empty()) locale = lowerEnv("LC_CTYPE");
    if (locale.empty()) locale = lowerEnv("LANG");
    const bool utf8 = locale.find("utf-8") != std::string::npos || locale.find("utf8") != std::string::npos;
    caps.glyphs = term == "linux" ? Capabilities::Glyphs::Console : utf8 ? Capabilities::Glyphs::Unicode : Capabilities::Glyphs::Ascii;
    const char* noColor = std::getenv("NO_COLOR");
    if ((noColor != nullptr && *noColor != '\0') || term.empty() || term == "dumb") {
        caps.colors = Capabilities::Colors::None;
    } else if (colorTerm == "truecolor" || colorTerm == "24bit") {
        caps.colors = Capabilities::Colors::TrueColor;
    } else if (term.find("256col") != std::string::npos) {
        caps.colors = Capabilities::Colors::Ansi256;
    } else {
        caps.colors = Capabilities::Colors::Ansi16;
        caps.boldBright = term == "linux";
    }
    return caps;
}

void Terminal::size(int& columns, int& rows) const {
    winsize window = {};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &window) == 0 && window.ws_col > 0 && window.ws_row > 0) {
        columns = window.ws_col;
        rows = window.ws_row;
    } else {
        columns = 80;
        rows = 24;
    }
}

void Terminal::readKeys(int timeoutMs, std::vector<Key>& keys) {
    State& state = *state_;
    int wait = timeoutMs;
    for (;;) {
        const int ready = waitFor(STDIN_FILENO, false, wait);
        if (ready > 0) {
            // Keys, or the end: a terminal that went away reads as the end of the input, or as an error.
            char buffer[512];
            const ssize_t count = ::read(STDIN_FILENO, buffer, sizeof(buffer));
            if (count > 0) {
                state.parser.feed(buffer, size_t(count), keys);
                if (!state.parser.pending()) return;
                wait = 30;  // The rest of a sequence comes at once; an Escape key alone does not
                continue;
            }
            if (count == 0 || (errno != EINTR && errno != EAGAIN)) state.lost = true;
        } else if (ready < 0 && errno == EBADF) {
            state.lost = true;
        }
        if (state.parser.pending()) state.parser.flush(keys);
        return;
    }
}

void Terminal::write(const std::string& bytes) {
    size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t count = ::write(STDOUT_FILENO, bytes.data() + done, bytes.size() - done);
        if (count > 0) {
            done += size_t(count);
        } else if (count < 0 && errno == EAGAIN) {
            waitFor(STDOUT_FILENO, true, 100);
        } else if (count < 0 && errno != EINTR) {
            state_->lost = true;
            return;
        }
    }
}

bool Terminal::lost() const {
    return state_->lost;
}

}  // namespace Tui
