#include "raster.h"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

#include "gpu.h"

namespace {

// A VRAM pixel is five bits each of red, green and blue, low to high,
// with the mask bit on top. Everything drawn ends up in this form.
constexpr u16 MASK_BIT = 0x8000;
constexpr u32 CHANNEL_MAX = 0xFF;

// The colour value that leaves a texel unchanged when it modulates it.
// Anything above it brightens, which is how a texture is made to glow
// without a second copy of it.
constexpr u32 NEUTRAL = 0x80;

// A colour in the eight-bits-per-channel form the arithmetic is done
// in. VRAM's five bits are stretched into it and squeezed back out.
struct Rgb {
    u32 r = 0;
    u32 g = 0;
    u32 b = 0;
};

Rgb from_command(u32 colour)
{
    return {colour & 0xFF, (colour >> 8) & 0xFF, (colour >> 16) & 0xFF};
}

Rgb from_pixel(u16 pixel)
{
    return {static_cast<u32>(pixel & 0x1F) << 3,
            static_cast<u32>((pixel >> 5) & 0x1F) << 3,
            static_cast<u32>((pixel >> 10) & 0x1F) << 3};
}

u16 to_pixel(const Rgb& colour)
{
    const u32 r = std::min(colour.r, CHANNEL_MAX) >> 3;
    const u32 g = std::min(colour.g, CHANNEL_MAX) >> 3;
    const u32 b = std::min(colour.b, CHANNEL_MAX) >> 3;
    return static_cast<u16>((b << 10) | (g << 5) | r);
}

// Which side of the line through a and b the point lies on, scaled by
// twice the area of the triangle they make. Zero on the line, and the
// same sign for every point on one side, which is the whole test for
// being inside a triangle — and, reused as a weight, the whole of the
// interpolation as well.
s32 edge(const Vertex& a, const Vertex& b, s32 x, s32 y)
{
    return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
}

// One quantity that varies across a triangle, at the point those edge
// weights describe. No perspective correction: the GPU has no depth to
// correct by, which is where the console's sliding textures come from
// and half the reason its graphics look the way they do.
u32 interpolate(const std::array<s32, 3>& weight,
                const std::array<u32, 3>& corner,
                s32 area)
{
    const s32 sum = weight[0] * static_cast<s32>(corner[0]) +
        weight[1] * static_cast<s32>(corner[1]) +
        weight[2] * static_cast<s32>(corner[2]);
    return static_cast<u32>(sum / area);
}

// Blends what is being drawn with what is already there, in one of the
// four fixed ratios the GPU offers. There is no general alpha: the mode
// is a property of the texture page, and these four are all of it.
Rgb blend(u32 mode, const Rgb& behind, const Rgb& front)
{
    switch (mode) {
    case 0:  // half and half
        return {behind.r / 2 + front.r / 2,
                behind.g / 2 + front.g / 2,
                behind.b / 2 + front.b / 2};
    case 1:  // additive, for light and fire
        return {behind.r + front.r, behind.g + front.g, behind.b + front.b};
    case 2:  // subtractive, for shadow
        return {behind.r - std::min(behind.r, front.r),
                behind.g - std::min(behind.g, front.g),
                behind.b - std::min(behind.b, front.b)};
    default:  // a quarter of the front, for a faint haze
        return {behind.r + front.r / 4,
                behind.g + front.g / 4,
                behind.b + front.b / 4};
    }
}

// Everything a primitive needs that is the same for all of its pixels,
// worked out once rather than per pixel.
struct DrawState {
    u32 clip_left = 0;
    u32 clip_top = 0;
    u32 clip_right = 0;
    u32 clip_bottom = 0;

    // Where the texture page starts in VRAM, how its texels are
    // stored, and where its palette is.
    u32 texture_x = 0;
    u32 texture_y = 0;
    u32 depth = 0;  // 0 = 4-bit, 1 = 8-bit, 2 and 3 = straight 15-bit
    u32 clut_x = 0;
    u32 clut_y = 0;

