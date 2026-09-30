// The VST3 plugin's window on macOS: an NSView in the host's (VST3's NSView platform type), drawn with Metal through
// Dear ImGui's Metal backend. Its input comes from the view's own events, translated here as the Linux editor translates
// X11's: Dear ImGui's macOS backend watches every event of the whole application for one context, which suits neither
// a plugin in someone else's application nor several instances. A timer on the main run loop draws it about 60 times a
// second. Hosts call the view on the main thread, as AppKit wants, so everything here runs there.
//
// Compiled with ARC (-fobjc-arc): the editor's Objective-C members are strong references it gives up by itself.

// Carbon's AssertMacros.h would otherwise define check(), verify() and require() as macros.
#define __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES 0

#import <Carbon/Carbon.h>  // kVK_ key codes, and the keyboard layout (TIS, UCKeyTranslate) for the piano's key names
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>

#include "PluginCore.h"

#include "App.h"
#include "Platform.h"
#include "UiStyle.h"
#include "Vst2.h"  // VKEY_ values, which VST3's VirtualKeyCodes share
#include "imgui.h"
#include "imgui_impl_metal.h"
#include "imgui_internal.h"  // The text cursor's position for input methods (ImGuiContext::PlatformImeData)

class PluginEditorMac;

// The editor's view, in the host's. It passes its events to the editor until detach(): the host or AppKit may hold on
// to the view a little longer than the editor has it.
@interface D110EmuPluginView : NSView <NSTextInputClient>
- (instancetype)initWithEditor:(PluginEditorMac*)editor frame:(NSRect)frame;
- (void)detach;
- (void)frameTimer:(NSTimer*)timer;
@end

