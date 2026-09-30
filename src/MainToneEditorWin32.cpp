// ToneEditor's Windows entry point: the realtime tone editor in the shared Win32/Direct3D 11 window (Win32Host.cpp).

#include <windows.h>

#include "Platform.h"
#include "ToneEditorApp.h"
#include "UiStyle.h"
#include "Win32Host.h"

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

}  // namespace

int WINAPI WinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ LPSTR, _In_ int) {
    ToneEditorApp app;
    ToneEditorHost host(app);
    const HostWindow window = {L"ToneEditorWindow", L"ToneEditor - realtime tone editor for the D-110, D-10, D-20 and MT-32", 1280, 820,
                               &UiStyle::apply};
    return runHost(
        instance, window, host,
        [&] {
            // Its files in %APPDATA%\D110Emu, with D110Emu's (earlier versions kept them next to the .exe).
            const std::filesystem::path folder = Platform::appDataDirectory("D110Emu");
            Platform::adoptOldFiles(folder, {Platform::programDirectory()}, "toneeditor.ini", {"toneeditor-library.syx"});
            ToneEditorOptions options;
            options.settingsFile = folder / "toneeditor.ini";
            options.libraryFile = folder / "toneeditor-library.syx";
            app.init(options);
        },
        [&] { app.shutdown(); });
}
