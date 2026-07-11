// PanoramaEngine example 04 (POSIX half) -- a real window instead of a .bmp.
//
// Loads a Panorama layout from an XML file on disk, lays it out, rasterizes
// it on the CPU (raster_view.hpp, the same rasterizer as
// examples/02_software_raster), and blits the result into an X11 window with
// XPutImage. Re-lays-out and re-rasterizes on every resize. See
// win32_main.cpp for the Windows equivalent -- both share raster_view.hpp
// and differ only in how they open a window and get pixels on screen.
#include "raster_view.hpp"

#include <X11/Xlib.h>
#include <X11/keysym.h>

#include <cstdio>
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
    XSelectInput(display, window, ExposureMask | StructureNotifyMask | KeyPressMask);

    Atom wm_delete_window = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &wm_delete_window, 1);

    XMapWindow(display, window);

    const PixelPacker packer(visual);
    std::vector<std::uint32_t> packed;
    GC gc = DefaultGC(display, screen);

    const auto repaint = [&](int paint_width, int paint_height) {
        document.layout_and_rasterize(paint_width, paint_height);
        pack_framebuffer(document.framebuffer(), packer, packed);

        XImage* image = XCreateImage(
            display, visual, static_cast<unsigned int>(depth), ZPixmap, 0,
            reinterpret_cast<char*>(packed.data()),
            static_cast<unsigned int>(paint_width), static_cast<unsigned int>(paint_height),
            32, 0);
        if (image == nullptr)
        {
            return;
        }
        XPutImage(
            display, window, gc, image, 0, 0, 0, 0,
            static_cast<unsigned int>(paint_width), static_cast<unsigned int>(paint_height));
        image->data = nullptr; // `packed` owns the pixel storage; don't let XDestroyImage free() it
        XDestroyImage(image);
    };

    repaint(width, height);

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
                repaint(width, height);
            }
            break;

        case ConfigureNotify:
            if (event.xconfigure.width != width || event.xconfigure.height != height)
            {
                width = event.xconfigure.width;
                height = event.xconfigure.height;
                repaint(width, height);
            }
            break;

        case KeyPress:
            if (XLookupKeysym(&event.xkey, 0) == XK_Escape)
            {
                running = false;
            }
            break;

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

    XCloseDisplay(display);
    return 0;
}
