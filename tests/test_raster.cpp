#include <cstddef>
#include <memory>

#include <doctest/doctest.h>

#include "gpu.h"
#include "raster.h"

namespace {

// A GPU with somewhere to draw. VRAM is a megabyte, too much for the
// stack a test runs on.
struct Screen {
    std::unique_ptr<Gpu> gpu = std::make_unique<Gpu>();

    Screen()
    {
        // A draw area of the whole of VRAM, so a test only has to set
        // one when clipping is the thing being tested.
        gpu->draw_area_top = 0xE3000000;
        gpu->draw_area_bottom = 0xE4000000 | (511u << 10) | 1023u;
    }

    u16 at(u32 x, u32 y) const
    {
        return gpu->vram[std::size_t{y} * Gpu::VRAM_WIDTH + x];
    }

    void put(u32 x, u32 y, u16 pixel) const
    {
        gpu->vram[std::size_t{y} * Gpu::VRAM_WIDTH + x] = pixel;
    }
};

// A right-angled triangle with its square corner at the origin, big
// enough that "well inside" and "well outside" are unambiguous.
std::array<Vertex, 3> wedge(u32 colour)
{
    return {Vertex{0, 0, colour, 0, 0},
            Vertex{64, 0, colour, 0, 0},
            Vertex{0, 64, colour, 0, 0}};
}

constexpr u16 WHITE = 0x7FFF;
constexpr u32 RED = 0x0000FF;  // the command word packs colours as BGR

}  // namespace

TEST_CASE("a triangle fills its inside and leaves the rest alone")
{
    Screen screen;
    draw_triangle(*screen.gpu, wedge(0xFFFFFF), Shading{});

    CHECK(screen.at(1, 1) == WHITE);    // just inside the square corner
    CHECK(screen.at(20, 20) == WHITE);  // and well inside the middle
    CHECK(screen.at(60, 60) == 0);      // past the diagonal
    CHECK(screen.at(200, 200) == 0);    // nowhere near it
}

// Whether the corners are given clockwise or anticlockwise decides the
// sign of every edge test, and a rasterizer that ignores it fills one
// winding and silently drops the other.
TEST_CASE("winding order does not change what is filled")
{
    Screen clockwise;
    draw_triangle(*clockwise.gpu, wedge(0xFFFFFF), Shading{});

    std::array<Vertex, 3> reversed = wedge(0xFFFFFF);
    std::swap(reversed[1], reversed[2]);
    Screen anticlockwise;
    draw_triangle(*anticlockwise.gpu, reversed, Shading{});

    CHECK(clockwise.gpu->vram == anticlockwise.gpu->vram);
}

TEST_CASE("three corners in a line have no inside to fill")
{
    Screen screen;
    const std::array<Vertex, 3> flat = {Vertex{0, 0, 0xFFFFFF, 0, 0},
                                        Vertex{32, 0, 0xFFFFFF, 0, 0},
                                        Vertex{64, 0, 0xFFFFFF, 0, 0}};
    draw_triangle(*screen.gpu, flat, Shading{});

    CHECK(screen.at(32, 0) == 0);
}

TEST_CASE("nothing is drawn outside the draw area")
{
    Screen screen;
    // Everything below or right of (32, 32) is off limits.
    screen.gpu->draw_area_bottom = 0xE4000000 | (32u << 10) | 32u;
    draw_triangle(*screen.gpu, wedge(0xFFFFFF), Shading{});

    CHECK(screen.at(20, 20) == WHITE);
    CHECK(screen.at(32, 0) == WHITE);  // the corner itself is inside
    CHECK(screen.at(33, 0) == 0);
    CHECK(screen.at(0, 33) == 0);
}

// The offset is applied when the command is decoded, so this goes in
// through GP0 rather than straight to the rasterizer.
TEST_CASE("the draw offset moves a primitive without moving its shape")
{
    Screen screen;
    screen.gpu->write_gp0(0xE5000000 | (100u << 11) | 100u);
    screen.gpu->write_gp0(0x28000000 | RED);  // a flat quad at the origin
    screen.gpu->write_gp0(0x00000000);        // (0, 0)
    screen.gpu->write_gp0(0x00000010);        // (16, 0)
    screen.gpu->write_gp0(0x00100000);        // (0, 16)
    screen.gpu->write_gp0(0x00100010);        // (16, 16)

    CHECK(screen.at(104, 104) != 0);
    CHECK(screen.at(4, 4) == 0);
}

TEST_CASE("a gouraud triangle carries each corner's own colour")
{
    Screen screen;
    Shading how;
    how.gouraud = true;

    std::array<Vertex, 3> corners = wedge(0);
    corners[0].colour = 0x0000F8;  // red
    corners[1].colour = 0x00F800;  // green
    corners[2].colour = 0xF80000;  // blue
    draw_triangle(*screen.gpu, corners, how);

    // Near a corner that corner's weight is nearly everything, so its
    // channel dominates; in the middle all three are mixed.
    const auto red = [](u16 pixel) { return pixel & 0x1F; };
    const auto green = [](u16 pixel) { return (pixel >> 5) & 0x1F; };
    const auto blue = [](u16 pixel) { return (pixel >> 10) & 0x1F; };

    const u16 near_red = screen.at(1, 1);
    CHECK(red(near_red) > green(near_red));
    CHECK(red(near_red) > blue(near_red));

    const u16 near_green = screen.at(60, 1);
    CHECK(green(near_green) > red(near_green));
    CHECK(green(near_green) > blue(near_green));

    const u16 near_blue = screen.at(1, 60);
    CHECK(blue(near_blue) > red(near_blue));
    CHECK(blue(near_blue) > green(near_blue));

    const u16 middle = screen.at(16, 16);
    CHECK(red(middle) > 0);
    CHECK(green(middle) > 0);
    CHECK(blue(middle) > 0);
}