    u32 blend_mode = 0;
    bool check_mask = false;
    u16 set_mask = 0;
};

DrawState prepare(const Gpu& gpu, const Shading& how)
{
    DrawState state;
    state.clip_left = gpu.clip_left();
    state.clip_top = gpu.clip_top();
    state.clip_right = gpu.clip_right();
    state.clip_bottom = gpu.clip_bottom();

    // A textured primitive names its own page; an untextured one is
    // still subject to the semi-transparency mode, which is why the
    // page from GP0(E1h) is the fallback rather than nothing.
    const u32 page = how.textured ? how.texpage : (gpu.draw_mode & 0x1FF);
    state.texture_x = (page & 0xF) * 64;
    state.texture_y = ((page >> 4) & 1) * 256;
    state.blend_mode = (page >> 5) & 3;
    state.depth = (page >> 7) & 3;

    state.clut_x = (how.clut & 0x3F) * 16;
    state.clut_y = (how.clut >> 6) & 0x1FF;

    state.check_mask = (gpu.mask_setting & 2) != 0;
    state.set_mask = (gpu.mask_setting & 1) != 0 ? MASK_BIT : 0;
    return state;
}

// The texel at (u, v) of the primitive's texture page, still in VRAM
// form so that its mask bit — which decides whether it is transparent,
// and whether it blends — survives to the caller.
u16 sample(const Gpu& gpu, const DrawState& state, u32 u, u32 v)
{
    const u32 y = (state.texture_y + (v & 0xFF)) % Gpu::VRAM_HEIGHT;
    const std::size_t row = std::size_t{y} * Gpu::VRAM_WIDTH;

    if (state.depth >= 2) {
        const u32 x = (state.texture_x + (u & 0xFF)) % Gpu::VRAM_WIDTH;
        return gpu.vram[row + x];
    }

    // Four or eight bits to a texel, so several share a VRAM pixel and
    // the value found there is an index into the palette rather than a
    // colour. Index zero is transparent, whatever the palette says.
    const u32 per_pixel = state.depth == 0 ? 4u : 2u;
    const u32 bits = 16 / per_pixel;
    const u32 x = (state.texture_x + (u & 0xFF) / per_pixel) % Gpu::VRAM_WIDTH;
    const u32 shift = ((u & 0xFF) % per_pixel) * bits;
    const u32 index = (gpu.vram[row + x] >> shift) & ((1u << bits) - 1);

    const u32 clut_x = (state.clut_x + index) % Gpu::VRAM_WIDTH;
    const std::size_t clut_row = std::size_t{state.clut_y} * Gpu::VRAM_WIDTH;
    return gpu.vram[clut_row + clut_x];
}

// Puts one pixel down, having decided its colour: the last steps that
// every primitive shares, and the only place VRAM is written.
void put(Gpu& gpu,
         const DrawState& state,
         u32 x,
         u32 y,
         Rgb colour,
         bool translucent)
{
    const std::size_t at = std::size_t{y} * Gpu::VRAM_WIDTH + x;
    const u16 behind = gpu.vram[at];
    if (state.check_mask && (behind & MASK_BIT) != 0) {
        return;
    }
    if (translucent) {
        colour = blend(state.blend_mode, from_pixel(behind), colour);
    }
    gpu.vram[at] = to_pixel(colour) | state.set_mask;
}

// What one texel comes to once the primitive's colour has been applied
// to it. A transparent texel is not drawn at all, which is decided
// here, before anything is blended.
struct Texel {
    bool drawn = false;
    Rgb colour;
    bool translucent = false;
};

Texel shade_texel(const Gpu& gpu,
                  const DrawState& state,
                  const Shading& how,
                  u32 u,
                  u32 v,
                  const Rgb& tint)
{
    const u16 found = sample(gpu, state, u, v);
    if (found == 0) {
        return {};  // fully transparent, and leaves VRAM alone
    }

    Texel texel;
    texel.drawn = true;
    texel.colour = from_pixel(found);
    if (!how.raw) {
        // The vertex colour scales the texel rather than replacing it,
        // which is how one texture is drawn at many brightnesses.
        texel.colour.r = texel.colour.r * tint.r / NEUTRAL;
        texel.colour.g = texel.colour.g * tint.g / NEUTRAL;
        texel.colour.b = texel.colour.b * tint.b / NEUTRAL;
    }

    // A texel's top bit is what asks for blending, so one texture can
    // be part solid and part translucent — but only if the primitive
    // was drawn translucent in the first place.
    texel.translucent = how.translucent && (found & MASK_BIT) != 0;
    return texel;
}

}  // namespace

