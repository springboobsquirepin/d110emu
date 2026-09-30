// A Win32 window rendered with Direct3D 11 through Dear ImGui's backends, shared by D110Emu, MT32Translator, ToneEditor
// and PatternCapture.
// Adapted from Dear ImGui's example_win32_directx11.

#include <windows.h>
#include <d3d11.h>
#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <filesystem>

#include "Win32Host.h"

#include "Platform.h"
#include "UiStyle.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swapChain = nullptr;
ID3D11RenderTargetView* g_renderTarget = nullptr;
bool g_swapChainOccluded = false;
UINT g_resizeWidth = 0;
UINT g_resizeHeight = 0;
HostedApp* g_app = nullptr;

void createRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    g_device->CreateRenderTargetView(backBuffer, nullptr, &g_renderTarget);
    backBuffer->Release();
}

void cleanupRenderTarget() {
    if (g_renderTarget) {
        g_renderTarget->Release();
        g_renderTarget = nullptr;
    }
}

bool createDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL level;
    HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                                   &sd, &g_swapChain, &g_device, &level, &g_context);
    if (result == DXGI_ERROR_UNSUPPORTED) {  // No GPU: fall back to the WARP software rasteriser
        result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                               &sd, &g_swapChain, &g_device, &level, &g_context);
    }
    if (result != S_OK) return false;
    createRenderTarget();
    return true;
}

void cleanupDevice() {
    cleanupRenderTarget();
    if (g_swapChain) {
        g_swapChain->Release();
        g_swapChain = nullptr;
    }
    if (g_context) {
        g_context->Release();
        g_context = nullptr;
    }
    if (g_device) {
        g_device->Release();
        g_device = nullptr;
    }
}

LRESULT WINAPI windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) return 0;
        g_resizeWidth = UINT(LOWORD(lParam));  // Resized in the main loop, not here
        g_resizeHeight = UINT(HIWORD(lParam));
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;  // Disable the ALT application menu
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
        // The computer-keyboard piano goes by physical key position (scancode), not by the layout's letters.
        // Extended keys (arrows, right Ctrl, keypad Enter...) are never piano keys.
        if (g_app != nullptr && ((lParam >> 24) & 1) == 0) {
            g_app->onKey(int((lParam >> 16) & 0xFF), msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
        }
        break;
    case WM_KILLFOCUS:
        if (g_app != nullptr) g_app->clearKeys();
        break;
    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wParam);
        wchar_t path[MAX_PATH * 4] = {};
        if (DragQueryFileW(drop, 0, path, UINT(sizeof(path) / sizeof(path[0]))) > 0 && g_app != nullptr) {
            g_app->openFile(std::filesystem::path(path));
        }
        DragFinish(drop);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// The app's style at `scale` (the monitor's DPI scale times the app's zoom), from a fresh style so that scalings do not
// pile up.
void setUpStyle(const HostWindow& window, float scale) {
    ImGui::GetStyle() = ImGuiStyle();
    window.applyStyle();
    ImGuiStyle& style = ImGui::GetStyle();
    style.FontSizeBase = 16.0f;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
}

