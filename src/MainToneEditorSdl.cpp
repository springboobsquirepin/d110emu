// ToneEditor's entry point where there is no Win32Host (Linux and macOS): the realtime tone editor in an SDL window
// (SdlHost.cpp). Its settings and library are in the user's D110Emu folder (Platform::appDataDirectory). On macOS it is
// the app bundle ToneEditor.app.

#include <cstdio>
#include <string>

#include "Platform.h"
#include "SdlHost.h"
#include "ToneEditorApp.h"
#include "UiStyle.h"

namespace {

class ToneEditorHost : public HostedApp {
public:
    explicit ToneEditorHost(ToneEditorApp& app) : app_(app) {}
    void frame() override { app_.frame(); }
    bool quitRequested() const override { return app_.quitRequested(); }
    void onKey(int scancode, bool down) override { app_.onKey(scancode, down); }
    void clearKeys() override { app_.clearKeys(); }
    void openFile(const std::filesystem::path& path) override { app_.openFile(path); }

private:
    ToneEditorApp& app_;
};

#ifdef __APPLE__
const char* const kUsage =
    "Usage: ToneEditor.app/Contents/MacOS/ToneEditor [file.syx ...]\n"
    "\n"
    "ToneEditor: a realtime tone editor for a real D-110, D-10, D-20 or MT-32. The tones in SysEx files named here go\n"
    "to its library. The settings and the library are kept in ~/Library/Application Support/D110Emu (toneeditor.ini,\n"
    "toneeditor-library.syx).\n";
#else
const char* const kUsage =
    "Usage: toneeditor [file.syx ...]\n"
    "\n"
    "ToneEditor: a realtime tone editor for a real D-110, D-10, D-20 or MT-32. The tones in SysEx files named here go\n"
    "to its library. The settings and the library are kept in $XDG_DATA_HOME/D110Emu (~/.local/share/D110Emu without\n"
    "it) as toneeditor.ini and toneeditor-library.syx.\n";
#endif

}  // namespace

int main(int argc, char** argv) {
    SdlHostOptions hostOptions;
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
        if (parseSdlHostTestOption(arg, i + 1 < argc ? argv[i + 1] : nullptr, hostOptions, usedValue)) {
            if (usedValue) i++;
        } else if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "toneeditor: unknown option %s\n\n%s", arg.c_str(), kUsage);
            return 2;
        } else {
            hostOptions.files.push_back(Platform::fromUtf8(arg));
        }
    }

    ToneEditorApp app;
    ToneEditorHost host(app);
    const SdlHostWindow window = {"ToneEditor", "ToneEditor - realtime tone editor for the D-110, D-10, D-20 and MT-32", "toneeditor", 1280,
                                  820, &UiStyle::apply};
    return runSdlHost(
        window, host, hostOptions,
        [&] {
            const std::filesystem::path folder = Platform::appDataDirectory("D110Emu");
            ToneEditorOptions options;
            options.settingsFile = folder / "toneeditor.ini";
            options.libraryFile = folder / "toneeditor-library.syx";
            app.init(options);
        },
        [&] { app.shutdown(); });
}
