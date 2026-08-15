#pragma once

#include <array>

#include "types.h"

struct Gpu;

// One corner of a primitive, in the form the GP0 command words carry
// it: a signed screen position, a colour, and a place in the texture
// page. Which of the three matter is decided by the command byte, not
// by the corner, so all of them are always here.
struct Vertex {
    s32 x = 0;
    s32 y = 0;
    u32 colour = 0;  // 0x00BBGGRR, as the command word packs it
    u32 u = 0;
    u32 v = 0;
};

// How the pixels between the corners get their colour. The GPU builds
// one of these out of the command byte's flag bits and the two extra
// words a textured primitive carries.
struct Shading {
    bool gouraud = false;      // interpolate the corner colours
    bool textured = false;     //
    bool raw = false;          // take the texel as-is, unmodulated
    bool translucent = false;  // blend with what is already there
    u32 clut = 0;              // where this primitive's palette sits
    u32 texpage = 0;           // where its texture sits, and how stored
};

// Fills the triangle the three corners describe. Everything with
// straight edges is built from this — a quad is two triangles sharing
// one — so it is the only place the inside of a shape is decided.
void draw_triangle(Gpu& gpu,
                   const std::array<Vertex, 3>& corners,
                   const Shading& how);

// An axis-aligned rectangle. It needs no interpolation at all: one
// flat colour, or a texture walked at one texel per pixel.
void draw_rectangle(
    Gpu& gpu, const Vertex& corner, u32 width, u32 height, const Shading& how);

// GP0(02h): a rectangle written straight into VRAM. It is not drawing
// so much as clearing, and ignores the draw area, the mask bits and
// every other piece of drawing state.
void fill_vram(Gpu& gpu, u32 x, u32 y, u32 width, u32 height, u32 colour);

// GP0(80h): a rectangle of VRAM copied somewhere else in VRAM, which
// is how a game moves a finished picture out of the way of the next.
void copy_vram(Gpu& gpu,
               u32 from_x,
               u32 from_y,
               u32 to_x,
               u32 to_y,
               u32 width,
               u32 height);
