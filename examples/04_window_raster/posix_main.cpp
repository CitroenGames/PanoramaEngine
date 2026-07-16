// PanoramaEngine example 04 (POSIX half) -- a real window instead of a .bmp.
//
// Loads a Panorama layout from an XML file on disk, lays it out, rasterizes
// it on the CPU (raster_view.hpp, the same rasterizer as
// examples/02_software_raster), and blits the result into an X11 window with
// XPutImage. Feeds mouse, keyboard, and text input to the engine's
// PanoramaInputController and boots the PanoramaRuntime (QuickJS) so buttons,
// text entries, and sliders are fully interactive. See win32_main.cpp for the
// Windows equivalent -- both share raster_view.hpp and differ only in how they
// open a window, gather input, and get pixels on screen.
#include "raster_view.hpp"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

namespace
{
// Packs the engine's straight R,G,B,A framebuffer into whatever channel
// order the X server's default visual actually wants, derived from its own
// red/green/blue masks rather than assumed -- almost always B,G,R on a
// little-endian TrueColor desktop, but this also works on an unusual one.
class PixelPacker
{
public:
    explicit PixelPacker(const Visual* visual)
        : red_shift_(mask_shift(visual->red_mask))
        , green_shift_(mask_shift(visual->green_mask))
        , blue_shift_(mask_shift(visual->blue_mask))
    {
    }

    [[nodiscard]] std::uint32_t pack(std::uint8_t r, std::uint8_t g, std::uint8_t b) const
    {
        return (static_cast<std::uint32_t>(r) << red_shift_) |
               (static_cast<std::uint32_t>(g) << green_shift_) |
               (static_cast<std::uint32_t>(b) << blue_shift_);
    }

private:
    static int mask_shift(unsigned long mask)
    {
        int shift = 0;
        while (mask != 0 && (mask & 1UL) == 0)
        {
            mask >>= 1;
            ++shift;
        }
        return shift;
    }

    int red_shift_;
    int green_shift_;
    int blue_shift_;
};

void pack_framebuffer(const panorama_example::Framebuffer& fb, const PixelPacker& packer, std::vector<std::uint32_t>& out)
{
    const std::size_t pixel_count = static_cast<std::size_t>(fb.width) * static_cast<std::size_t>(fb.height);
    out.resize(pixel_count);
    for (std::size_t i = 0; i < pixel_count; ++i)
    {
        out[i] = packer.pack(fb.rgba[i * 4 + 0], fb.rgba[i * 4 + 1], fb.rgba[i * 4 + 2]);
    }
}

// Map X11 KeySyms to the engine's PanoramaKey enum.
panorama::PanoramaKey keysym_to_panorama_key(KeySym keysym)
{
    switch (keysym)
    {
    case XK_Left:    return panorama::PanoramaKey::ArrowLeft;
    case XK_Right:   return panorama::PanoramaKey::ArrowRight;
    case XK_Up:      return panorama::PanoramaKey::ArrowUp;
    case XK_Down:    return panorama::PanoramaKey::ArrowDown;
    case XK_Home:    return panorama::PanoramaKey::Home;
    case XK_End:     return panorama::PanoramaKey::End;
    case XK_BackSpace: return panorama::PanoramaKey::Backspace;
    case XK_Delete:  return panorama::PanoramaKey::Delete;
    case XK_Return:  return panorama::PanoramaKey::Enter;
    case XK_Tab:     return panorama::PanoramaKey::Tab;
    case XK_Escape:  return panorama::PanoramaKey::Escape;
    case XK_a:       return panorama::PanoramaKey::A;
    default:         return panorama::PanoramaKey::Unknown;
    }
}

// Blit the document's framebuffer to the X11 window.
void blit(Display* display, Window window, GC gc, int width, int height,
          const panorama_example::RasterDocument& document, const PixelPacker& packer,
          std::vector<std::uint32_t>& packed)
{
    pack_framebuffer(document.framebuffer(), packer, packed);

    Visual* visual = DefaultVisual(display, DefaultScreen(display));
    const int depth = DefaultDepth(display, DefaultScreen(display));

    XImage* image = XCreateImage(
        display, visual, static_cast<unsigned int>(depth), ZPixmap, 0,
        reinterpret_cast<char*>(packed.data()),
        static_cast<unsigned int>(width), static_cast<unsigned int>(height),
        32, 0);
    if (image == nullptr)
    {
        return;
    }
    XPutImage(
        display, window, gc, image, 0, 0, 0, 0,
        static_cast<unsigned int>(width), static_cast<unsigned int>(height));
    image->data = nullptr; // `packed` owns the pixel storage; don't let XDestroyImage free() it
    XDestroyImage(image);
}
}