namespace {

constexpr int kBaseWidth = 1200;  // D110Emu's own window at 100%, in points
constexpr int kBaseHeight = 780;
constexpr NSTimeInterval kFrameInterval = 1.0 / 60.0;

// Dear ImGui keeps the current context in a global, which all instances of the plugin share (they live in one bundle):
// each draws with its own context under this lock and puts back the one that was current. A modal panel in one
// instance's frame runs the run loop, in which the other instances' windows keep drawing.
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

// ISO keyboards (with the extra key beside the left Shift) report that key and the one left of 1 the other way round
// (as SDL knows too).
bool isoKeyboard() {
    return KBGetLayoutType(LMGetKbdType()) == kKeyboardISO;
}

// A key's position (macOS's virtual key code, the same in every layout) as the PC scancode (set 1) of the key there,
// for the computer-keyboard piano, which goes by position as on the other systems; 0 for the keys it never plays.
int pcScancode(unsigned short keyCode) {
    switch (keyCode) {
    case kVK_Escape: return 0x01;
    case kVK_ANSI_1: return 0x02;
    case kVK_ANSI_2: return 0x03;
    case kVK_ANSI_3: return 0x04;
    case kVK_ANSI_4: return 0x05;
    case kVK_ANSI_5: return 0x06;
    case kVK_ANSI_6: return 0x07;
    case kVK_ANSI_7: return 0x08;
    case kVK_ANSI_8: return 0x09;
    case kVK_ANSI_9: return 0x0A;
    case kVK_ANSI_0: return 0x0B;
    case kVK_ANSI_Minus: return 0x0C;
    case kVK_ANSI_Equal: return 0x0D;
    case kVK_Delete: return 0x0E;  // Backspace
    case kVK_Tab: return 0x0F;
    case kVK_ANSI_Q: return 0x10;
    case kVK_ANSI_W: return 0x11;
    case kVK_ANSI_E: return 0x12;
    case kVK_ANSI_R: return 0x13;
    case kVK_ANSI_T: return 0x14;
    case kVK_ANSI_Y: return 0x15;
    case kVK_ANSI_U: return 0x16;
    case kVK_ANSI_I: return 0x17;
    case kVK_ANSI_O: return 0x18;
    case kVK_ANSI_P: return 0x19;
    case kVK_ANSI_LeftBracket: return 0x1A;
    case kVK_ANSI_RightBracket: return 0x1B;
    case kVK_Return: return 0x1C;
    case kVK_ANSI_A: return 0x1E;
    case kVK_ANSI_S: return 0x1F;
    case kVK_ANSI_D: return 0x20;
    case kVK_ANSI_F: return 0x21;
    case kVK_ANSI_G: return 0x22;
    case kVK_ANSI_H: return 0x23;
    case kVK_ANSI_J: return 0x24;
    case kVK_ANSI_K: return 0x25;
    case kVK_ANSI_L: return 0x26;
    case kVK_ANSI_Semicolon: return 0x27;
    case kVK_ANSI_Quote: return 0x28;
    case kVK_ANSI_Grave: return isoKeyboard() ? 0x56 : 0x29;
    case kVK_ANSI_Backslash: return 0x2B;
    case kVK_ANSI_Z: return 0x2C;
    case kVK_ANSI_X: return 0x2D;
    case kVK_ANSI_C: return 0x2E;
    case kVK_ANSI_V: return 0x2F;
    case kVK_ANSI_B: return 0x30;
    case kVK_ANSI_N: return 0x31;
    case kVK_ANSI_M: return 0x32;
    case kVK_ANSI_Comma: return 0x33;
    case kVK_ANSI_Period: return 0x34;
    case kVK_ANSI_Slash: return 0x35;
    case kVK_Space: return 0x39;
    case kVK_ISO_Section: return isoKeyboard() ? 0x29 : 0x56;
    default: return 0;
    }
}

// Keys by position, as Dear ImGui takes them (letters and digits go by the layout instead: see keyOf()).
ImGuiKey imguiKey(unsigned short keyCode) {
    switch (keyCode) {
    case kVK_ANSI_A: return ImGuiKey_A;
    case kVK_ANSI_B: return ImGuiKey_B;
    case kVK_ANSI_C: return ImGuiKey_C;
    case kVK_ANSI_D: return ImGuiKey_D;
    case kVK_ANSI_E: return ImGuiKey_E;
    case kVK_ANSI_F: return ImGuiKey_F;
    case kVK_ANSI_G: return ImGuiKey_G;
    case kVK_ANSI_H: return ImGuiKey_H;
    case kVK_ANSI_I: return ImGuiKey_I;
    case kVK_ANSI_J: return ImGuiKey_J;
    case kVK_ANSI_K: return ImGuiKey_K;
    case kVK_ANSI_L: return ImGuiKey_L;
    case kVK_ANSI_M: return ImGuiKey_M;
    case kVK_ANSI_N: return ImGuiKey_N;
    case kVK_ANSI_O: return ImGuiKey_O;
    case kVK_ANSI_P: return ImGuiKey_P;
    case kVK_ANSI_Q: return ImGuiKey_Q;
    case kVK_ANSI_R: return ImGuiKey_R;
    case kVK_ANSI_S: return ImGuiKey_S;
    case kVK_ANSI_T: return ImGuiKey_T;
    case kVK_ANSI_U: return ImGuiKey_U;
    case kVK_ANSI_V: return ImGuiKey_V;
    case kVK_ANSI_W: return ImGuiKey_W;
    case kVK_ANSI_X: return ImGuiKey_X;
    case kVK_ANSI_Y: return ImGuiKey_Y;
    case kVK_ANSI_Z: return ImGuiKey_Z;
    case kVK_ANSI_0: return ImGuiKey_0;
    case kVK_ANSI_1: return ImGuiKey_1;
    case kVK_ANSI_2: return ImGuiKey_2;
    case kVK_ANSI_3: return ImGuiKey_3;
    case kVK_ANSI_4: return ImGuiKey_4;
    case kVK_ANSI_5: return ImGuiKey_5;
    case kVK_ANSI_6: return ImGuiKey_6;
    case kVK_ANSI_7: return ImGuiKey_7;
    case kVK_ANSI_8: return ImGuiKey_8;
    case kVK_ANSI_9: return ImGuiKey_9;
    case kVK_ANSI_Minus: return ImGuiKey_Minus;
    case kVK_ANSI_Equal: return ImGuiKey_Equal;
    case kVK_ANSI_LeftBracket: return ImGuiKey_LeftBracket;
    case kVK_ANSI_RightBracket: return ImGuiKey_RightBracket;
    case kVK_ANSI_Semicolon: return ImGuiKey_Semicolon;
    case kVK_ANSI_Quote: return ImGuiKey_Apostrophe;
    case kVK_ANSI_Backslash: return ImGuiKey_Backslash;
    case kVK_ANSI_Comma: return ImGuiKey_Comma;
    case kVK_ANSI_Period: return ImGuiKey_Period;
    case kVK_ANSI_Slash: return ImGuiKey_Slash;
    case kVK_ANSI_Grave: return ImGuiKey_GraveAccent;
    case kVK_ANSI_Keypad0: return ImGuiKey_Keypad0;
    case kVK_ANSI_Keypad1: return ImGuiKey_Keypad1;
    case kVK_ANSI_Keypad2: return ImGuiKey_Keypad2;
    case kVK_ANSI_Keypad3: return ImGuiKey_Keypad3;
    case kVK_ANSI_Keypad4: return ImGuiKey_Keypad4;
    case kVK_ANSI_Keypad5: return ImGuiKey_Keypad5;
    case kVK_ANSI_Keypad6: return ImGuiKey_Keypad6;
    case kVK_ANSI_Keypad7: return ImGuiKey_Keypad7;
    case kVK_ANSI_Keypad8: return ImGuiKey_Keypad8;
    case kVK_ANSI_Keypad9: return ImGuiKey_Keypad9;
    case kVK_ANSI_KeypadDecimal: return ImGuiKey_KeypadDecimal;
    case kVK_ANSI_KeypadMultiply: return ImGuiKey_KeypadMultiply;
    case kVK_ANSI_KeypadPlus: return ImGuiKey_KeypadAdd;
    case kVK_ANSI_KeypadMinus: return ImGuiKey_KeypadSubtract;
    case kVK_ANSI_KeypadDivide: return ImGuiKey_KeypadDivide;
    case kVK_ANSI_KeypadEnter: return ImGuiKey_KeypadEnter;
    case kVK_ANSI_KeypadEquals: return ImGuiKey_KeypadEqual;
    case kVK_ANSI_KeypadClear: return ImGuiKey_NumLock;
    case kVK_Return: return ImGuiKey_Enter;
    case kVK_Tab: return ImGuiKey_Tab;
    case kVK_Space: return ImGuiKey_Space;
    case kVK_Delete: return ImGuiKey_Backspace;
    case kVK_ForwardDelete: return ImGuiKey_Delete;
    case kVK_Escape: return ImGuiKey_Escape;
    case kVK_Help: return ImGuiKey_Insert;
    case kVK_Home: return ImGuiKey_Home;
    case kVK_End: return ImGuiKey_End;
    case kVK_PageUp: return ImGuiKey_PageUp;
    case kVK_PageDown: return ImGuiKey_PageDown;
    case kVK_LeftArrow: return ImGuiKey_LeftArrow;
    case kVK_RightArrow: return ImGuiKey_RightArrow;
    case kVK_UpArrow: return ImGuiKey_UpArrow;
    case kVK_DownArrow: return ImGuiKey_DownArrow;
    case kVK_CapsLock: return ImGuiKey_CapsLock;
    case kVK_F1: return ImGuiKey_F1;
    case kVK_F2: return ImGuiKey_F2;
    case kVK_F3: return ImGuiKey_F3;
    case kVK_F4: return ImGuiKey_F4;
    case kVK_F5: return ImGuiKey_F5;
    case kVK_F6: return ImGuiKey_F6;
    case kVK_F7: return ImGuiKey_F7;
    case kVK_F8: return ImGuiKey_F8;
    case kVK_F9: return ImGuiKey_F9;
    case kVK_F10: return ImGuiKey_F10;
    case kVK_F11: return ImGuiKey_F11;
    case kVK_F12: return ImGuiKey_F12;
    default: return ImGuiKey_None;
    }
}

// A key event's key: letters and digits as the keyboard layout has them (shortcuts such as Cmd+Z go by letter, as in
// D110Emu's own window), the other keys by position. Only for key-down and key-up events.
ImGuiKey keyOf(NSEvent* event) {
    const ImGuiKey positional = imguiKey(event.keyCode);
    const bool alphanumeric = (positional >= ImGuiKey_A && positional <= ImGuiKey_Z) || (positional >= ImGuiKey_0 && positional <= ImGuiKey_9);
    NSString* characters = event.charactersIgnoringModifiers;
    if (!alphanumeric || characters.length != 1) return positional;
    const unichar c = [characters characterAtIndex:0];
    if (c >= 'a' && c <= 'z') return ImGuiKey(ImGuiKey_A + (c - 'a'));
    if (c >= 'A' && c <= 'Z') return ImGuiKey(ImGuiKey_A + (c - 'A'));
    if (c >= '0' && c <= '9') return ImGuiKey(ImGuiKey_0 + (c - '0'));
    return positional;
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

NSCursor* cocoaCursor(ImGuiMouseCursor cursor) {
    switch (cursor) {
    case ImGuiMouseCursor_TextInput: return [NSCursor IBeamCursor];
    case ImGuiMouseCursor_ResizeAll: return [NSCursor closedHandCursor];
    case ImGuiMouseCursor_ResizeNS: return [NSCursor resizeUpDownCursor];
    case ImGuiMouseCursor_ResizeEW: return [NSCursor resizeLeftRightCursor];
    case ImGuiMouseCursor_Hand: return [NSCursor pointingHandCursor];
    case ImGuiMouseCursor_NotAllowed: return [NSCursor operationNotAllowedCursor];
    default: return [NSCursor arrowCursor];  // Also the diagonal resizes, which macOS has no public cursors for
    }
}

// ---------------------------------------------------------------------------------------------
// Dialogs and key names: Platform's toolkit in the plugin, which has no SDL

// The key caps of PC scancodes (set 1) in the current keyboard layout, for Platform::keyName (the computer-keyboard
// piano's hint); read when a window opens.
std::array<std::string, 0x59> g_keyNames;

void readKeyNames() {
    for (std::string& name : g_keyNames) name.clear();
    TISInputSourceRef source = TISCopyCurrentKeyboardLayoutInputSource();
    CFDataRef data = source != nullptr ? static_cast<CFDataRef>(TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData)) : nullptr;
    if (data == nullptr) {
        // An input method without a layout of its own (Japanese, say): the layout it types letters with.
        if (source != nullptr) CFRelease(source);
        source = TISCopyCurrentASCIICapableKeyboardLayoutInputSource();
        data = source != nullptr ? static_cast<CFDataRef>(TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData)) : nullptr;
    }
    if (data != nullptr) {
        const auto* layout = reinterpret_cast<const UCKeyboardLayout*>(CFDataGetBytePtr(data));
        const UInt32 keyboardType = LMGetKbdType();
        for (unsigned short keyCode = 0; keyCode < 0x80; keyCode++) {
            const int scancode = pcScancode(keyCode);
            if (scancode <= 0 || scancode >= int(g_keyNames.size())) continue;
            UInt32 deadKeys = 0;
            UniChar characters[8] = {};
            UniCharCount length = 0;
            if (UCKeyTranslate(layout, keyCode, kUCKeyActionDisplay, 0, keyboardType, kUCKeyTranslateNoDeadKeysMask, &deadKeys, 8,
                               &length, characters) != noErr ||
                length == 0 || characters[0] < 0x20 || characters[0] == 0x7F) {
                continue;
            }
            // Key caps show capitals, as the Windows, X11 and SDL names do.
            for (UniCharCount i = 0; i < length; i++) {
                UniChar& c = characters[i];
                if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7)) c = UniChar(c - 0x20);
            }
            const char* utf8 = [NSString stringWithCharacters:characters length:length].UTF8String;
            if (utf8 != nullptr) g_keyNames[size_t(scancode)] = utf8;
        }
    }
    if (source != nullptr) CFRelease(source);
}

