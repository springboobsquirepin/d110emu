#pragma once

#include <filesystem>
#include <string>

#include "TranslatorCore.h"

// MT32Translator in a window: TranslatorCore with a Dear ImGui interface. Like App, the UI is platform-independent;
// MainTranslatorWin32.cpp (Windows) and MainTranslatorSdl.cpp (Linux, macOS) host it.
class TranslatorApp : public TranslatorCore {
public:
    void frame();  // Builds the UI; call between ImGui::NewFrame() and ImGui::Render()
    // A dropped file: SysEx (.syx, .dat) goes to the unit through the pipe, a MIDI file is offered for translation.
    void openFile(const std::filesystem::path& path);
    void showPresetsTab() { rightTab_ = 3; }

private:
    void translateFile(const std::filesystem::path& path);  // Asks where the translated copy goes

    void drawMenu();
    void drawPorts();
    void drawUnit();
    void drawPresets();
    void drawCache();
    void drawTiming();
    void drawPresetEditor();
    void drawActions();
    void drawMonitor();

    int rightTab_ = 0;  // A tab to show on the next frame: 2 monitor, 3 presets, 0 none
};