// The window at the app's new zoom as it opens at that zoom (the requested size at `scale`, within 95% of the work
// area of its monitor), where it is: choosing a size again gives that size, however the window was dragged. A maximised
// window keeps its size.
void resizeForZoom(HWND hwnd, const HostWindow& window, float scale) {
    if (IsZoomed(hwnd)) return;
    MONITORINFO monitor = {};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
    const RECT& work = monitor.rcWork;
    const int width = std::min(int(float(window.width) * scale), int(float(work.right - work.left) * 0.95f));
    const int height = std::min(int(float(window.height) * scale), int(float(work.bottom - work.top) * 0.95f));
    RECT current = {};
    GetWindowRect(hwnd, &current);
    const int x = std::clamp(int(current.left), int(work.left), int(work.right) - width);
    const int y = std::clamp(int(current.top), int(work.top), int(work.bottom) - height);
    SetWindowPos(hwnd, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

}  // namespace

void loadUiFonts() {
    UiStyle::loadFonts();
}

int runHost(HINSTANCE instance, const HostWindow& window, HostedApp& app, const std::function<void()>& start,
            const std::function<void()>& stop) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // For the folder picker dialog
    ImGui_ImplWin32_EnableDpiAwareness();
    const float dpiScale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
    int zoom = 100;  // The app's (View > Window size), known once it has started

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = windowProc;
    wc.hInstance = instance;
    // The program's icon (resource 1: D110Emu's res/D110Emu.rc), at the sizes of the title bar and the taskbar; Windows'
    // own for programs without one.
    wc.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                             GetSystemMetrics(SM_CYICON), LR_SHARED));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    if (wc.hIcon == nullptr) wc.hIcon = LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION));
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.lpszClassName = window.className;
    RegisterClassExW(&wc);
    // The requested size at the monitor's scaling and the app's zoom, shrunk to fit the work area of smaller or high-DPI
    // screens, and centred.
    RECT workArea = {0, 0, 1920, 1080};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    auto placeWindow = [&](HWND hwnd) {
        const float scale = dpiScale * float(zoom) / 100.0f;
        const int width = std::min(int(float(window.width) * scale), int(float(workArea.right - workArea.left) * 0.95f));
        const int height = std::min(int(float(window.height) * scale), int(float(workArea.bottom - workArea.top) * 0.95f));
        SetWindowPos(hwnd, nullptr, workArea.left + (workArea.right - workArea.left - width) / 2,
                     workArea.top + (workArea.bottom - workArea.top - height) / 2, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    };
    // Shown once the app has started, at its size.
    HWND hwnd = CreateWindowW(wc.lpszClassName, window.title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                              CW_USEDEFAULT, nullptr, nullptr, instance, nullptr);
    placeWindow(hwnd);

    if (!createDevice(hwnd)) {
        cleanupDevice();
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, instance);
        MessageBoxW(nullptr, L"Direct3D 11 could not be initialised.", window.title, MB_ICONERROR);
        return 1;
    }
    DragAcceptFiles(hwnd, TRUE);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;  // Single fixed layout: nothing worth persisting
    setUpStyle(window, dpiScale);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);
    loadUiFonts();

    g_app = &app;
    start();
    zoom = app.uiZoom();
    if (zoom != 100) {
        placeWindow(hwnd);
        setUpStyle(window, dpiScale * float(zoom) / 100.0f);
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv != nullptr) {
        if (argc > 1) app.openFile(std::filesystem::path(argv[1]));  // e.g. a file dropped on the .exe
        LocalFree(argv);
    }

    const float clearColor[4] = {0.09f, 0.09f, 0.10f, 1.0f};
    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done || app.quitRequested()) break;

        // Minimised or screen locked: audio and MIDI keep running on their own threads.
        if (g_swapChainOccluded && g_swapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            Sleep(10);
            continue;
        }
        g_swapChainOccluded = false;

        // View > Window size: the text and the window follow before the next frame.
        const int wantedZoom = app.uiZoom();
        if (wantedZoom != zoom) {
            zoom = wantedZoom;
            resizeForZoom(hwnd, window, dpiScale * float(zoom) / 100.0f);
            setUpStyle(window, dpiScale * float(zoom) / 100.0f);
        }

        if (g_resizeWidth != 0 && g_resizeHeight != 0) {
            cleanupRenderTarget();
            g_swapChain->ResizeBuffers(0, g_resizeWidth, g_resizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeWidth = g_resizeHeight = 0;
            createRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        app.frame();
        ImGui::Render();
        g_context->OMSetRenderTargets(1, &g_renderTarget, nullptr);
        g_context->ClearRenderTargetView(g_renderTarget, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        const HRESULT result = g_swapChain->Present(1, 0);  // vsync
        g_swapChainOccluded = result == DXGI_STATUS_OCCLUDED;
    }

    stop();
    g_app = nullptr;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanupDevice();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, instance);
    CoUninitialize();
    return 0;
}