std::string extensionOf(Platform::FileKind kind) {
    switch (kind) {
    case Platform::FileKind::Midi: return "mid";
    case Platform::FileKind::Rom: return "rom";
    case Platform::FileKind::MidiOrSysex:
    case Platform::FileKind::Sysex:
    default: return "syx";
    }
}

NSString* nsString(const std::string& utf8) {
    NSString* string = [NSString stringWithUTF8String:utf8.c_str()];
    return string != nil ? string : @"";
}

std::filesystem::path pathOf(NSURL* url) {
    const char* path = url != nil && [url isFileURL] ? url.fileSystemRepresentation : nullptr;
    return path != nullptr ? std::filesystem::path(path) : std::filesystem::path();
}

// macOS's open and save panels, run modally as D110Emu's own window runs them (the host waits, as for any
// application-modal panel): from the App's frame, which the timer does not draw again meanwhile. Every file can be
// chosen, as in D110Emu's own window, whose dialogs offer "All files" too (a panel has no filter menu).
class CocoaToolkit : public Platform::Toolkit {
public:
    bool openFileDialog(Platform::FileKind /*kind*/, std::filesystem::path& result) override {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        panel.allowsMultipleSelection = NO;
        return runOpen(panel, result);
    }
    bool saveFileDialog(Platform::FileKind kind, const std::string& defaultName, std::filesystem::path& result) override {
        result.clear();
        NSSavePanel* panel = [NSSavePanel savePanel];
        panel.canCreateDirectories = YES;
        panel.nameFieldStringValue = nsString(defaultName);
        if ([panel runModal] != NSModalResponseOK) return false;
        result = pathOf(panel.URL);
        if (result.empty()) return false;
        if (!result.has_extension()) result += "." + extensionOf(kind);
        return true;
    }
    bool pickFolderDialog(const std::filesystem::path& initialFolder, std::filesystem::path& result) override {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = NO;
        panel.canChooseDirectories = YES;
        panel.canCreateDirectories = YES;
        panel.allowsMultipleSelection = NO;
        std::error_code ec;
        if (!initialFolder.empty() && std::filesystem::is_directory(initialFolder, ec)) {
            panel.directoryURL = [NSURL fileURLWithPath:nsString(Platform::toUtf8(initialFolder)) isDirectory:YES];
        }
        return runOpen(panel, result);
    }
    std::string takeDialogError() override { return std::string(); }  // macOS's panels always open
    std::string keyName(int scancode) override {
        return scancode > 0 && scancode < int(g_keyNames.size()) ? g_keyNames[size_t(scancode)] : std::string();
    }

private:
    static bool runOpen(NSOpenPanel* panel, std::filesystem::path& result) {
        result.clear();
        if ([panel runModal] != NSModalResponseOK) return false;
        result = pathOf(panel.URLs.firstObject);
        return !result.empty();
    }
};

