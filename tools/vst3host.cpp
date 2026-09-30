// vst3host: a minimal VST3 host for Linux, for D110Emu's plugin window without a DAW. It loads a built plugin (the .so in
// its bundle, as hosts do), makes instances (the processor and the controller, connected) and opens their editors in X11
// windows of its own, running their event loop as a host does (VST3's Linux::IRunLoop: file descriptors and timers).
// An audio thread calls process() as a DAW's does (48 kHz, 480 frames every 10 ms, the mix only) and reports the level.
// Clicks, keys and a file dropped with XDND can be sent at given times, the editor closed and opened again, and the
// windows saved as PNG. Needs an X display (DISPLAY; Xvfb will do) and the ROMs, found as the plugin finds them.
// Prints what happened; exit code 0 when the editors opened, ran and closed cleanly (and a drop, if any, was taken).

#include <poll.h>
#include <signal.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <dlfcn.h>

#include "TestDataFolder.h"
#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmessage.h"

// X11's headers last: their macros (None, Bool, Status...) would clash with names above.
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

namespace Steinberg {
DEF_CLASS_IID (Vst::IComponent)
DEF_CLASS_IID (Vst::IAudioProcessor)
DEF_CLASS_IID (Vst::IEditController)
DEF_CLASS_IID (Vst::IConnectionPoint)
DEF_CLASS_IID (Vst::IHostApplication)
DEF_CLASS_IID (IPlugView)
DEF_CLASS_IID (IPlugFrame)
DEF_CLASS_IID (IPlugViewContentScaleSupport)
DEF_CLASS_IID (Linux::IRunLoop)
DEF_CLASS_IID (Linux::IEventHandler)
DEF_CLASS_IID (Linux::ITimerHandler)
}  // namespace Steinberg

using namespace Steinberg;

namespace {

using Clock = std::chrono::steady_clock;

int g_failures = 0;

void fail(const std::string& what) {
    std::printf("FAIL  %s\n", what.c_str());
    g_failures++;
}

double seconds(Clock::time_point since) {
    return std::chrono::duration<double>(Clock::now() - since).count();
}

// The host's side of VST3 that plugins ask for: its name (messages: none, the plugin makes its own).
class HostApplication : public U::Implements<U::Directly<Vst::IHostApplication>> {
public:
    tresult PLUGIN_API getName(Vst::String128 name) override {
        const char* text = "vst3host";
        for (int i = 0; i < 128; i++) {
            name[i] = Vst::TChar(text[i]);
            if (text[i] == 0) break;
        }
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(TUID, TUID, void** obj) override {
        *obj = nullptr;
        return kNotImplemented;
    }
};

// The event loop plugins run their windows in: descriptors watched for input and timers, as a Linux host's.
struct RunLoop {
    struct Handler {
        Linux::IEventHandler* handler;
        int fd;
    };
    struct Timer {
        Linux::ITimerHandler* handler;
        double interval;  // Seconds
        Clock::time_point next;
    };
    std::vector<Handler> handlers;
    std::vector<Timer> timers;
    int timerCalls = 0;
    int eventCalls = 0;

    // One pass: waits at most `timeout` seconds for input on the descriptors (and on the host's own, `extraFd`), then
    // calls the handlers with input and the timers that are due. Returns whether the host's descriptor has input.
    bool pass(double timeout, int extraFd) {
        std::vector<pollfd> fds;
        for (const Handler& handler : handlers) fds.push_back({handler.fd, POLLIN, 0});
        fds.push_back({extraFd, POLLIN, 0});
        const Clock::time_point now = Clock::now();
        for (const Timer& timer : timers) {
            timeout = std::min(timeout, std::max(0.0, std::chrono::duration<double>(timer.next - now).count()));
        }
        ::poll(fds.data(), nfds_t(fds.size()), int(timeout * 1000.0));
        // Copies: a callback may register or unregister (an editor closing).
        const std::vector<Handler> ready = handlers;
        for (size_t i = 0; i < ready.size() && i < fds.size(); i++) {
            if ((fds[i].revents & POLLIN) == 0) continue;
            const bool stillThere = std::any_of(handlers.begin(), handlers.end(), [&](const Handler& h) { return h.handler == ready[i].handler; });
            if (!stillThere) continue;
            eventCalls++;
            ready[i].handler->onFDIsSet(ready[i].fd);
        }
        const std::vector<Timer> due = timers;
        for (const Timer& timer : due) {
            if (Clock::now() < timer.next) continue;
            auto found = std::find_if(timers.begin(), timers.end(), [&](const Timer& t) { return t.handler == timer.handler; });
            if (found == timers.end()) continue;
            found->next = Clock::now() + std::chrono::microseconds(int64_t(timer.interval * 1e6));
            timerCalls++;
            timer.handler->onTimer();
        }
        return (fds.back().revents & POLLIN) != 0;
    }
};

RunLoop g_runLoop;

// A view's frame: resizes the host's window when the plugin asks (then onSize, as hosts do), and gives the run loop.
class Frame : public U::Implements<U::Directly<IPlugFrame, Linux::IRunLoop>> {
public:
    Frame(Display* display, Window window) : display_(display), window_(window) {}

    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* newSize) override {
        if (view == nullptr || newSize == nullptr) return kInvalidArgument;
        resizes++;
        std::printf("      resizeView %d x %d\n", newSize->getWidth(), newSize->getHeight());
        XResizeWindow(display_, window_, unsigned(newSize->getWidth()), unsigned(newSize->getHeight()));
        XSync(display_, False);
        view->onSize(newSize);
        return kResultTrue;
    }

