// D110Emu's Windows entry point: the emulator's App in the shared Win32/Direct3D 11 window (Win32Host.cpp).

#include <windows.h>

#include "App.h"
#include "Platform.h"
#include "Win32Host.h"

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

}  // namespace

int WINAPI WinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ LPSTR, _In_ int) {
    App app;
    D110Host host(app);
    const HostWindow window = {L"D110EmuWindow", L"D110Emu - Roland D-110 emulator", 1200, 780, &App::applyStyle};
    return runHost(
        instance, window, host,
        [&] {
            // The settings and memory in %APPDATA%\D110Emu, which D110EmuTUI.exe uses too.
            AppOptions options;
            useStandaloneFolder(options, Platform::appDataDirectory("D110Emu"), true);
            app.init(options);
        },
        [&] { app.shutdown(); });
}