CocoaToolkit& cocoaToolkit() {
    static CocoaToolkit toolkit;
    return toolkit;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// The editor

class PluginEditorMac : public PluginEditor {
public:
    explicit PluginEditorMac(PluginCore& core) : core_(core) {}
    ~PluginEditorMac() override;
    PluginEditorMac(const PluginEditorMac&) = delete;
    PluginEditorMac& operator=(const PluginEditorMac&) = delete;

    bool open(void* parentWindow) override;
    void close() override;
    void size(int& width, int& height) override;
    bool key(int character, int virtualKey, bool shift, bool control, bool alt, bool down) override;
    void drawViewMenu() override;
    void setHostResize(std::function<bool(int, int)> resize) override { hostResize_ = std::move(resize); }
    void setSize(int width, int height) override;
    // macOS counts in points, and the view draws at its window's backing scale by itself: a host's content scale (which
    // VST3 leaves to Windows and Linux) would count it twice.
    void setScale(float /*scale*/) override {}

    // The view's events, on the main thread.
    void onFrame();
    void mouseMoved(NSEvent* event);
    void mouseInside(bool inside);
    void mouseButton(NSEvent* event, int button, bool down);
    void scrollWheel(NSEvent* event);
    bool keyEvent(NSEvent* event, bool down);  // True when the key goes on to the input manager as text
    void flagsChanged(NSEvent* event);
    bool keyEquivalent(NSEvent* event);
    void insertText(const char* utf8);
    void focusChanged(bool focused);
    bool dropped(const std::filesystem::path& file);
    void cursorUpdate();
    NSRect textCursorRect() const;  // In the view, for input methods
    void placeView();               // At the top left corner of the host's view, whichever way up that counts

private:
    bool ready() const { return backendsReady_ && io_ != nullptr; }
    void setModifiers(NSEventModifierFlags flags);
    void applyScale(float scale);  // The standalone's style and 16-point text, scaled (with this context current)
    void computeSize(int& width, int& height) const;  // The view's size at zoom_, within the screen
    float scale() const { return float(zoom_) / 100.0f; }
    bool resizeHostWindow(int width, int height) { return hostResize_ && hostResize_(width, height); }

    PluginCore& core_;
    std::function<bool(int, int)> hostResize_;
    D110EmuPluginView* view_ = nil;
    CAMetalLayer* layer_ = nil;  // The view's
    id<MTLDevice> device_ = nil;
    id<MTLCommandQueue> queue_ = nil;
    NSTimer* timer_ = nil;
    // Kept while the plugin lives, as the App's timers run on its clock; the backend only while the view is open.
    ImGuiContext* context_ = nullptr;
    ImGuiIO* io_ = nullptr;
    bool backendsReady_ = false;
    bool inside_ = false;  // The pointer is over the view
    ImGuiMouseCursor cursor_ = ImGuiMouseCursor_Arrow;
    std::chrono::steady_clock::time_point lastFrame_{};
    float styleScale_ = 0.0f;   // The scale the style was last built for
    int zoom_ = 100;            // Percent
    bool zoomChanged_ = false;  // Asks the host for the new size before the next frame
    int width_ = 0;             // The view's size, in points
    int height_ = 0;
    // A modal panel in the App's frame runs the run loop, in which the timer must not draw again.
    bool inFrame_ = false;
    bool closePending_ = false;  // The host closed the editor during a frame
};

PluginEditorMac::~PluginEditorMac() {
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

bool PluginEditorMac::open(void* parentWindow) {
    close();
    NSView* parent = (__bridge NSView*)parentWindow;
    if (parent == nil) return false;
    device_ = MTLCreateSystemDefaultDevice();
    queue_ = device_ != nil ? [device_ newCommandQueue] : nil;
    if (queue_ == nil) {
        std::fprintf(stderr, "D110Emu: no Metal device for the plugin window\n");
        device_ = nil;
        return false;
    }
    zoom_ = core_.editorZoom();
    view_ = [[D110EmuPluginView alloc] initWithEditor:this frame:NSMakeRect(0, 0, kBaseWidth, kBaseHeight)];
    layer_ = [view_.layer isKindOfClass:[CAMetalLayer class]] ? (CAMetalLayer*)view_.layer : nil;
    if (layer_ == nil) {
        std::fprintf(stderr, "D110Emu: no Metal layer for the plugin window\n");
        [view_ detach];
        view_ = nil;
        queue_ = nil;
        device_ = nil;
        return false;
    }
    layer_.device = device_;
    [parent addSubview:view_];
    computeSize(width_, height_);  // On the screen of the host's window, now that the view is in it
    placeView();
    if (parent.window != nil) layer_.contentsScale = parent.window.backingScaleFactor;
    readKeyNames();
    Platform::setToolkit(&cocoaToolkit());

    {
        std::lock_guard<std::recursive_mutex> lock(uiMutex());
        const bool newContext = context_ == nullptr;
        if (newContext) {
            ImGuiContext* previous = ImGui::GetCurrentContext();
            context_ = ImGui::CreateContext();
            ImGui::SetCurrentContext(context_);
            io_ = &ImGui::GetIO();
            io_->IniFilename = nullptr;  // A single fixed layout, as in the standalone
            io_->BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
            // The system's clipboard (Dear ImGui's own would keep copies inside this instance).
            ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
            platform.Platform_SetClipboardTextFn = [](ImGuiContext*, const char* text) {
                NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
                [pasteboard clearContents];
                [pasteboard setString:nsString(text != nullptr ? text : "") forType:NSPasteboardTypeString];
            };
            platform.Platform_GetClipboardTextFn = [](ImGuiContext*) -> const char* {
                static std::string text;  // Dear ImGui copies it at once
                NSString* string = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
                if (string == nil || string.UTF8String == nullptr) return nullptr;
                text = string.UTF8String;
                return text.c_str();
            };
            ImGui::SetCurrentContext(previous);
        }
        ContextScope scope(context_);
        styleScale_ = 0.0f;  // The style is built for the zoom at the first frame
        ImGui_ImplMetal_Init(device_);
        if (newContext) UiStyle::loadFonts();  // Kept with the context; the new renderer uploads them again
        backendsReady_ = true;
    }
    // The run loop's common modes: the timer keeps drawing while the host tracks a menu or a resize, and during modal
    // panels (where the App's frame, which runs them, keeps it from drawing this instance).
    timer_ = [NSTimer timerWithTimeInterval:kFrameInterval target:view_ selector:@selector(frameTimer:) userInfo:nil repeats:YES];
    [[NSRunLoop mainRunLoop] addTimer:timer_ forMode:NSRunLoopCommonModes];
    lastFrame_ = std::chrono::steady_clock::now();
    return true;
}

void PluginEditorMac::close() {
    if (view_ == nil) return;
    if (inFrame_) {
        // Inside the App's frame (in a modal panel's run loop): the view goes once the frame is over.
        closePending_ = true;
        view_.hidden = YES;
        return;
    }
    closePending_ = false;
    [timer_ invalidate];  // The run loop lets go of the timer, and the timer of the view
    timer_ = nil;
    if (backendsReady_) {
        ContextScope scope(context_);
        ImGui_ImplMetal_Shutdown();
        backendsReady_ = false;
    }
    [view_ detach];
    [view_ removeFromSuperview];
    view_ = nil;
    layer_ = nil;
    queue_ = nil;
    device_ = nil;
    inside_ = false;
    cursor_ = ImGuiMouseCursor_Arrow;
}

void PluginEditorMac::size(int& width, int& height) {
    if (view_ != nil) {
        width = width_;
        height = height_;
        return;
    }
    // Before the view exists (hosts ask to size theirs): as open() makes it.
    zoom_ = core_.editorZoom();
    computeSize(width, height);
}

void PluginEditorMac::setSize(int width, int height) {
    if (view_ == nil || width <= 0 || height <= 0) return;
    width_ = width;
    height_ = height;
    placeView();
}

void PluginEditorMac::placeView() {
    NSView* parent = view_.superview;
    if (parent == nil) return;
    const CGFloat y = parent.isFlipped ? 0.0 : parent.bounds.size.height - CGFloat(height_);
    const NSRect frame = NSMakeRect(0.0, y, CGFloat(width_), CGFloat(height_));
    if (!NSEqualRects(view_.frame, frame)) view_.frame = frame;
}

void PluginEditorMac::computeSize(int& width, int& height) const {
    width = int(float(kBaseWidth) * scale() + 0.5f);
    height = int(float(kBaseHeight) * scale() + 0.5f);
    // Within the screen (without the menu bar and the Dock), leaving room for the host's frame around it.
    NSScreen* screen = view_ != nil && view_.window != nil ? view_.window.screen : nil;
    if (screen == nil) screen = [NSScreen mainScreen];
    if (screen != nil) {
        const NSRect area = screen.visibleFrame;
        width = std::min(width, int(area.size.width * 0.95));
        height = std::min(height, int(area.size.height * 0.85));
    }
    width = std::max(width, 320);
    height = std::max(height, 240);
}

bool PluginEditorMac::key(int character, int virtualKey, bool shift, bool control, bool alt, bool down) {
    // Only while a text field has the keyboard; otherwise the host keeps its keys (its shortcuts, its own keyboard).
    if (!ready() || !io_->WantTextInput) return false;
    ContextScope scope(context_);
    // `control` is VST3's command key, Command on a Mac: Dear ImGui's Super, which it takes as its Ctrl on macOS.
    io_->AddKeyEvent(ImGuiMod_Super, control);
    io_->AddKeyEvent(ImGuiMod_Shift, shift);
    io_->AddKeyEvent(ImGuiMod_Alt, alt);
    const ImGuiKey key = imguiKeyFromVirtual(virtualKey);
    if (key != ImGuiKey_None) {
        io_->AddKeyEvent(key, down);
        return true;
    }
    if (control && character >= 'a' && character <= 'z') {  // Cmd+A, C, V, X, Z, Y
        io_->AddKeyEvent(ImGuiKey(ImGuiKey_A + (character - 'a')), down);
        return true;
    }
    if (!down || control) return true;
    unsigned int text = unsigned(character);
    if (virtualKey == vst2::VKEY_SPACE) text = ' ';
    if (virtualKey >= vst2::VKEY_NUMPAD0 && virtualKey <= vst2::VKEY_NUMPAD0 + 9) text = unsigned('0' + (virtualKey - vst2::VKEY_NUMPAD0));
    if (text >= 32 && text != 127) io_->AddInputCharacter(text);
    return true;
}

void PluginEditorMac::drawViewMenu() {
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
// Input: into this instance's context, without making it current (another instance may be drawing)

void PluginEditorMac::setModifiers(NSEventModifierFlags flags) {
    // The keys as they are; Dear ImGui swaps Command and Control itself on macOS (io.ConfigMacOSXBehaviors).
    io_->AddKeyEvent(ImGuiMod_Ctrl, (flags & NSEventModifierFlagControl) != 0);
    io_->AddKeyEvent(ImGuiMod_Shift, (flags & NSEventModifierFlagShift) != 0);
    io_->AddKeyEvent(ImGuiMod_Alt, (flags & NSEventModifierFlagOption) != 0);
    io_->AddKeyEvent(ImGuiMod_Super, (flags & NSEventModifierFlagCommand) != 0);
}

void PluginEditorMac::mouseMoved(NSEvent* event) {
    if (!ready() || view_ == nil) return;
    // The view counts up from its bottom (as SDL's and MetalKit's Metal views do), Dear ImGui down from the top.
    const NSPoint point = [view_ convertPoint:event.locationInWindow fromView:nil];
    setModifiers(event.modifierFlags);
    io_->AddMousePosEvent(float(point.x), float(view_.bounds.size.height - point.y));
}

void PluginEditorMac::mouseInside(bool inside) {
    inside_ = inside;
    if (!inside && ready()) io_->AddMousePosEvent(-FLT_MAX, -FLT_MAX);
}

void PluginEditorMac::mouseButton(NSEvent* event, int button, bool down) {
    if (!ready()) return;
    mouseMoved(event);  // Its position and the modifiers first (Dear ImGui makes Control+click a right click on macOS)
    if (button >= 0 && button < ImGuiMouseButton_COUNT) io_->AddMouseButtonEvent(button, down);
}

void PluginEditorMac::scrollWheel(NSEvent* event) {
    if (!ready() || event.phase == NSEventPhaseCancelled) return;
    // As SDL gives them to D110Emu's own window: a trackpad's (and a Magic Mouse's) precise deltas in tenths, a wheel's
    // steps whole (at least one each). Both already point the way the system's scrolling direction has them.
    double x = 0.0;
    double y = 0.0;
    if (event.hasPreciseScrollingDeltas) {
        x = event.scrollingDeltaX * 0.1;
        y = event.scrollingDeltaY * 0.1;
    } else {
        x = event.deltaX;
        y = event.deltaY;
        x = x > 0.0 ? std::ceil(x) : std::floor(x);
        y = y > 0.0 ? std::ceil(y) : std::floor(y);
    }
    mouseMoved(event);
    if (x != 0.0 || y != 0.0) io_->AddMouseWheelEvent(float(x), float(y));
}

bool PluginEditorMac::keyEvent(NSEvent* event, bool down) {
    if (!ready()) return false;
    const NSEventModifierFlags flags = event.modifierFlags;
    const bool command = (flags & NSEventModifierFlagCommand) != 0;
    setModifiers(flags);
    if (![event isARepeat]) {  // Dear ImGui repeats held keys itself
        const ImGuiKey key = keyOf(event);
        if (key != ImGuiKey_None) {
            io_->AddKeyEvent(key, down);
            if (down && command) io_->AddKeyEvent(key, false);  // AppKit sends no key-up while Command is down
        }
        // The computer-keyboard piano goes by key position, as PC scancodes (set 1) like the other systems'. Not with
        // Command: the key-up would never come.
        const int scancode = pcScancode(event.keyCode);
        if (scancode > 0 && !inFrame_ && !(down && command)) core_.withApp([&](App& app) { app.onKey(scancode, down); });
    }
    // Text goes through the input manager (dead keys, input methods) while a text field has the keyboard.
    return down && io_->WantTextInput && (flags & (NSEventModifierFlagCommand | NSEventModifierFlagControl)) == 0;
}

void PluginEditorMac::flagsChanged(NSEvent* event) {
    if (!ready()) return;
    const NSEventModifierFlags flags = event.modifierFlags;
    setModifiers(flags);
    // The modifier key that changed, by its own bit among the device-dependent ones (left and right apart).
    ImGuiKey key = ImGuiKey_None;
    NSEventModifierFlags mask = 0;
    switch (event.keyCode) {
    case kVK_Shift: key = ImGuiKey_LeftShift; mask = 0x0002; break;
    case kVK_RightShift: key = ImGuiKey_RightShift; mask = 0x0004; break;
    case kVK_Control: key = ImGuiKey_LeftCtrl; mask = 0x0001; break;
    case kVK_RightControl: key = ImGuiKey_RightCtrl; mask = 0x2000; break;
    case kVK_Option: key = ImGuiKey_LeftAlt; mask = 0x0020; break;
    case kVK_RightOption: key = ImGuiKey_RightAlt; mask = 0x0040; break;
    case kVK_Command: key = ImGuiKey_LeftSuper; mask = 0x0008; break;
    case kVK_RightCommand: key = ImGuiKey_RightSuper; mask = 0x0010; break;
    default: break;
    }
    if (key != ImGuiKey_None) io_->AddKeyEvent(key, (flags & mask) != 0);
    // Keys let go of while Command is down send no key-up: the piano lets go of its keys when Command goes down.
    if ((flags & NSEventModifierFlagCommand) != 0 && !inFrame_) core_.withApp([](App& app) { app.clearKeys(); });
}

bool PluginEditorMac::keyEquivalent(NSEvent* event) {
    // Command with a key reaches the view here before the host's menus: while a text field has the keyboard, its
    // editing keys (select all, copy, cut, paste, undo, redo, the arrows, Backspace) are the text field's.
    if (!ready() || !io_->WantTextInput || event.type != NSEventTypeKeyDown || (event.modifierFlags & NSEventModifierFlagCommand) == 0) {
        return false;
    }
    const ImGuiKey key = keyOf(event);
    switch (key) {
    case ImGuiKey_A:
    case ImGuiKey_C:
    case ImGuiKey_V:
    case ImGuiKey_X:
    case ImGuiKey_Y:
    case ImGuiKey_Z:
    case ImGuiKey_LeftArrow:
    case ImGuiKey_RightArrow:
    case ImGuiKey_UpArrow:
    case ImGuiKey_DownArrow:
    case ImGuiKey_Backspace:
    case ImGuiKey_Delete:
        break;
    default:
        return false;
    }
    setModifiers(event.modifierFlags);
    io_->AddKeyEvent(key, true);
    io_->AddKeyEvent(key, false);
    return true;
}

void PluginEditorMac::insertText(const char* utf8) {
    if (ready() && utf8 != nullptr) io_->AddInputCharactersUTF8(utf8);
}

void PluginEditorMac::focusChanged(bool focused) {
    if (!ready()) return;
    io_->AddFocusEvent(focused);
    // Keys let go of elsewhere never come back here as key-ups.
    if (!focused && !inFrame_) core_.withApp([](App& app) { app.clearKeys(); });
}

bool PluginEditorMac::dropped(const std::filesystem::path& file) {
    if (inFrame_ || file.empty()) return false;
    core_.withApp([&](App& app) { app.openFile(file); });
    return true;
}

void PluginEditorMac::cursorUpdate() {
    [cocoaCursor(cursor_) set];
}

NSRect PluginEditorMac::textCursorRect() const {
    if (context_ == nullptr || view_ == nil) return NSZeroRect;
    const ImGuiPlatformImeData& ime = context_->PlatformImeData;
    return NSMakeRect(ime.InputPos.x, view_.bounds.size.height - ime.InputPos.y - ime.InputLineHeight, 0.0, ime.InputLineHeight);
}

// ---------------------------------------------------------------------------------------------
// Drawing

void PluginEditorMac::onFrame() {
    if (inFrame_ || view_ == nil || !backendsReady_) return;
    NSWindow* window = view_.window;
    // Nothing to draw into: the view is in no window (yet, or any more) or hidden, or the window is out of sight, where
    // drawables stop coming back (and waiting for one would stall the host).
    if (window == nil || [view_ isHiddenOrHasHiddenAncestor] || (window.occlusionState & NSWindowOcclusionStateVisible) == 0) return;
    inFrame_ = true;
    @autoreleasepool {
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
                placeView();
            } else {
                width_ = oldWidth;
                height_ = oldHeight;
            }
        }
        if (scale() != styleScale_) applyScale(scale());

        const NSSize size = view_.bounds.size;
        const CGFloat density = std::max<CGFloat>(window.backingScaleFactor, 1.0);  // 2 on a Retina display
        const CGSize pixels = CGSizeMake(std::round(size.width * density), std::round(size.height * density));
        id<CAMetalDrawable> drawable = nil;
        if (pixels.width >= 1.0 && pixels.height >= 1.0) {
            if (layer_.contentsScale != density) layer_.contentsScale = density;
            if (layer_.drawableSize.width != pixels.width || layer_.drawableSize.height != pixels.height) layer_.drawableSize = pixels;
            drawable = [layer_ nextDrawable];
        }
        if (drawable != nil) {
            const auto now = std::chrono::steady_clock::now();
            io_->DeltaTime = std::clamp(std::chrono::duration<float>(now - lastFrame_).count(), 0.0001f, 0.25f);
            lastFrame_ = now;
            io_->DisplaySize = ImVec2(float(size.width), float(size.height));
            io_->DisplayFramebufferScale = ImVec2(float(density), float(density));  // Dear ImGui draws its text at it
            MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = drawable.texture;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor = MTLClearColorMake(0.09, 0.09, 0.10, 1.0);
            ImGui_ImplMetal_NewFrame(pass);
            ImGui::NewFrame();
            core_.drawFrame();
            ImGui::Render();
            id<MTLCommandBuffer> commands = [queue_ commandBuffer];
            id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
            if (encoder != nil) {
                ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), commands, encoder);
                [encoder endEncoding];
            }
            [commands presentDrawable:drawable];
            [commands commit];
            const ImGuiMouseCursor cursor = ImGui::GetMouseCursor();
            if (cursor != cursor_) {
                cursor_ = cursor;
                if (inside_) [cocoaCursor(cursor) set];
            }
        }
    }
    inFrame_ = false;
    core_.afterFrame();
    if (closePending_) close();
}