    tresult PLUGIN_API registerEventHandler(Linux::IEventHandler* handler, Linux::FileDescriptor fd) override {
        if (handler == nullptr || fd < 0) return kInvalidArgument;
        handler->addRef();
        g_runLoop.handlers.push_back({handler, fd});
        return kResultTrue;
    }
    tresult PLUGIN_API unregisterEventHandler(Linux::IEventHandler* handler) override {
        auto found = std::find_if(g_runLoop.handlers.begin(), g_runLoop.handlers.end(), [&](const RunLoop::Handler& h) { return h.handler == handler; });
        if (found == g_runLoop.handlers.end()) return kInvalidArgument;
        g_runLoop.handlers.erase(found);
        handler->release();
        return kResultTrue;
    }
    tresult PLUGIN_API registerTimer(Linux::ITimerHandler* handler, Linux::TimerInterval milliseconds) override {
        if (handler == nullptr || milliseconds == 0) return kInvalidArgument;
        handler->addRef();
        const double interval = double(milliseconds) / 1000.0;
        g_runLoop.timers.push_back({handler, interval, Clock::now() + std::chrono::microseconds(int64_t(interval * 1e6))});
        return kResultTrue;
    }
    tresult PLUGIN_API unregisterTimer(Linux::ITimerHandler* handler) override {
        auto found = std::find_if(g_runLoop.timers.begin(), g_runLoop.timers.end(), [&](const RunLoop::Timer& t) { return t.handler == handler; });
        if (found == g_runLoop.timers.end()) return kInvalidArgument;
        g_runLoop.timers.erase(found);
        handler->release();
        return kResultTrue;
    }

