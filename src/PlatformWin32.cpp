#include "Platform.h"

#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace Platform {

std::filesystem::path executableDirectory() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
        if (length == 0) return std::filesystem::current_path();
        if (length < buffer.size()) return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
        buffer.resize(buffer.size() * 2);
    }
}

std::filesystem::path programDirectory() {
    return executableDirectory();
}

std::filesystem::path bundleResourcesDirectory() {
    return {};
}

std::filesystem::path moduleDirectory() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&moduleDirectory), &module)) {
        return executableDirectory();
    }
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD length = GetModuleFileNameW(module, buffer.data(), DWORD(buffer.size()));
        if (length == 0) return executableDirectory();
        if (length < buffer.size()) return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
        buffer.resize(buffer.size() * 2);
    }
}

std::filesystem::path userDataDirectory() {
    const wchar_t* forTests = _wgetenv(L"D110EMU_DATA_HOME");
    if (forTests != nullptr && *forTests != L'\0') return std::filesystem::path(forTests);
    wchar_t path[MAX_PATH] = {};  // The roaming AppData
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, path))) return std::filesystem::path(path);
    return std::filesystem::temp_directory_path();
}

std::filesystem::path oldUserDataDirectory() {
    return userDataDirectory();
}

std::string keyName(int scancode) {
    wchar_t name[32] = {};
    const int length = GetKeyNameTextW(LONG(scancode & 0xFF) << 16, name, int(sizeof(name) / sizeof(name[0])));
    if (length <= 0) return std::string();
    return toUtf8(std::filesystem::path(std::wstring(name, size_t(length))));
}

namespace {

struct DialogFilter {
    const wchar_t* filter;
    const wchar_t* extension;  // Default extension for saving, without the dot
};

DialogFilter dialogFilter(FileKind kind) {
    switch (kind) {
    case FileKind::Midi:
        return {L"MIDI files (*.mid, *.midi, *.smf, *.rmi)\0*.mid;*.midi;*.smf;*.rmi\0All files (*.*)\0*.*\0", L"mid"};
    case FileKind::MidiOrSysex:
        return {L"MIDI and SysEx files (*.mid, *.midi, *.syx, *.dat)\0*.mid;*.midi;*.syx;*.dat\0All files (*.*)\0*.*\0", L"syx"};
    case FileKind::Rom:
        return {L"ROM images (*.rom, *.bin)\0*.rom;*.bin\0All files (*.*)\0*.*\0", L"rom"};
    case FileKind::Sysex:
    default:
        return {L"SysEx files (*.syx)\0*.syx\0All files (*.*)\0*.*\0", L"syx"};
    }
}

}  // namespace

std::string takeDialogError() {
    return std::string();  // The common dialogs are always there
}

// The common dialogs are modal, with a message loop of their own in which the host's windows keep running.
std::unique_ptr<PendingDialog> startOpenFileDialog(FileKind) {
    return nullptr;
}

std::unique_ptr<PendingDialog> startSaveFileDialog(FileKind, const std::string&) {
    return nullptr;
}

std::unique_ptr<PendingDialog> startPickFolderDialog(const std::filesystem::path&) {
    return nullptr;
}

bool openFileDialog(FileKind kind, std::filesystem::path& result) {
    wchar_t fileName[4096] = {};
    const DialogFilter filter = dialogFilter(kind);
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = filter.filter;
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = DWORD(sizeof(fileName) / sizeof(fileName[0]));
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return false;
    result = fileName;
    return true;
}

bool saveFileDialog(FileKind kind, const std::string& defaultName, std::filesystem::path& result) {
    wchar_t fileName[4096] = {};
    const std::wstring name = fromUtf8(defaultName).wstring();
    name.copy(fileName, std::min<size_t>(name.size(), 4095));
    const DialogFilter filter = dialogFilter(kind);
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = filter.filter;
    ofn.lpstrDefExt = filter.extension;
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = DWORD(sizeof(fileName) / sizeof(fileName[0]));
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return false;
    result = fileName;
    return true;
}

bool pickFolderDialog(const std::filesystem::path& initialFolder, std::filesystem::path& result) {
    // Requires COM, which the application initialises on the UI thread at startup.
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return false;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(L"Choose the ROM folder");
    if (!initialFolder.empty()) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(initialFolder.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dialog->SetFolder(folder);
            folder->Release();
        }
    }
    bool picked = false;
    if (SUCCEEDED(dialog->Show(GetActiveWindow()))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = path;
                CoTaskMemFree(path);
                picked = true;
            }
            item->Release();
        }
    }
    dialog->Release();
    return picked;
}

}  // namespace Platform
