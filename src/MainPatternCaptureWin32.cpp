// PatternCapture's Windows entry point: its UI in the shared Win32/Direct3D 11 window (Win32Host.cpp).

#include <windows.h>

#include "PatternCaptureApp.h"
#include "Platform.h"
#include "UiStyle.h"
#include "Win32Host.h"

namespace {

class PatternCaptureHost : public HostedApp {
public:
    explicit PatternCaptureHost(PatternCaptureApp& app) : app_(app) {}
    void frame() override { app_.frame(); }
    bool quitRequested() const override { return app_.quitRequested(); }
    void openFile(const std::filesystem::path& path) override { app_.openFile(path); }

private:
    PatternCaptureApp& app_;
};

}  // namespace

int WINAPI WinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ LPSTR, _In_ int) {
    PatternCaptureApp app;
    PatternCaptureHost host(app);
    const HostWindow window = {L"PatternCaptureWindow", L"PatternCapture - the D-20's rhythm patterns from its MIDI OUT", 1240, 760,
                               &UiStyle::apply};
    return runHost(
        instance, window, host,
        [&] {
            // Its files in %APPDATA%\D110Emu, with D110Emu's (earlier versions kept them next to the .exe).
            const std::filesystem::path folder = Platform::appDataDirectory("D110Emu");
            Platform::adoptOldFiles(folder, {Platform::programDirectory()}, "patterncapture.ini", {"patterncapture-session.syx"});
            PatternCaptureOptions options;
            options.settingsFile = folder / "patterncapture.ini";
            options.sessionFile = folder / "patterncapture-session.syx";
            app.init(options);
        },
        [&] { app.shutdown(); });
}
