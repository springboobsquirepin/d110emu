// MT32Translator's Windows entry point: the translator's UI in the shared Win32/Direct3D 11 window (Win32Host.cpp).

#include <windows.h>

#include "Platform.h"
#include "TranslatorApp.h"
#include "UiStyle.h"
#include "Win32Host.h"

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

}  // namespace

int WINAPI WinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ LPSTR, _In_ int) {
    TranslatorApp app;
    TranslatorHost host(app);
    const HostWindow window = {L"MT32TranslatorWindow", L"MT32Translator - MT-32 MIDI for the D-110, D-10 and D-20", 1100, 720,
                               &UiStyle::apply};
    return runHost(
        instance, window, host,
        [&] {
            // Its files in %APPDATA%\D110Emu, with D110Emu's (earlier versions kept them next to the .exe).
            const std::filesystem::path folder = Platform::appDataDirectory("D110Emu");
            Platform::adoptOldFiles(folder, {Platform::programDirectory()}, "mt32translator.ini", {"mt32translator-cache.syx"});
            TranslatorOptions options;
            options.settingsFile = folder / "mt32translator.ini";
            options.cacheFile = folder / "mt32translator-cache.syx";
            options.romSearchDirs = {Platform::executableDirectory(), std::filesystem::current_path(), folder};
            app.init(options);
        },
        [&] { app.shutdown(); });
}
