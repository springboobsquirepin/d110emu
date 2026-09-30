// An SDL 3 window drawn with SDL's renderer through Dear ImGui's backends, for the Dear ImGui apps where there is no
// Win32Host (Linux: Wayland or X11; macOS: Cocoa). Adapted from Dear ImGui's example_sdl3_sdlrenderer3. SDL's renderer
// draws with what the machine has (Metal on a Mac; OpenGL, OpenGL ES, Vulkan, else software), which suits a Raspberry Pi
// as well as a desktop.

#include "SdlHost.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include "Platform.h"
#include "UiStyle.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

namespace {

// The main keyboard block's PC scancodes (set 1) by SDL scancode (the key's USB usage): the computer-keyboard piano
// goes by the keys' positions, and Win32Host passes these numbers. The other keys never play notes.
struct ScancodePair {
    SDL_Scancode sdl;
    int pc;
};
const ScancodePair kScancodes[] = {
    {SDL_SCANCODE_ESCAPE, 0x01},       {SDL_SCANCODE_1, 0x02},          {SDL_SCANCODE_2, 0x03},
    {SDL_SCANCODE_3, 0x04},            {SDL_SCANCODE_4, 0x05},          {SDL_SCANCODE_5, 0x06},
    {SDL_SCANCODE_6, 0x07},            {SDL_SCANCODE_7, 0x08},          {SDL_SCANCODE_8, 0x09},
    {SDL_SCANCODE_9, 0x0A},            {SDL_SCANCODE_0, 0x0B},          {SDL_SCANCODE_MINUS, 0x0C},
    {SDL_SCANCODE_EQUALS, 0x0D},       {SDL_SCANCODE_BACKSPACE, 0x0E},  {SDL_SCANCODE_TAB, 0x0F},
    {SDL_SCANCODE_Q, 0x10},            {SDL_SCANCODE_W, 0x11},          {SDL_SCANCODE_E, 0x12},
    {SDL_SCANCODE_R, 0x13},            {SDL_SCANCODE_T, 0x14},          {SDL_SCANCODE_Y, 0x15},
    {SDL_SCANCODE_U, 0x16},            {SDL_SCANCODE_I, 0x17},          {SDL_SCANCODE_O, 0x18},
    {SDL_SCANCODE_P, 0x19},            {SDL_SCANCODE_LEFTBRACKET, 0x1A}, {SDL_SCANCODE_RIGHTBRACKET, 0x1B},
    {SDL_SCANCODE_RETURN, 0x1C},       {SDL_SCANCODE_LCTRL, 0x1D},      {SDL_SCANCODE_A, 0x1E},
    {SDL_SCANCODE_S, 0x1F},            {SDL_SCANCODE_D, 0x20},          {SDL_SCANCODE_F, 0x21},
    {SDL_SCANCODE_G, 0x22},            {SDL_SCANCODE_H, 0x23},          {SDL_SCANCODE_J, 0x24},
    {SDL_SCANCODE_K, 0x25},            {SDL_SCANCODE_L, 0x26},          {SDL_SCANCODE_SEMICOLON, 0x27},
    {SDL_SCANCODE_APOSTROPHE, 0x28},   {SDL_SCANCODE_GRAVE, 0x29},      {SDL_SCANCODE_LSHIFT, 0x2A},
    {SDL_SCANCODE_BACKSLASH, 0x2B},    {SDL_SCANCODE_NONUSHASH, 0x2B},  {SDL_SCANCODE_Z, 0x2C},
    {SDL_SCANCODE_X, 0x2D},            {SDL_SCANCODE_C, 0x2E},          {SDL_SCANCODE_V, 0x2F},
    {SDL_SCANCODE_B, 0x30},            {SDL_SCANCODE_N, 0x31},          {SDL_SCANCODE_M, 0x32},
    {SDL_SCANCODE_COMMA, 0x33},        {SDL_SCANCODE_PERIOD, 0x34},     {SDL_SCANCODE_SLASH, 0x35},
    {SDL_SCANCODE_RSHIFT, 0x36},       {SDL_SCANCODE_LALT, 0x38},       {SDL_SCANCODE_SPACE, 0x39},
    {SDL_SCANCODE_CAPSLOCK, 0x3A},     {SDL_SCANCODE_NONUSBACKSLASH, 0x56},
};

int pcScancode(SDL_Scancode code) {
    for (const ScancodePair& pair : kScancodes) {
        if (pair.sdl == code) return pair.pc;
    }
    return 0;
}

SDL_Scancode sdlScancode(int pc) {
    for (const ScancodePair& pair : kScancodes) {
        if (pair.pc == pc) return pair.sdl;
    }
    return SDL_SCANCODE_UNKNOWN;
}

// The dialogs' filters by kind of file, as PlatformWin32.cpp's, with the extension a saved file gets when it has none.
struct Filters {
    const SDL_DialogFileFilter* list;
    int count;
    const char* extension;
};

Filters dialogFilters(Platform::FileKind kind) {
    static const SDL_DialogFileFilter midi[] = {{"MIDI files (*.mid, *.midi, *.smf, *.rmi)", "mid;midi;smf;rmi"}, {"All files", "*"}};
    static const SDL_DialogFileFilter midiOrSysex[] = {{"MIDI and SysEx files (*.mid, *.midi, *.syx, *.dat)", "mid;midi;syx;dat"},
                                                       {"All files", "*"}};
    static const SDL_DialogFileFilter rom[] = {{"ROM images (*.rom, *.bin)", "rom;bin"}, {"All files", "*"}};
    static const SDL_DialogFileFilter sysex[] = {{"SysEx files (*.syx)", "syx"}, {"All files", "*"}};
    switch (kind) {
    case Platform::FileKind::Midi: return {midi, 2, "mid"};
    case Platform::FileKind::MidiOrSysex: return {midiOrSysex, 2, "syx"};
    case Platform::FileKind::Rom: return {rom, 2, "rom"};
    case Platform::FileKind::Sysex:
    default: return {sysex, 2, "syx"};
    }
}

// Platform's dialogs and key names through SDL: its file dialogs (the desktop's own through xdg-desktop-portal, else
// zenity; macOS's own), shown modally as the Windows ones are, and its keyboard layout.
class SdlToolkit : public Platform::Toolkit {
public:
    explicit SdlToolkit(SDL_Window* window) : window_(window) {}

