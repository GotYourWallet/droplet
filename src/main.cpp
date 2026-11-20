#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <cstdio>
#include <cmath>
#include <string>
#include <chrono>
#include "renderer.h"

using namespace math;

namespace {

struct AppState {
    Renderer renderer;
    DemoSettings settings;
    bool running = true;
    bool forceDrop = false;
    bool vsync = false;
    bool isFullscreen = false;
    RECT savedWindowRect = {};
    DWORD savedWindowStyle = 0;

    // Interactive orbit camera offsets
    bool dragging = false;
    POINT lastMouse = {};
    float userYaw = 0.0f;
    float userPitch = 0.0f;
    float userZoom = 1.0f;
};

AppState gApp;

void toggleFullscreen(HWND hwnd) {
    if (!gApp.isFullscreen) {
        gApp.savedWindowStyle = GetWindowLong(hwnd, GWL_STYLE);
        GetWindowRect(hwnd, &gApp.savedWindowRect);
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfo(mon, &mi);
        SetWindowLong(hwnd, GWL_STYLE, gApp.savedWindowStyle & ~(WS_CAPTION | WS_THICKFRAME));
        SetWindowPos(hwnd, HWND_TOP,
            mi.rcMonitor.left, mi.rcMonitor.top,
            mi.rcMonitor.right - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top,
            SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        gApp.isFullscreen = true;
    } else {
        SetWindowLong(hwnd, GWL_STYLE, gApp.savedWindowStyle);
        SetWindowPos(hwnd, nullptr,
            gApp.savedWindowRect.left, gApp.savedWindowRect.top,
            gApp.savedWindowRect.right - gApp.savedWindowRect.left,
            gApp.savedWindowRect.bottom - gApp.savedWindowRect.top,
            SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        gApp.isFullscreen = false;
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_DESTROY:
            gApp.running = false;
            PostQuitMessage(0);
            return 0;

        case WM_SIZE: {
            uint32_t w = LOWORD(lp);
            uint32_t h = HIWORD(lp);
            if (w >= 64 && h >= 64) {
                gApp.renderer.resize(w, h);
            }
            return 0;
        }

        case WM_LBUTTONDOWN:
            gApp.dragging = true;
            gApp.lastMouse.x = GET_X_LPARAM(lp);
            gApp.lastMouse.y = GET_Y_LPARAM(lp);
            SetCapture(hwnd);
            return 0;

        case WM_LBUTTONUP:
            gApp.dragging = false;
            ReleaseCapture();
            return 0;

        case WM_MOUSEMOVE:
            if (gApp.dragging) {
                int x = GET_X_LPARAM(lp);
                int y = GET_Y_LPARAM(lp);
                gApp.userYaw   += (x - gApp.lastMouse.x) * 0.0055f;
                gApp.userPitch = clamp(gApp.userPitch + (y - gApp.lastMouse.y) * 0.0045f, -0.75f, 0.75f);
                gApp.lastMouse = {x, y};
            }
            return 0;

        case WM_MOUSEWHEEL: {
            short delta = GET_WHEEL_DELTA_WPARAM(wp);
            gApp.userZoom = clamp(gApp.userZoom * (delta > 0 ? 0.90f : 1.11f), 0.35f, 2.6f);
            return 0;
        }

        case WM_KEYDOWN:
            switch (wp) {
                case VK_ESCAPE: gApp.running = false; PostQuitMessage(0); break;
                case VK_SPACE:  gApp.settings.paused = !gApp.settings.paused; break;
                case 'R':       gApp.forceDrop = true; break;
                case 'W':       gApp.settings.wireframe = !gApp.settings.wireframe; break;
                case 'D':       gApp.settings.dofEnabled = !gApp.settings.dofEnabled; break;
                case 'V':       gApp.settings.volumetricEnabled = !gApp.settings.volumetricEnabled; break;
                case 'B':       gApp.settings.bloomEnabled = !gApp.settings.bloomEnabled; break;
                case 'H':       gApp.settings.showFps = !gApp.settings.showFps; break;
                case '1': gApp.settings.cameraPreset = 0; gApp.userYaw = gApp.userPitch = 0.0f; gApp.userZoom = 1.0f; break;
                case '2': gApp.settings.cameraPreset = 1; gApp.userYaw = gApp.userPitch = 0.0f; gApp.userZoom = 1.0f; break;
                case '3': gApp.settings.cameraPreset = 2; gApp.userYaw = gApp.userPitch = 0.0f; gApp.userZoom = 1.0f; break;
                case '4': gApp.settings.cameraPreset = 3; gApp.userYaw = gApp.userPitch = 0.0f; gApp.userZoom = 1.0f; break;
                case '5': gApp.settings.cameraPreset = 4; gApp.userYaw = gApp.userPitch = 0.0f; gApp.userZoom = 1.0f; break;
                case VK_F11:    toggleFullscreen(hwnd); break;
            }
            return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

void computeCamera(int preset, float time, Vec3& outPos, Vec3& outTarget) {
    float swayX = std::sin(time * 0.28f) * 0.06f;
    float swayY = std::cos(time * 0.21f) * 0.035f;

    Vec3 baseOffset;
    switch (preset) {
        case 1: // Macro Droplet Close-Up
            outTarget = Vec3(0.0f, 1.18f, 0.0f);
            baseOffset = Vec3(0.28f + swayX * 0.5f, 0.04f + swayY * 0.5f, -0.78f);
            break;
        case 2: // Pool Impact & Underwater Caustics
            outTarget = Vec3(0.0f, -0.35f, 0.0f);
            baseOffset = Vec3(0.95f + swayX, 1.05f + swayY, -1.75f);
            break;
        case 3: // Backlit Sunbeams & SSS Leaves
            outTarget = Vec3(-0.18f, 1.48f, 0.08f);
            baseOffset = Vec3(-0.75f + swayX, -0.52f + swayY, -1.85f);
            break;
        case 4: { // Slow Cinematic Orbit
            outTarget = Vec3(-0.12f, 0.82f, 0.0f);
            float a = time * 0.22f;
            baseOffset = Vec3(std::sin(a) * 2.55f, 0.48f + swayY, -std::cos(a) * 2.55f);
            break;
        }
        case 0:
        default: // Hero Composition (Branch + Droplet + Pool + Sunbeams)
            outTarget = Vec3(-0.18f, 0.84f, 0.0f);
            baseOffset = Vec3(0.58f + swayX, 0.26f + swayY, -2.28f);
            break;
    }

    // Apply interactive user orbit & zoom
    float r = length(baseOffset) * gApp.userZoom;
    float baseYaw = std::atan2(baseOffset.x, baseOffset.z);
    float basePitch = std::asin(clamp(baseOffset.y / std::max(length(baseOffset), 1e-4f), -0.95f, 0.95f));

    float yaw = baseYaw + gApp.userYaw;
    float pitch = clamp(basePitch + gApp.userPitch, -1.15f, 1.25f);

    Vec3 finalOffset(
        r * std::cos(pitch) * std::sin(yaw),
        r * std::sin(pitch),
        r * std::cos(pitch) * std::cos(yaw)
    );
    outPos = outTarget + finalOffset;
}

std::wstring findShaderDir() {
    // Check ./shaders first, then ../shaders relative to exe directory
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring exeDir(exePath);
    size_t slash = exeDir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) exeDir = exeDir.substr(0, slash);

    auto exists = [](const std::wstring& p) {
        DWORD attr = GetFileAttributesW((p + L"\\common.hlsli").c_str());
        return attr != INVALID_FILE_ATTRIBUTES;
    };
    if (exists(L"shaders")) return L"shaders";
    if (exists(exeDir + L"\\shaders")) return exeDir + L"\\shaders";
    if (exists(exeDir + L"\\..\\shaders")) return exeDir + L"\\..\\shaders";
    return L"shaders";
}

} // namespace

int main(int argc, char** argv) {
    // Default target: 1440p (2560x1440); automatically fits desktop if smaller unless explicit --width/--height given
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    uint32_t reqW = (screenW >= 2560) ? 2560 : std::min(screenW - 80, 1920);
    uint32_t reqH = (screenH >= 1440) ? 1440 : std::min(screenH - 80, 1080);
    if (reqW < 640) reqW = 1280;
    if (reqH < 480) reqH = 720;

    std::string screenshotPath;
    int maxFrames = -1;
    bool startFullscreen = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--width" && i + 1 < argc)      reqW = std::atoi(argv[++i]);
        else if (arg == "--height" && i + 1 < argc) reqH = std::atoi(argv[++i]);
        else if (arg == "--screenshot" && i + 1 < argc) screenshotPath = argv[++i];
        else if (arg == "--frames" && i + 1 < argc) maxFrames = std::atoi(argv[++i]);
        else if (arg == "--preset" && i + 1 < argc) gApp.settings.cameraPreset = std::atoi(argv[++i]) - 1;
        else if (arg == "--wireframe")              gApp.settings.wireframe = true;
        else if (arg == "--fullscreen")             startFullscreen = true;
        else if (arg == "--vsync")                  gApp.vsync = true;
    }

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DropletDemoWindowClass";
    RegisterClassExW(&wc);

    RECT rc = {0, 0, static_cast<LONG>(reqW), static_cast<LONG>(reqH)};
    DWORD style = WS_OVERLAPPEDWINDOW;
    if (screenshotPath.empty()) {
        AdjustWindowRect(&rc, style, FALSE);
    } else {
        style = WS_POPUP; // Allow exact 2560x1440 capture even on smaller desktop
    }

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"Droplet DX11 Graphics Demo (1440p)",
        style, CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!hwnd) {
        fprintf(stderr, "Failed to create Win32 window.\n");
        return 1;
    }

    std::wstring shaderDir = findShaderDir();
    if (!gApp.renderer.init(hwnd, reqW, reqH, shaderDir)) {
        fprintf(stderr, "Failed to initialize DirectX 11 renderer.\n");
        return 1;
    }

    if (screenshotPath.empty()) {
        ShowWindow(hwnd, SW_SHOW);
        if (startFullscreen) toggleFullscreen(hwnd);
    }

    printf("Droplet DX11 Graphics Demo initialized.\n");
    printf("  GPU Adapter : %s\n", gApp.renderer.adapterName().c_str());
    printf("  Resolution  : %ux%u\n", gApp.renderer.width(), gApp.renderer.height());
    printf("  Controls    : Mouse Drag=Orbit | Scroll=Zoom | 1-5=Cameras | R=Drop | W=Wireframe | D=DoF | V=GodRays | B=Bloom | H=FPS HUD | Space=Pause | F11=Fullscreen\n");

    auto tStart = std::chrono::high_resolution_clock::now();
    auto tPrev  = tStart;
    double fpsAccum = 0.0;
    int fpsFrames = 0;
    int totalFrames = 0;

    MSG msg = {};
    while (gApp.running) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!gApp.running) break;

