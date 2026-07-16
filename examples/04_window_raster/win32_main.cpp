// PanoramaEngine example 04 (Win32 half) -- a real window instead of a .bmp.
//
// Loads a Panorama layout from an XML file on disk, lays it out, rasterizes
// it on the CPU (raster_view.hpp, the same rasterizer as
// examples/02_software_raster), and blits the result into a Win32 window
// with StretchDIBits. Feeds mouse, keyboard, and text input to the engine's
// PanoramaInputController and boots the PanoramaRuntime (QuickJS) so buttons,
// text entries, and sliders are fully interactive. See posix_main.cpp for the
// X11 equivalent -- both share raster_view.hpp and differ only in how they
// open a window, gather input, and get pixels on screen.
#include "raster_view.hpp"

#include <windows.h>
#include <windowsx.h>

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
    bool mouse_down = false;
    float mouse_x = 0.0F;
    float mouse_y = 0.0F;
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

// Helper: run a full update_frame + repack + invalidate.
void refresh(HWND hwnd, AppState& state, float wheel = 0.0F,
             const panorama::PanoramaKeyEvent* key = nullptr, std::string_view text = {})
{
    state.document.update_frame(
        state.width, state.height,
        state.mouse_x, state.mouse_y,
        state.mouse_down, wheel,
        key, text);
    repack_bgra(state.document.framebuffer(), state.bgra);
    InvalidateRect(hwnd, nullptr, FALSE);
}

// Map Win32 virtual-key codes to the engine's PanoramaKey enum.
panorama::PanoramaKey vk_to_panorama_key(WPARAM vk)
{
    switch (vk)
    {
    case VK_LEFT:   return panorama::PanoramaKey::ArrowLeft;
    case VK_RIGHT:  return panorama::PanoramaKey::ArrowRight;
    case VK_UP:     return panorama::PanoramaKey::ArrowUp;
    case VK_DOWN:   return panorama::PanoramaKey::ArrowDown;
    case VK_HOME:   return panorama::PanoramaKey::Home;
    case VK_END:    return panorama::PanoramaKey::End;
    case VK_BACK:   return panorama::PanoramaKey::Backspace;
    case VK_DELETE: return panorama::PanoramaKey::Delete;
    case VK_RETURN: return panorama::PanoramaKey::Enter;
    case VK_TAB:    return panorama::PanoramaKey::Tab;
    case VK_ESCAPE: return panorama::PanoramaKey::Escape;
    case 'A':       return panorama::PanoramaKey::A;
    default:        return panorama::PanoramaKey::Unknown;
    }
}

// Convert a single BMP wchar_t to UTF-8 (handles BMP characters only, which
// covers virtually all text-entry use cases). Returns the number of bytes
// written into `out`.
int wchar_to_utf8(wchar_t ch, char* out)
{
    if (ch < 0x80)
    {
        out[0] = static_cast<char>(ch);
        return 1;
    }
    if (ch < 0x800)
    {
        out[0] = static_cast<char>(0xC0 | (ch >> 6));
        out[1] = static_cast<char>(0x80 | (ch & 0x3F));
        return 2;
    }
    out[0] = static_cast<char>(0xE0 | (ch >> 12));
    out[1] = static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
    out[2] = static_cast<char>(0x80 | (ch & 0x3F));
    return 3;
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
            state->width = LOWORD(lparam);
            state->height = HIWORD(lparam);
            if (state->width > 0 && state->height > 0)
            {
                refresh(hwnd, *state);
            }
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

    case WM_MOUSEMOVE:
        if (state != nullptr)
        {
            state->mouse_x = static_cast<float>(GET_X_LPARAM(lparam));
            state->mouse_y = static_cast<float>(GET_Y_LPARAM(lparam));
            refresh(hwnd, *state);
        }
        return 0;

    case WM_LBUTTONDOWN:
        if (state != nullptr)
        {
            SetCapture(hwnd);
            state->mouse_down = true;
            state->mouse_x = static_cast<float>(GET_X_LPARAM(lparam));
            state->mouse_y = static_cast<float>(GET_Y_LPARAM(lparam));
            refresh(hwnd, *state);
        }
        return 0;

    case WM_LBUTTONUP:
        if (state != nullptr)
        {
            ReleaseCapture();
            state->mouse_down = false;
            state->mouse_x = static_cast<float>(GET_X_LPARAM(lparam));
            state->mouse_y = static_cast<float>(GET_Y_LPARAM(lparam));
            refresh(hwnd, *state);
        }
        return 0;

    case WM_MOUSEWHEEL:
        if (state != nullptr)
        {
            // WM_MOUSEWHEEL delivers pointer position in SCREEN coordinates.
            POINT pt;
            pt.x = GET_X_LPARAM(lparam);
            pt.y = GET_Y_LPARAM(lparam);
            ScreenToClient(hwnd, &pt);
            state->mouse_x = static_cast<float>(pt.x);
            state->mouse_y = static_cast<float>(pt.y);

            const float wheel_ticks =
                static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / static_cast<float>(WHEEL_DELTA);
            refresh(hwnd, *state, wheel_ticks);
        }
        return 0;

    case WM_KEYDOWN:
        if (state != nullptr)
        {
            if (wparam == VK_ESCAPE)
            {
                DestroyWindow(hwnd);
                return 0;
            }

            const panorama::PanoramaKey key = vk_to_panorama_key(wparam);
            if (key != panorama::PanoramaKey::Unknown)
            {
                panorama::PanoramaKeyEvent event;
                event.key = key;
                event.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                event.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                event.alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
                refresh(hwnd, *state, 0.0F, &event);
            }
        }
        return 0;

    case WM_CHAR:
        if (state != nullptr)
        {
            const wchar_t ch = static_cast<wchar_t>(wparam);

            // Skip control characters (tab, enter, backspace, etc.) -- those
            // are handled via WM_KEYDOWN above. WM_CHAR also sends '\r' for
            // Enter and '\t' for Tab; skip them to avoid double-handling.
            if (ch < 0x20 || ch == 0x7F || ch == L'\r')
            {
                break;
            }

            char utf8[4] = {};
            const int len = wchar_to_utf8(ch, utf8);
            refresh(hwnd, *state, 0.0F, nullptr, std::string_view(utf8, len));
        }
        break;

    case WM_MOUSELEAVE:
        if (state != nullptr)
        {
            state->mouse_x = -1.0F;
            state->mouse_y = -1.0F;
            refresh(hwnd, *state);
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
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
    state.width = client_rect.right - client_rect.left;
    state.height = client_rect.bottom - client_rect.top;

    // Initial layout + rasterize before the runtime exists.
    state.document.layout_and_rasterize(state.width, state.height);
    repack_bgra(state.document.framebuffer(), state.bgra);

    // Boot the JS runtime against the already-laid-out tree.
    if (!state.document.init_runtime())
    {
        std::fprintf(stderr, "failed to init runtime\n");
        return 1;
    }

    // Runtime init may have mutated the DOM; re-layout + rasterize.
    state.document.update_frame(
        state.width, state.height,
        state.mouse_x, state.mouse_y,
        state.mouse_down, 0.0F,
        nullptr, nullptr);
    repack_bgra(state.document.framebuffer(), state.bgra);

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    state.document.shutdown_runtime();
    return static_cast<int>(msg.wParam);
}
