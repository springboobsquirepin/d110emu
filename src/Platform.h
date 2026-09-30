#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

// Small OS-specific helpers (PlatformWin32.cpp / PlatformPosix.cpp).
namespace Platform {

enum class FileKind { Midi, Sysex, MidiOrSysex, Rom };  // MidiOrSysex: .mid, .syx and .dat (MT-32 data files)

std::filesystem::path executableDirectory();
// Where the user sees the program: the executable's folder, or on macOS the folder that holds its app bundle (for
// D110Emu.app/Contents/MacOS/D110Emu, the folder D110Emu.app is in).
std::filesystem::path programDirectory();
// The folder of the module this code is in: the .exe's, or the plugin's (a DLL in a host's process).
std::filesystem::path moduleDirectory();
// Inside the app itself: on macOS the app bundle's Contents/Resources (D110Emu.app/Contents/Resources); empty for a
// program that is not in a bundle, and on the other systems.
std::filesystem::path bundleResourcesDirectory();
// The user's folder for the programs' own files: the roaming %APPDATA% on Windows, ~/Library/Application Support on
// macOS, $XDG_DATA_HOME (else ~/.local/share) on Linux. The environment variable D110EMU_DATA_HOME, when set, takes its
// place: the plugin tests set it, so as not to change the real one.
std::filesystem::path userDataDirectory();
// Where earlier versions kept them in the user's folder when they could not keep them next to the program: on Linux
// $XDG_CONFIG_HOME (else ~/.config); elsewhere the same as userDataDirectory().
std::filesystem::path oldUserDataDirectory();

// Native dialogs. Return false when cancelled or not available on this platform.
bool openFileDialog(FileKind kind, std::filesystem::path& result);
bool saveFileDialog(FileKind kind, const std::string& defaultName, std::filesystem::path& result);
bool pickFolderDialog(const std::filesystem::path& initialFolder, std::filesystem::path& result);
// Why the last dialog did not open (no dialog program on a Linux desktop, say), once; empty when it opened.
std::string takeDialogError();

// A dialog that runs beside the program instead of blocking it, where the platform has one: on Linux without a toolkit
// (the plugin), zenity or kdialog in a process of its own, so that the plugin host's windows keep running meanwhile.
// Poll done() once a frame: true once the dialog has closed, with the path chosen in `result` (empty when cancelled or
// when it failed; takeDialogError() then says why). Destroying it closes a dialog still open.
class PendingDialog {
public:
    virtual ~PendingDialog() = default;
    virtual bool done(std::filesystem::path& result) = 0;
};
// nullptr where the platform has no such dialogs (Windows, SDL's): the blocking ones above then.
std::unique_ptr<PendingDialog> startOpenFileDialog(FileKind kind);
std::unique_ptr<PendingDialog> startSaveFileDialog(FileKind kind, const std::string& defaultName);
std::unique_ptr<PendingDialog> startPickFolderDialog(const std::filesystem::path& initialFolder);

// The key cap of a physical key (PC scancode set 1) in the current keyboard layout, as UTF-8; empty if unknown.
std::string keyName(int scancode);

// Where the operating system has no dialogs or key names of its own (Linux), the window's toolkit gives them
// (SdlHost.cpp): PlatformPosix.cpp's functions above ask it once it is set, and do without until then.
struct Toolkit {
    virtual ~Toolkit() = default;
    virtual bool openFileDialog(FileKind kind, std::filesystem::path& result) = 0;
    virtual bool saveFileDialog(FileKind kind, const std::string& defaultName, std::filesystem::path& result) = 0;
    virtual bool pickFolderDialog(const std::filesystem::path& initialFolder, std::filesystem::path& result) = 0;
    virtual std::string takeDialogError() = 0;
    virtual std::string keyName(int scancode) = 0;
};
void setToolkit(Toolkit* toolkit);  // PlatformPosix.cpp; nullptr: none
// Key names from the window system where no toolkit gives them (PlatformPosix.cpp: the Linux plugin's X11 window).
void setKeyNameSource(std::string (*source)(int scancode));

// The folder of the programs' files (settings, the memory and the like): userDataDirectory()/name, made if needed.
// All the programs of D110Emu share one, "D110Emu" (file names tell them apart).
inline std::filesystem::path appDataDirectory(const std::string& name) {
    std::error_code ec;
    const std::filesystem::path folder = userDataDirectory() / name;
    std::filesystem::create_directories(folder, ec);
    return folder;
}

// Earlier versions kept a program's files next to the program (else in oldUserDataDirectory()). The first time a
// program keeps them in `folder`, which has no `marker` yet, they are copied there from the first of `oldFolders` that
// has the marker: the marker, and those of `others` that `folder` lacks (another program sharing it may have them). The
// old files stay where they are. Returns the folder they came from; empty when nothing was copied.
inline std::filesystem::path adoptOldFiles(const std::filesystem::path& folder, const std::vector<std::filesystem::path>& oldFolders,
                                           const std::string& marker, const std::vector<std::string>& others) {
    std::error_code ec;
    if (std::filesystem::exists(folder / marker, ec)) return {};
    for (const std::filesystem::path& old : oldFolders) {
        if (old.empty() || !std::filesystem::is_regular_file(old / marker, ec) || std::filesystem::equivalent(old, folder, ec)) continue;
        if (!std::filesystem::copy_file(old / marker, folder / marker, std::filesystem::copy_options::skip_existing, ec)) continue;
        for (const std::string& name : others) {
            if (std::filesystem::is_regular_file(old / name, ec)) {
                std::filesystem::copy_file(old / name, folder / name, std::filesystem::copy_options::skip_existing, ec);
            }
        }
        return old;
    }
    return {};
}

// C++20 made u8string() a std::u8string and u8path() deprecated; the Audio Unit's sources are compiled as C++23.
inline std::string toUtf8(const std::filesystem::path& path) {
#if defined(__cpp_char8_t)
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
#else
    return path.u8string();  // std::string in C++17
#endif
}

inline std::filesystem::path fromUtf8(const std::string& text) {
#if defined(__cpp_char8_t)
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
#else
    return std::filesystem::u8path(text);
#endif
}

}  // namespace Platform