    bool openFileDialog(Platform::FileKind kind, std::filesystem::path& result) override {
        const Filters filters = dialogFilters(kind);
        Wait wait;
        SDL_ShowOpenFileDialog(&SdlToolkit::onChosen, &wait, window_, filters.list, filters.count, nullptr, false);
        return finish(wait, result);
    }

    bool saveFileDialog(Platform::FileKind kind, const std::string& defaultName, std::filesystem::path& result) override {
        const Filters filters = dialogFilters(kind);
        Wait wait;
        const char* home = SDL_GetUserFolder(SDL_FOLDER_HOME);
        const std::string location = home != nullptr ? std::string(home) + defaultName : defaultName;
        SDL_ShowSaveFileDialog(&SdlToolkit::onChosen, &wait, window_, filters.list, filters.count, location.c_str());
        if (!finish(wait, result)) return false;
        if (!result.has_extension()) result += std::string(".") + filters.extension;
        return true;
    }

    bool pickFolderDialog(const std::filesystem::path& initialFolder, std::filesystem::path& result) override {
        Wait wait;
        const std::string location = Platform::toUtf8(initialFolder);
        SDL_ShowOpenFolderDialog(&SdlToolkit::onChosen, &wait, window_, location.empty() ? nullptr : location.c_str(), false);
        return finish(wait, result);
    }

    std::string takeDialogError() override {
        std::string error;
        error.swap(error_);
        return error;
    }

    std::string keyName(int scancode) override {
        const SDL_Scancode code = sdlScancode(scancode);
        if (code == SDL_SCANCODE_UNKNOWN) return std::string();
        const char* name = SDL_GetKeyName(SDL_GetKeyFromScancode(code, SDL_KMOD_NONE, false));
        return name != nullptr ? std::string(name) : std::string();
    }

private:
    struct Wait {
        std::atomic<bool> done{false};
        std::mutex mutex;  // The callback may come on another thread
        bool chosen = false;
        std::string path;
        std::string error;
    };

    static void SDLCALL onChosen(void* user, const char* const* files, int /*filter*/) {
        Wait& wait = *static_cast<Wait*>(user);
        {
            std::lock_guard<std::mutex> lock(wait.mutex);
            if (files == nullptr) {
                wait.error = SDL_GetError();
            } else if (files[0] != nullptr) {
                wait.chosen = true;
                wait.path = files[0];
            }
        }
        wait.done = true;
    }

