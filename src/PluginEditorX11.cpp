// The VST3 plugin's window on Linux: an X11 child of the host's window (VST3's X11EmbedWindowID), drawn with OpenGL
// (GLX) through Dear ImGui's OpenGL3 backend; its input comes from X11 here, as Dear ImGui has no X11 backend. The host's
// event loop runs it (VST3's Linux::IRunLoop, through Vst3Plugin.cpp): events when the X connection has some, and a
// frame on a timer, all on the host's UI thread, as the Windows editor's messages and timer are.

#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "PluginCore.h"

#include "App.h"
#include "Platform.h"
#include "UiStyle.h"
#include "Vst2.h"  // VKEY_ values, which VST3's VirtualKeyCodes share
#include "imgui.h"
#include "imgui_impl_opengl3.h"

// X11's headers last: they define macros (None, Status, Bool, Success...) that would clash with names above.
#include <GL/glx.h>
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>

namespace {

constexpr int kBaseWidth = 1200;  // D110Emu's own window at 100% scaling
constexpr int kBaseHeight = 780;
constexpr long kXdndVersion = 5;

// Dear ImGui keeps the current context in a global, which all instances of the plugin share (they live in one library):
// each draws with its own context under this lock and puts back the one that was current.
std::recursive_mutex& uiMutex() {
    static std::recursive_mutex mutex;
    return mutex;
}

class ContextScope {
public:
    explicit ContextScope(ImGuiContext* context) : lock_(uiMutex()), previous_(ImGui::GetCurrentContext()) {
        ImGui::SetCurrentContext(context);
    }
    ~ContextScope() { ImGui::SetCurrentContext(previous_); }
    ContextScope(const ContextScope&) = delete;
    ContextScope& operator=(const ContextScope&) = delete;

private:
    std::lock_guard<std::recursive_mutex> lock_;
    ImGuiContext* previous_;
};

// Makes an editor's GL context current on its window (none at all without a window: the window has gone, and what is
// drawn then goes nowhere) and puts back whatever was current: the host may draw with OpenGL on its UI thread too.
class GlScope {
public:
    GlScope(Display* display, Window window, GLXContext context)
        : ours_(display), display_(glXGetCurrentDisplay()), draw_(glXGetCurrentDrawable()), read_(glXGetCurrentReadDrawable()),
          context_(glXGetCurrentContext()) {
        current_ = window != 0 && glXMakeCurrent(display, window, context);
        if (!current_) glXMakeCurrent(display, 0, nullptr);
    }
    ~GlScope() {
        if (context_ != nullptr && display_ != nullptr) {
            glXMakeContextCurrent(display_, draw_, read_, context_);
        } else {
            glXMakeCurrent(ours_, 0, nullptr);
        }
    }
    GlScope(const GlScope&) = delete;
    GlScope& operator=(const GlScope&) = delete;
    bool current() const { return current_; }

private:
    Display* ours_;
    Display* display_;
    GLXDrawable draw_;
    GLXDrawable read_;
    GLXContext context_;
    bool current_ = false;
};

// X errors on the editors' own connections (a request on a window the host had already destroyed with its own, say)
// must not end the host, which Xlib's default handler does: they are ignored, and others go to the handler before.
std::vector<Display*>& editorDisplays() {
    static std::vector<Display*> displays;
    return displays;
}
XErrorHandler g_previousErrorHandler = nullptr;
bool g_errorHandlerSet = false;

int onXError(Display* display, XErrorEvent* error) {
    const std::vector<Display*>& displays = editorDisplays();
    if (std::find(displays.begin(), displays.end(), display) != displays.end()) return 0;
    return g_previousErrorHandler != nullptr ? g_previousErrorHandler(display, error) : 0;
}

void addEditorDisplay(Display* display) {
    std::lock_guard<std::recursive_mutex> lock(uiMutex());
    editorDisplays().push_back(display);
    if (!g_errorHandlerSet) {
        g_previousErrorHandler = XSetErrorHandler(onXError);
        g_errorHandlerSet = true;
    }
}

void removeEditorDisplay(Display* display) {
    std::lock_guard<std::recursive_mutex> lock(uiMutex());
    std::vector<Display*>& displays = editorDisplays();
    displays.erase(std::remove(displays.begin(), displays.end(), display), displays.end());
    if (displays.empty() && g_errorHandlerSet) {
        // Put back the handler before ours, unless another has been set on top of ours since.
        const XErrorHandler current = XSetErrorHandler(g_previousErrorHandler);
        if (current != onXError) XSetErrorHandler(current);
        g_errorHandlerSet = false;
    }
}

// The screen's scale from the X resources (Xft.dpi, which desktops set for HiDPI screens), 1 without it.
float resourceScale(Display* display) {
    const char* resources = XResourceManagerString(display);
    if (resources == nullptr) return 1.0f;
    const char* entry = std::strstr(resources, "Xft.dpi:");
    if (entry == nullptr) return 1.0f;
    const float dpi = float(std::atof(entry + 8));
    return dpi >= 48.0f && dpi <= 480.0f ? dpi / 96.0f : 1.0f;
}

void appendUtf8(std::string& text, uint32_t code) {
    if (code < 0x80) {
        text += char(code);
    } else if (code < 0x800) {
        text += char(0xC0 | (code >> 6));
        text += char(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
        text += char(0xE0 | (code >> 12));
        text += char(0x80 | ((code >> 6) & 0x3F));
        text += char(0x80 | (code & 0x3F));
    } else {
        text += char(0xF0 | (code >> 18));
        text += char(0x80 | ((code >> 12) & 0x3F));
        text += char(0x80 | ((code >> 6) & 0x3F));
        text += char(0x80 | (code & 0x3F));
    }
}

// The key caps of PC scancodes (set 1) in the X server's keyboard layout, for Platform::keyName (the computer-keyboard
// piano's hint); read when a window opens. Only the UI thread uses them.
std::array<std::string, 0x59> g_keyNames;

std::string x11KeyName(int scancode) {
    return scancode > 0 && scancode < int(g_keyNames.size()) ? g_keyNames[size_t(scancode)] : std::string();
}

void readKeyNames(Display* display) {
    for (size_t scancode = 1; scancode < g_keyNames.size(); scancode++) {
        // X's keycodes are the kernel's plus 8, and the kernel's of the main block are the PC scancodes.
        const KeySym keysym = XkbKeycodeToKeysym(display, KeyCode(scancode + 8), 0, 0);
        uint32_t code = 0;
        if ((keysym >= 0x20 && keysym <= 0x7E) || (keysym >= 0xA0 && keysym <= 0xFF)) {
            code = uint32_t(keysym);  // Latin-1 keysyms are their characters
        } else if ((keysym & 0xFF000000) == 0x01000000) {
            code = uint32_t(keysym & 0x00FFFFFF);  // Unicode keysyms
        }
        std::string name;
        if (code != 0) {
            // Key caps show capitals, as the Windows and SDL names do.
            if ((code >= 'a' && code <= 'z') || (code >= 0xE0 && code <= 0xFE && code != 0xF7)) code -= 0x20;
            appendUtf8(name, code);
        }
        g_keyNames[scancode] = name;
    }
    Platform::setKeyNameSource(&x11KeyName);
}

// Keys a host passes on (IPlugView::onKeyDown) that Dear ImGui takes as keys rather than as text.
ImGuiKey imguiKeyFromVirtual(int virtualKey) {
    switch (virtualKey) {
    case vst2::VKEY_BACK: return ImGuiKey_Backspace;
    case vst2::VKEY_TAB: return ImGuiKey_Tab;
    case vst2::VKEY_RETURN:
    case vst2::VKEY_ENTER: return ImGuiKey_Enter;
    case vst2::VKEY_ESCAPE: return ImGuiKey_Escape;
    case vst2::VKEY_END: return ImGuiKey_End;
    case vst2::VKEY_HOME: return ImGuiKey_Home;
    case vst2::VKEY_LEFT: return ImGuiKey_LeftArrow;
    case vst2::VKEY_UP: return ImGuiKey_UpArrow;
    case vst2::VKEY_RIGHT: return ImGuiKey_RightArrow;
    case vst2::VKEY_DOWN: return ImGuiKey_DownArrow;
    case vst2::VKEY_PAGEUP: return ImGuiKey_PageUp;
    case vst2::VKEY_NEXT:
    case vst2::VKEY_PAGEDOWN: return ImGuiKey_PageDown;
    case vst2::VKEY_INSERT: return ImGuiKey_Insert;
    case vst2::VKEY_DELETE: return ImGuiKey_Delete;
    default: return ImGuiKey_None;
    }
}

// An X keysym (the key's symbol without modifiers, in the user's layout) as Dear ImGui's key.
ImGuiKey imguiKeyFromKeysym(KeySym keysym) {
    if (keysym >= XK_a && keysym <= XK_z) return ImGuiKey(ImGuiKey_A + int(keysym - XK_a));
    if (keysym >= XK_A && keysym <= XK_Z) return ImGuiKey(ImGuiKey_A + int(keysym - XK_A));
    if (keysym >= XK_0 && keysym <= XK_9) return ImGuiKey(ImGuiKey_0 + int(keysym - XK_0));
    if (keysym >= XK_F1 && keysym <= XK_F12) return ImGuiKey(ImGuiKey_F1 + int(keysym - XK_F1));
    if (keysym >= XK_KP_0 && keysym <= XK_KP_9) return ImGuiKey(ImGuiKey_Keypad0 + int(keysym - XK_KP_0));
    switch (keysym) {
    case XK_Tab:
    case XK_ISO_Left_Tab: return ImGuiKey_Tab;
    case XK_Left: return ImGuiKey_LeftArrow;
    case XK_Right: return ImGuiKey_RightArrow;
    case XK_Up: return ImGuiKey_UpArrow;
    case XK_Down: return ImGuiKey_DownArrow;
    case XK_Page_Up: return ImGuiKey_PageUp;
    case XK_Page_Down: return ImGuiKey_PageDown;
    case XK_Home: return ImGuiKey_Home;
    case XK_End: return ImGuiKey_End;
    case XK_Insert: return ImGuiKey_Insert;
    case XK_Delete: return ImGuiKey_Delete;
    case XK_BackSpace: return ImGuiKey_Backspace;
    case XK_space: return ImGuiKey_Space;
    case XK_Return: return ImGuiKey_Enter;
    case XK_Escape: return ImGuiKey_Escape;
    case XK_apostrophe: return ImGuiKey_Apostrophe;
    case XK_comma: return ImGuiKey_Comma;
    case XK_minus: return ImGuiKey_Minus;
    case XK_period: return ImGuiKey_Period;
    case XK_slash: return ImGuiKey_Slash;
    case XK_semicolon: return ImGuiKey_Semicolon;
    case XK_equal: return ImGuiKey_Equal;
    case XK_bracketleft: return ImGuiKey_LeftBracket;
    case XK_backslash: return ImGuiKey_Backslash;
    case XK_bracketright: return ImGuiKey_RightBracket;
    case XK_grave: return ImGuiKey_GraveAccent;
    case XK_Caps_Lock: return ImGuiKey_CapsLock;
    case XK_Scroll_Lock: return ImGuiKey_ScrollLock;
    case XK_Num_Lock: return ImGuiKey_NumLock;
    case XK_Print: return ImGuiKey_PrintScreen;
    case XK_Pause: return ImGuiKey_Pause;
    case XK_KP_Decimal: return ImGuiKey_KeypadDecimal;
    case XK_KP_Divide: return ImGuiKey_KeypadDivide;
    case XK_KP_Multiply: return ImGuiKey_KeypadMultiply;
    case XK_KP_Subtract: return ImGuiKey_KeypadSubtract;
    case XK_KP_Add: return ImGuiKey_KeypadAdd;
    case XK_KP_Enter: return ImGuiKey_KeypadEnter;
    case XK_KP_Equal: return ImGuiKey_KeypadEqual;
    case XK_Shift_L: return ImGuiKey_LeftShift;
    case XK_Shift_R: return ImGuiKey_RightShift;
    case XK_Control_L: return ImGuiKey_LeftCtrl;
    case XK_Control_R: return ImGuiKey_RightCtrl;
    case XK_Alt_L:
    case XK_Meta_L: return ImGuiKey_LeftAlt;
    case XK_Alt_R:
    case XK_Meta_R:
    case XK_ISO_Level3_Shift: return ImGuiKey_RightAlt;
    case XK_Super_L: return ImGuiKey_LeftSuper;
    case XK_Super_R: return ImGuiKey_RightSuper;
    case XK_Menu: return ImGuiKey_Menu;
    default: return ImGuiKey_None;
    }
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// The first local file of a text/uri-list (file:// URIs, percent-encoded), or empty.
std::filesystem::path firstFileOfUriList(const std::string& list) {
    size_t start = 0;
    while (start < list.size()) {
        size_t end = list.find('\n', start);
        if (end == std::string::npos) end = list.size();
        std::string line = list.substr(start, end - start);
        start = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\0')) line.pop_back();
        if (line.empty() || line[0] == '#' || line.compare(0, 7, "file://") != 0) continue;
        const size_t pathStart = line.find('/', 7);  // After the host, usually empty
        if (pathStart == std::string::npos) continue;
        std::string path;
        for (size_t i = pathStart; i < line.size(); i++) {
            if (line[i] == '%' && i + 2 < line.size() && hexDigit(line[i + 1]) >= 0 && hexDigit(line[i + 2]) >= 0) {
                path += char(hexDigit(line[i + 1]) * 16 + hexDigit(line[i + 2]));
                i += 2;
            } else {
                path += line[i];
            }
        }
        return Platform::fromUtf8(path);
    }
    return std::filesystem::path();
}

class PluginEditorX11 : public PluginEditor {
public:
    explicit PluginEditorX11(PluginCore& core) : core_(core) {}
    ~PluginEditorX11() override;
    PluginEditorX11(const PluginEditorX11&) = delete;
    PluginEditorX11& operator=(const PluginEditorX11&) = delete;

    bool open(void* parentWindow) override;
    void close() override;
    void size(int& width, int& height) override;
    bool key(int character, int virtualKey, bool shift, bool control, bool alt, bool down) override;
    void drawViewMenu() override;
    void setHostResize(std::function<bool(int, int)> resize) override { hostResize_ = std::move(resize); }
    void setSize(int width, int height) override;
    void setScale(float scale) override;
    int eventDescriptor() override { return display_ != nullptr ? ConnectionNumber(display_) : -1; }
    void processEvents() override;
    void idle() override;

private:
    struct Atoms {
        Atom xdndAware, xdndEnter, xdndPosition, xdndStatus, xdndLeave, xdndDrop, xdndFinished, xdndSelection, xdndActionCopy,
            xdndTypeList, uriList, xembedInfo;
    };

    void handle(XEvent& event);
    void keyEvent(XKeyEvent& event, bool down);
    void setModifiers(unsigned int state, KeySym changed, bool down);
    void dragAndDrop(const XClientMessageEvent& message);
    void dropped(const XSelectionEvent& selection);
    void sendXdnd(Window target, Atom type, long data1, long data2, long data3, long data4);
    void render();
    void applyScale(float scale);   // The standalone's style and 16-pixel text, scaled (with this context current)
    void computeSize(int& width, int& height) const;  // The window's size at dpiScale_ and zoom_, within the screen
    float scale() const { return dpiScale_ * float(zoom_) / 100.0f; }
    // The scale the window is drawn at: the host's (VST3's IPlugViewContentScaleSupport), else the X resources'.
    float screenScale(Display* display) const { return hostScale_ > 0.0f ? hostScale_ : resourceScale(display); }
    bool resizeHostWindow(int width, int height) { return hostResize_ && hostResize_(width, height); }
    void setCursor(ImGuiMouseCursor cursor);
    void destroyWindow();

    PluginCore& core_;
    std::function<bool(int, int)> hostResize_;
    float hostScale_ = 0.0f;
    Display* display_ = nullptr;  // A connection of the editor's own, used only on the host's UI thread
    Window window_ = 0;
    bool windowDestroyed_ = false;  // With the host's window, before the host closed the editor
    bool mapped_ = false;
    Colormap colormap_ = 0;
    GLXContext gl_ = nullptr;
    XIM inputMethod_ = nullptr;
    XIC inputContext_ = nullptr;
    Atoms atoms_ = {};
    Window dropSource_ = 0;  // XDND: the window a drag comes from, while it is over this one
    int dropVersion_ = 0;
    bool dropAccepted_ = false;  // It offers file names
    std::map<unsigned int, Cursor> cursors_;
    ImGuiMouseCursor cursor_ = ImGuiMouseCursor_Arrow;
    int screenWidth_ = 1920;
    int screenHeight_ = 1080;
    // Kept while the plugin lives, as the App's timers run on its clock; the backend only while the window is open.
    ImGuiContext* context_ = nullptr;
    ImGuiIO* io_ = nullptr;
    bool backendsReady_ = false;
    std::chrono::steady_clock::time_point lastFrame_{};
    std::chrono::steady_clock::time_point nextFrame_{};  // Not before: frames leave the host at least half its UI thread
    float dpiScale_ = 1.0f;
    float styleScale_ = 0.0f;  // The scale the style was last built for
    int zoom_ = 100;           // Percent
    bool zoomChanged_ = false; // Asks the host for the new size before the next frame
    int width_ = 0;            // The window's size, in pixels
    int height_ = 0;
    bool inFrame_ = false;
    bool closePending_ = false;  // The host closed the editor during a frame
};

PluginEditorX11::~PluginEditorX11() {
    closePending_ = false;
    inFrame_ = false;
    close();
    if (context_ != nullptr) {
        std::lock_guard<std::recursive_mutex> lock(uiMutex());
        ImGui::DestroyContext(context_);  // Puts back the context that was current, unless it was this one
        context_ = nullptr;
        io_ = nullptr;
    }
}

bool PluginEditorX11::open(void* parentWindow) {
    close();
    const Window parent = Window(reinterpret_cast<uintptr_t>(parentWindow));
    if (parent == 0) return false;
    display_ = XOpenDisplay(nullptr);
    if (display_ == nullptr) {
        std::fprintf(stderr, "D110Emu: cannot open the X display for the plugin window\n");
        return false;
    }
    addEditorDisplay(display_);
    const int screen = DefaultScreen(display_);
    screenWidth_ = DisplayWidth(display_, screen);
    screenHeight_ = DisplayHeight(display_, screen);

    // A double-buffered true-colour configuration for OpenGL, preferably of the screen's depth (a 32-bit visual would
    // make the window see-through under a compositor).
    static const int kAttributes[] = {GLX_X_RENDERABLE, True, GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT, GLX_RENDER_TYPE, GLX_RGBA_BIT,
                                      GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8,
                                      GLX_DOUBLEBUFFER, True, 0};
    int count = 0;
    GLXFBConfig* configs = glXChooseFBConfig(display_, screen, kAttributes, &count);
    GLXFBConfig config = nullptr;
    XVisualInfo* visual = nullptr;
    for (int i = 0; configs != nullptr && i < count; i++) {
        XVisualInfo* candidate = glXGetVisualFromFBConfig(display_, configs[i]);
        if (candidate == nullptr) continue;
        if (visual == nullptr || candidate->depth == DefaultDepth(display_, screen)) {
            if (visual != nullptr) XFree(visual);
            visual = candidate;
            config = configs[i];
            if (candidate->depth == DefaultDepth(display_, screen)) break;
        } else {
            XFree(candidate);
        }
    }
    if (configs != nullptr) XFree(configs);
    if (visual == nullptr) {
        std::fprintf(stderr, "D110Emu: no OpenGL configuration for the plugin window\n");
        destroyWindow();
        return false;
    }
    zoom_ = core_.editorZoom();
    dpiScale_ = screenScale(display_);
    computeSize(width_, height_);
    colormap_ = XCreateColormap(display_, RootWindow(display_, screen), visual->visual, AllocNone);
    XSetWindowAttributes attributes = {};
    attributes.colormap = colormap_;
    attributes.border_pixel = 0;
    attributes.background_pixmap = 0;  // None: no flash of a background before the first frame
    attributes.event_mask = ExposureMask | StructureNotifyMask | KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
                            PointerMotionMask | EnterWindowMask | LeaveWindowMask | FocusChangeMask;
    windowDestroyed_ = false;
    mapped_ = false;
    window_ = XCreateWindow(display_, parent, 0, 0, unsigned(width_), unsigned(height_), 0, visual->depth, InputOutput, visual->visual,
                            CWColormap | CWBorderPixel | CWBackPixmap | CWEventMask, &attributes);
    XFree(visual);
    if (window_ == 0) {
        destroyWindow();
        return false;
    }
    const char* atomNames[] = {"XdndAware",    "XdndEnter",     "XdndPosition",   "XdndStatus",   "XdndLeave",     "XdndDrop",
                               "XdndFinished", "XdndSelection", "XdndActionCopy", "XdndTypeList", "text/uri-list", "_XEMBED_INFO"};
    Atom atoms[12] = {};
    XInternAtoms(display_, const_cast<char**>(atomNames), 12, False, atoms);
    atoms_ = {atoms[0], atoms[1], atoms[2], atoms[3], atoms[4], atoms[5], atoms[6], atoms[7], atoms[8], atoms[9], atoms[10], atoms[11]};
    // Files dropped on the window open as in D110Emu's own (from drag-and-drop sources that look into child windows).
    const Atom version = Atom(kXdndVersion);
    XChangeProperty(display_, window_, atoms_.xdndAware, XA_ATOM, 32, PropModeReplace, reinterpret_cast<const unsigned char*>(&version), 1);
    // For hosts that embed with XEmbed: version 0, mapped.
    const long xembedInfo[2] = {0, 1};
    XChangeProperty(display_, window_, atoms_.xembedInfo, atoms_.xembedInfo, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(xembedInfo), 2);
    XMapWindow(display_, window_);
    XkbSetDetectableAutoRepeat(display_, True, nullptr);  // Held keys repeat as presses alone, without releases between
    inputMethod_ = XOpenIM(display_, nullptr, nullptr, nullptr);
    if (inputMethod_ != nullptr) {
        inputContext_ = XCreateIC(inputMethod_, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, window_, XNFocusWindow,
                                  window_, nullptr);
    }
    readKeyNames(display_);
    gl_ = glXCreateNewContext(display_, config, GLX_RGBA_TYPE, nullptr, True);
    if (gl_ == nullptr) {
        std::fprintf(stderr, "D110Emu: cannot create an OpenGL context for the plugin window\n");
        destroyWindow();
        return false;
    }
    XSync(display_, False);

    bool ready = false;
    {
        GlScope gl(display_, window_, gl_);
        if (gl.current()) {
            // No wait for the display's refresh: this runs on the host's UI thread, with the other plugins' windows.
            using SwapIntervalExt = void (*)(Display*, GLXDrawable, int);
            using SwapIntervalMesa = int (*)(unsigned int);
            if (const auto ext = reinterpret_cast<SwapIntervalExt>(glXGetProcAddressARB(reinterpret_cast<const GLubyte*>("glXSwapIntervalEXT")))) {
                ext(display_, window_, 0);
            } else if (const auto mesa = reinterpret_cast<SwapIntervalMesa>(glXGetProcAddressARB(reinterpret_cast<const GLubyte*>("glXSwapIntervalMESA")))) {
                mesa(0);
            }
            // GLSL 1.30 from OpenGL 3.0 on; 1.20 for older contexts (a Raspberry Pi 4's desktop OpenGL is 2.1).
            const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
            const char* glsl = version != nullptr && std::atoi(version) >= 3 ? "#version 130" : "#version 120";

            std::lock_guard<std::recursive_mutex> lock(uiMutex());
            const bool newContext = context_ == nullptr;
            if (newContext) {
                ImGuiContext* previous = ImGui::GetCurrentContext();
                context_ = ImGui::CreateContext();
                ImGui::SetCurrentContext(context_);
                io_ = &ImGui::GetIO();
                io_->IniFilename = nullptr;  // A single fixed layout, as in the standalone
                io_->BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
                ImGui::SetCurrentContext(previous);
            }
            ContextScope scope(context_);
            styleScale_ = 0.0f;  // The style is built for the window's scale at the first frame
            if (ImGui_ImplOpenGL3_Init(glsl)) {
                if (newContext) UiStyle::loadFonts();  // Kept with the context; the new renderer uploads them again
                backendsReady_ = true;
                ready = true;
            }
        }
    }
    if (!ready) {
        std::fprintf(stderr, "D110Emu: cannot draw in the plugin window with OpenGL\n");
        destroyWindow();
        return false;
    }
    lastFrame_ = std::chrono::steady_clock::now();
    XFlush(display_);
    return true;
}

void PluginEditorX11::close() {
    if (display_ == nullptr) return;
    if (inFrame_) {
        // Inside the App's frame: the window goes once the frame is over.
        closePending_ = true;
        if (!windowDestroyed_ && window_ != 0) XUnmapWindow(display_, window_);
        return;
    }
    closePending_ = false;
    destroyWindow();
}

void PluginEditorX11::destroyWindow() {
    if (display_ == nullptr) return;
    if (backendsReady_) {
        GlScope gl(display_, windowDestroyed_ ? 0 : window_, gl_);  // Without the window, the GL calls do nothing
        ContextScope scope(context_);
        ImGui_ImplOpenGL3_Shutdown();
        backendsReady_ = false;
    }
    if (gl_ != nullptr) {
        glXDestroyContext(display_, gl_);
        gl_ = nullptr;
    }
    if (inputContext_ != nullptr) {
        XDestroyIC(inputContext_);
        inputContext_ = nullptr;
    }
    if (inputMethod_ != nullptr) {
        XCloseIM(inputMethod_);
        inputMethod_ = nullptr;
    }
    for (const auto& cursor : cursors_) XFreeCursor(display_, cursor.second);
    cursors_.clear();
    cursor_ = ImGuiMouseCursor_Arrow;
    if (window_ != 0 && !windowDestroyed_) XDestroyWindow(display_, window_);
    window_ = 0;
    windowDestroyed_ = false;
    mapped_ = false;
    dropSource_ = 0;
    if (colormap_ != 0) {
        XFreeColormap(display_, colormap_);
        colormap_ = 0;
    }
    XSync(display_, False);  // Any error (a window gone with the host's) arrives while ours is the handler for it
    removeEditorDisplay(display_);
    XCloseDisplay(display_);
    display_ = nullptr;
}

void PluginEditorX11::size(int& width, int& height) {
    if (display_ != nullptr && window_ != 0 && !windowDestroyed_) {
        width = width_;
        height = height_;
        return;
    }
    // Before the window exists (hosts ask to size theirs): at the screen's scale, as open() makes it.
    zoom_ = core_.editorZoom();
    Display* display = XOpenDisplay(nullptr);
    if (display != nullptr) {
        dpiScale_ = screenScale(display);
        screenWidth_ = DisplayWidth(display, DefaultScreen(display));
        screenHeight_ = DisplayHeight(display, DefaultScreen(display));
        XCloseDisplay(display);
    } else {
        dpiScale_ = hostScale_ > 0.0f ? hostScale_ : 1.0f;
    }
    computeSize(width, height);
}

void PluginEditorX11::setSize(int width, int height) {
    if (display_ == nullptr || window_ == 0 || windowDestroyed_ || width <= 0 || height <= 0) return;
    if (width == width_ && height == height_) return;
    width_ = width;
    height_ = height;
    XResizeWindow(display_, window_, unsigned(width), unsigned(height));
    XFlush(display_);
}

void PluginEditorX11::setScale(float scale) {
    hostScale_ = scale > 0.0f ? scale : 0.0f;
    if (display_ != nullptr && screenScale(display_) != dpiScale_) {
        dpiScale_ = screenScale(display_);
        zoomChanged_ = true;  // The text and, if the host lets it, the window follow before the next frame
    }
}

void PluginEditorX11::computeSize(int& width, int& height) const {
    width = int(float(kBaseWidth) * scale() + 0.5f);
    height = int(float(kBaseHeight) * scale() + 0.5f);
    // Within the screen, leaving room for the host's frame around it.
    width = std::max(std::min(width, int(float(screenWidth_) * 0.95f)), 320);
    height = std::max(std::min(height, int(float(screenHeight_) * 0.85f)), 240);
}

bool PluginEditorX11::key(int character, int virtualKey, bool shift, bool control, bool alt, bool down) {
    // Only while a text field has the keyboard; otherwise the host keeps its keys (its shortcuts, its own keyboard).
    if (!backendsReady_ || io_ == nullptr || !io_->WantTextInput) return false;
    ContextScope scope(context_);
    io_->AddKeyEvent(ImGuiMod_Ctrl, control);
    io_->AddKeyEvent(ImGuiMod_Shift, shift);
    io_->AddKeyEvent(ImGuiMod_Alt, alt);
    const ImGuiKey key = imguiKeyFromVirtual(virtualKey);
    if (key != ImGuiKey_None) {
        io_->AddKeyEvent(key, down);
        return true;
    }
    if (control && character >= 'a' && character <= 'z') {  // Ctrl+A, C, V, X, Z, Y
        io_->AddKeyEvent(ImGuiKey(ImGuiKey_A + (character - 'a')), down);
        return true;
    }
    if (!down || control || alt) return true;
    unsigned int text = unsigned(character);
    if (virtualKey == vst2::VKEY_SPACE) text = ' ';
    if (virtualKey >= vst2::VKEY_NUMPAD0 && virtualKey <= vst2::VKEY_NUMPAD0 + 9) text = unsigned('0' + (virtualKey - vst2::VKEY_NUMPAD0));
    if (text >= 32 && text != 127) io_->AddInputCharacter(text);
    return true;
}

void PluginEditorX11::drawViewMenu() {
    if (!ImGui::BeginMenu("Window size")) {
        UiStyle::setItemTooltip("The plugin window with its text: 100%% is D110Emu's own window at the screen's scaling. "
                                "New instances open at the same size.");
        return;
    }
    for (int zoom : UiStyle::kZoomSteps) {
        const std::string label = std::to_string(zoom) + "%";
        if (ImGui::MenuItem(label.c_str(), nullptr, zoom == zoom_) && zoom != zoom_) {
            zoom_ = zoom;
            core_.setEditorZoom(zoom);
            zoomChanged_ = true;  // Resized before the next frame, not in the middle of this one
        }
    }
    ImGui::EndMenu();
}

// ---------------------------------------------------------------------------------------------
// Events

void PluginEditorX11::processEvents() {
    if (inFrame_) return;
    while (display_ != nullptr && XPending(display_) > 0) {
        XEvent event;
        XNextEvent(display_, &event);
        if (XFilterEvent(&event, 0)) continue;  // The input method's (a compose sequence)
        handle(event);
    }
}

void PluginEditorX11::setModifiers(unsigned int state, KeySym changed, bool down) {
    // The state an X event carries is from before it: a modifier key's own event changes its modifier.
    bool control = (state & ControlMask) != 0;
    bool shift = (state & ShiftMask) != 0;
    bool alt = (state & Mod1Mask) != 0;
    bool super = (state & Mod4Mask) != 0;
    if (changed == XK_Control_L || changed == XK_Control_R) control = down;
    if (changed == XK_Shift_L || changed == XK_Shift_R) shift = down;
    if (changed == XK_Alt_L || changed == XK_Alt_R || changed == XK_Meta_L || changed == XK_Meta_R) alt = down;
    if (changed == XK_Super_L || changed == XK_Super_R) super = down;
    io_->AddKeyEvent(ImGuiMod_Ctrl, control);
    io_->AddKeyEvent(ImGuiMod_Shift, shift);
    io_->AddKeyEvent(ImGuiMod_Alt, alt);
    io_->AddKeyEvent(ImGuiMod_Super, super);
}

void PluginEditorX11::keyEvent(XKeyEvent& event, bool down) {
    const KeySym keysym = XkbKeycodeToKeysym(display_, KeyCode(event.keycode), 0, 0);
    setModifiers(event.state, keysym, down);
    const ImGuiKey key = imguiKeyFromKeysym(keysym);
    if (key != ImGuiKey_None) io_->AddKeyEvent(key, down);
    if (down && (event.state & (ControlMask | Mod1Mask)) == 0) {
        char text[64] = {};
        int length = 0;
        if (inputContext_ != nullptr) {
            KeySym symbol = 0;
            Status status = 0;
            length = Xutf8LookupString(inputContext_, &event, text, int(sizeof(text)) - 1, &symbol, &status);
            if (status != XLookupChars && status != XLookupBoth) length = 0;
        } else {
            // Without an input method: Latin-1, as UTF-8.
            char latin[16] = {};
            const int count = XLookupString(&event, latin, int(sizeof(latin)), nullptr, nullptr);
            std::string utf8;
            for (int i = 0; i < count; i++) appendUtf8(utf8, static_cast<unsigned char>(latin[i]));
            length = int(std::min(utf8.size(), sizeof(text) - 1));
            std::memcpy(text, utf8.data(), size_t(length));
        }
        if (length > 0 && static_cast<unsigned char>(text[0]) >= 32 && text[0] != 127) {
            text[length] = '\0';
            io_->AddInputCharactersUTF8(text);
        }
    }
    // The computer-keyboard piano goes by physical key position: X's keycode is the kernel's plus 8, and the kernel's
    // codes of the main block are the PC scancodes (set 1) the Windows editor passes; the other keys never play notes.
    const int scancode = int(event.keycode) - 8;
    if (scancode >= 1 && scancode <= 0x58) core_.withApp([&](App& app) { app.onKey(scancode, down); });
}

void PluginEditorX11::handle(XEvent& event) {
    if (!backendsReady_ || io_ == nullptr) return;
    if (event.xany.window != window_) return;
    ContextScope scope(context_);
    switch (event.type) {
    case MotionNotify:
        setModifiers(event.xmotion.state, 0, false);
        io_->AddMousePosEvent(float(event.xmotion.x), float(event.xmotion.y));
        break;
    case EnterNotify:
        io_->AddMousePosEvent(float(event.xcrossing.x), float(event.xcrossing.y));
        break;
    case LeaveNotify:
        if (event.xcrossing.mode == NotifyNormal) io_->AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        break;
    case ButtonPress:
    case ButtonRelease: {
        const bool down = event.type == ButtonPress;
        setModifiers(event.xbutton.state, 0, false);
        io_->AddMousePosEvent(float(event.xbutton.x), float(event.xbutton.y));
        switch (event.xbutton.button) {
        case Button1: io_->AddMouseButtonEvent(0, down); break;
        case Button2: io_->AddMouseButtonEvent(2, down); break;
        case Button3: io_->AddMouseButtonEvent(1, down); break;
        case Button4: if (down) io_->AddMouseWheelEvent(0.0f, 1.0f); break;
        case Button5: if (down) io_->AddMouseWheelEvent(0.0f, -1.0f); break;
        case 6: if (down) io_->AddMouseWheelEvent(1.0f, 0.0f); break;
        case 7: if (down) io_->AddMouseWheelEvent(-1.0f, 0.0f); break;
        case 8: io_->AddMouseButtonEvent(3, down); break;
        case 9: io_->AddMouseButtonEvent(4, down); break;
        default: break;
        }
        // The keyboard (text fields, the computer-keyboard piano) follows a click, as in the Windows editor.
        if (down && event.xbutton.button <= Button3) XSetInputFocus(display_, window_, RevertToParent, CurrentTime);
        break;
    }
    case KeyPress:
    case KeyRelease:
        keyEvent(event.xkey, event.type == KeyPress);
        break;
    case FocusIn:
        io_->AddFocusEvent(true);
        if (inputContext_ != nullptr) XSetICFocus(inputContext_);
        break;
    case FocusOut:
        io_->AddFocusEvent(false);
        if (inputContext_ != nullptr) XUnsetICFocus(inputContext_);
        core_.withApp([](App& app) { app.clearKeys(); });
        break;
    case ConfigureNotify:
        if (event.xconfigure.width > 0 && event.xconfigure.height > 0) {
            width_ = event.xconfigure.width;
            height_ = event.xconfigure.height;
        }
        break;
    case MapNotify:
        mapped_ = true;
        break;
    case UnmapNotify:
        mapped_ = false;
        break;
    case DestroyNotify:
        windowDestroyed_ = true;  // With the host's window, before the host closed the editor
        break;
    case ClientMessage:
        dragAndDrop(event.xclient);
        break;
    case SelectionNotify:
        dropped(event.xselection);
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------------------------
// Drag and drop (XDND, freedesktop.org's protocol): a file dropped on the window opens as in D110Emu's own.

void PluginEditorX11::sendXdnd(Window target, Atom type, long data1, long data2, long data3, long data4) {
    XEvent message = {};
    message.xclient.type = ClientMessage;
    message.xclient.display = display_;
    message.xclient.window = target;
    message.xclient.message_type = type;
    message.xclient.format = 32;
    message.xclient.data.l[0] = long(window_);
    message.xclient.data.l[1] = data1;
    message.xclient.data.l[2] = data2;
    message.xclient.data.l[3] = data3;
    message.xclient.data.l[4] = data4;
    XSendEvent(display_, target, False, NoEventMask, &message);
    XFlush(display_);
}

void PluginEditorX11::dragAndDrop(const XClientMessageEvent& message) {
    if (message.format != 32) return;
    const Window source = Window(message.data.l[0]);
    if (message.message_type == atoms_.xdndEnter) {
        dropSource_ = source;
        dropVersion_ = int((unsigned long)(message.data.l[1]) >> 24);
        dropAccepted_ = false;
        if ((message.data.l[1] & 1) != 0) {
            // More than three types: the whole list is on the source's window.
            Atom type = 0;
            int format = 0;
            unsigned long count = 0;
            unsigned long after = 0;
            unsigned char* data = nullptr;
            if (XGetWindowProperty(display_, source, atoms_.xdndTypeList, 0, 1024, False, XA_ATOM, &type, &format, &count, &after, &data) ==
                    Success &&
                data != nullptr) {
                const Atom* types = reinterpret_cast<const Atom*>(data);
                for (unsigned long i = 0; i < count; i++) dropAccepted_ = dropAccepted_ || types[i] == atoms_.uriList;
                XFree(data);
            }
        } else {
            for (int i = 2; i <= 4; i++) dropAccepted_ = dropAccepted_ || Atom(message.data.l[i]) == atoms_.uriList;
        }
    } else if (message.message_type == atoms_.xdndPosition) {
        if (source != dropSource_) return;
        // Accepted anywhere in the window (an empty rectangle: positions keep coming), as a copy.
        sendXdnd(source, atoms_.xdndStatus, dropAccepted_ ? 1 : 0, 0, 0, dropAccepted_ ? long(atoms_.xdndActionCopy) : 0);
    } else if (message.message_type == atoms_.xdndLeave) {
        if (source == dropSource_) dropSource_ = 0;
    } else if (message.message_type == atoms_.xdndDrop) {
        if (source != dropSource_) return;
        if (!dropAccepted_) {
            sendXdnd(source, atoms_.xdndFinished, 0, 0, 0, 0);
            dropSource_ = 0;
            return;
        }
        // The file names come as the selection's text/uri-list (SelectionNotify, then dropped()).
        const Time time = dropVersion_ >= 1 ? Time(message.data.l[2]) : CurrentTime;
        XConvertSelection(display_, atoms_.xdndSelection, atoms_.uriList, atoms_.xdndSelection, window_, time);
        XFlush(display_);
    }
}

void PluginEditorX11::dropped(const XSelectionEvent& selection) {
    if (selection.selection != atoms_.xdndSelection || dropSource_ == 0) return;
    std::string list;
    if (selection.property != 0) {
        Atom type = 0;
        int format = 0;
        unsigned long count = 0;
        unsigned long after = 0;
        unsigned char* data = nullptr;
        if (XGetWindowProperty(display_, window_, selection.property, 0, 65536, True, AnyPropertyType, &type, &format, &count, &after, &data) ==
                Success &&
            data != nullptr) {
            if (format == 8) list.assign(reinterpret_cast<const char*>(data), count);
            XFree(data);
        }
    }
    const std::filesystem::path file = firstFileOfUriList(list);
    if (dropVersion_ >= 2) {
        sendXdnd(dropSource_, atoms_.xdndFinished, file.empty() ? 0 : 1, file.empty() ? 0 : long(atoms_.xdndActionCopy), 0, 0);
    }
    dropSource_ = 0;
    if (!file.empty()) core_.withApp([&](App& app) { app.openFile(file); });
}

void PluginEditorX11::setCursor(ImGuiMouseCursor cursor) {
    if (cursor == cursor_ || window_ == 0) return;
    cursor_ = cursor;
    unsigned int shape = XC_left_ptr;
    switch (cursor) {
    case ImGuiMouseCursor_TextInput: shape = XC_xterm; break;
    case ImGuiMouseCursor_ResizeAll: shape = XC_fleur; break;
    case ImGuiMouseCursor_ResizeNS: shape = XC_sb_v_double_arrow; break;
    case ImGuiMouseCursor_ResizeEW: shape = XC_sb_h_double_arrow; break;
    case ImGuiMouseCursor_ResizeNESW: shape = XC_bottom_left_corner; break;
    case ImGuiMouseCursor_ResizeNWSE: shape = XC_bottom_right_corner; break;
    case ImGuiMouseCursor_Hand: shape = XC_hand2; break;
    case ImGuiMouseCursor_NotAllowed: shape = XC_X_cursor; break;
    default: break;
    }
    auto found = cursors_.find(shape);
    if (found == cursors_.end()) found = cursors_.emplace(shape, XCreateFontCursor(display_, shape)).first;
    XDefineCursor(display_, window_, found->second);
}

// ---------------------------------------------------------------------------------------------
// Drawing

void PluginEditorX11::idle() {
    // At most half of the host's UI thread: where OpenGL is done in software (no GPU driver, a virtual machine), a
    // frame can take longer than the timer's 16 ms, and the host's own windows must keep running.
    if (std::chrono::steady_clock::now() < nextFrame_) {
        processEvents();
        return;
    }
    render();
}

void PluginEditorX11::render() {
    if (inFrame_ || display_ == nullptr || !backendsReady_) return;
    processEvents();  // Input first, and a window gone with the host's is known before drawing in it
    if (windowDestroyed_ || window_ == 0 || !mapped_) return;
    const auto frameStart = std::chrono::steady_clock::now();
    inFrame_ = true;
    {
        GlScope gl(display_, window_, gl_);
        if (gl.current()) {
            ContextScope scope(context_);
            if (zoomChanged_) {
                zoomChanged_ = false;
                int width = 0;
                int height = 0;
                computeSize(width, height);
                // Only as the host resizes its window: otherwise the text is scaled within the present size, and the new
                // size applies when the window opens again. A host may ask for the size (getSize) meanwhile.
                const int oldWidth = width_;
                const int oldHeight = height_;
                width_ = width;
                height_ = height;
                if (resizeHostWindow(width, height)) {
                    XResizeWindow(display_, window_, unsigned(width), unsigned(height));
                } else {
                    width_ = oldWidth;
                    height_ = oldHeight;
                }
            }
            if (scale() != styleScale_) applyScale(scale());

            const auto now = std::chrono::steady_clock::now();
            io_->DeltaTime = std::clamp(std::chrono::duration<float>(now - lastFrame_).count(), 0.0001f, 0.25f);
            lastFrame_ = now;
            io_->DisplaySize = ImVec2(float(width_), float(height_));
            io_->DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
            ImGui_ImplOpenGL3_NewFrame();
            ImGui::NewFrame();
            core_.drawFrame();
            ImGui::Render();
            setCursor(ImGui::GetMouseCursor());
            glViewport(0, 0, width_, height_);
            glClearColor(0.09f, 0.09f, 0.10f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glXSwapBuffers(display_, window_);
        }
    }
    XFlush(display_);
    const auto frameEnd = std::chrono::steady_clock::now();
    nextFrame_ = frameEnd + (frameEnd - frameStart);
    inFrame_ = false;
    core_.afterFrame();
    if (closePending_) close();
}

void PluginEditorX11::applyScale(float scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    App::applyStyle();
    style.FontSizeBase = 16.0f;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    styleScale_ = scale;
}

}  // namespace

std::unique_ptr<PluginEditor> createPluginEditor(PluginCore& core) {
    return std::unique_ptr<PluginEditor>(new PluginEditorX11(core));
}
