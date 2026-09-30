// D110Emu's entry point where there is no Win32Host (Linux and macOS): the emulator's App in an SDL window (SdlHost.cpp).
// It keeps its settings and memory in the user's D110Emu folder (Platform::appDataDirectory), as the terminal version
// does. On macOS it is the app bundle D110Emu.app.

#include <SDL3/SDL.h>

#include <cstdio>
#include <string>

#include "App.h"
#include "Platform.h"
#include "SdlHost.h"

namespace {

class D110Host : public HostedApp {
public:
    explicit D110Host(App& app) : app_(app) {}
    void frame() override { app_.frame(); }
    bool quitRequested() const override { return app_.quitRequested(); }
    void onKey(int scancode, bool down) override { app_.onKey(scancode, down); }
    void clearKeys() override { app_.clearKeys(); }
    void openFile(const std::filesystem::path& path) override { app_.openFile(path); }
    int uiZoom() const override { return app_.uiZoom(); }

private:
    App& app_;
};

#ifdef __APPLE__
const char* const kUsage =
    "Usage: D110Emu.app/Contents/MacOS/D110Emu [file.mid | file.syx ...]\n"
    "\n"
    "D110Emu, the Roland D-110 emulator. MIDI files named here play and SysEx files load at the start.\n"
    "The settings and memory are kept in ~/Library/Application Support/D110Emu, which d110emu-tui, the terminal\n"
    "version, uses too; a roms folder there is found, as is one in D110Emu.app/Contents/Resources.\n";
#else
const char* const kUsage =
    "Usage: d110emu [file.mid | file.syx ...]\n"
    "\n"
    "D110Emu, the Roland D-110 emulator. MIDI files named here play and SysEx files load at the start.\n"
    "The settings and memory are kept in $XDG_DATA_HOME/D110Emu (~/.local/share/D110Emu without it), which\n"
    "d110emu-tui, the terminal version, uses too; a roms folder there is found.\n";
#endif

}  // namespace

int main(int argc, char** argv) {
    SdlHostOptions hostOptions;
    bool noAudio = false;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        bool usedValue = false;
        if (arg == "--help" || arg == "-h") {
            std::printf("%s", kUsage);
            return 0;
        }
#ifdef __APPLE__
        if (arg.rfind("-psn_", 0) == 0) continue;  // What older macOS gives an app that Finder starts
#endif
        if (arg == "--no-audio") {  // For tests, as the test options: miniaudio's null device
            noAudio = true;
        } else if (parseSdlHostTestOption(arg, i + 1 < argc ? argv[i + 1] : nullptr, hostOptions, usedValue)) {
            if (usedValue) i++;
        } else if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "d110emu: unknown option %s\n\n%s", arg.c_str(), kUsage);
            return 2;
        } else {
            hostOptions.files.push_back(Platform::fromUtf8(arg));
        }
    }

    App app;
    D110Host host(app);
    AppOptions options;
    useStandaloneFolder(options, Platform::appDataDirectory("D110Emu"), true);
    options.nullAudio = noAudio;
    const int sdl = SDL_GetVersion();
    options.aboutExtra = "Window: SDL " + std::to_string(SDL_VERSIONNUM_MAJOR(sdl)) + "." + std::to_string(SDL_VERSIONNUM_MINOR(sdl)) + "." +
                         std::to_string(SDL_VERSIONNUM_MICRO(sdl)) + " (zlib)";
    const SdlHostWindow window = {"D110Emu", "D110Emu - Roland D-110 emulator", "d110emu", 1200, 780, &App::applyStyle};
    return runSdlHost(window, host, hostOptions, [&] { app.init(options); }, [&] { app.shutdown(); });
}
