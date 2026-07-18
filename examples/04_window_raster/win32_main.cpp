// panoramaEngine example 04 (Win32 half) -- a real window instead of a .bmp.
//
// Loads a panorama layout from an XML file on disk, lays it out, rasterizes
// it on the CPU (raster_view.hpp, the same rasterizer as
// examples/02_software_raster), and blits the result into a Win32 window
// with StretchDIBits. Feeds mouse, keyboard, and text input to the engine's
// panoramaInputController and boots the panoramaRuntime (QuickJS) so buttons,
// text entries, and sliders are fully interactive. See posix_main.cpp for the
// X11 equivalent -- both share raster_view.hpp and differ only in how they
// open a window, gather input, and get pixels on screen.
#include "raster_view.hpp"

#include <windows.h>
#include <windowsx.h>

#include <cstdio>
#include <filesystem>
#include <vector>
#include <string>

namespace
{
    constexpr wchar_t kWindowClassName[] = L"panoramaExampleWindowRaster";

    struct AppState
    {
        panorama_example::RasterDocument document;
        std::vector<std::uint8_t> bgra; // repacked from document.framebuffer() for StretchDIBits
        int width = 0;
        int height = 0;
        bool mouse_down = false;
        float mouse_x = 0.0F;
        float mouse_y = 0.0F;

        bool is_dirty = false;
        float wheel_delta = 0.0F;
        std::vector<panorama::panoramaKeyEvent> pending_keys;
        std::string pending_text;
    };

    // StretchDIBits expects each pixel as B,G,R,X in memory (little-endian
    // 0x00RRGGBB); the engine's draw list -- and our Framebuffer -- use straight
    // R,G,B,A. Optimized to cast to 32-bit uints for streamlined vectorization.
    void repack_bgra(const panorama_example::Framebuffer& fb, std::vector<std::uint8_t>& out)
    {
        out.resize(fb.rgba.size());
        const std::uint32_t* src = reinterpret_cast<const std::uint32_t*>(fb.rgba.data());
        std::uint32_t* dst = reinterpret_cast<std::uint32_t*>(out.data());
        const std::size_t pixel_count = fb.rgba.size() / 4;

        for (std::size_t i = 0; i < pixel_count; ++i)
        {
            const std::uint32_t rgba = src[i];
            const std::uint32_t r = (rgba & 0x000000FF) << 16;
            const std::uint32_t g = (rgba & 0x0000FF00);
            const std::uint32_t b = (rgba & 0x00FF0000) >> 16;
            dst[i] = r | g | b;
        }
    }

    // Map Win32 virtual-key codes to the engine's panoramaKey enum.
    panorama::panoramaKey vk_to_panorama_key(WPARAM vk)
    {
        switch (vk)
        {
        case VK_LEFT:   return panorama::panoramaKey::ArrowLeft;
        case VK_RIGHT:  return panorama::panoramaKey::ArrowRight;
        case VK_UP:     return panorama::panoramaKey::ArrowUp;
        case VK_DOWN:   return panorama::panoramaKey::ArrowDown;
        case VK_HOME:   return panorama::panoramaKey::Home;
        case VK_END:    return panorama::panoramaKey::End;
        case VK_BACK:   return panorama::panoramaKey::Backspace;
        case VK_DELETE: return panorama::panoramaKey::Delete;
        case VK_RETURN: return panorama::panoramaKey::Enter;
        case VK_TAB:    return panorama::panoramaKey::Tab;
        case VK_ESCAPE: return panorama::panoramaKey::Escape;
        case 'A':       return panorama::panoramaKey::A;
        default:        return panorama::panoramaKey::Unknown;
        }
    }

    // Convert a single BMP wchar_t to UTF-8
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