    int resizes = 0;

private:
    Display* display_;
    Window window_;
};

// A plugin instance with its editor in a window of the host's.
struct Instance {
    Vst::IComponent* component = nullptr;
    Vst::IAudioProcessor* processor = nullptr;
    Vst::IEditController* controller = nullptr;
    Vst::IConnectionPoint* componentPoint = nullptr;
    Vst::IConnectionPoint* controllerPoint = nullptr;
    IPlugView* view = nullptr;
    Frame* frame = nullptr;
    Window window = 0;  // The host's, the view's parent
};

struct Action {
    enum Kind { Click, RightClick, Hover, Key, Drop, Reopen, Shot } kind = Click;
    double time = 1.0;
    int x = 0;
    int y = 0;
    int keycode = 0;
    std::string path;
    bool done = false;
};

struct Host {
    Display* display = nullptr;
    IPluginFactory* factory = nullptr;
    HostApplication* application = nullptr;
    float scale = 0.0f;
    std::vector<Instance> instances;
    // XDND, as a drag's source: a window of its own owns the selection with the file's URI.
    Window dropSource = 0;
    std::string dropUri;
    bool dropAccepted = false;
    bool dropFinished = false;
    bool dropSucceeded = false;
    Atom xdndAware = 0, xdndEnter = 0, xdndPosition = 0, xdndStatus = 0, xdndDrop = 0, xdndFinished = 0, xdndSelection = 0,
         xdndActionCopy = 0, uriList = 0;
};

Window pluginWindow(Display* display, Window parent) {
    Window root = 0;
    Window parentOfParent = 0;
    Window* children = nullptr;
    unsigned int count = 0;
    Window child = 0;
    if (XQueryTree(display, parent, &root, &parentOfParent, &children, &count) != 0 && count > 0) child = children[0];
    if (children != nullptr) XFree(children);
    return child;
}

bool openEditor(Host& host, Instance& instance, int index) {
    instance.view = instance.controller->createView(Vst::ViewType::kEditor);
    if (instance.view == nullptr) {
        fail("createView gave no editor");
        return false;
    }
    if (instance.view->isPlatformTypeSupported(kPlatformTypeX11EmbedWindowID) != kResultTrue) {
        fail("the editor does not take an X11 window");
        return false;
    }
    if (host.scale > 0.0f) {
        IPlugViewContentScaleSupport* scaling = nullptr;
        if (instance.view->queryInterface(IPlugViewContentScaleSupport::iid, reinterpret_cast<void**>(&scaling)) == kResultOk && scaling != nullptr) {
            scaling->setContentScaleFactor(host.scale);
            scaling->release();
        }
    }
    ViewRect rect;
    instance.view->getSize(&rect);
    const int width = std::max(rect.getWidth(), 64);
    const int height = std::max(rect.getHeight(), 64);
    if (instance.window == 0) {
        XSetWindowAttributes attributes = {};
        attributes.background_pixel = BlackPixel(host.display, DefaultScreen(host.display));
        attributes.event_mask = StructureNotifyMask;
        instance.window = XCreateWindow(host.display, DefaultRootWindow(host.display), 20 + 40 * index, 20 + 40 * index, unsigned(width),
                                        unsigned(height), 0, CopyFromParent, InputOutput, CopyFromParent, CWBackPixel | CWEventMask, &attributes);
        const std::string title = "vst3host " + std::to_string(index + 1);
        XStoreName(host.display, instance.window, title.c_str());
        XMapWindow(host.display, instance.window);
    } else {
        XResizeWindow(host.display, instance.window, unsigned(width), unsigned(height));
    }
    XSync(host.display, False);
    instance.frame = new Frame(host.display, instance.window);
    instance.view->setFrame(instance.frame);
    const auto opening = Clock::now();
    const tresult attached = instance.view->attached(reinterpret_cast<void*>(uintptr_t(instance.window)), kPlatformTypeX11EmbedWindowID);
    std::printf("      editor %d: %d x %d, attached %s in %.0f ms; %zu descriptor(s), %zu timer(s) registered\n", index + 1, width, height,
                attached == kResultOk ? "OK" : "FAILED", seconds(opening) * 1000.0, g_runLoop.handlers.size(), g_runLoop.timers.size());
    if (attached != kResultOk) {
        fail("attached() failed");
        return false;
    }
    return true;
}

void closeEditor(Instance& instance) {
    if (instance.view == nullptr) return;
    instance.view->removed();
    instance.view->setFrame(nullptr);
    instance.view->release();
    instance.view = nullptr;
    if (instance.frame != nullptr) instance.frame->release();
    instance.frame = nullptr;
}

void sendClient(Host& host, Window target, Atom type, long a, long b, long c, long d, long e) {
    XEvent event = {};
    event.xclient.type = ClientMessage;
    event.xclient.display = host.display;
    event.xclient.window = target;
    event.xclient.message_type = type;
    event.xclient.format = 32;
    event.xclient.data.l[0] = a;
    event.xclient.data.l[1] = b;
    event.xclient.data.l[2] = c;
    event.xclient.data.l[3] = d;
    event.xclient.data.l[4] = e;
    XSendEvent(host.display, target, False, NoEventMask, &event);
    XFlush(host.display);
}

// Input as the X server would deliver it to the plugin's window (XSendEvent: the plugin does not ask where it came from).
void sendPointer(Host& host, Window target, int type, int x, int y, unsigned int button, unsigned int state) {
    XEvent event = {};
    if (type == MotionNotify) {
        event.xmotion.type = MotionNotify;
        event.xmotion.display = host.display;
        event.xmotion.window = target;
        event.xmotion.x = x;
        event.xmotion.y = y;
        event.xmotion.state = state;
        event.xmotion.same_screen = True;
    } else if (type == LeaveNotify) {
        event.xcrossing.type = LeaveNotify;
        event.xcrossing.display = host.display;
        event.xcrossing.window = target;
        event.xcrossing.x = -1;
        event.xcrossing.y = -1;
        event.xcrossing.mode = NotifyNormal;
    } else {
        event.xbutton.type = type;
        event.xbutton.display = host.display;
        event.xbutton.window = target;
        event.xbutton.x = x;
        event.xbutton.y = y;
        event.xbutton.button = button;
        event.xbutton.state = state;
        event.xbutton.same_screen = True;
    }
    XSendEvent(host.display, target, True, NoEventMask, &event);
    XFlush(host.display);
}

void sendKey(Host& host, Window target, int type, int keycode) {
    XEvent event = {};
    event.xkey.type = type;
    event.xkey.display = host.display;
    event.xkey.window = target;
    event.xkey.root = DefaultRootWindow(host.display);
    event.xkey.keycode = unsigned(keycode);
    event.xkey.same_screen = True;
    XSendEvent(host.display, target, True, NoEventMask, &event);
    XFlush(host.display);
}

std::string fileUri(const std::filesystem::path& path) {
    const std::string text = std::filesystem::absolute(path).string();
    std::string uri = "file://";
    for (unsigned char c : text) {
        if (std::isalnum(c) || std::strchr("/-_.~", c) != nullptr) {
            uri += char(c);
        } else {
            char escaped[4];
            std::snprintf(escaped, sizeof(escaped), "%%%02X", c);
            uri += escaped;
        }
    }
    return uri;
}

// A drag from the host's source window: enter, a position over the window (then, when accepted, the drop), as a file
// manager's drag does. The rest follows in handleHostEvent: status, the selection's request, finished.
void startDrop(Host& host, Window target, const std::filesystem::path& file) {
    host.dropUri = fileUri(file) + "\r\n";
    host.dropAccepted = false;
    host.dropFinished = false;
    XSetSelectionOwner(host.display, host.xdndSelection, host.dropSource, CurrentTime);
    sendClient(host, target, host.xdndEnter, long(host.dropSource), long(5) << 24, long(host.uriList), 0, 0);
    int rootX = 0;
    int rootY = 0;
    Window child = 0;
    XTranslateCoordinates(host.display, target, DefaultRootWindow(host.display), 100, 100, &rootX, &rootY, &child);
    sendClient(host, target, host.xdndPosition, long(host.dropSource), 0, (long(rootX) << 16) | long(rootY), CurrentTime, long(host.xdndActionCopy));
    std::printf("      drag of %s entered the window\n", file.string().c_str());
}

void handleHostEvent(Host& host, XEvent& event) {
    if (event.type == ClientMessage && event.xclient.window == host.dropSource) {
        const Window target = Window(event.xclient.data.l[0]);
        if (Atom(event.xclient.message_type) == host.xdndStatus) {
            const bool accepted = (event.xclient.data.l[1] & 1) != 0;
            std::printf("      XdndStatus: %s (action %s)\n", accepted ? "accepted" : "refused",
                        Atom(event.xclient.data.l[4]) == host.xdndActionCopy ? "copy" : "none");
            if (accepted && !host.dropAccepted) {
                host.dropAccepted = true;
                sendClient(host, target, host.xdndDrop, long(host.dropSource), 0, CurrentTime, 0, 0);
            }
        } else if (Atom(event.xclient.message_type) == host.xdndFinished) {
            host.dropFinished = true;
            host.dropSucceeded = (event.xclient.data.l[1] & 1) != 0;
            std::printf("      XdndFinished: %s\n", host.dropSucceeded ? "the drop was taken" : "the drop was refused");
        }
    } else if (event.type == SelectionRequest && event.xselectionrequest.owner == host.dropSource) {
        const XSelectionRequestEvent& request = event.xselectionrequest;
        XEvent reply = {};
        reply.xselection.type = SelectionNotify;
        reply.xselection.display = host.display;
        reply.xselection.requestor = request.requestor;
        reply.xselection.selection = request.selection;
        reply.xselection.target = request.target;
        reply.xselection.time = request.time;
        reply.xselection.property = 0;
        if (request.target == host.uriList && request.property != 0) {
            XChangeProperty(host.display, request.requestor, request.property, host.uriList, 8, PropModeReplace,
                            reinterpret_cast<const unsigned char*>(host.dropUri.data()), int(host.dropUri.size()));
            reply.xselection.property = request.property;
        }
        XSendEvent(host.display, request.requestor, False, NoEventMask, &reply);
        XFlush(host.display);
        std::printf("      the selection (text/uri-list) was asked for and given\n");
    }
}

uint32_t crc32(const std::vector<uint8_t>& data, size_t start) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t n = 0; n < 256; n++) {
            uint32_t c = n;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
        ready = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = start; i < data.size(); i++) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

