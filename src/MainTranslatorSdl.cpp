// MT32Translator's entry point where there is no Win32Host (Linux and macOS): the translator's UI in an SDL window
// (SdlHost.cpp). Its settings are in the user's D110Emu folder (Platform::appDataDirectory), which mt32translator-tui,
// the terminal version, uses too. On macOS it is the app bundle MT32Translator.app.

#include <cstdio>
#include <string>

#include "Platform.h"
#include "SdlHost.h"
#include "TranslatorApp.h"
#include "UiStyle.h"

namespace {

class TranslatorHost : public HostedApp {
public:
    explicit TranslatorHost(TranslatorApp& app) : app_(app) {}
    void frame() override { app_.frame(); }
    bool quitRequested() const override { return app_.quitRequested(); }
    void openFile(const std::filesystem::path& path) override { app_.openFile(path); }

private:
    TranslatorApp& app_;
};

#ifdef __APPLE__
const char* const kUsage =
    "Usage: MT32Translator.app/Contents/MacOS/MT32Translator [file.syx | file.mid ...]\n"
    "\n"
    "MT32Translator: MIDI for an MT-32 in, MIDI for a real D-110, D-10 or D-20 out. SysEx files named here go to the\n"
    "unit through the translation; MIDI files are offered for translation. The settings are kept in\n"
    "~/Library/Application Support/D110Emu (mt32translator.ini), which mt32translator-tui, the terminal version, uses too.\n";
#else
const char* const kUsage =
    "Usage: mt32translator [file.syx | file.mid ...]\n"
    "\n"
    "MT32Translator: MIDI for an MT-32 in, MIDI for a real D-110, D-10 or D-20 out. SysEx files named here go to the\n"
    "unit through the translation; MIDI files are offered for translation. The settings are kept in\n"
    "$XDG_DATA_HOME/D110Emu (~/.local/share/D110Emu without it) as mt32translator.ini, which mt32translator-tui, the\n"
    "terminal version, uses too.\n";
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
            std::fprintf(stderr, "mt32translator: unknown option %s\n\n%s", arg.c_str(), kUsage);
            return 2;
        } else {
            hostOptions.files.push_back(Platform::fromUtf8(arg));
        }
    }

    TranslatorApp app;
    TranslatorHost host(app);
    const SdlHostWindow window = {"MT32Translator", "MT32Translator - MT-32 MIDI for the D-110, D-10 and D-20", "mt32translator", 1100, 720,
                                  &UiStyle::apply};
    return runSdlHost(
        window, host, hostOptions,
        [&] {
            const std::filesystem::path folder = Platform::appDataDirectory("D110Emu");
            TranslatorOptions options;
            options.settingsFile = folder / "mt32translator.ini";
            options.cacheFile = folder / "mt32translator-cache.syx";
            // A "roms" folder with an MT-32 control ROM: in the app (macOS), next to the program or above, in the user's
            // D110Emu folder.
            const std::filesystem::path resources = Platform::bundleResourcesDirectory();
            if (!resources.empty()) options.romSearchDirs.push_back(resources);
            std::error_code ec;
            options.romSearchDirs.push_back(Platform::executableDirectory());
            options.romSearchDirs.push_back(std::filesystem::current_path(ec));
            options.romSearchDirs.push_back(folder);
            app.init(options);
        },
        [&] { app.shutdown(); });
}