        if (state == nullptr && message != WM_CREATE && message != WM_DESTROY)
        {
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }

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
                    state->is_dirty = true;
                }
            }
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            if (state != nullptr && state->width > 0 && state->height > 0)
            {
                BITMAPINFO bmi{};
                bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bmi.bmiHeader.biWidth = state->width;
                bmi.bmiHeader.biHeight = -state->height;
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
                state->is_dirty = true;
            }
            return 0;

        case WM_LBUTTONDOWN:
            if (state != nullptr)
            {
                SetCapture(hwnd);
                state->mouse_down = true;
                state->mouse_x = static_cast<float>(GET_X_LPARAM(lparam));
                state->mouse_y = static_cast<float>(GET_Y_LPARAM(lparam));
                state->is_dirty = true;
            }
            return 0;

        case WM_LBUTTONUP:
            if (state != nullptr)
            {
                ReleaseCapture();
                state->mouse_down = false;
                state->mouse_x = static_cast<float>(GET_X_LPARAM(lparam));
                state->mouse_y = static_cast<float>(GET_Y_LPARAM(lparam));
                state->is_dirty = true;
            }
            return 0;

        case WM_MOUSEWHEEL:
            if (state != nullptr)
            {
                POINT pt;
                pt.x = GET_X_LPARAM(lparam);
                pt.y = GET_Y_LPARAM(lparam);
                ScreenToClient(hwnd, &pt);
                state->mouse_x = static_cast<float>(pt.x);
                state->mouse_y = static_cast<float>(pt.y);

                state->wheel_delta +=
                    static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / static_cast<float>(WHEEL_DELTA);
                state->is_dirty = true;
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

                const panorama::panoramaKey key = vk_to_panorama_key(wparam);
                if (key != panorama::panoramaKey::Unknown)
                {
                    panorama::panoramaKeyEvent event;
                    event.key = key;
                    event.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                    event.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                    event.alt = (GetKeyState(VK_MENU) & 0x8000) != 0;

                    state->pending_keys.push_back(event);
                    state->is_dirty = true;
                }
            }
            return 0;

        case WM_CHAR:
            if (state != nullptr)
            {
                const wchar_t ch = static_cast<wchar_t>(wparam);

                if (ch < 0x20 || ch == 0x7F || ch == L'\r')
                {
                    break;
                }

                char utf8[4] = {};
                const int len = wchar_to_utf8(ch, utf8);
                state->pending_text.append(utf8, len);
                state->is_dirty = true;
            }
            break;

        case WM_MOUSELEAVE:
            if (state != nullptr)
            {
                state->mouse_x = -1.0F;
                state->mouse_y = -1.0F;
                state->is_dirty = true;
            }
            return 0;

        case WM_DESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
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
        : std::filesystem::path("../examples/04_window_raster/sample/raster.xml");

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
    window_class.hbrBackground = nullptr;
    window_class.lpszClassName = kWindowClassName;
    RegisterClassExW(&window_class);

    constexpr int kInitialWidth = 960;
    constexpr int kInitialHeight = 540;

    RECT window_rect{ 0, 0, kInitialWidth, kInitialHeight };
    AdjustWindowRect(&window_rect, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindowExW(
        0, kWindowClassName, L"panoramaEngine - Window Raster",
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

    state.document.layout_and_rasterize(state.width, state.height);
    repack_bgra(state.document.framebuffer(), state.bgra);

    if (!state.document.init_runtime())
    {
        std::fprintf(stderr, "failed to init runtime\n");
        return 1;
    }

    state.document.update_frame(
        state.width, state.height,
        state.mouse_x, state.mouse_y,
        state.mouse_down, 0.0F,
        nullptr, {});
    repack_bgra(state.document.framebuffer(), state.bgra);

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    // Dynamic, decoupled main game loop implementation
    MSG msg{};
    bool running = true;
    while (running)
    {
        // 1. Flush ALL queued Windows input messages into our state buffers immediately
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (!running)
        {
            break;
        }

        // 2. Process our engine frame exactly once per tick if inputs have modified the environment
        if (state.is_dirty)
        {
            // Process any accumulated keyboard triggers sequentially or run the latest
            if (!state.pending_keys.empty())
            {
                for (size_t i = 0; i < state.pending_keys.size(); ++i)
                {
                    bool is_last = (i == state.pending_keys.size() - 1);
                    state.document.update_frame(
                        state.width, state.height,
                        state.mouse_x, state.mouse_y,
                        state.mouse_down,
                        is_last ? state.wheel_delta : 0.0F,
                        &state.pending_keys[i],
                        is_last ? std::string_view(state.pending_text) : std::string_view{}
                    );
                }
            }
            else
            {
                // Simple mouse frame processing layout step
                state.document.update_frame(
                    state.width, state.height,
                    state.mouse_x, state.mouse_y,
                    state.mouse_down, state.wheel_delta,
                    nullptr, state.pending_text
                );
            }

            // Clear buffers for subsequent tick
            state.wheel_delta = 0.0F;
            state.pending_keys.clear();
            state.pending_text.clear();
            state.is_dirty = false;

            // Repack using optimized 32-bit pixel loop and request draw operations
            repack_bgra(state.document.framebuffer(), state.bgra);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        else
        {
            // Yield CPU back to OS during idle states to minimize processing overhead
            Sleep(1);
        }
    }

    state.document.shutdown_runtime();
    panorama_example::CpuTextureStore::s_is_exiting = true;

    return static_cast<int>(msg.wParam);
}