// Uncompressed PNG (zlib "stored" blocks), as uisnap writes them.
bool writePng(const std::filesystem::path& path, int width, int height, const std::vector<uint8_t>& rgb) {
    std::vector<uint8_t> raw;
    raw.reserve((size_t(width) * 3 + 1) * size_t(height));
    for (int y = 0; y < height; y++) {
        raw.push_back(0);
        raw.insert(raw.end(), rgb.begin() + long(size_t(y) * size_t(width) * 3), rgb.begin() + long(size_t(y + 1) * size_t(width) * 3));
    }
    std::vector<uint8_t> zlib = {0x78, 0x01};
    for (size_t pos = 0; pos < raw.size();) {
        const size_t length = std::min<size_t>(65535, raw.size() - pos);
        zlib.push_back(pos + length == raw.size() ? 1 : 0);
        zlib.push_back(uint8_t(length));
        zlib.push_back(uint8_t(length >> 8));
        zlib.push_back(uint8_t(~length));
        zlib.push_back(uint8_t(~length >> 8));
        zlib.insert(zlib.end(), raw.begin() + long(pos), raw.begin() + long(pos + length));
        pos += length;
    }
    uint32_t a = 1;
    uint32_t b = 0;
    for (uint8_t byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    const uint32_t adler = (b << 16) | a;
    for (int shift = 24; shift >= 0; shift -= 8) zlib.push_back(uint8_t(adler >> shift));
    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    auto chunk = [&](const char* type, const std::vector<uint8_t>& body) {
        for (int shift = 24; shift >= 0; shift -= 8) png.push_back(uint8_t(body.size() >> shift));
        const size_t start = png.size();
        png.insert(png.end(), type, type + 4);
        png.insert(png.end(), body.begin(), body.end());
        const uint32_t crc = crc32(png, start);
        for (int shift = 24; shift >= 0; shift -= 8) png.push_back(uint8_t(crc >> shift));
    };
    std::vector<uint8_t> header;
    for (uint32_t value : {uint32_t(width), uint32_t(height)}) {
        for (int shift = 24; shift >= 0; shift -= 8) header.push_back(uint8_t(value >> shift));
    }
    header.insert(header.end(), {8, 2, 0, 0, 0});
    chunk("IHDR", header);
    chunk("IDAT", zlib);
    chunk("IEND", {});
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
    return bool(out);
}

// Runs the loop for `duration` seconds, taking the host's own X events on the way.
void runFor(Host& host, double duration) {
    const Clock::time_point until = Clock::now() + std::chrono::microseconds(int64_t(duration * 1e6));
    while (Clock::now() < until) {
        g_runLoop.pass(std::min(0.01, std::chrono::duration<double>(until - Clock::now()).count()), ConnectionNumber(host.display));
        while (XPending(host.display) > 0) {
            XEvent event;
            XNextEvent(host.display, &event);
            handleHostEvent(host, event);
        }
    }
}

// The window with the editor in it (raised first, and given a few frames to draw), as PNG.
bool screenshot(Host& host, const Instance& instance, const std::filesystem::path& path) {
    XRaiseWindow(host.display, instance.window);
    XSync(host.display, False);
    runFor(host, 0.15);
    XWindowAttributes attributes;
    if (XGetWindowAttributes(host.display, instance.window, &attributes) == 0) return false;
    XImage* image = XGetImage(host.display, instance.window, 0, 0, unsigned(attributes.width), unsigned(attributes.height), AllPlanes, ZPixmap);
    if (image == nullptr) return false;
    std::vector<uint8_t> rgb(size_t(attributes.width) * size_t(attributes.height) * 3);
    const auto channel = [](unsigned long pixel, unsigned long mask) {
        if (mask == 0) return uint8_t(0);
        int shift = 0;
        while (((mask >> shift) & 1) == 0) shift++;
        const unsigned long value = (pixel & mask) >> shift;
        const unsigned long maximum = mask >> shift;
        return uint8_t(value * 255 / maximum);
    };
    for (int y = 0; y < attributes.height; y++) {
        for (int x = 0; x < attributes.width; x++) {
            const unsigned long pixel = XGetPixel(image, x, y);
            uint8_t* out = &rgb[(size_t(y) * size_t(attributes.width) + size_t(x)) * 3];
            out[0] = channel(pixel, image->red_mask);
            out[1] = channel(pixel, image->green_mask);
            out[2] = channel(pixel, image->blue_mask);
        }
    }
    XDestroyImage(image);
    const bool written = writePng(path, attributes.width, attributes.height, rgb);
    std::printf("      %s: %d x %d%s\n", path.string().c_str(), attributes.width, attributes.height, written ? "" : " (NOT WRITTEN)");
    return written;
}

bool parsePoint(const char* text, int& x, int& y, double& time) {
    time = 1.0;
    return std::sscanf(text, "%d,%d,%lf", &x, &y, &time) >= 2;
}

void printUsage() {
    std::fprintf(stderr,
                 "Usage: vst3host [options] D110Emu.so\n"
                 "  --seconds S            Run for S seconds (default 3)\n"
                 "  --instances N          N plugin instances, each with its editor in a window (default 1)\n"
                 "  --scale F              The host's content scale for the editors\n"
                 "  --click X,Y[,T]        A left click in the first editor at T seconds (default 1)\n"
                 "  --right-click X,Y[,T]  A right click\n"
                 "  --hover X,Y[,T]        The pointer moved there (and left there)\n"
                 "  --key KEYCODE[,T]      An X keycode pressed and released 0.1 s later\n"
                 "  --drop FILE[,T]        FILE dropped on the first editor (XDND)\n"
                 "  --reopen T             The first editor closed and a new one opened at T, as when a host's window is\n"
                 "  --shot T,FILE          The first editor's window as PNG at T\n"
                 "  --screenshot FILE      Every editor's window as PNG at the end (FILE, then FILE-2.png...)\n"
                 "  --ignore-sigchld       Children reaped by the system, as some hosts have it (the plugin's file dialogs)\n");
}

}  // namespace

