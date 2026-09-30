// The VST2 plugin's window on Windows: a child of the host's window, drawn with Direct3D 11 through Dear ImGui's
// backends like D110Emu's own window (Win32Host.cpp), and redrawn by a timer.

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <string>

#include "VstPlugin.h"
#include "Win32Host.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

// Dear ImGui's message handler for a given context: it leaves the current context alone.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandlerEx(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, ImGuiIO& io);

#ifndef WM_DPICHANGED_AFTERPARENT
#define WM_DPICHANGED_AFTERPARENT 0x02E3
#endif

namespace {

constexpr int kBaseWidth = 1200;  // D110Emu's own window at 100% scaling
constexpr int kBaseHeight = 780;
constexpr UINT_PTR kFrameTimer = 1;
constexpr UINT kFrameMilliseconds = 16;  // About 60 frames a second
const int kZoomSteps[] = {75, 100, 125, 150, 200};
const wchar_t* const kWindowClass = L"D110EmuVstEditor";

// Dear ImGui keeps the current context in a global, which all instances of the plugin share (they live in one DLL).
// Each draws with its own context under this lock and puts back the one that was current: a modal dialog in one
// instance's frame runs a message loop in which the host's other plugin windows keep drawing.
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

int g_windowClassUsers = 0;  // Editors with a window, under uiMutex()

HINSTANCE moduleInstance() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&moduleInstance), &module);
    return module;
}

template <typename Function>
Function user32Function(const char* name) {
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    return user32 != nullptr ? reinterpret_cast<Function>(reinterpret_cast<void (*)()>(GetProcAddress(user32, name))) : nullptr;
}

// The DPI a window is drawn at, as the host's DPI mode has it: its monitor's for a per-monitor aware host, the
// system's for a system-aware one, 96 for a DPI-unaware one (Windows scales its windows then). Without a window: the
// system's, as this thread sees it.
UINT dpiOf(HWND window) {
    using GetDpiForWindowFunction = UINT(WINAPI*)(HWND);
    using GetDpiForSystemFunction = UINT(WINAPI*)();
    static const auto getDpiForWindow = user32Function<GetDpiForWindowFunction>("GetDpiForWindow");  // Windows 10 1607+
    static const auto getDpiForSystem = user32Function<GetDpiForSystemFunction>("GetDpiForSystem");
    if (window != nullptr && getDpiForWindow != nullptr) {
        const UINT dpi = getDpiForWindow(window);
        if (dpi != 0) return dpi;
    }
    if (getDpiForSystem != nullptr) {
        const UINT dpi = getDpiForSystem();
        if (dpi != 0) return dpi;
    }
    const HDC dc = GetDC(nullptr);
    const int dpi = dc != nullptr ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc != nullptr) ReleaseDC(nullptr, dc);
    return dpi > 0 ? UINT(dpi) : 96;
}

