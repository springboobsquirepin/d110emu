#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "HostedApp.h"

struct SdlHostWindow {
    const char* name;   // The application's name ("D110Emu"), as macOS's application menu shows it
    const char* title;
    const char* appId;  // For the desktop (Wayland's app ID, X11's class), e.g. "d110emu"
    int width;          // At 100% scaling; grown for high-DPI displays and shrunk to fit smaller screens
    int height;
    void (*applyStyle)();  // Dear ImGui style, before the scaling
};

struct SdlHostOptions {
    std::vector<std::filesystem::path> files;  // Opened after start (the command line's)

    // For tests: input at given times (seconds after start), and after `seconds` the window's contents saved to
    // `screenshot` (a BMP), after which the host quits.
    struct TestEvent {
        enum Kind { Click, Key, Drop, Wheel };
        Kind kind = Click;
        double time = 0.0;
        float x = 0.0f;    // Click and Wheel, in window coordinates: the pointer arrives at `time`, the button goes down
        float y = 0.0f;    // (or the wheel turns) 0.1 s later
        float amount = 0.0f;  // Wheel: how far it turns (1: a notch away from the user; trackpads send fractions)
        int scancode = 0;  // Key: an SDL scancode, pressed for 0.2 s
        std::string path;  // Drop: a file dropped on the window
    };
    std::vector<TestEvent> testEvents;
    std::filesystem::path screenshot;
    double seconds = 0.0;
    int width = 0;  // For tests: the window's size, whatever the display's (0: as the display allows)
    int height = 0;
};

// The test options every SDL window takes (not in their usage): --screenshot FILE.bmp --seconds S (the window's contents
// after S seconds, then quit), --window WxH, --click X,Y,T, --wheel X,Y,AMOUNT,T (1 is a notch away from the user),
// --key SCANCODE,T (an SDL scancode, held 0.2 s), --drop FILE,T. True when `arg` is one (`usedValue`: it took `value`).
bool parseSdlHostTestOption(const std::string& arg, const char* value, SdlHostOptions& options, bool& usedValue);

// Runs an SDL 3 window (Wayland or X11 on Linux, Cocoa on macOS) drawn with SDL's renderer through Dear ImGui's
// backends (from Dear ImGui's example_sdl3_sdlrenderer3): `start` once Dear ImGui is ready, then `app` every frame until
// the window closes or the app quits, then `stop`. While it runs, Platform's dialogs and key names are SDL's
// (Platform::setToolkit).
// Returns the process exit code.
int runSdlHost(const SdlHostWindow& window, HostedApp& app, const SdlHostOptions& options, const std::function<void()>& start,
               const std::function<void()>& stop);