int main(int argc, char** argv)
{
    const std::filesystem::path xml_path = argc > 1
        ? std::filesystem::path(argv[1])
        : std::filesystem::path("../../../examples/04_window_raster/sample/raster.xml");

    if (argc <= 1)
    {
        std::printf("usage: %s <layout.xml>  (no path given, trying %s)\n", argv[0], xml_path.c_str());
    }

    panorama_example::RasterDocument document;
    if (!document.load(xml_path))
    {
        std::fprintf(stderr, "failed to load %s\n", xml_path.c_str());
        return 1;
    }

    Display* display = XOpenDisplay(nullptr);
    if (display == nullptr)
    {
        std::fprintf(stderr, "failed to open X display (is $DISPLAY set?)\n");
        return 1;
    }

    const int screen = DefaultScreen(display);
    Visual* visual = DefaultVisual(display, screen);
    const int depth = DefaultDepth(display, screen);
    if (depth < 24)
    {
        std::fprintf(stderr, "this example needs a >=24-bit TrueColor display (got depth %d)\n", depth);
        XCloseDisplay(display);
        return 1;
    }

    int width = 960;
    int height = 540;
    Window window = XCreateSimpleWindow(
        display, RootWindow(display, screen),
        0, 0, static_cast<unsigned int>(width), static_cast<unsigned int>(height),
        0, BlackPixel(display, screen), BlackPixel(display, screen));
    XStoreName(display, window, "PanoramaEngine - Window Raster");
    XSelectInput(display, window, ExposureMask | StructureNotifyMask | KeyPressMask | KeyReleaseMask |
                                  ButtonPressMask | ButtonReleaseMask | PointerMotionMask);

    Atom wm_delete_window = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &wm_delete_window, 1);

    XMapWindow(display, window);

    const PixelPacker packer(visual);
    std::vector<std::uint32_t> packed;
    GC gc = DefaultGC(display, screen);

    // Track pointer state across events.
    float mouse_x = 0.0F;
    float mouse_y = 0.0F;
    bool mouse_down = false;
    bool needs_refresh = false;

    // Helper: run a full update_frame + blit.
    auto refresh = [&]() {
        document.update_frame(
            width, height,
            mouse_x, mouse_y,
            mouse_down, 0.0F,
            nullptr, nullptr);
        blit(display, window, gc, width, height, document, packer, packed);
    };

    // Initial layout + rasterize before the runtime exists.
    document.layout_and_rasterize(width, height);
    blit(display, window, gc, width, height, document, packer, packed);

    // Boot the JS runtime.
    if (!document.init_runtime())
    {
        std::fprintf(stderr, "failed to init runtime\n");
        XCloseDisplay(display);
        return 1;
    }

    // Runtime init may have mutated the DOM; re-layout + rasterize.
    refresh();

    // X11 text input context for composed text (IME / dead keys).
    XIC xic = nullptr;
    {
        XIM xim = XOpenIM(display, nullptr, nullptr, nullptr);
        if (xim != nullptr)
        {
            xic = XCreateIC(xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                            XNClientWindow, window, nullptr);
        }
    }

    bool running = true;
    while (running)
    {
        XEvent event;
        XNextEvent(display, &event);

        switch (event.type)
        {
        case Expose:
            if (event.xexpose.count == 0)
            {
                blit(display, window, gc, width, height, document, packer, packed);
            }
            break;

        case ConfigureNotify:
            if (event.xconfigure.width != width || event.xconfigure.height != height)
            {
                width = event.xconfigure.width;
                height = event.xconfigure.height;
                refresh();
            }
            break;

        case MotionNotify:
            mouse_x = static_cast<float>(event.xmotion.x);
            mouse_y = static_cast<float>(event.xmotion.y);
            refresh();
            break;

        case ButtonPress:
            if (event.xbutton.button == Button1)
            {
                mouse_down = true;
                mouse_x = static_cast<float>(event.xbutton.x);
                mouse_y = static_cast<float>(event.xbutton.y);
                refresh();
            }
            else if (event.xbutton.button == Button4 || event.xbutton.button == Button5)
            {
                // Scroll wheel: Button4 = up, Button5 = down.
                const float wheel = (event.xbutton.button == Button4) ? 1.0F : -1.0F;
                mouse_x = static_cast<float>(event.xbutton.x);
                mouse_y = static_cast<float>(event.xbutton.y);
                document.update_frame(
                    width, height,
                    mouse_x, mouse_y,
                    mouse_down, wheel,
                    nullptr, nullptr);
                blit(display, window, gc, width, height, document, packer, packed);
            }
            break;

        case ButtonRelease:
            if (event.xbutton.button == Button1)
            {
                mouse_down = false;
                mouse_x = static_cast<float>(event.xbutton.x);
                mouse_y = static_cast<float>(event.xbutton.y);
                refresh();
            }
            break;

        case KeyPress: {
            KeySym keysym = XLookupKeysym(&event.xkey, 0);

            if (keysym == XK_Escape)
            {
                running = false;
                break;
            }

            // Try to get composed text from the input context first.
            char buf[32] = {};
            Status status;
            if (xic != nullptr && Xutf8LookupString(xic, &event.xkey, buf, sizeof(buf) - 1, &keysym, &status) > 0)
            {
                // Got composed text -- feed it as text input.
                document.update_frame(
                    width, height,
                    mouse_x, mouse_y,
                    mouse_down, 0.0F,
                    nullptr,
                    std::string_view(buf));
                blit(display, window, gc, width, height, document, packer, packed);
            }
            else
            {
                // Non-character key: feed as a key-down event.
                const panorama::PanoramaKey key = keysym_to_panorama_key(keysym);
                if (key != panorama::PanoramaKey::Unknown)
                {
                    panorama::PanoramaKeyEvent event_key;
                    event_key.key = key;
                    event_key.shift = (event.xkey.state & ShiftMask) != 0;
                    event_key.ctrl = (event.xkey.state & ControlMask) != 0;
                    event_key.alt = (event.xkey.state & Mod1Mask) != 0;
                    document.update_frame(
                        width, height,
                        mouse_x, mouse_y,
                        mouse_down, 0.0F,
                        &event_key, nullptr);
                    blit(display, window, gc, width, height, document, packer, packed);
                }
            }
            break;
        }

        case ClientMessage:
            if (static_cast<Atom>(event.xclient.data.l[0]) == wm_delete_window)
            {
                running = false;
            }
            break;

        default:
            break;
        }
    }

    if (xic != nullptr)
    {
        XDestroyIC(xic);
    }
    document.shutdown_runtime();
    XCloseDisplay(display);
    return 0;
}