void PluginEditorMac::applyScale(float scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    App::applyStyle();
    style.FontSizeBase = 16.0f;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    styleScale_ = scale;
}

// ---------------------------------------------------------------------------------------------
// The view

@implementation D110EmuPluginView {
    PluginEditorMac* _editor;  // Until detach
}

- (instancetype)initWithEditor:(PluginEditorMac*)editor frame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        _editor = editor;
        self.wantsLayer = YES;  // makeBackingLayer's
        self.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;  // The timer draws
        self.layerContentsPlacement = NSViewLayerContentsPlacementTopLeft;
        // Pointer moves, entering and leaving, and the cursor, over the view as it is, in any window state (a host's
        // plugin window may never become the key window).
        const NSTrackingAreaOptions options = NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved | NSTrackingCursorUpdate |
                                              NSTrackingActiveAlways | NSTrackingInVisibleRect;
        [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:NSZeroRect options:options owner:self userInfo:nil]];
        [self registerForDraggedTypes:@[NSPasteboardTypeFileURL]];
    }
    return self;
}

- (void)detach {
    _editor = nullptr;
}

- (void)frameTimer:(NSTimer*)timer {
    (void)timer;
    if (_editor != nullptr) _editor->onFrame();
}

- (CALayer*)makeBackingLayer {
    CAMetalLayer* layer = [CAMetalLayer layer];
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = YES;
    layer.opaque = YES;
    return layer;
}

