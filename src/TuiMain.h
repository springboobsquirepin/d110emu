#pragma once

// What the terminal programs' entry points share (MainTui.cpp, MainTranslatorTui.cpp): the command line as UTF-8, quitting
// on signals, and the standard error kept off the interface while it shows.

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace Tui {

// The command line after the program's name, as UTF-8 (Windows passes it to main() in the ANSI code page).
inline std::vector<std::string> commandLineArguments(int argc, char** argv) {
    std::vector<std::string> args;
#ifdef _WIN32
    (void)argc;
    (void)argv;
    int count = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);
    for (int i = 1; wide != nullptr && i < count; i++) {
        const int length = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
        std::string arg(size_t(std::max(length - 1, 0)), '\0');
        if (length > 1) WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, &arg[0], length, nullptr, nullptr);
        args.push_back(arg);
    }
    if (wide != nullptr) LocalFree(wide);
#else
    for (int i = 1; i < argc; i++) args.push_back(argv[i]);
#endif
    return args;
}

inline std::atomic<bool> gQuitSignalled{false};

#ifdef _WIN32
inline BOOL WINAPI onQuitEvent(DWORD /*event*/) {
    gQuitSignalled = true;  // Ctrl+C without the interface, Ctrl+Break, the console closing
    return TRUE;
}
#else
inline void onQuitSignal(int /*signal*/) {
    gQuitSignalled = true;
}
#endif

// From now on, what asks a program to quit sets quitSignalled(): Ctrl+C without the interface (with it, Ctrl+C is a
// key), SIGTERM from a service manager, SIGHUP when the terminal (an SSH session) goes; on Windows Ctrl+Break and the
// console closing. A pipe that closes (SIGPIPE) is not one.
inline void catchQuitSignals() {
#ifdef _WIN32
    SetConsoleCtrlHandler(onQuitEvent, TRUE);
#else
    std::signal(SIGINT, onQuitSignal);
    std::signal(SIGTERM, onQuitSignal);
    std::signal(SIGHUP, onQuitSignal);
    std::signal(SIGPIPE, SIG_IGN);
#endif
}

inline bool quitSignalled() {
    return gQuitSignalled;
}

// Stray output on the standard error (the ALSA library reports problems there) would write over the interface: while
// it shows, the standard error goes into a pipe whose lines the program puts in its log.
class ErrorCapture {
public:
    ~ErrorCapture() { stop(); }
    void start() {
#ifndef _WIN32
        int fds[2];
        if (pipe(fds) != 0) return;
        std::fflush(stderr);
        saved_ = dup(STDERR_FILENO);
        dup2(fds[1], STDERR_FILENO);
        ::close(fds[1]);
        fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
        read_ = fds[0];
#endif
    }
    std::vector<std::string> take() {
        std::vector<std::string> lines;
#ifndef _WIN32
        if (read_ < 0) return lines;
        std::fflush(stderr);
        char buffer[4096];
        ssize_t count;
        while ((count = ::read(read_, buffer, sizeof(buffer))) > 0) pending_.append(buffer, size_t(count));
        size_t end;
        while ((end = pending_.find('\n')) != std::string::npos) {
            if (end > 0) lines.push_back(pending_.substr(0, end));
            pending_.erase(0, end + 1);
        }
#endif
        return lines;
    }
    void stop() {
#ifndef _WIN32
        if (saved_ < 0) return;
        std::fflush(stderr);
        dup2(saved_, STDERR_FILENO);
        ::close(saved_);
        saved_ = -1;
        ::close(read_);
        read_ = -1;
#endif
    }

private:
#ifndef _WIN32
    int saved_ = -1;
    int read_ = -1;
    std::string pending_;
#endif
};

}  // namespace Tui