        auto tNow = std::chrono::high_resolution_clock::now();
        double rawDt = std::chrono::duration<double>(tNow - tPrev).count();
        tPrev = tNow;
        float dt = clamp(static_cast<float>(rawDt), 0.001f, 0.05f);

        // Deterministic timestep when running headless screenshot capture
        if (!screenshotPath.empty()) dt = 1.0f / 60.0f;
        static float simTime = 0.0f;
        if (!gApp.settings.paused) simTime += dt;

        gApp.renderer.updateSimulation(dt, simTime, gApp.settings, gApp.forceDrop);
        gApp.forceDrop = false;

        Vec3 camPos, camTarget;
        computeCamera(gApp.settings.cameraPreset, simTime, camPos, camTarget);
        gApp.renderer.renderFrame(camPos, camTarget, simTime, gApp.settings);
        gApp.renderer.present(gApp.vsync);

        ++totalFrames;
        fpsFrames++;
        fpsAccum += rawDt;

        // Keep on-screen FPS counter responsive even during short runs, smoothed every 0.25s interactively
        if (fpsAccum >= 0.25 || (!screenshotPath.empty() && totalFrames >= 10)) {
            float fps = static_cast<float>(fpsFrames / std::max(fpsAccum, 1e-6));
            float ms = 1000.0f / std::max(fps, 0.1f);
            gApp.settings.displayFps = fps;
            gApp.settings.displayFrameMs = ms;
            if (fpsAccum >= 0.25 && screenshotPath.empty()) {
                wchar_t title[256];
                swprintf_s(title,
                    L"Droplet DX11 | %ux%u | %.1f FPS (%.2f ms) | Cam %d | [R] Drop [W] Wire:%ls [D] DoF:%ls [V] Rays:%ls [H] HUD [F11] Full",
                    gApp.renderer.width(), gApp.renderer.height(), fps, ms,
                    gApp.settings.cameraPreset + 1,
                    gApp.settings.wireframe ? L"ON" : L"OFF",
                    gApp.settings.dofEnabled ? L"ON" : L"OFF",
                    gApp.settings.volumetricEnabled ? L"ON" : L"OFF");
                SetWindowTextW(hwnd, title);
                fpsFrames = 0;
                fpsAccum = 0.0;
            }
        }

        if (maxFrames > 0 && totalFrames >= maxFrames) {
            auto tEnd = std::chrono::high_resolution_clock::now();
            double elapsedSec = std::chrono::duration<double>(tEnd - tStart).count();
            double avgFps = totalFrames / std::max(elapsedSec, 1e-6);
            printf("Rendered %d frames in %.2f s -> Avg %.1f FPS (%.2f ms/frame) at %ux%u\n",
                totalFrames, elapsedSec, avgFps, 1000.0 / std::max(avgFps, 0.1),
                gApp.renderer.width(), gApp.renderer.height());
            if (!screenshotPath.empty()) {
                if (gApp.renderer.saveBackbufferBMP(screenshotPath)) {
                    printf("Saved screenshot to %s\n", screenshotPath.c_str());
                } else {
                    fprintf(stderr, "Failed to save screenshot to %s\n", screenshotPath.c_str());
                }
            }
            break;
        }
    }

    return 0;
}