- (BOOL)wantsUpdateLayer {
    return YES;  // AppKit leaves the layer's contents alone...
}

- (void)updateLayer {
    // ...as the timer draws them, never AppKit's display pass (a frame may run a modal panel).
}

- (BOOL)isOpaque {
    return YES;
}

- (BOOL)mouseDownCanMoveWindow {
    return NO;
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (BOOL)acceptsFirstMouse:(NSEvent*)event {
    (void)event;
    return YES;  // A click in the host's inactive window reaches the editor too
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    if (self.window != nil) self.layer.contentsScale = self.window.backingScaleFactor;
}

- (void)resizeWithOldSuperviewSize:(NSSize)oldSize {
    // The host's view changed size: the editor keeps its own, at the top left.
    if (_editor != nullptr) {
        _editor->placeView();
    } else {
        [super resizeWithOldSuperviewSize:oldSize];
    }
}

// The keyboard follows the window: when it stops being the key window, keys let go of elsewhere never come back.
- (void)viewWillMoveToWindow:(NSWindow*)window {
    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    if (self.window != nil) {
        [center removeObserver:self name:NSWindowDidBecomeKeyNotification object:self.window];
        [center removeObserver:self name:NSWindowDidResignKeyNotification object:self.window];
    }
    if (window != nil) {
        [center addObserver:self selector:@selector(windowKeyChanged:) name:NSWindowDidBecomeKeyNotification object:window];
        [center addObserver:self selector:@selector(windowKeyChanged:) name:NSWindowDidResignKeyNotification object:window];
    }
    [super viewWillMoveToWindow:window];
}

- (void)windowKeyChanged:(NSNotification*)notification {
    (void)notification;
    if (_editor != nullptr) _editor->focusChanged([self.window isKeyWindow] && self.window.firstResponder == self);
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
}

// Mouse

- (void)mouseMoved:(NSEvent*)event {
    if (_editor != nullptr) _editor->mouseMoved(event);
}

- (void)mouseDragged:(NSEvent*)event {
    if (_editor != nullptr) _editor->mouseMoved(event);
}

- (void)rightMouseDragged:(NSEvent*)event {
    if (_editor != nullptr) _editor->mouseMoved(event);
}

- (void)otherMouseDragged:(NSEvent*)event {
    if (_editor != nullptr) _editor->mouseMoved(event);
}

- (void)mouseEntered:(NSEvent*)event {
    if (_editor == nullptr) return;
    _editor->mouseInside(true);
    _editor->mouseMoved(event);
}

- (void)mouseExited:(NSEvent*)event {
    (void)event;
    if (_editor != nullptr) _editor->mouseInside(false);
}

// The keyboard (text fields, the computer-keyboard piano) follows a click, as in the other systems' editors.
- (void)mouseDown:(NSEvent*)event {
    [self.window makeFirstResponder:self];
    if (_editor != nullptr) _editor->mouseButton(event, 0, true);
}

- (void)mouseUp:(NSEvent*)event {
    if (_editor != nullptr) _editor->mouseButton(event, 0, false);
}

- (void)rightMouseDown:(NSEvent*)event {
    [self.window makeFirstResponder:self];
    if (_editor != nullptr) _editor->mouseButton(event, 1, true);
}

- (void)rightMouseUp:(NSEvent*)event {
    if (_editor != nullptr) _editor->mouseButton(event, 1, false);
}

- (void)otherMouseDown:(NSEvent*)event {
    [self.window makeFirstResponder:self];
    if (_editor != nullptr) _editor->mouseButton(event, int(event.buttonNumber), true);  // 2 is the middle button
}

- (void)otherMouseUp:(NSEvent*)event {
    if (_editor != nullptr) _editor->mouseButton(event, int(event.buttonNumber), false);
}

- (void)scrollWheel:(NSEvent*)event {
    if (_editor != nullptr) _editor->scrollWheel(event);
}

- (void)cursorUpdate:(NSEvent*)event {
    if (_editor != nullptr) {
        _editor->cursorUpdate();
    } else {
        [super cursorUpdate:event];
    }
}

// Keyboard

- (void)keyDown:(NSEvent*)event {
    if (_editor == nullptr) {
        [super keyDown:event];
        return;
    }
    if (_editor->keyEvent(event, true)) [self interpretKeyEvents:@[event]];
}

- (void)keyUp:(NSEvent*)event {
    if (_editor != nullptr) {
        _editor->keyEvent(event, false);
    } else {
        [super keyUp:event];
    }
}

- (void)flagsChanged:(NSEvent*)event {
    if (_editor != nullptr) {
        _editor->flagsChanged(event);
    } else {
        [super flagsChanged:event];
    }
}

- (BOOL)performKeyEquivalent:(NSEvent*)event {
    if (_editor != nullptr && self.window.firstResponder == self && _editor->keyEquivalent(event)) return YES;
    return [super performKeyEquivalent:event];
}

- (BOOL)becomeFirstResponder {
    if (_editor != nullptr) _editor->focusChanged(true);
    return YES;
}

- (BOOL)resignFirstResponder {
    if (_editor != nullptr) _editor->focusChanged(false);
    return YES;
}

// Text input (NSTextInputClient): what the input manager makes of the keys while a text field has the keyboard.
// Composing text (marked text) stays with the input method.

- (void)insertText:(id)string replacementRange:(NSRange)replacementRange {
    (void)replacementRange;
    NSString* text = [string isKindOfClass:[NSAttributedString class]] ? [(NSAttributedString*)string string] : (NSString*)string;
    if (_editor != nullptr && [text isKindOfClass:[NSString class]]) _editor->insertText(text.UTF8String);
}

- (void)doCommandBySelector:(SEL)selector {
    (void)selector;  // Return, the arrows, Backspace...: Dear ImGui has them as keys (and no beep)
}

- (void)setMarkedText:(id)string selectedRange:(NSRange)selectedRange replacementRange:(NSRange)replacementRange {
    (void)string;
    (void)selectedRange;
    (void)replacementRange;
}

- (void)unmarkText {
}

- (NSRange)selectedRange {
    return NSMakeRange(NSNotFound, 0);
}

- (NSRange)markedRange {
    return NSMakeRange(NSNotFound, 0);
}

- (BOOL)hasMarkedText {
    return NO;
}

- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
    (void)range;
    (void)actualRange;
    return nil;
}

- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText {
    return @[];
}

- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actualRange {
    (void)range;
    (void)actualRange;
    // An input method's window goes at the text cursor, in screen coordinates.
    const NSRect rect = _editor != nullptr ? _editor->textCursorRect() : NSZeroRect;
    return self.window != nil ? [self.window convertRectToScreen:[self convertRect:rect toView:nil]] : NSZeroRect;
}

- (NSUInteger)characterIndexForPoint:(NSPoint)point {
    (void)point;
    return NSNotFound;
}

// Files dropped on the view open as in D110Emu's own window.

- (NSURL*)droppedFile:(id<NSDraggingInfo>)sender {
    NSArray<NSURL*>* files = [sender.draggingPasteboard readObjectsForClasses:@[[NSURL class]]
                                                                      options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
    return files.firstObject;
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    return _editor != nullptr && [self droppedFile:sender] != nil ? NSDragOperationCopy : NSDragOperationNone;
}

- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender {
    return [self draggingEntered:sender];
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    NSURL* file = [self droppedFile:sender];
    return _editor != nullptr && file != nil && _editor->dropped(pathOf(file));
}

@end

std::unique_ptr<PluginEditor> createPluginEditor(PluginCore& core) {
    return std::unique_ptr<PluginEditor>(new PluginEditorMac(core));
}
