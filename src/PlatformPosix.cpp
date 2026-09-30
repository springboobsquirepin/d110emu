// The platform helpers on Linux and macOS.

#include "Platform.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#include <crt_externs.h>
#include <mach-o/dyld.h>
#include <pwd.h>
#endif

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <system_error>
#include <thread>
#include <vector>

#ifndef __APPLE__
extern char** environ;
#endif

namespace Platform {

std::filesystem::path executableDirectory() {
    std::error_code ec;
#ifdef __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);  // Only the size it needs
    std::string path(size, '\0');
    if (size > 0 && _NSGetExecutablePath(&path[0], &size) == 0) {
        path.resize(std::strlen(path.c_str()));
        // The path it was started by, which may hold a symbolic link or "..".
        const std::filesystem::path exe = std::filesystem::canonical(path, ec);
        if (!ec) return exe.parent_path();
    }
    return std::filesystem::current_path();
#else
    const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::current_path() : exe.parent_path();
#endif
}

namespace {

// The app bundle the program is in (on macOS, D110Emu.app for D110Emu.app/Contents/MacOS/D110Emu); empty if none.
std::filesystem::path appBundle() {
#ifdef __APPLE__
    const std::filesystem::path folder = executableDirectory();
    const std::filesystem::path contents = folder.parent_path();
    const std::filesystem::path bundle = contents.parent_path();
    if (folder.filename() == "MacOS" && contents.filename() == "Contents" && bundle.extension() == ".app") return bundle;
#endif
    return {};
}

}  // namespace

std::filesystem::path programDirectory() {
    const std::filesystem::path bundle = appBundle();
    return bundle.empty() ? executableDirectory() : bundle.parent_path();
}

std::filesystem::path bundleResourcesDirectory() {
    const std::filesystem::path bundle = appBundle();
    return bundle.empty() ? bundle : bundle / "Contents" / "Resources";
}

std::filesystem::path moduleDirectory() {
    Dl_info info;
    if (dladdr(reinterpret_cast<void*>(&moduleDirectory), &info) != 0 && info.dli_fname != nullptr) {
        std::error_code ec;
        const std::filesystem::path module = std::filesystem::canonical(info.dli_fname, ec);
        if (!ec) return module.parent_path();
    }
    return executableDirectory();
}

std::filesystem::path userDataDirectory() {
    const char* forTests = std::getenv("D110EMU_DATA_HOME");
    if (forTests != nullptr && *forTests != '\0') return fromUtf8(forTests);
    const char* home = std::getenv("HOME");
#ifdef __APPLE__
    // In a sandboxed host (GarageBand), HOME is the host's container: the files are in the user's own folder, which a
    // host that has lowered its security for a plugin that is not sandbox-safe may let it read.
    const bool sandboxed = std::getenv("APP_SANDBOX_CONTAINER_ID") != nullptr ||
                           (home != nullptr && std::strstr(home, "/Library/Containers/") != nullptr);
    if (sandboxed) {
        const struct passwd* user = getpwuid(getuid());
        if (user != nullptr && user->pw_dir != nullptr && *user->pw_dir != '\0') home = user->pw_dir;
    }
    if (home != nullptr && *home != '\0') return std::filesystem::path(home) / "Library" / "Application Support";
#else
    const char* data = std::getenv("XDG_DATA_HOME");
    if (data != nullptr && *data != '\0') return data;
    if (home != nullptr && *home != '\0') return std::filesystem::path(home) / ".local" / "share";
#endif
    return std::filesystem::temp_directory_path();
}

std::filesystem::path oldUserDataDirectory() {
#ifdef __APPLE__
    return userDataDirectory();
#else
    const char* config = std::getenv("XDG_CONFIG_HOME");
    if (config != nullptr && *config != '\0') return config;
    const char* home = std::getenv("HOME");
    if (home != nullptr && *home != '\0') return std::filesystem::path(home) / ".config";
    return std::filesystem::temp_directory_path();
#endif
}

namespace {

// The environment, for the programs started here. A library (a plugin) cannot reach environ itself on macOS.
char** environment() {
#ifdef __APPLE__
    return *_NSGetEnviron();
#else
    return environ;
#endif
}

// A pipe whose ends the programs started later do not inherit.
bool closeOnExecPipe(int ends[2]) {
#ifdef __APPLE__
    if (::pipe(ends) != 0) return false;  // macOS has no pipe2()
    ::fcntl(ends[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(ends[1], F_SETFD, FD_CLOEXEC);
    return true;
#else
    return ::pipe2(ends, O_CLOEXEC) == 0;
#endif
}

Toolkit* gToolkit = nullptr;
std::string (*gKeyNameSource)(int) = nullptr;

// Without a toolkit (the plugin), the dialogs are zenity's or kdialog's, which run beside the program: the plugin host's
// windows keep running while one is open. Only the UI thread uses these.
std::string gDialogError;           // Why the last one did not open, until taken
std::filesystem::path gLastFolder;  // Where the last one was answered: the next opens there

enum class DialogOp { Open, Save, Folder };

// The dialogs' filters by kind of file, as SdlHost.cpp's, with the extension a saved file gets when it has none.
struct DialogFilter {
    const char* name;
    const char* patterns;  // Both cases: GTK 3's patterns are case-sensitive
    const char* extension;
};

DialogFilter dialogFilter(FileKind kind) {
    switch (kind) {
    case FileKind::Midi: return {"MIDI files (*.mid, *.midi, *.smf, *.rmi)", "*.mid *.MID *.midi *.MIDI *.smf *.SMF *.rmi *.RMI", "mid"};
    case FileKind::MidiOrSysex:
        return {"MIDI and SysEx files (*.mid, *.midi, *.syx, *.dat)", "*.mid *.MID *.midi *.MIDI *.syx *.SYX *.dat *.DAT", "syx"};
    case FileKind::Rom: return {"ROM images (*.rom, *.bin)", "*.rom *.ROM *.bin *.BIN", "rom"};
    case FileKind::Sysex:
    default: return {"SysEx files (*.syx)", "*.syx *.SYX", "syx"};
    }
}

// A program on the PATH, or empty.
std::string findProgram(const char* name) {
    const char* path = std::getenv("PATH");
    const std::string folders = path != nullptr && *path != '\0' ? path : "/usr/local/bin:/usr/bin:/bin";
    size_t start = 0;
    while (start <= folders.size()) {
        const size_t end = std::min(folders.find(':', start), folders.size());
        const std::string folder = end > start ? folders.substr(start, end - start) : std::string(".");
        const std::string candidate = folder + "/" + name;
        if (::access(candidate.c_str(), X_OK) == 0) return candidate;
        start = end + 1;
    }
    return std::string();
}

std::filesystem::path startFolder(const std::filesystem::path& wanted) {
    std::error_code ec;
    if (!wanted.empty() && std::filesystem::is_directory(wanted, ec)) return wanted;
    if (!gLastFolder.empty() && std::filesystem::is_directory(gLastFolder, ec)) return gLastFolder;
    const char* home = std::getenv("HOME");
    return home != nullptr && *home != '\0' ? std::filesystem::path(home) : std::filesystem::current_path(ec);
}

// A dialog program's process: what it prints on its standard output is the path chosen.
class ProcessDialog : public PendingDialog {
public:
    ProcessDialog(pid_t pid, int output, DialogOp op, const char* extension, std::string program)
        : pid_(pid), output_(output), op_(op), extension_(extension), program_(std::move(program)) {}
    ~ProcessDialog() override {
        if (output_ >= 0) ::close(output_);
        if (pid_ > 0) {
            ::kill(pid_, SIGTERM);
            ::waitpid(pid_, nullptr, 0);
        }
    }
    ProcessDialog(const ProcessDialog&) = delete;
    ProcessDialog& operator=(const ProcessDialog&) = delete;

    bool done(std::filesystem::path& result) override {
        result.clear();
        if (pid_ <= 0) return true;
        const bool ended = readOutput();
        int status = 0;
        const pid_t waited = ::waitpid(pid_, &status, WNOHANG);
        if (waited == 0) return false;  // Still open
        // It has exited. (Where the host reaps its children itself, waitpid knows nothing of it: the end of its output
        // tells then. A helper it started may still hold the output open, so its exit is what counts otherwise.)
        if (waited < 0 && !ended) return false;
        readOutput();  // What is left, now that it has exited
        const bool exited = waited == pid_ && WIFEXITED(status);
        pid_ = -1;
        ::close(output_);
        output_ = -1;
        while (!text_.empty() && (text_.back() == '\n' || text_.back() == '\r')) text_.pop_back();
        if (text_.empty()) {
            // Exit code 1 is Cancel (or a closed window); higher ones are failures, such as an option it lacks.
            if (exited && WEXITSTATUS(status) > 1) gDialogError = program_ + " failed (exit code " + std::to_string(WEXITSTATUS(status)) + ")";
            return true;
        }
        result = fromUtf8(text_);
        if (op_ == DialogOp::Save && !result.has_extension()) result += std::string(".") + extension_;
        gLastFolder = op_ == DialogOp::Folder ? result : result.parent_path();
        return true;
    }

private:
    // Takes what the dialog has printed so far; true at the end of its output.
    bool readOutput() {
        char buffer[4096];
        for (;;) {
            const ssize_t count = ::read(output_, buffer, sizeof(buffer));
            if (count > 0) {
                text_.append(buffer, size_t(count));
                continue;
            }
            if (count < 0 && errno == EINTR) continue;
            return count == 0;  // End of file, or nothing more for now (EAGAIN)
        }
    }

    pid_t pid_;
    int output_;
    DialogOp op_;
    const char* extension_;
    std::string program_;
    std::string text_;
};

std::unique_ptr<PendingDialog> startDialog(DialogOp op, FileKind kind, const std::string& defaultName, const std::filesystem::path& initialFolder) {
    // kdialog on KDE's desktop, zenity (GNOME's, which most desktops have) elsewhere.
    const char* desktop = std::getenv("XDG_CURRENT_DESKTOP");
    const bool kde = desktop != nullptr && std::strstr(desktop, "KDE") != nullptr;
    const std::string zenity = findProgram("zenity");
    const std::string kdialog = findProgram("kdialog");
    const bool useKdialog = !kdialog.empty() && (kde || zenity.empty());
    if (zenity.empty() && kdialog.empty()) {
        gDialogError = "this desktop has no file dialog for programs: sudo apt install zenity gives it one";
        return nullptr;
    }
    const DialogFilter filter = dialogFilter(kind);
    const std::string folder = toUtf8(startFolder(initialFolder));
    const std::string title = op == DialogOp::Open ? "Open" : op == DialogOp::Save ? "Save as" : "Choose a folder";
    std::vector<std::string> args;
    if (useKdialog) {
        args = {kdialog, "--title", title};
        const std::string filters = std::string(filter.patterns) + "|" + filter.name + "\n*|All files";
        if (op == DialogOp::Open) {
            args.insert(args.end(), {"--getopenfilename", folder, filters});
        } else if (op == DialogOp::Save) {
            args.insert(args.end(), {"--getsavefilename", toUtf8(fromUtf8(folder) / fromUtf8(defaultName)), filters});
        } else {
            args.insert(args.end(), {"--getexistingdirectory", folder});
        }
    } else {
        args = {zenity, "--file-selection", "--title=" + title};
        if (op == DialogOp::Folder) {
            args.insert(args.end(), {"--directory", "--filename=" + folder + "/"});
        } else {
            if (op == DialogOp::Save) {
                // zenity 4 always asks before replacing a file (and only warns about the option); zenity 3 needs it.
                args.insert(args.end(), {"--save", "--confirm-overwrite", "--filename=" + toUtf8(fromUtf8(folder) / fromUtf8(defaultName))});
            } else {
                args.push_back("--filename=" + folder + "/");
            }
            args.push_back(std::string("--file-filter=") + filter.name + " | " + filter.patterns);
            args.push_back("--file-filter=All files | *");
        }
    }

    int ends[2] = {-1, -1};
    if (!closeOnExecPipe(ends)) {
        gDialogError = std::string("cannot start ") + (useKdialog ? "kdialog" : "zenity") + ": " + std::strerror(errno);
        return nullptr;
    }
    ::fcntl(ends[0], F_SETFL, ::fcntl(ends[0], F_GETFL) | O_NONBLOCK);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, ends[1], 1);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);  // GTK's and Qt's chatter
    // The host's signal mask and ignored signals are not the dialog's.
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    sigset_t signals;
    sigemptyset(&signals);
    posix_spawnattr_setsigmask(&attributes, &signals);
    for (int number : {SIGPIPE, SIGCHLD, SIGINT, SIGTERM, SIGHUP, SIGQUIT}) sigaddset(&signals, number);
    posix_spawnattr_setsigdefault(&attributes, &signals);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    std::vector<char*> argv;
    for (std::string& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    pid_t pid = -1;
    const int error = posix_spawn(&pid, args[0].c_str(), &actions, &attributes, argv.data(), environment());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    ::close(ends[1]);
    if (error != 0) {
        ::close(ends[0]);
        gDialogError = "cannot start " + args[0] + ": " + std::strerror(error);
        return nullptr;
    }
    return std::unique_ptr<PendingDialog>(new ProcessDialog(pid, ends[0], op, filter.extension, useKdialog ? "kdialog" : "zenity"));
}

// A dialog without a toolkit, waited for (nothing runs meanwhile).
bool waitForDialog(std::unique_ptr<PendingDialog> dialog, std::filesystem::path& result) {
    result.clear();
    if (dialog == nullptr) return false;
    while (!dialog->done(result)) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return !result.empty();
}

}  // namespace

void setToolkit(Toolkit* toolkit) {
    gToolkit = toolkit;
}

void setKeyNameSource(std::string (*source)(int scancode)) {
    gKeyNameSource = source;
}

bool openFileDialog(FileKind kind, std::filesystem::path& result) {
    if (gToolkit != nullptr) return gToolkit->openFileDialog(kind, result);
    return waitForDialog(startDialog(DialogOp::Open, kind, std::string(), std::filesystem::path()), result);
}

bool saveFileDialog(FileKind kind, const std::string& defaultName, std::filesystem::path& result) {
    if (gToolkit != nullptr) return gToolkit->saveFileDialog(kind, defaultName, result);
    return waitForDialog(startDialog(DialogOp::Save, kind, defaultName, std::filesystem::path()), result);
}

bool pickFolderDialog(const std::filesystem::path& initialFolder, std::filesystem::path& result) {
    if (gToolkit != nullptr) return gToolkit->pickFolderDialog(initialFolder, result);
    return waitForDialog(startDialog(DialogOp::Folder, FileKind::Sysex, std::string(), initialFolder), result);
}

std::string takeDialogError() {
    if (gToolkit != nullptr) return gToolkit->takeDialogError();
    std::string error;
    error.swap(gDialogError);
    return error;
}

std::unique_ptr<PendingDialog> startOpenFileDialog(FileKind kind) {
    return gToolkit != nullptr ? nullptr : startDialog(DialogOp::Open, kind, std::string(), std::filesystem::path());
}

std::unique_ptr<PendingDialog> startSaveFileDialog(FileKind kind, const std::string& defaultName) {
    return gToolkit != nullptr ? nullptr : startDialog(DialogOp::Save, kind, defaultName, std::filesystem::path());
}

std::unique_ptr<PendingDialog> startPickFolderDialog(const std::filesystem::path& initialFolder) {
    return gToolkit != nullptr ? nullptr : startDialog(DialogOp::Folder, FileKind::Sysex, std::string(), initialFolder);
}

std::string keyName(int scancode) {
    if (gToolkit != nullptr) return gToolkit->keyName(scancode);
    return gKeyNameSource != nullptr ? gKeyNameSource(scancode) : std::string();
}

}  // namespace Platform
