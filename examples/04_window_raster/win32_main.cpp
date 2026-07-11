// PanoramaEngine example 04 (Win32 half) -- a real window instead of a .bmp.
//
// Loads a Panorama layout from an XML file on disk, lays it out, rasterizes
// it on the CPU (raster_view.hpp, the same rasterizer as
// examples/02_software_raster), and blits the result into a Win32 window
// with StretchDIBits. Re-lays-out and re-rasterizes on every resize. See
// posix_main.cpp for the X11 equivalent -- both share raster_view.hpp and
// differ only in how they open a window and get pixels on screen.
#include "raster_view.hpp"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <vector>

namespace
{
constexpr wchar_t kWindowClassName[] = L"PanoramaExampleWindowRaster";

struct AppState
{
    panorama_example::RasterDocument document;
    std::vector<std::uint8_t> bgra; // repacked from document.framebuffer() for StretchDIBits
    int width = 0;
    int height = 0;
};

// StretchDIBits expects each pixel as B,G,R,X in memory (little-endian
// 0x00RRGGBB); the engine's draw list -- and our Framebuffer -- use straight
// R,G,B,A, so swap channels once per resize instead of once per WM_PAINT.
void repack_bgra(const panorama_example::Framebuffer& fb, std::vector<std::uint8_t>& out)
{
    out.resize(fb.rgba.size());
    for (std::size_t i = 0; i < fb.rgba.size(); i += 4)
    {
        out[i + 0] = fb.rgba[i + 2]; // B
        out[i + 1] = fb.rgba[i + 1]; // G
        out[i + 2] = fb.rgba[i + 0]; // R
        out[i + 3] = 0;
    }
}

void relayout(HWND hwnd, AppState& state, int width, int height)
{
    if (width <= 0 || height <= 0)
    {
        return;
    }
    state.width = width;
    state.height = height;
    state.document.layout_and_rasterize(width, height);
    repack_bgra(state.document.framebuffer(), state.bgra);
    InvalidateRect(hwnd, nullptr, FALSE);
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    AppState* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message)
    {
    case WM_CREATE:
        return 0;

    case WM_SIZE:
        if (state != nullptr && wparam != SIZE_MINIMIZED)
        {
            relayout(hwnd, *state, LOWORD(lparam), HIWORD(lparam));
        }
        return 0;

    case WM_ERASEBKGND:
        return 1; // WM_PAINT repaints every pixel; avoid the default flicker-y clear

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (state != nullptr && state->width > 0 && state->height > 0)
        {
            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = state->width;
            bmi.bmiHeader.biHeight = -state->height; // negative: top-down DIB
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            StretchDIBits(
                hdc,
                0, 0, state->width, state->height,
                0, 0, state->width, state->height,
                state->bgra.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE)
        {
            DestroyWindow(hwnd);
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}
}

int main(int argc, char** argv)
{
    const std::filesystem::path xml_path = argc > 1
        ? std::filesystem::path(argv[1])
        : std::filesystem::path("../../../examples/04_window_raster/sample/raster.xml");

    if (argc <= 1)
    {
        std::printf("usage: %s <layout.xml>  (no path given, trying %ls)\n", argv[0], xml_path.c_str());
    }

    AppState state;
    if (!state.document.load(xml_path))
    {
        std::fprintf(stderr, "failed to load %ls\n", xml_path.wstring().c_str());
        return 1;
    }

    HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(WNDCLASSEXW);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = nullptr; // WM_ERASEBKGND handles this
    window_class.lpszClassName = kWindowClassName;
    RegisterClassExW(&window_class);

    constexpr int kInitialWidth = 960;
    constexpr int kInitialHeight = 540;

    RECT window_rect{0, 0, kInitialWidth, kInitialHeight};
    AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindowExW(
        0, kWindowClassName, L"PanoramaEngine - Window Raster",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        window_rect.right - window_rect.left, window_rect.bottom - window_rect.top,
        nullptr, nullptr, instance, nullptr);
    if (hwnd == nullptr)
    {
        std::fprintf(stderr, "failed to create window\n");
        return 1;
    }

    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));

    RECT client_rect{};
    GetClientRect(hwnd, &client_rect);
    relayout(hwnd, state, client_rect.right - client_rect.left, client_rect.bottom - client_rect.top);

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
