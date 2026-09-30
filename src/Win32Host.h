#pragma once

#include <windows.h>

#include <filesystem>
#include <functional>

#include "HostedApp.h"

struct HostWindow {
    const wchar_t* className;
    const wchar_t* title;
    int width;   // At 100% scaling; shrunk to fit smaller or high-DPI screens
    int height;
    void (*applyStyle)();  // Dear ImGui style, before the DPI scaling
};

// Segoe UI (else Dear ImGui's own font) at the apps' 16-pixel base size, into the current Dear ImGui context.
void loadUiFonts();

// Runs a Win32 window drawn with Direct3D 11 through Dear ImGui's backends (from Dear ImGui's
// example_win32_directx11): `start` once Dear ImGui is ready, then `app` every frame until the window closes or the
// app quits, then `stop`. Returns the process exit code.
int runHost(HINSTANCE instance, const HostWindow& window, HostedApp& app, const std::function<void()>& start,
            const std::function<void()>& stop);