void draw_triangle(Gpu& gpu,
                   const std::array<Vertex, 3>& corners,
                   const Shading& how)
{
    std::array<Vertex, 3> v = corners;

    // Twice the signed area. Zero means the three corners are in a
    // line, which has no inside to fill; negative means they are wound
    // the other way, and swapping two of them turns it round so that
    // "inside" is one test rather than two.
    s32 area = edge(v[0], v[1], v[2].x, v[2].y);
    if (area == 0) {
        return;
    }
    if (area < 0) {
        std::swap(v[1], v[2]);
        area = -area;
    }

    const DrawState state = prepare(gpu, how);

    // Only the triangle's own bounding box is walked, narrowed to the
    // draw area, so a small shape on a big screen costs its own size
    // rather than the screen's.
    const s32 left = std::max(static_cast<s32>(state.clip_left),
                              std::min({v[0].x, v[1].x, v[2].x}));
    const s32 right = std::min(static_cast<s32>(state.clip_right),
                               std::max({v[0].x, v[1].x, v[2].x}));
    const s32 top = std::max(static_cast<s32>(state.clip_top),
                             std::min({v[0].y, v[1].y, v[2].y}));
    const s32 bottom = std::min(static_cast<s32>(state.clip_bottom),
                                std::max({v[0].y, v[1].y, v[2].y}));

    const std::array<Rgb, 3> corner = {from_command(v[0].colour),
                                       from_command(v[1].colour),
                                       from_command(v[2].colour)};
    const std::array<u32, 3> reds = {corner[0].r, corner[1].r, corner[2].r};
    const std::array<u32, 3> greens = {corner[0].g, corner[1].g, corner[2].g};
    const std::array<u32, 3> blues = {corner[0].b, corner[1].b, corner[2].b};
    const std::array<u32, 3> across = {v[0].u, v[1].u, v[2].u};
    const std::array<u32, 3> down = {v[0].v, v[1].v, v[2].v};

    for (s32 y = top; y <= bottom; y++) {
        for (s32 x = left; x <= right; x++) {
            // The three edge functions at this point. All of them
            // non-negative means the point is inside, and each is also
            // the weight of the corner opposite it.
            const std::array<s32, 3> weight = {edge(v[1], v[2], x, y),
                                               edge(v[2], v[0], x, y),
                                               edge(v[0], v[1], x, y)};
            if (weight[0] < 0 || weight[1] < 0 || weight[2] < 0) {
                continue;
            }

            Rgb tint = corner[0];
            if (how.gouraud) {
                tint.r = interpolate(weight, reds, area);
                tint.g = interpolate(weight, greens, area);
                tint.b = interpolate(weight, blues, area);
            }

            Rgb colour = tint;
            bool translucent = how.translucent;
            if (how.textured) {
                const u32 u = interpolate(weight, across, area);
                const u32 t = interpolate(weight, down, area);
                const Texel texel = shade_texel(gpu, state, how, u, t, tint);
                if (!texel.drawn) {
                    continue;
                }
                colour = texel.colour;
                translucent = texel.translucent;
            }

            put(gpu,
                state,
                static_cast<u32>(x),
                static_cast<u32>(y),
                colour,
                translucent);
        }
    }
}

void draw_rectangle(
    Gpu& gpu, const Vertex& corner, u32 width, u32 height, const Shading& how)
{
    const DrawState state = prepare(gpu, how);
    const Rgb tint = from_command(corner.colour);

    for (u32 row = 0; row < height; row++) {
        const s32 y = corner.y + static_cast<s32>(row);
        if (y < static_cast<s32>(state.clip_top) ||
            y > static_cast<s32>(state.clip_bottom)) {
            continue;
        }
        for (u32 column = 0; column < width; column++) {
            const s32 x = corner.x + static_cast<s32>(column);
            if (x < static_cast<s32>(state.clip_left) ||
                x > static_cast<s32>(state.clip_right)) {
                continue;
            }

            Rgb colour = tint;
            bool translucent = how.translucent;
            if (how.textured) {
                // A sprite's texture is walked one texel to the pixel
                // from its corner, with no interpolation to get wrong.
                const Texel texel = shade_texel(
                    gpu, state, how, corner.u + column, corner.v + row, tint);
                if (!texel.drawn) {
                    continue;
                }
                colour = texel.colour;
                translucent = texel.translucent;
            }

            put(gpu,
                state,
                static_cast<u32>(x),
                static_cast<u32>(y),
                colour,
                translucent);
        }
    }
}

void fill_vram(Gpu& gpu, u32 x, u32 y, u32 width, u32 height, u32 colour)
{
    const u16 pixel = to_pixel(from_command(colour));
    for (u32 row = 0; row < height; row++) {
        const std::size_t at =
            std::size_t{(y + row) % Gpu::VRAM_HEIGHT} * Gpu::VRAM_WIDTH;
        for (u32 column = 0; column < width; column++) {
            gpu.vram[at + (x + column) % Gpu::VRAM_WIDTH] = pixel;
        }
    }
}

void copy_vram(
    Gpu& gpu, u32 from_x, u32 from_y, u32 to_x, u32 to_y, u32 width, u32 height)
{
    // Read the whole source before writing any of it, so a copy onto
    // overlapping ground moves what was there rather than a trail of
    // what it has just written.
    std::vector<u16> source(std::size_t{width} * height);
    for (u32 row = 0; row < height; row++) {
        const std::size_t at =
            std::size_t{(from_y + row) % Gpu::VRAM_HEIGHT} * Gpu::VRAM_WIDTH;
        for (u32 column = 0; column < width; column++) {
            source[std::size_t{row} * width + column] =
                gpu.vram[at + (from_x + column) % Gpu::VRAM_WIDTH];
        }
    }

    const bool check_mask = (gpu.mask_setting & 2) != 0;
    const u16 set_mask = (gpu.mask_setting & 1) != 0 ? MASK_BIT : 0;
    for (u32 row = 0; row < height; row++) {
        const std::size_t at =
            std::size_t{(to_y + row) % Gpu::VRAM_HEIGHT} * Gpu::VRAM_WIDTH;
        for (u32 column = 0; column < width; column++) {
            const std::size_t to = at + (to_x + column) % Gpu::VRAM_WIDTH;
            if (check_mask && (gpu.vram[to] & MASK_BIT) != 0) {
                continue;
            }
            gpu.vram[to] = source[std::size_t{row} * width + column] | set_mask;
        }
    }
}