int main(int argc, char** argv) {
    useTestDataFolder("d110emu-vst3host", false);  // The plugin's settings (its window size) apart from the user's
    double duration = 3.0;
    int instanceCount = 1;
    float scale = 0.0f;
    std::vector<Action> actions;
    std::filesystem::path screenshotPath;
    std::string modulePath;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        Action action;
        if (arg == "--seconds" && hasValue) {
            duration = std::atof(argv[++i]);
        } else if (arg == "--instances" && hasValue) {
            instanceCount = std::max(1, std::atoi(argv[++i]));
        } else if (arg == "--scale" && hasValue) {
            scale = float(std::atof(argv[++i]));
        } else if ((arg == "--click" || arg == "--right-click" || arg == "--hover") && hasValue) {
            action.kind = arg == "--click" ? Action::Click : arg == "--hover" ? Action::Hover : Action::RightClick;
            if (!parsePoint(argv[++i], action.x, action.y, action.time)) {
                printUsage();
                return 2;
            }
            actions.push_back(action);
        } else if (arg == "--key" && hasValue) {
            action.kind = Action::Key;
            if (std::sscanf(argv[++i], "%d,%lf", &action.keycode, &action.time) < 1) {
                printUsage();
                return 2;
            }
            actions.push_back(action);
        } else if (arg == "--drop" && hasValue) {
            action.kind = Action::Drop;
            std::string value = argv[++i];
            const size_t comma = value.rfind(',');
            if (comma != std::string::npos && comma + 1 < value.size() && std::strspn(value.c_str() + comma + 1, "0123456789.") == value.size() - comma - 1) {
                action.time = std::atof(value.c_str() + comma + 1);
                value.resize(comma);
            }
            action.path = value;
            actions.push_back(action);
        } else if (arg == "--reopen" && hasValue) {
            action.kind = Action::Reopen;
            action.time = std::atof(argv[++i]);
            actions.push_back(action);
        } else if (arg == "--shot" && hasValue) {
            action.kind = Action::Shot;
            const std::string value = argv[++i];
            const size_t comma = value.find(',');
            if (comma == std::string::npos) {
                printUsage();
                return 2;
            }
            action.time = std::atof(value.substr(0, comma).c_str());
            action.path = value.substr(comma + 1);
            actions.push_back(action);
        } else if (arg == "--screenshot" && hasValue) {
            screenshotPath = argv[++i];
        } else if (arg == "--ignore-sigchld") {
            ::signal(SIGCHLD, SIG_IGN);
        } else if (!arg.empty() && arg[0] != '-' && modulePath.empty()) {
            modulePath = arg;
        } else {
            printUsage();
            return 2;
        }
    }
    if (modulePath.empty()) {
        printUsage();
        return 2;
    }
    Host host;
    host.scale = scale;
    host.display = XOpenDisplay(nullptr);
    if (host.display == nullptr) {
        std::fprintf(stderr, "No X display (DISPLAY is %s)\n", std::getenv("DISPLAY") != nullptr ? std::getenv("DISPLAY") : "not set");
        return 2;
    }
    const char* atomNames[] = {"XdndAware", "XdndEnter", "XdndPosition", "XdndStatus", "XdndDrop", "XdndFinished", "XdndSelection",
                               "XdndActionCopy", "text/uri-list"};
    Atom atoms[9] = {};
    XInternAtoms(host.display, const_cast<char**>(atomNames), 9, False, atoms);
    host.xdndAware = atoms[0];
    host.xdndEnter = atoms[1];
    host.xdndPosition = atoms[2];
    host.xdndStatus = atoms[3];
    host.xdndDrop = atoms[4];
    host.xdndFinished = atoms[5];
    host.xdndSelection = atoms[6];
    host.xdndActionCopy = atoms[7];
    host.uriList = atoms[8];
    host.dropSource = XCreateSimpleWindow(host.display, DefaultRootWindow(host.display), 0, 0, 1, 1, 0, 0, 0);

    // The plugin, loaded as hosts load a bundle's library.
    void* module = dlopen(modulePath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (module == nullptr) {
        std::fprintf(stderr, "Cannot load %s: %s\n", modulePath.c_str(), dlerror());
        return 2;
    }
    const auto moduleEntry = reinterpret_cast<bool (*)(void*)>(dlsym(module, "ModuleEntry"));
    const auto moduleExit = reinterpret_cast<bool (*)()>(dlsym(module, "ModuleExit"));
    const auto getFactory = reinterpret_cast<IPluginFactory*(PLUGIN_API*)()>(dlsym(module, "GetPluginFactory"));
    if (moduleEntry == nullptr || moduleExit == nullptr || getFactory == nullptr || !moduleEntry(module)) {
        std::fprintf(stderr, "%s lacks the VST3 entry points, or ModuleEntry failed\n", modulePath.c_str());
        return 2;
    }
    host.factory = getFactory();
    host.application = new HostApplication;
    IPluginFactory3* factory3 = nullptr;
    if (host.factory->queryInterface(IPluginFactory3::iid, reinterpret_cast<void**>(&factory3)) == kResultOk && factory3 != nullptr) {
        factory3->setHostContext(host.application);
        factory3->release();
    }
    PClassInfo processorInfo;
    host.factory->getClassInfo(0, &processorInfo);

    // The pointer out of the way (in a corner), so that nothing shows as hovered unless a test moves it there.
    XWarpPointer(host.display, 0, DefaultRootWindow(host.display), 0, 0, 0, 0, DisplayWidth(host.display, DefaultScreen(host.display)) - 1,
                 DisplayHeight(host.display, DefaultScreen(host.display)) - 1);
    XSync(host.display, False);

    const Clock::time_point start = Clock::now();
    host.instances.resize(size_t(instanceCount));
    for (int index = 0; index < instanceCount; index++) {
        Instance& instance = host.instances[size_t(index)];
        host.factory->createInstance(processorInfo.cid, Vst::IComponent::iid, reinterpret_cast<void**>(&instance.component));
        if (instance.component == nullptr) {
            fail("no processor");
            return 1;
        }
        instance.component->initialize(host.application);
        TUID controllerCid;
        instance.component->getControllerClassId(controllerCid);
        host.factory->createInstance(controllerCid, Vst::IEditController::iid, reinterpret_cast<void**>(&instance.controller));
        if (instance.controller == nullptr) {
            fail("no controller");
            return 1;
        }
        instance.controller->initialize(host.application);
        instance.component->queryInterface(Vst::IAudioProcessor::iid, reinterpret_cast<void**>(&instance.processor));
        instance.component->queryInterface(Vst::IConnectionPoint::iid, reinterpret_cast<void**>(&instance.componentPoint));
        instance.controller->queryInterface(Vst::IConnectionPoint::iid, reinterpret_cast<void**>(&instance.controllerPoint));
        if (instance.componentPoint != nullptr && instance.controllerPoint != nullptr) {
            instance.componentPoint->connect(instance.controllerPoint);
            instance.controllerPoint->connect(instance.componentPoint);
        }
        if (instance.processor != nullptr) {
            Vst::ProcessSetup setup = {Vst::kRealtime, Vst::kSample32, 480, 48000.0};
            instance.processor->setupProcessing(setup);
            instance.component->setActive(true);
            instance.processor->setProcessing(true);
        }
        openEditor(host, instance, index);
    }
    std::printf("      %d instance(s) up in %.2f s\n", instanceCount, seconds(start));

    // The audio: blocks of 480 frames every 10 ms for every instance, the first one's peak kept per half second.
    std::atomic<bool> audioRunning{true};
    std::vector<float> levels(size_t(duration * 2.0) + 2, 0.0f);
    std::atomic<int> blocks{0};
    const Clock::time_point audioStart = Clock::now();
    std::thread audio([&] {
        std::vector<float> left(480);
        std::vector<float> right(480);
        Clock::time_point next = Clock::now();
        while (audioRunning) {
            for (size_t index = 0; index < host.instances.size(); index++) {
                Vst::IAudioProcessor* processor = host.instances[index].processor;
                if (processor == nullptr) continue;
                float* channels[2] = {left.data(), right.data()};
                Vst::AudioBusBuffers bus;
                bus.numChannels = 2;
                bus.channelBuffers32 = channels;
                Vst::ProcessData data;
                data.processMode = Vst::kRealtime;
                data.symbolicSampleSize = Vst::kSample32;
                data.numSamples = 480;
                data.numOutputs = 1;
                data.outputs = &bus;
                processor->process(data);
                if (index != 0) continue;
                float peak = 0.0f;
                for (size_t i = 0; i < left.size(); i++) peak = std::max(peak, std::max(std::fabs(left[i]), std::fabs(right[i])));
                const size_t slot = std::min(levels.size() - 1, size_t(seconds(audioStart) * 2.0));
                levels[slot] = std::max(levels[slot], peak);
            }
            blocks++;
            // A block every 10 ms; when rendering falls behind (a sanitizer build), still a pause between blocks, as an
            // audio device's callbacks have, so that the UI thread gets the locks the rendering takes.
            next = std::max(next + std::chrono::milliseconds(10), Clock::now() + std::chrono::milliseconds(3));
            std::this_thread::sleep_until(next);
        }
    });

    // The event loop, with the actions at their times.
    const Clock::time_point loopStart = Clock::now();
    struct Release {
        double time;
        Window target;
        int type;
        int keycode;
        unsigned int button;
        int x;
        int y;
    };
    std::vector<Release> releases;
    while (seconds(loopStart) < duration) {
        const double now = seconds(loopStart);
        Instance& first = host.instances[0];
        for (Action& action : actions) {
            if (action.done || action.time > now) continue;
            action.done = true;
            const Window target = first.window != 0 ? pluginWindow(host.display, first.window) : 0;
            if (target == 0 && action.kind != Action::Reopen && action.kind != Action::Shot) {
                fail("no editor window for an action");
                continue;
            }
            switch (action.kind) {
            case Action::Click:
            case Action::RightClick: {
                // The pointer arrives first, a few frames before the press, as a hand's does (Dear ImGui's tabs and other
                // overlapping items take a click only where the pointer already was in the frame before).
                const unsigned int button = action.kind == Action::Click ? Button1 : Button3;
                sendPointer(host, target, MotionNotify, action.x, action.y, 0, 0);
                releases.push_back({now + 0.1, target, ButtonPress, 0, button, action.x, action.y});
                releases.push_back({now + 0.15, target, ButtonRelease, 0, button, action.x, action.y});
                releases.push_back({now + 0.3, target, LeaveNotify, 0, 0, 0, 0});
                std::printf("      %s at %d,%d (%.2f s)\n", action.kind == Action::Click ? "click" : "right click", action.x, action.y, now);
                break;
            }
            case Action::Hover:
                sendPointer(host, target, MotionNotify, action.x, action.y, 0, 0);
                std::printf("      pointer at %d,%d (%.2f s)\n", action.x, action.y, now);
                break;
            case Action::Key:
                sendKey(host, target, KeyPress, action.keycode);
                releases.push_back({now + 0.1, target, KeyRelease, action.keycode, 0, 0, 0});
                std::printf("      key %d (%.2f s)\n", action.keycode, now);
                break;
            case Action::Drop:
                startDrop(host, target, action.path);
                break;
            case Action::Reopen: {
                const auto closing = Clock::now();
                closeEditor(first);
                std::printf("      editor 1 closed in %.0f ms: %zu descriptor(s), %zu timer(s) left registered\n", seconds(closing) * 1000.0,
                            g_runLoop.handlers.size(), g_runLoop.timers.size());
                if (instanceCount == 1 && (!g_runLoop.handlers.empty() || !g_runLoop.timers.empty())) fail("the closed editor left handlers in the run loop");
                openEditor(host, first, 0);
                break;
            }
            case Action::Shot:
                if (!screenshot(host, first, action.path)) fail("screenshot " + action.path);
                break;
            }
        }
        for (size_t i = 0; i < releases.size();) {
            if (releases[i].time > now) {
                i++;
                continue;
            }
            const Release release = releases[i];
            releases.erase(releases.begin() + long(i));
            if (release.type == KeyRelease) {
                sendKey(host, release.target, KeyRelease, release.keycode);
            } else if (release.type == LeaveNotify) {
                sendPointer(host, release.target, LeaveNotify, 0, 0, 0, 0);
            } else if (release.type == ButtonPress) {
                sendPointer(host, release.target, ButtonPress, release.x, release.y, release.button, 0);
            } else {
                sendPointer(host, release.target, ButtonRelease, release.x, release.y, release.button,
                            release.button == Button1 ? Button1Mask : Button3Mask);
            }
        }
        runFor(host, 0.005);
    }
    std::printf("      ran %.1f s: %d timer calls, %d input calls, %d audio blocks\n", seconds(loopStart), g_runLoop.timerCalls,
                g_runLoop.eventCalls, blocks.load());
    std::string peaks;
    for (size_t slot = 0; slot + 1 < levels.size(); slot++) {
        char text[16];
        std::snprintf(text, sizeof(text), " %.3f", double(levels[slot]));
        peaks += text;
    }
    std::printf("      the first instance's peak level per half second:%s\n", peaks.c_str());
    // (At 16 ms a frame they would be called 60 times a second; the plugin skips frames where its drawing is slow.)
    if (g_runLoop.timerCalls < int(duration * 10.0)) fail("the editors' timers ran too seldom");

    if (!screenshotPath.empty()) {
        for (size_t index = 0; index < host.instances.size(); index++) {
            std::filesystem::path path = screenshotPath;
            if (index > 0) path.replace_filename(screenshotPath.stem().string() + "-" + std::to_string(index + 1) + screenshotPath.extension().string());
            if (!screenshot(host, host.instances[index], path)) fail("screenshot " + path.string());
        }
    }
    bool dropAsked = false;
    for (const Action& action : actions) dropAsked = dropAsked || action.kind == Action::Drop;
    if (dropAsked && !(host.dropFinished && host.dropSucceeded)) fail("the dropped file was not taken");

    // Everything closed as a host closes it: the audio, the editors (their handlers leave the run loop), the instances.
    audioRunning = false;
    audio.join();
    for (Instance& instance : host.instances) {
        if (instance.processor == nullptr) continue;
        instance.processor->setProcessing(false);
        instance.component->setActive(false);
        instance.processor->release();
        instance.processor = nullptr;
    }
    for (Instance& instance : host.instances) {
        closeEditor(instance);
        if (instance.window != 0) XDestroyWindow(host.display, instance.window);
        instance.window = 0;
    }
    std::printf("      editors closed: %zu descriptor(s), %zu timer(s) left registered\n", g_runLoop.handlers.size(), g_runLoop.timers.size());
    if (!g_runLoop.handlers.empty() || !g_runLoop.timers.empty()) fail("closed editors left handlers in the run loop");
    for (Instance& instance : host.instances) {
        if (instance.componentPoint != nullptr && instance.controllerPoint != nullptr) {
            instance.componentPoint->disconnect(instance.controllerPoint);
            instance.controllerPoint->disconnect(instance.componentPoint);
        }
        if (instance.componentPoint != nullptr) instance.componentPoint->release();
        if (instance.controllerPoint != nullptr) instance.controllerPoint->release();
        instance.controller->terminate();
        instance.component->terminate();
        instance.controller->release();
        instance.component->release();
    }
    host.instances.clear();
    moduleExit();
    dlclose(module);
    host.application->release();
    XDestroyWindow(host.display, host.dropSource);
    XCloseDisplay(host.display);
    std::printf("%s: %d failures\n", g_failures == 0 ? "ALL PASS" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