    // Waits for the dialog, modally as the Windows dialogs do: the window's input is dropped meanwhile, and what the
    // host must still see (quitting, the window's own events) is kept for after it.
    bool finish(Wait& wait, std::filesystem::path& result) {
        std::vector<SDL_Event> kept;
        while (!wait.done) {
            SDL_Event event;
            if (!SDL_WaitEventTimeout(&event, 20)) continue;
            if (event.type == SDL_EVENT_QUIT || (event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST)) {
                kept.push_back(event);
            }
        }
        for (SDL_Event& event : kept) SDL_PushEvent(&event);
        std::lock_guard<std::mutex> lock(wait.mutex);
        if (wait.error.find("unsupported") != std::string::npos) {
            // Neither a desktop portal nor zenity: SDL has no dialog to show.
            error_ = "this desktop has no file dialog for programs: sudo apt install zenity gives it one";
        } else if (!wait.error.empty()) {
            error_ = wait.error;
        }
        if (!wait.chosen) return false;
        result = Platform::fromUtf8(wait.path);
        return true;
    }

    SDL_Window* window_;
    std::string error_;
};

// The UI's scale in window coordinates: the display's scale, less what the window's pixel density already gives (Wayland
// and macOS count a window in points, X11 in pixels).
float uiScale(SDL_Window* window) {
    const float density = SDL_GetWindowPixelDensity(window);
    const float display = SDL_GetWindowDisplayScale(window);
    return density > 0.0f && display > 0.0f ? std::clamp(display / density, 0.5f, 4.0f) : 1.0f;
}

void setUpStyle(const SdlHostWindow& window, float scale) {
    ImGui::GetStyle() = ImGuiStyle();
    window.applyStyle();
    ImGuiStyle& style = ImGui::GetStyle();
    style.FontSizeBase = 16.0f;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
}

}  // namespace

bool parseSdlHostTestOption(const std::string& arg, const char* value, SdlHostOptions& options, bool& usedValue) {
    usedValue = false;
    if (value == nullptr) return false;
    usedValue = true;
    SdlHostOptions::TestEvent event;
    const std::string text = value;
    if (arg == "--screenshot") {
        options.screenshot = Platform::fromUtf8(text);
    } else if (arg == "--window" && std::sscanf(value, "%dx%d", &options.width, &options.height) == 2) {
    } else if (arg == "--seconds") {
        options.seconds = std::atof(value);
    } else if (arg == "--click" && std::sscanf(value, "%f,%f,%lf", &event.x, &event.y, &event.time) == 3) {
        event.kind = SdlHostOptions::TestEvent::Click;
        options.testEvents.push_back(event);
    } else if (arg == "--wheel" && std::sscanf(value, "%f,%f,%f,%lf", &event.x, &event.y, &event.amount, &event.time) == 4) {
        event.kind = SdlHostOptions::TestEvent::Wheel;
        options.testEvents.push_back(event);
    } else if (arg == "--key" && std::sscanf(value, "%d,%lf", &event.scancode, &event.time) == 2) {
        event.kind = SdlHostOptions::TestEvent::Key;
        options.testEvents.push_back(event);
    } else if (arg == "--drop" && text.rfind(',') != std::string::npos) {
        event.kind = SdlHostOptions::TestEvent::Drop;
        event.path = text.substr(0, text.rfind(','));
        event.time = std::atof(text.substr(text.rfind(',') + 1).c_str());
        options.testEvents.push_back(event);
    } else {
        usedValue = false;
        return false;
    }
    return true;
}