// A quad is two triangles sharing a diagonal. If the split is wrong
// the shape comes out as one half of itself, which the far corner
// catches and the near one does not.
TEST_CASE("a quad covers all four of its corners")
{
    Screen screen;
    screen.gpu->write_gp0(0x28000000 | 0xFFFFFF);
    screen.gpu->write_gp0(0x00000000);  // (0, 0)
    screen.gpu->write_gp0(0x00000040);  // (64, 0)
    screen.gpu->write_gp0(0x00400000);  // (0, 64)
    screen.gpu->write_gp0(0x00400040);  // (64, 64)

    CHECK(screen.at(1, 1) == WHITE);
    CHECK(screen.at(62, 1) == WHITE);
    CHECK(screen.at(1, 62) == WHITE);
    CHECK(screen.at(62, 62) == WHITE);
}

TEST_CASE("a four-bit texture reads its colour out of the palette")
{
    Screen screen;
    // A palette at (0, 256): entry 1 is red, entry 2 is green.
    screen.put(1, 256, 0x001F);
    screen.put(2, 256, 0x03E0);
    // One VRAM pixel holds four texels, the leftmost in the lowest
    // nibble, so 0x0012 is the run 2, 1, 0, 0.
    screen.put(0, 0, 0x0012);

    Shading how;
    how.textured = true;
    how.raw = true;
    how.texpage = 0;              // page at (0, 0), four bits a texel
    how.clut = (256u << 6) | 0u;  // palette at (0, 256)

    draw_rectangle(*screen.gpu, Vertex{100, 100, 0x808080, 0, 0}, 2, 1, how);

    CHECK(screen.at(100, 100) == 0x03E0);  // the first texel: index 2
    CHECK(screen.at(101, 100) == 0x001F);  // the second: index 1
}

// Palette entry zero is transparent whatever colour it holds, which is
// how a texture gets a shape that is not a rectangle.
TEST_CASE("a transparent texel leaves what was underneath")
{
    Screen screen;
    screen.put(0, 0, 0x0000);  // every texel of this pixel is index 0
    screen.put(100, 100, WHITE);

    Shading how;
    how.textured = true;
    how.raw = true;

    draw_rectangle(*screen.gpu, Vertex{100, 100, 0x808080, 0, 0}, 1, 1, how);

    CHECK(screen.at(100, 100) == WHITE);
}

TEST_CASE("a masked pixel is not drawn over when the mask is being checked")
{
    Screen screen;
    screen.put(10, 10, 0x8000);  // masked, and otherwise black
    screen.gpu->mask_setting = 2;

    draw_triangle(*screen.gpu, wedge(0xFFFFFF), Shading{});

    CHECK(screen.at(10, 10) == 0x8000);
    CHECK(screen.at(11, 11) == WHITE);  // its neighbour is not protected
}

TEST_CASE("a fill ignores the draw area, since it is not drawing")
{
    Screen screen;
    screen.gpu->draw_area_bottom = 0xE4000000;  // a one-pixel draw area
    fill_vram(*screen.gpu, 0, 0, 16, 16, 0xFFFFFF);

    CHECK(screen.at(0, 0) == 0x7FFF);
    CHECK(screen.at(15, 15) == 0x7FFF);
}

TEST_CASE("a VRAM copy moves what was there, not what it has just written")
{
    Screen screen;
    screen.put(0, 0, 0x1111);
    screen.put(1, 0, 0x2222);

    // Overlapping, one pixel to the right: a copy that wrote as it read
    // would smear the first pixel across both.
    copy_vram(*screen.gpu, 0, 0, 1, 0, 2, 1);

    CHECK(screen.at(1, 0) == 0x1111);
    CHECK(screen.at(2, 0) == 0x2222);
}

TEST_CASE("the display window is the size the mode asks for")
{
    Screen screen;

    screen.gpu->display_mode = 0;
    CHECK(screen.gpu->display_width() == 256);
    CHECK(screen.gpu->display_height() == 240);

    screen.gpu->display_mode = 3;
    CHECK(screen.gpu->display_width() == 640);

    // Bit 6 asks for 368 and overrules the field below it.
    screen.gpu->display_mode = 0x43;
    CHECK(screen.gpu->display_width() == 368);

    // The tall mode only exists interlaced.
    screen.gpu->display_mode = 0x04;
    CHECK(screen.gpu->display_height() == 240);
    screen.gpu->display_mode = 0x24;
    CHECK(screen.gpu->display_height() == 480);
}

TEST_CASE("a displayed pixel is read from where GP1(05h) points")
{
    Screen screen;
    screen.gpu->display_mode = 0;
    screen.gpu->display_start = (2u << 10) | 8u;  // (8, 2)
    screen.put(8, 2, 0x001F);

    const Gpu::Colour corner = screen.gpu->display_pixel(0, 0);
    CHECK(corner.r == 0xFF);
    CHECK(corner.g == 0);
    CHECK(corner.b == 0);

    // Off the edge of the picture is black rather than whatever VRAM
    // happens to hold there.
    const Gpu::Colour outside = screen.gpu->display_pixel(1000, 0);
    CHECK(outside.r == 0);
}
