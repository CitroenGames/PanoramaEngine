// PanoramaEngine example 04 (POSIX half) -- a real window instead of a .bmp.
//
// Loads a Panorama layout from an XML file on disk, lays it out, rasterizes
// it on the CPU (raster_view.hpp, the same rasterizer as
// examples/02_software_raster), and blits the result into an X11 window with
// XPutImage. Feeds mouse, keyboard, and text input to PanoramaView, whose
// dirty-stage tracking keeps idle frame ticks cheap while QuickJS timers and
// animations continue to advance. See win32_main.cpp for the Windows equivalent.
#include "raster_view.hpp"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <poll.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace
{
constexpr int kInitialWidth = 960;
constexpr int kInitialHeight = 540;
constexpr int kFrameIntervalMilliseconds = 16;
constexpr std::size_t kBenchmarkFrames = 120;
using FrameClock = std::chrono::steady_clock;

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

void pack_framebuffer(
    const panorama_example::Framebuffer& fb, const PixelPacker& packer, std::vector<std::uint32_t>& out)
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

// Reuses one XImage and native-format pixel buffer until the window changes
// size. XCreateImage/XDestroyImage allocation is kept out of the frame path.
class X11Surface
{
public:
    X11Surface(Display* display, Visual* visual, int depth)
        : display_(display)
        , visual_(visual)
        , depth_(depth)
    {
    }

    ~X11Surface() { reset(); }

    bool update(const panorama_example::Framebuffer& framebuffer, const PixelPacker& packer)
    {
        if (!resize(framebuffer.width, framebuffer.height))
        {
            return false;
        }
        pack_framebuffer(framebuffer, packer, pixels_);
        return true;
    }

    void blit(Window window, GC gc) const
    {
        if (image_ != nullptr)
        {
            XPutImage(
                display_, window, gc, image_, 0, 0, 0, 0,
                static_cast<unsigned int>(width_), static_cast<unsigned int>(height_));
        }
    }

private:
    bool resize(int width, int height)
    {
        if (image_ != nullptr && width_ == width && height_ == height)
        {
            return true;
        }

        reset();
        width_ = std::max(width, 1);
        height_ = std::max(height, 1);
        pixels_.resize(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_));
        image_ = XCreateImage(
            display_, visual_, static_cast<unsigned int>(depth_), ZPixmap, 0,
            reinterpret_cast<char*>(pixels_.data()),
            static_cast<unsigned int>(width_), static_cast<unsigned int>(height_),
            32, 0);
        return image_ != nullptr;
    }

    void reset()
    {
        if (image_ != nullptr)
        {
            image_->data = nullptr; // pixels_ owns the storage
            XDestroyImage(image_);
            image_ = nullptr;
        }
    }

    Display* display_;
    Visual* visual_;
    int depth_;
    int width_ = 0;
    int height_ = 0;
    std::vector<std::uint32_t> pixels_;
    XImage* image_ = nullptr;
};
}