int runSdlHost(const SdlHostWindow& window, HostedApp& app, const SdlHostOptions& options, const std::function<void()>& start,
               const std::function<void()>& stop) {
    SDL_SetAppMetadata(window.name, nullptr, window.appId);
    SDL_SetHint(SDL_HINT_APP_ID, window.appId);
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "%s: cannot start SDL's video: %s\n", window.appId, SDL_GetError());
        return 1;
    }
    SDL_Window* sdlWindow = SDL_CreateWindow(window.title, window.width, window.height,
                                             SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_Renderer* renderer = sdlWindow != nullptr ? SDL_CreateRenderer(sdlWindow, nullptr) : nullptr;
    if (renderer == nullptr) {
        std::fprintf(stderr, "%s: cannot open a window: %s\n", window.appId, SDL_GetError());
        if (sdlWindow != nullptr) SDL_DestroyWindow(sdlWindow);
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);
    // The requested size at the display's scale and the app's zoom (View > Window size, known once the app has started),
    // shrunk to fit the display's usable area, and centred.
    float scale = uiScale(sdlWindow);
    int zoom = 100;
    auto zoomedScale = [&] { return scale * float(zoom) / 100.0f; };
    auto placeWindow = [&] {
        if (options.width > 0 && options.height > 0) {
            SDL_SetWindowSize(sdlWindow, options.width, options.height);
        } else {
            SDL_Rect usable = {0, 0, 1920, 1080};
            SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(sdlWindow), &usable);
            SDL_SetWindowSize(sdlWindow, std::min(int(float(window.width) * zoomedScale()), int(float(usable.w) * 0.95f)),
                              std::min(int(float(window.height) * zoomedScale()), int(float(usable.h) * 0.95f)));
        }
        SDL_SetWindowPosition(sdlWindow, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    };
    placeWindow();  // Shown once the app has started, at its size

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;  // Single fixed layout: nothing worth persisting
    setUpStyle(window, scale);
    ImGui_ImplSDL3_InitForSDLRenderer(sdlWindow, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    UiStyle::loadFonts();

    SdlToolkit toolkit(sdlWindow);
    Platform::setToolkit(&toolkit);
    start();
    zoom = app.uiZoom();
    if (zoom != 100) {
        placeWindow();
        setUpStyle(window, zoomedScale());
    }
    SDL_ShowWindow(sdlWindow);
    for (const std::filesystem::path& file : options.files) app.openFile(file);

    constexpr Uint64 kFrameNs = 1000000000ull / 60;
    const Uint64 startNs = SDL_GetTicksNS();
    const SDL_WindowID windowId = SDL_GetWindowID(sdlWindow);
    std::vector<bool> testDone(options.testEvents.size(), false);
    std::vector<std::pair<double, SDL_Event>> later;  // Test input for later frames: a click's press and release, key releases
    bool done = false;
    while (!done) {
        const Uint64 frameStart = SDL_GetTicksNS();
        const double time = double(frameStart - startNs) / 1e9;

        // Test input, as the window system would send it.
        for (size_t i = 0; i < options.testEvents.size(); i++) {
            const SdlHostOptions::TestEvent& test = options.testEvents[i];
            if (testDone[i] || time < test.time) continue;
            testDone[i] = true;
            SDL_Event event = {};
            if (test.kind == SdlHostOptions::TestEvent::Click) {
                // The pointer arrives a few frames before the press, as a hand's does: Dear ImGui's tabs and other
                // overlapping items take a click only where the pointer already was in the frame before.
                event.type = SDL_EVENT_MOUSE_MOTION;
                event.motion.windowID = windowId;
                event.motion.x = test.x;
                event.motion.y = test.y;
                SDL_PushEvent(&event);
                for (const bool down : {true, false}) {
                    event = {};
                    event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
                    event.button.windowID = windowId;
                    event.button.button = SDL_BUTTON_LEFT;
                    event.button.down = down;
                    event.button.clicks = 1;
                    event.button.x = test.x;
                    event.button.y = test.y;
                    later.emplace_back(time + (down ? 0.1 : 0.15), event);
                }
            } else if (test.kind == SdlHostOptions::TestEvent::Wheel) {
                event.type = SDL_EVENT_MOUSE_MOTION;
                event.motion.windowID = windowId;
                event.motion.x = test.x;
                event.motion.y = test.y;
                SDL_PushEvent(&event);
                event = {};
                event.type = SDL_EVENT_MOUSE_WHEEL;
                event.wheel.windowID = windowId;
                event.wheel.y = test.amount;
                event.wheel.mouse_x = test.x;
                event.wheel.mouse_y = test.y;
                later.emplace_back(time + 0.1, event);
            } else if (test.kind == SdlHostOptions::TestEvent::Key) {
                event.type = SDL_EVENT_KEY_DOWN;
                event.key.windowID = windowId;
                event.key.scancode = SDL_Scancode(test.scancode);
                event.key.key = SDL_GetKeyFromScancode(event.key.scancode, SDL_KMOD_NONE, false);
                event.key.down = true;
                SDL_PushEvent(&event);
                event.type = SDL_EVENT_KEY_UP;
                event.key.down = false;
                later.emplace_back(time + 0.2, event);
            } else {
                event.type = SDL_EVENT_DROP_FILE;
                event.drop.windowID = windowId;
                event.drop.data = test.path.c_str();
                SDL_PushEvent(&event);
            }
        }
        for (auto pending = later.begin(); pending != later.end();) {
            if (time < pending->first) {
                ++pending;
                continue;
            }
            SDL_PushEvent(&pending->second);
            pending = later.erase(pending);
        }

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            switch (event.type) {
            case SDL_EVENT_QUIT:
                done = true;
                break;
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                if (event.window.windowID == windowId) done = true;
                break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                // The computer-keyboard piano goes by physical key position (scancode), not by the layout's letters.
                if (!event.key.repeat) {
                    const int pc = pcScancode(event.key.scancode);
                    if (pc != 0) app.onKey(pc, event.type == SDL_EVENT_KEY_DOWN);
                }
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                app.clearKeys();
                break;
            case SDL_EVENT_DROP_FILE:
                if (event.drop.data != nullptr) app.openFile(Platform::fromUtf8(event.drop.data));
                break;
            case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
                // Moved to a display of another scale, or the scale setting changed.
                const float newScale = uiScale(sdlWindow);
                if (newScale != scale) {
                    scale = newScale;
                    setUpStyle(window, zoomedScale());
                }
                break;
            }
            default:
                break;
            }
        }
        if (done || app.quitRequested()) break;

        // View > Window size: the text follows before the next frame, and the window takes the size it opens with at that
        // zoom, where it is (not a maximised or full-screen window, nor a test's fixed size): choosing a size again gives
        // that size, however the window was dragged.
        const int wantedZoom = app.uiZoom();
        if (wantedZoom != zoom) {
            zoom = wantedZoom;
            const SDL_WindowFlags flags = SDL_GetWindowFlags(sdlWindow);
            if ((flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN)) == 0 && options.width <= 0) {
                SDL_Rect usable = {0, 0, 1920, 1080};
                SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(sdlWindow), &usable);
                const int width = std::min(int(float(window.width) * zoomedScale()), int(float(usable.w) * 0.95f));
                const int height = std::min(int(float(window.height) * zoomedScale()), int(float(usable.h) * 0.95f));
                int x = 0;
                int y = 0;
                SDL_GetWindowPosition(sdlWindow, &x, &y);
                SDL_SetWindowSize(sdlWindow, width, height);
                SDL_SetWindowPosition(sdlWindow, std::clamp(x, usable.x, usable.x + usable.w - width), std::clamp(y, usable.y, usable.y + usable.h - height));
            }
            setUpStyle(window, zoomedScale());
        }

        // Minimised: audio and MIDI keep running on their own threads.
        if ((SDL_GetWindowFlags(sdlWindow) & SDL_WINDOW_MINIMIZED) != 0) {
            SDL_Delay(10);
            continue;
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        app.frame();
        ImGui::Render();
        const ImGuiIO& io = ImGui::GetIO();
        SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColorFloat(renderer, 0.09f, 0.09f, 0.10f, 1.0f);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        if (!options.screenshot.empty() && time >= options.seconds) {
            SDL_Surface* shot = SDL_RenderReadPixels(renderer, nullptr);
            if (shot == nullptr || !SDL_SaveBMP(shot, Platform::toUtf8(options.screenshot).c_str())) {
                std::fprintf(stderr, "%s: screenshot: %s\n", window.appId, SDL_GetError());
            }
            if (shot != nullptr) SDL_DestroySurface(shot);
            done = true;
        }
        SDL_RenderPresent(renderer);  // Waits for the display's refresh (vsync) where the driver can
        // Without vsync (some drivers, the software renderer): about 60 frames a second.
        const Uint64 spent = SDL_GetTicksNS() - frameStart;
        if (spent < kFrameNs * 2 / 3) SDL_DelayNS(kFrameNs - spent);
    }

    stop();
    Platform::setToolkit(nullptr);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(sdlWindow);
    SDL_Quit();
    return 0;
}