// Keys from effEditKeyDown that Dear ImGui takes as keys rather than as text.
ImGuiKey imguiKey(int virtualKey) {
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

class VstEditorWin32 : public VstEditor {
public:
    explicit VstEditorWin32(VstPlugin& plugin) : plugin_(plugin) {}
    ~VstEditorWin32() override;
    VstEditorWin32(const VstEditorWin32&) = delete;
    VstEditorWin32& operator=(const VstEditorWin32&) = delete;

    bool open(void* parentWindow) override;
    void close() override;
    void size(int& width, int& height) override;
    bool key(int character, int virtualKey, int modifiers, bool down) override;
    void drawViewMenu() override;

private:
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handle(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    bool createDevice();
    void destroyDevice();
    void createRenderTarget();
    void releaseRenderTarget();
    void destroyWindow();
    void render();
    // The standalone's style and 16-pixel text, scaled (with this context current).
    void applyScale(float scale);
    // The window's size at dpiScale_ and zoom_, within the screen.
    void computeSize(int& width, int& height) const;
    float scale() const { return dpiScale_ * float(zoom_) / 100.0f; }

    VstPlugin& plugin_;
    HWND hwnd_ = nullptr;
    bool windowDestroyed_ = false;  // The host destroyed it with its own window, before effEditClose
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* deviceContext_ = nullptr;
    IDXGISwapChain* swapChain_ = nullptr;
    ID3D11RenderTargetView* renderTarget_ = nullptr;
    // Kept while the plugin lives, as the App's timers run on its clock; the backends only while the window is open.
    ImGuiContext* context_ = nullptr;
    ImGuiIO* io_ = nullptr;
    bool backendsReady_ = false;
    float dpiScale_ = 1.0f;
    float styleScale_ = 0.0f;  // The scale the style was last built for
    int zoom_ = 100;           // Percent
    bool zoomChanged_ = false; // Asks the host for the new size before the next frame
    int width_ = 0;            // The window's size
    int height_ = 0;
    UINT resizeWidth_ = 0;     // A new size for the swap chain, before the next frame
    UINT resizeHeight_ = 0;
    // A modal dialog in the App's frame runs its own message loop, in which the timer must not draw again.
    bool inFrame_ = false;
    bool closePending_ = false;  // effEditClose came during a frame
    bool comInitialized_ = false;
};

VstEditorWin32::~VstEditorWin32() {
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

bool VstEditorWin32::open(void* parentWindow) {
    close();
    const HWND parent = static_cast<HWND>(parentWindow);
    if (parent == nullptr || hwnd_ != nullptr) return false;
    const HINSTANCE instance = moduleInstance();
    {
        std::lock_guard<std::recursive_mutex> lock(uiMutex());
        if (g_windowClassUsers == 0) {
            WNDCLASSEXW wc = {};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = &windowProc;
            wc.hInstance = instance;
            wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
            wc.lpszClassName = kWindowClass;
            if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        }
        g_windowClassUsers++;
    }

    zoom_ = plugin_.editorZoom();
    dpiScale_ = float(dpiOf(parent)) / 96.0f;
    computeSize(width_, height_);
    windowDestroyed_ = false;
    hwnd_ = CreateWindowExW(0, kWindowClass, L"D110Emu", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, width_,
                            height_, parent, nullptr, instance, this);
    if (hwnd_ == nullptr) {
        destroyWindow();
        return false;
    }
    // The window's own DPI mode can differ from its parent's (a host may create plugin windows in another mode).
    const float windowScale = float(dpiOf(hwnd_)) / 96.0f;
    if (windowScale != dpiScale_) {
        dpiScale_ = windowScale;
        computeSize(width_, height_);
        SetWindowPos(hwnd_, nullptr, 0, 0, width_, height_, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        plugin_.resizeHostWindow(width_, height_);
    }
    if (!createDevice()) {
        destroyWindow();
        return false;
    }
    comInitialized_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));  // For the folder picker

    {
        std::lock_guard<std::recursive_mutex> lock(uiMutex());
        const bool newContext = context_ == nullptr;
        if (newContext) {
            ImGuiContext* previous = ImGui::GetCurrentContext();
            context_ = ImGui::CreateContext();
            ImGui::SetCurrentContext(context_);
            io_ = &ImGui::GetIO();
            io_->IniFilename = nullptr;  // A single fixed layout, as in the standalone
            ImGui::SetCurrentContext(previous);
        }
        ContextScope scope(context_);
        styleScale_ = 0.0f;  // The style is built for the window's scale at the first frame
        ImGui_ImplWin32_Init(hwnd_);
        ImGui_ImplDX11_Init(device_, deviceContext_);
        if (newContext) loadUiFonts();  // Kept with the context; the new renderer uploads them again
        backendsReady_ = true;
    }
    DragAcceptFiles(hwnd_, TRUE);
    SetTimer(hwnd_, kFrameTimer, kFrameMilliseconds, nullptr);
    return true;
}

void VstEditorWin32::close() {
    if (hwnd_ == nullptr) return;
    if (inFrame_) {
        // Inside the App's frame (in a dialog's message loop): the window goes once the frame is over.
        closePending_ = true;
        if (!windowDestroyed_) ShowWindow(hwnd_, SW_HIDE);
        return;
    }
    closePending_ = false;
    if (!windowDestroyed_) KillTimer(hwnd_, kFrameTimer);
    if (backendsReady_) {
        ContextScope scope(context_);
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        backendsReady_ = false;
    }
    destroyDevice();
    destroyWindow();
    if (comInitialized_) {
        CoUninitialize();
        comInitialized_ = false;
    }
}

void VstEditorWin32::destroyWindow() {
    if (hwnd_ != nullptr && !windowDestroyed_) {
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);  // Its last messages go to DefWindowProc
        DestroyWindow(hwnd_);
    }
    hwnd_ = nullptr;
    windowDestroyed_ = false;
    std::lock_guard<std::recursive_mutex> lock(uiMutex());
    if (g_windowClassUsers > 0 && --g_windowClassUsers == 0) UnregisterClassW(kWindowClass, moduleInstance());
}

void VstEditorWin32::size(int& width, int& height) {
    if (hwnd_ != nullptr && !windowDestroyed_) {
        width = width_;
        height = height_;
        return;
    }
    // Before the window exists (hosts ask to size theirs): at the system's DPI; open() corrects it for the window's.
    zoom_ = plugin_.editorZoom();
    dpiScale_ = float(dpiOf(nullptr)) / 96.0f;
    computeSize(width, height);
}

void VstEditorWin32::computeSize(int& width, int& height) const {
    width = int(float(kBaseWidth) * scale() + 0.5f);
    height = int(float(kBaseHeight) * scale() + 0.5f);
    // Within the screen, leaving room for the host's frame around it.
    MONITORINFO monitor = {};
    monitor.cbSize = sizeof(monitor);
    const HWND reference = hwnd_ != nullptr ? hwnd_ : GetForegroundWindow();
    if (GetMonitorInfoW(MonitorFromWindow(reference, MONITOR_DEFAULTTOPRIMARY), &monitor)) {
        width = std::min(width, int(float(monitor.rcWork.right - monitor.rcWork.left) * 0.95f));
        height = std::min(height, int(float(monitor.rcWork.bottom - monitor.rcWork.top) * 0.85f));
    }
    width = std::max(width, 320);
    height = std::max(height, 240);
}

bool VstEditorWin32::key(int character, int virtualKey, int modifiers, bool down) {
    // Only while a text field has the keyboard; otherwise the host keeps its keys (its shortcuts, its own keyboard).
    if (!backendsReady_ || io_ == nullptr || !io_->WantTextInput) return false;
    ContextScope scope(context_);
    const bool control = (modifiers & vst2::MODIFIER_CONTROL) != 0;
    const bool alt = (modifiers & vst2::MODIFIER_ALTERNATE) != 0;
    io_->AddKeyEvent(ImGuiMod_Ctrl, control);
    io_->AddKeyEvent(ImGuiMod_Shift, (modifiers & vst2::MODIFIER_SHIFT) != 0);
    io_->AddKeyEvent(ImGuiMod_Alt, alt);
    const ImGuiKey key = imguiKey(virtualKey);
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

void VstEditorWin32::drawViewMenu() {
    if (!ImGui::BeginMenu("Window size")) {
        ImGui::SetItemTooltip("The plugin window with its text: 100%% is D110Emu's own window at the screen's scaling. "
                              "New instances open at the same size.");
        return;
    }
    for (int zoom : kZoomSteps) {
        const std::string label = std::to_string(zoom) + "%";
        if (ImGui::MenuItem(label.c_str(), nullptr, zoom == zoom_) && zoom != zoom_) {
            zoom_ = zoom;
            plugin_.setEditorZoom(zoom);
            zoomChanged_ = true;  // Resized before the next frame, not in the middle of this one
        }
    }
    ImGui::EndMenu();
}

// ---------------------------------------------------------------------------------------------
// The window

LRESULT CALLBACK VstEditorWin32::windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* editor = reinterpret_cast<VstEditorWin32*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (editor == nullptr) return DefWindowProcW(hwnd, msg, wParam, lParam);
    return editor->handle(hwnd, msg, wParam, lParam);
}

LRESULT VstEditorWin32::handle(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Input goes into this instance's context without making it current (another instance may be drawing).
    if (backendsReady_ && io_ != nullptr && ImGui_ImplWin32_WndProcHandlerEx(hwnd, msg, wParam, lParam, *io_)) return 1;
    switch (msg) {
    case WM_TIMER:
        if (wParam == kFrameTimer) render();
        return 0;
    case WM_PAINT:
        ValidateRect(hwnd, nullptr);
        render();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED && LOWORD(lParam) != 0 && HIWORD(lParam) != 0) {
            width_ = LOWORD(lParam);
            height_ = HIWORD(lParam);
            resizeWidth_ = UINT(width_);  // The swap chain follows before the next frame
            resizeHeight_ = UINT(height_);
        }
        return 0;
    case WM_DPICHANGED_AFTERPARENT: {
        // The host's window moved to a screen with other scaling: the text follows, and the window if the host lets it.
        const float windowScale = float(dpiOf(hwnd)) / 96.0f;
        if (windowScale != dpiScale_) {
            dpiScale_ = windowScale;
            zoomChanged_ = true;
        }
        return 0;
    }
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTARROWS | DLGC_WANTCHARS | DLGC_WANTTAB;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        SetFocus(hwnd);  // The keyboard (text fields, the computer-keyboard piano) follows a click
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
        // The computer-keyboard piano goes by physical key position (scancode); extended keys are never piano keys.
        if (!inFrame_ && ((lParam >> 24) & 1) == 0) {
            const int scancode = int((lParam >> 16) & 0xFF);
            const bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
            plugin_.withApp([&](App& app) { app.onKey(scancode, down); });
        }
        if (msg == WM_KEYDOWN || msg == WM_KEYUP) return 0;
        break;  // Alt combinations go on to the host's window
    case WM_KILLFOCUS:
        if (!inFrame_) plugin_.withApp([](App& app) { app.clearKeys(); });
        break;
    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wParam);
        wchar_t path[MAX_PATH * 4] = {};
        if (!inFrame_ && DragQueryFileW(drop, 0, path, UINT(sizeof(path) / sizeof(path[0]))) > 0) {
            const std::filesystem::path file(path);
            plugin_.withApp([&](App& app) { app.openFile(file); });
        }
        DragFinish(drop);
        return 0;
    }
    case WM_NCDESTROY:
        windowDestroyed_ = true;  // With the host's window, before effEditClose
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void VstEditorWin32::render() {
    if (inFrame_ || hwnd_ == nullptr || windowDestroyed_ || !backendsReady_ || !IsWindowVisible(hwnd_)) return;
    inFrame_ = true;
    {
        ContextScope scope(context_);
        if (zoomChanged_) {
            zoomChanged_ = false;
            int width = 0;
            int height = 0;
            computeSize(width, height);
            // Only as the host resizes its window: otherwise the text is scaled within the present size, and the new
            // size applies when the window opens again. A host may ask for the size (effEditGetRect) meanwhile.
            const int oldWidth = width_;
            const int oldHeight = height_;
            width_ = width;
            height_ = height;
            if (plugin_.resizeHostWindow(width, height)) {
                SetWindowPos(hwnd_, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            } else {
                width_ = oldWidth;
                height_ = oldHeight;
            }
        }
        if (resizeWidth_ != 0 && resizeHeight_ != 0) {
            releaseRenderTarget();
            swapChain_->ResizeBuffers(0, resizeWidth_, resizeHeight_, DXGI_FORMAT_UNKNOWN, 0);
            resizeWidth_ = 0;
            resizeHeight_ = 0;
            createRenderTarget();
        }
        if (scale() != styleScale_) applyScale(scale());

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        plugin_.drawFrame();
        ImGui::Render();
        const float clearColor[4] = {0.09f, 0.09f, 0.10f, 1.0f};
        if (renderTarget_ != nullptr) {
            deviceContext_->OMSetRenderTargets(1, &renderTarget_, nullptr);
            deviceContext_->ClearRenderTargetView(renderTarget_, clearColor);
        }
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        swapChain_->Present(0, 0);  // No vsync: this runs on the host's UI thread, with other plugins' windows
    }
    inFrame_ = false;
    plugin_.afterFrame();
    if (closePending_) close();
}

void VstEditorWin32::applyScale(float scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    App::applyStyle();
    style.FontSizeBase = 16.0f;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    styleScale_ = scale;
}

// ---------------------------------------------------------------------------------------------
// Direct3D 11, as in Win32Host.cpp

bool VstEditorWin32::createDevice() {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd_;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL level;
    HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
                                                   &swapChain_, &device_, &level, &deviceContext_);
    if (result == DXGI_ERROR_UNSUPPORTED) {  // No GPU: the WARP software rasteriser
        result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
                                               &swapChain_, &device_, &level, &deviceContext_);
    }
    if (FAILED(result)) {
        destroyDevice();
        return false;
    }
    // Keep DXGI's Alt+Enter full-screen switch away from the host's window.
    IDXGIFactory* factory = nullptr;
    if (SUCCEEDED(swapChain_->GetParent(IID_PPV_ARGS(&factory)))) {
        factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        factory->Release();
    }
    createRenderTarget();
    return true;
}

void VstEditorWin32::destroyDevice() {
    releaseRenderTarget();
    if (swapChain_ != nullptr) {
        swapChain_->Release();
        swapChain_ = nullptr;
    }
    if (deviceContext_ != nullptr) {
        deviceContext_->Release();
        deviceContext_ = nullptr;
    }
    if (device_ != nullptr) {
        device_->Release();
        device_ = nullptr;
    }
}

void VstEditorWin32::createRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) return;
    device_->CreateRenderTargetView(backBuffer, nullptr, &renderTarget_);
    backBuffer->Release();
}

void VstEditorWin32::releaseRenderTarget() {
    if (renderTarget_ != nullptr) {
        renderTarget_->Release();
        renderTarget_ = nullptr;
    }
}

}  // namespace

std::unique_ptr<VstEditor> createVstEditor(VstPlugin& plugin) {
    return std::unique_ptr<VstEditor>(new VstEditorWin32(plugin));
}