int main(int argc, char** argv)
{
    const bool benchmark = argc > 1 && std::string_view(argv[1]) == "--benchmark";
    const int path_argument = benchmark ? 2 : 1;
    const std::filesystem::path xml_path = argc > path_argument
        ? std::filesystem::path(argv[path_argument])
        : std::filesystem::path("../../../examples/04_window_raster/sample/raster.xml");

    if (argc <= path_argument)
    {
        std::printf(
            "usage: %s [--benchmark] [layout.xml]  (no path given, trying %s)\n", argv[0], xml_path.c_str());
    }

    panorama_example::RasterDocument document;
    if (!document.load(xml_path, kInitialWidth, kInitialHeight))
    {
        std::fprintf(stderr, "failed to load %s\n", xml_path.c_str());
        return 1;
    }
    if (benchmark)
    {
        const panorama_example::RasterBenchmarkResult result = document.benchmark_rasterizer(kBenchmarkFrames);
        std::printf(
            "CPU raster: %zu frames in %.2f ms, %.3f ms/frame, %.2f MP/s\n",
            result.frames,
            result.total_milliseconds,
            result.average_milliseconds,
            result.megapixels_per_second);
        return 0;
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

    int width = kInitialWidth;
    int height = kInitialHeight;
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
    X11Surface surface(display, visual, depth);
    GC gc = DefaultGC(display, screen);

    // Track pointer state across events.
    float mouse_x = 0.0F;
    float mouse_y = 0.0F;
    bool mouse_down = false;
    if (!surface.update(document.framebuffer(), packer))
    {
        std::fprintf(stderr, "failed to create X11 image surface\n");
        XCloseDisplay(display);
        return 1;
    }
    surface.blit(window, gc);

    // X11 text input context for composed text (IME / dead keys).
    XIM xim = XOpenIM(display, nullptr, nullptr, nullptr);
    XIC xic = nullptr;
    if (xim != nullptr)
    {
        xic = XCreateIC(xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                        XNClientWindow, window, nullptr);
        if (xic != nullptr)
        {
            XSetICFocus(xic);
        }
    }

    bool running = true;
    FrameClock::time_point last_update = FrameClock::now();
    while (running)
    {
        // Wait for native input or the next 60 Hz runtime tick, whichever comes
        // first. An idle tick advances schedules/animations but normally skips
        // draw-list rebuild, CPU rasterization, native packing, and XPutImage.
        if (XPending(display) == 0)
        {
            pollfd connection{};
            connection.fd = ConnectionNumber(display);
            connection.events = POLLIN;
            (void)poll(&connection, 1, kFrameIntervalMilliseconds);
        }

        bool update_requested = false;
        bool blit_requested = false;
        while (running && XPending(display) > 0)
        {
            XEvent event;
            XNextEvent(display, &event);

            switch (event.type)
            {
            case Expose:
                blit_requested |= event.xexpose.count == 0;
                break;

            case ConfigureNotify:
                if (event.xconfigure.width != width || event.xconfigure.height != height)
                {
                    width = event.xconfigure.width;
                    height = event.xconfigure.height;
                    update_requested = true;
                }
                break;

            case MotionNotify:
                mouse_x = static_cast<float>(event.xmotion.x);
                mouse_y = static_cast<float>(event.xmotion.y);
                update_requested |= document.update_pointer(mouse_x, mouse_y, mouse_down);
                break;

            case ButtonPress:
                mouse_x = static_cast<float>(event.xbutton.x);
                mouse_y = static_cast<float>(event.xbutton.y);
                if (event.xbutton.button == Button1)
                {
                    mouse_down = true;
                    (void)document.update_pointer(mouse_x, mouse_y, mouse_down);
                    update_requested = true;
                }
                else if (event.xbutton.button == Button4 || event.xbutton.button == Button5)
                {
                    const float wheel = (event.xbutton.button == Button4) ? 1.0F : -1.0F;
                    (void)document.update_wheel(mouse_x, mouse_y, wheel);
                    update_requested = true;
                }
                break;

            case ButtonRelease:
                if (event.xbutton.button == Button1)
                {
                    mouse_down = false;
                    mouse_x = static_cast<float>(event.xbutton.x);
                    mouse_y = static_cast<float>(event.xbutton.y);
                    (void)document.update_pointer(mouse_x, mouse_y, mouse_down);
                    update_requested = true;
                }
                break;

            case KeyPress: {
                KeySym keysym = XLookupKeysym(&event.xkey, 0);
                if (keysym == XK_Escape)
                {
                    running = false;
                    break;
                }

                char buf[32] = {};
                Status status;
                const int text_length = xic != nullptr
                    ? Xutf8LookupString(xic, &event.xkey, buf, sizeof(buf) - 1, &keysym, &status)
                    : 0;
                if (text_length > 0 && status != XBufferOverflow)
                {
                    (void)document.handle_text_input(std::string_view(buf, static_cast<std::size_t>(text_length)));
                    update_requested = true;
                }
                else
                {
                    const panorama::PanoramaKey key = keysym_to_panorama_key(keysym);
                    if (key != panorama::PanoramaKey::Unknown)
                    {
                        panorama::PanoramaKeyEvent event_key;
                        event_key.key = key;
                        event_key.shift = (event.xkey.state & ShiftMask) != 0;
                        event_key.ctrl = (event.xkey.state & ControlMask) != 0;
                        event_key.alt = (event.xkey.state & Mod1Mask) != 0;
                        (void)document.handle_key_down(event_key);
                        update_requested = true;
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

        const FrameClock::time_point now = FrameClock::now();
        const float dt_seconds = std::clamp(
            std::chrono::duration<float>(now - last_update).count(), 0.0F, 0.1F);
        if (running && (update_requested || dt_seconds * 1000.0F >= kFrameIntervalMilliseconds))
        {
            last_update = now;
            if (document.update_frame(width, height, dt_seconds))
            {
                blit_requested = surface.update(document.framebuffer(), packer);
            }
        }
        if (running && blit_requested)
        {
            surface.blit(window, gc);
        }
    }

    if (xic != nullptr)
    {
        XUnsetICFocus(xic);
        XDestroyIC(xic);
    }
    if (xim != nullptr)
    {
        XCloseIM(xim);
    }
    XCloseDisplay(display);
    return 0;
}
