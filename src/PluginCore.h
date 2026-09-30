#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "App.h"
#include "Settings.h"
#include "SynthEngine.h"

// The plugin's window, per platform (PluginEditorWin32.cpp); createPluginEditor() returns nullptr where there is none.
class PluginEditor {
public:
    virtual ~PluginEditor() = default;
    virtual bool open(void* parentWindow) = 0;  // A child of the host's window
    virtual void close() = 0;
    virtual void size(int& width, int& height) = 0;  // In pixels
    // Keys a host passes on instead of letting the window have them (VST2 effEditKeyDown/Up, VST3 IPlugView::onKeyDown/Up):
    // `virtualKey` in the VST numbering (VST2's VKEY_ values and VST3's VirtualKeyCodes are the same); true if used.
    virtual bool key(int character, int virtualKey, bool shift, bool control, bool alt, bool down) = 0;
    virtual void drawViewMenu() = 0;  // The editor's own View menu items, inside the App's frame
    // How the editor asks the host to resize its window (true if it did); VST3 hosts then call onSize (setSize).
    virtual void setHostResize(std::function<bool(int, int)> resize) = 0;
    virtual void setSize(int width, int height) = 0;  // The host resized the window
    virtual void setScale(float scale) = 0;           // The host's scaling for the window (VST3); 0: the window's DPI
    // Linux, where the host's event loop runs the window (VST3's IRunLoop): the descriptor on which the window system's
    // events arrive, what to do when it has some, and a frame, about 60 times a second. Windows' editor has its own.
    virtual int eventDescriptor() { return -1; }
    virtual void processEvents() {}
    virtual void idle() {}
};

class PluginCore;
std::unique_ptr<PluginEditor> createPluginEditor(PluginCore& core);

// D110Emu inside a plugin host, whatever the plugin format (VstPlugin.cpp: VST2, Vst3Plugin.cpp: VST3, AuPlugin.mm: the
// Audio Unit): the App (the
// whole interface, the engine, the settings) with the host's MIDI in and its audio out. Its settings and battery-backed
// memory live in the host's project (saveState/loadState); the ROM choice, the LCD colours and the editor's size are also
// kept in d110emu-vst.ini for new instances.
//
// Threads: the host's audio thread calls queueMidi, queueSysex and render; its UI thread everything else, and the editor
// draws the App from there too. appMutex_ guards the App; it is recursive because the App's modal dialogs run a message
// loop in which the host may call in again. audioMutex_ only guards the engine the audio thread renders with.
enum class PluginFormat { Vst2, Vst3, Au };

class PluginCore {
public:
    // Every format gives each part an output of its own besides the mix (render()'s part outputs), which the App's Output
    // menus put the parts on, and the D-110's MULTI outputs 1-6: six mono outputs, or three stereo pairs (1+2, 3+4, 5+6).
    static constexpr int kMultiOutputs = 2 * SynthEngine::kMultiPairs;

    explicit PluginCore(PluginFormat format);
    ~PluginCore();
    PluginCore(const PluginCore&) = delete;
    PluginCore& operator=(const PluginCore&) = delete;

    // The host's setup, on its UI thread.
    void setSampleRate(double sampleRate);  // Restarts the synth, keeping its memory
    void activate();                        // Builds the App: not at construction, so that a host's scan loads no ROMs
    std::vector<uint8_t> saveState();
    bool loadState(const uint8_t* data, size_t size);
    // VST3: the outputs the host takes (bit n = part number n's own, bit 16 + n = MULTI n + 1), which the App shows (a part
    // on an output the host has off is not heard). They do not move the parts: the App's Output menus do.
    void setHostOutputs(uint32_t mask);

