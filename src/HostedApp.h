#pragma once

#include <filesystem>

// What a Dear ImGui application gives the window host (Win32Host.cpp on Windows, SdlHost.cpp elsewhere).
class HostedApp {
public:
    virtual ~HostedApp() = default;
    virtual void frame() = 0;  // Builds the UI; called between ImGui::NewFrame() and ImGui::Render()
    virtual bool quitRequested() const { return false; }
    // Physical key presses (PC scancodes, set 1), e.g. for a computer-keyboard piano.
    virtual void onKey(int scancode, bool down) {
        (void)scancode;
        (void)down;
    }
    virtual void clearKeys() {}
    // A file dropped on the window or named on the command line.
    virtual void openFile(const std::filesystem::path& path) { (void)path; }
    // The window's size in percent (View > Window size), on top of the display's scaling: the host sizes the window and
    // its text by it when the app has started, and follows changes.
    virtual int uiZoom() const { return 100; }
};