    // The host's MIDI for the next render(), at frames into its block: from the audio thread in VST2 and VST3, from any
    // thread in the Audio Unit (hosts may send it from others).
    void queueMidi(uint32_t frame, uint8_t status, uint8_t data1, uint8_t data2);
    void queueSysex(uint32_t frame, const uint8_t* data, size_t length);  // F0 ... F7; either is added where missing
    // `frames` frames of the mix into left and right, of each part into partLeft and partRight[part number]
    // (SynthEngine::kPartOutputs entries, null for parts not wanted; null arrays for none) and of MULTI 1-6 into
    // multi[0-5] (kMultiOutputs mono channels, likewise), added to what is there with `accumulate`. A render of no
    // frames keeps the MIDI for the next one.
    void render(float* left, float* right, float* const* partLeft, float* const* partRight, float* const* multi, uint32_t frames,
                bool accumulate);

    // The editor, on the UI thread. drawFrame() draws the App (between ImGui::NewFrame() and ImGui::Render()), and
    // afterFrame() applies what had to wait for the frame to end (a project state the host loaded during a dialog).
    void setEditor(PluginEditor* editor);  // Its View menu items appear in the App's
    void drawFrame();
    void afterFrame();
    // Runs `action` on the App, which is created if needed.
    template <typename Action>
    void withApp(Action&& action) {
        std::lock_guard<std::recursive_mutex> lock(appMutex_);
        ensureApp();
        action(*app_);
    }
    // The editor's size in percent of the standalone window's (at the screen's scaling), kept for new instances.
    int editorZoom();
    void setEditorZoom(int percent);
    // MULTI 1-6 as three stereo pairs rather than six mono outputs: this instance's, fixed when it is made (hosts set up
    // a plugin's outputs when they add it), from the computer's choice; always for the Audio Unit. The format's outputs
    // follow it; render()'s `multi` channels are the same either way.
    bool multiOutputPairs() const { return multiPairs_; }

    // The project state's format: "D11V", a version, then sections of a tag, a length and the data.
    static std::vector<uint8_t> encodeState(const std::string& settings, const std::vector<uint8_t>& memory);
    static bool decodeState(const uint8_t* data, size_t size, std::string& settings, std::vector<uint8_t>& memory);

private:
    // With appMutex_ held.
    void ensureApp();
    std::unique_ptr<App> createApp(const std::string& settingsText, std::vector<uint8_t> memory, uint64_t generation);
    void installApp(std::unique_ptr<App> app, uint64_t generation);
    // A project's settings with this computer's ROMs when the project's ROM folder is not here.
    std::string localSettings(const std::string& projectSettings);
    void loadGlobalSettings();
    void saveGlobalSettings(const Settings& appSettings);

    const PluginFormat format_;
    bool multiPairs_ = false;         // This instance's MULTI layout
    bool multiPairsForNew_ = false;   // The computer's choice (d110emu-vst.ini), for instances made from now on
    PluginEditor* editor_ = nullptr;
    std::atomic<uint32_t> hostOutputs_{0};

    std::recursive_mutex appMutex_;
    std::unique_ptr<App> app_;
    uint64_t appGeneration_ = 0;  // app_'s; an App being built or replaced saves no global settings
    uint64_t lastGeneration_ = 0;
    int frameDepth_ = 0;          // App frames under way (a modal dialog in one lets the host call in)
    std::vector<uint8_t> pendingState_;  // A state the host loaded during a frame, applied after it
    bool statePending_ = false;
    Settings global_;             // d110emu-vst.ini
    double sampleRate_ = 44100.0;

    std::mutex audioMutex_;
    SynthEngine* engine_ = nullptr;  // app_'s, for the audio thread
    std::mutex queueMutex_;          // Guards the MIDI queued for the next block, which render() takes in one swap
    std::vector<SynthEngine::TimedMidi> incoming_;
    std::vector<uint8_t> incomingSysex_;
    std::vector<SynthEngine::TimedMidi> events_;  // The block's MIDI, being rendered
    std::vector<uint8_t> sysex_;
    std::vector<float> buffer_;  // Interleaved stereo: the mix
    std::vector<std::vector<float>> partBuffers_;  // Interleaved stereo: each part's output (part outputs)
    std::vector<std::vector<float>> multiBuffers_;  // Interleaved stereo: the MULTI pairs (1 and 2, 3 and 4, 5 and 6)
};
