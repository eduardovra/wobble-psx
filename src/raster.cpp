#include "raster.h"

#include <algorithm>
#include <array>
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

// The ordered dither the GPU adds before it throws away the low three
// bits of each channel, indexed by the pixel's position. Software turns
// it on in GP0(E1h); it costs the hardware nothing and spreads a
// gradient that would otherwise band across only 32 levels.
constexpr std::array<std::array<s32, 4>, 4> DITHER = {{
    {-4, +0, -3, +1},
    {+2, -2, +3, -1},
    {-3, +1, -4, +0},
    {+3, -1, +2, -2},
}};

// The offset is added before the channel is clamped, not after, so a
// channel already at full brightness can still be dithered downwards.
u16 to_pixel(const Rgb& colour, s32 offset)
{
    const auto channel = [offset](u32 value) {
        const s32 shifted = static_cast<s32>(value) + offset;
        const s32 limited =
            std::clamp(shifted, 0, static_cast<s32>(CHANNEL_MAX));
        return static_cast<u32>(limited) >> 3;
    };
    return static_cast<u16>((channel(colour.b) << 10) |
                            (channel(colour.g) << 5) | channel(colour.r));
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

// Where two triangles meet, the pixels on the seam belong to exactly
// one of them, or the seam is drawn twice — visible the moment either
// is translucent — and where nothing meets, a shape that ends at x is
// one pixel wider than it asked to be. The rule that settles it: a
// pixel exactly on an edge counts as inside only if that edge is the
// shape's top or left one. Every other triangle sharing the same edge
// sees it as a bottom or a right and gives the pixel up.
//
// With the corners wound so that the area is positive and screen y
// growing downwards, "top" is a horizontal edge running to the right
// and "left" is any edge running upwards.
bool top_or_left(const Vertex& a, const Vertex& b)
{
    if (a.y == b.y) {
        return b.x > a.x;
    }
    return b.y < a.y;
}

// The GPU does not work out each pixel's share of the three corners.
// For every quantity that varies across a triangle — the three colour
// channels and the two texture coordinates — it fixes a plane before
// drawing a single pixel and then reads that plane at each one: a
// value at one corner, and a step for each screen axis.
//
// The steps are held to twelve fractional bits, and truncated to fit.
// That truncation is the whole of the difference between this and
// working the shares out exactly: a slope of 85/138 is no binary
// fraction, and by the far side of a large triangle the two answers
// have drifted a shade apart. It is not much — a thousand pixels of
// gpu/triangle and six hundred of gpu/uv-interpolation — but it is not
// noise either, and nothing rounder reproduces it.
//
// No perspective correction anywhere in it: the GPU has no depth to
// correct by, which is where the console's sliding textures come from
// and half the reason its graphics look the way they do.
constexpr u32 PLANE_BITS = 12;   // fractional bits kept in a step
constexpr u32 PLANE_SHIFT = 24;  // and where the whole part ends up

struct Plane {
    u32 corner = 0;
    u32 step_x = 0;
    u32 step_y = 0;

    // Deliberately wrapping, and deliberately truncated: both are what
    // the hardware's own registers do at the edges of a big triangle.
    u32 at(s32 x, s32 y) const
    {
        const u32 across = step_x * static_cast<u32>(x);
        const u32 down = step_y * static_cast<u32>(y);
        return (corner + across + down) >> PLANE_SHIFT;
    }
};

u32 plane_step(s32 numerator, s32 det)
{
    const s64 scaled = (s64{numerator} << PLANE_BITS) / det;
    return static_cast<u32>(static_cast<s32>(scaled)) << PLANE_BITS;
}

// One attribute's plane, from corners already sorted down the screen.
// Both numerators are the triangle's own cross product with one screen
// axis swapped for the thing being interpolated.
Plane plane_of(const std::array<Vertex, 3>& v,
               std::size_t anchor,
               const std::array<s32, 3>& value,
               s32 det)
{
    Plane plane;
    plane.step_x = plane_step((value[1] - value[0]) * (v[2].y - v[1].y) -
                                  (value[2] - value[1]) * (v[1].y - v[0].y),
                              det);
    plane.step_y = plane_step((v[1].x - v[0].x) * (value[2] - value[1]) -
                                  (v[2].x - v[1].x) * (value[1] - value[0]),
                              det);

    // Half a unit at the anchor, so that the truncation on the way out
    // rounds rather than always losing the fraction.
    const u32 start = (static_cast<u32>(value[anchor]) << PLANE_BITS) |
        (1u << (PLANE_BITS - 1));
    plane.corner = (start << PLANE_BITS) -
        plane.step_x * static_cast<u32>(v[anchor].x) -
        plane.step_y * static_cast<u32>(v[anchor].y);
    return plane;
}

// Every plane a triangle carries.
struct Planes {
    Plane red, green, blue, across, down;
};

// The corners arrive in the order the command listed them, which is
// what decides the anchor: the leftmost of the three, chosen before
// they are sorted down the screen and carried through the sort.
Planes plan(const std::array<Vertex, 3>& corners)
{
    std::array<Vertex, 3> v = corners;

    std::size_t anchor = 0;
    if (v[1].x <= v[0].x) {
        anchor = v[2].x <= v[1].x ? 2 : 1;
    } else if (v[2].x < v[0].x) {
        anchor = 2;
    }

    const auto sort = [&v, &anchor](std::size_t a, std::size_t b) {
        if (v[b].y >= v[a].y) {
            return;
        }
        std::swap(v[a], v[b]);
        if (anchor == a) {
            anchor = b;
        } else if (anchor == b) {
            anchor = a;
        }
    };
    sort(1, 2);
    sort(0, 1);
    sort(1, 2);

    const s32 det = (v[1].x - v[0].x) * (v[2].y - v[1].y) -
        (v[2].x - v[1].x) * (v[1].y - v[0].y);

    const auto channel = [&v, anchor, det](u32 shift) {
        const std::array<s32, 3> value = {
            static_cast<s32>((v[0].colour >> shift) & 0xFF),
            static_cast<s32>((v[1].colour >> shift) & 0xFF),
            static_cast<s32>((v[2].colour >> shift) & 0xFF)};
        return plane_of(v, anchor, value, det);
    };

    Planes planes;
    planes.red = channel(0);
    planes.green = channel(8);
    planes.blue = channel(16);
    planes.across = plane_of(v,
                             anchor,
                             {static_cast<s32>(v[0].u),
                              static_cast<s32>(v[1].u),
                              static_cast<s32>(v[2].u)},
                             det);
    planes.down = plane_of(v,
                           anchor,
                           {static_cast<s32>(v[0].v),
                            static_cast<s32>(v[1].v),
                            static_cast<s32>(v[2].v)},
                           det);
    return planes;
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

    // The texture window, which folds a coordinate back into a
    // rectangle smaller than the page. Both parts are in texels.
    u32 window_mask_x = 0;
    u32 window_mask_y = 0;
    u32 window_offset_x = 0;
    u32 window_offset_y = 0;

    u32 blend_mode = 0;
    bool check_mask = false;
    bool dither = false;
    u16 set_mask = 0;
};

// Reads this primitive's palette into the GPU's, unless the one it is
// already holding will do. A 15-bit texture has no palette at all, and
// a 4-bit one is content with the first sixteen entries of a palette
// read for an 8-bit one.
void hold_clut(Gpu& gpu, const DrawState& state, u32 clut)
{
    if (state.depth >= 2) {
        return;
    }
    const u32 wanted = state.depth == 0 ? 16u : Gpu::CLUT_ENTRIES;
    if (clut == gpu.clut_source && wanted <= gpu.clut_entries) {
        return;
    }

    const std::size_t row = std::size_t{state.clut_y} * Gpu::VRAM_WIDTH;
    for (u32 entry = 0; entry < wanted; entry++) {
        const u32 x = (state.clut_x + entry) % Gpu::VRAM_WIDTH;
        gpu.clut[entry] = gpu.vram[row + x];
    }
    gpu.clut_source = clut;
    gpu.clut_entries = wanted;
}

DrawState prepare(Gpu& gpu, const Shading& how)
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

    // GP0(E2h). The four fields are in units of eight texels, which is
    // why nothing here divides: they are shifted up into texels once,
    // and the mask is used as a mask rather than as a size.
    constexpr u32 WINDOW_STEP = 8;
    state.window_mask_x = (gpu.texture_window & 0x1F) * WINDOW_STEP;
    state.window_mask_y = ((gpu.texture_window >> 5) & 0x1F) * WINDOW_STEP;
    state.window_offset_x = ((gpu.texture_window >> 10) & 0x1F) * WINDOW_STEP;
    state.window_offset_y = ((gpu.texture_window >> 15) & 0x1F) * WINDOW_STEP;

    state.check_mask = (gpu.mask_setting & 2) != 0;
    state.set_mask = (gpu.mask_setting & 1) != 0 ? MASK_BIT : 0;

    // GP0(E1h) bit 9 asks for the dither, but only a primitive with a
    // gradient across it gets one. A flat fill and a raw texture are
    // already the colours they will be stored as, and dithering them
    // would add noise to something that has no banding to hide.
    const bool graded = how.gouraud || (how.textured && !how.raw);
    state.dither = ((gpu.draw_mode >> 9) & 1) != 0 && graded;

    if (how.textured) {
        hold_clut(gpu, state, how.clut);
    }
    return state;
}

// The texel at (u, v) of the primitive's texture page, still in VRAM
// form so that its mask bit — which decides whether it is transparent,
// and whether it blends — survives to the caller.
u16 sample(const Gpu& gpu, const DrawState& state, u32 u, u32 v)
{
    // The window first: clearing the masked bits and forcing the
    // offset into them makes a texture smaller than a page repeat
    // across one, which is how a tiled surface is drawn without
    // storing the tile more than once.
    u = ((u & 0xFF) & ~state.window_mask_x) |
        (state.window_offset_x & state.window_mask_x);
    v = ((v & 0xFF) & ~state.window_mask_y) |
        (state.window_offset_y & state.window_mask_y);

    const u32 y = (state.texture_y + v) % Gpu::VRAM_HEIGHT;
    const std::size_t row = std::size_t{y} * Gpu::VRAM_WIDTH;

    if (state.depth >= 2) {
        const u32 x = (state.texture_x + u) % Gpu::VRAM_WIDTH;
        return gpu.vram[row + x];
    }

    // Four or eight bits to a texel, so several share a VRAM pixel and
    // the value found there is an index into the palette rather than a
    // colour. Index zero is transparent, whatever the palette says.
    const u32 per_pixel = state.depth == 0 ? 4u : 2u;
    const u32 bits = 16 / per_pixel;
    const u32 x = (state.texture_x + u / per_pixel) % Gpu::VRAM_WIDTH;
    const u32 shift = (u % per_pixel) * bits;
    const u32 index = (gpu.vram[row + x] >> shift) & ((1u << bits) - 1);

    return gpu.clut[index];
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
    const s32 offset = state.dither ? DITHER[y & 3][x & 3] : 0;
    gpu.vram[at] = to_pixel(colour, offset) | state.set_mask;
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

    // Twice the signed area, which is wanted for its sign alone. Zero
    // means the three corners are in a line, which has no inside to
    // fill; negative means they are wound the other way, and swapping
    // two of them turns it round so that "inside" is one test rather
    // than two.
    const s32 area = edge(v[0], v[1], v[2].x, v[2].y);
    if (area == 0) {
        return;
    }
    if (area < 0) {
        std::swap(v[1], v[2]);
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

    // Built from the corners as the command gave them, not as they
    // were wound: which corner the planes are anchored to depends on
    // that order, and turning the triangle round to make the inside
    // test one comparison would move it.
    const Planes planes = plan(corners);

    // What each edge function has to reach for the pixel to count as
    // inside: nothing at all on a top or left edge, so that a pixel
    // sitting exactly on it is drawn, and one more than that on the
    // others, so that a pixel sitting exactly on those is not.
    const std::array<s32, 3> least = {top_or_left(v[1], v[2]) ? 0 : 1,
                                      top_or_left(v[2], v[0]) ? 0 : 1,
                                      top_or_left(v[0], v[1]) ? 0 : 1};

    for (s32 y = top; y <= bottom; y++) {
        for (s32 x = left; x <= right; x++) {
            // The three edge functions at this point. Each clearing
            // its own threshold means the point is inside, and each is
            // also the weight of the corner opposite it.
            const std::array<s32, 3> weight = {edge(v[1], v[2], x, y),
                                               edge(v[2], v[0], x, y),
                                               edge(v[0], v[1], x, y)};
            const bool inside = weight[0] >= least[0] &&
                weight[1] >= least[1] && weight[2] >= least[2];
            if (!inside) {
                continue;
            }

            const Rgb tint = {planes.red.at(x, y),
                              planes.green.at(x, y),
                              planes.blue.at(x, y)};

            Rgb colour = tint;
            bool translucent = how.translucent;
            if (how.textured) {
                const u32 u = planes.across.at(x, y);
                const u32 t = planes.down.at(x, y);
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

void draw_line(Gpu& gpu,
               const Vertex& from,
               const Vertex& to,
               const Shading& how)
{
    const s32 dx = to.x - from.x;
    const s32 dy = to.y - from.y;

    // Too long to draw rather than too long to fit: the GPU abandons a
    // line that spans more than VRAM instead of clipping it down.
    if (std::abs(dx) >= static_cast<s32>(Gpu::VRAM_WIDTH) ||
        std::abs(dy) >= static_cast<s32>(Gpu::VRAM_HEIGHT)) {
        return;
    }

    DrawState state = prepare(gpu, how);

    // Every line is a graded one as far as the hardware is concerned: a
    // flat line is a gradient whose two ends happen to be the same
    // colour, and it goes through the dither the same way. gpu/lines
    // draws a flat blue line with GP0(E1h) bit 9 set, and the console
    // lays the pattern down over it.
    state.dither = ((gpu.draw_mode >> 9) & 1) != 0;

    // The GPU walks every line left to right, whichever end the command
    // named first.
    const bool backwards = from.x >= to.x && dx != 0;
    const Vertex& first = backwards ? to : from;
    const Vertex& second = backwards ? from : to;

    // One step per pixel along whichever axis the line covers more of,
    // so the other axis moves less than a pixel at a time and leaves no
    // gaps. Position is carried with a 32-bit fraction and colour with
    // a 12-bit one.
    constexpr u32 POSITION_BITS = 32;
    constexpr u32 COLOUR_BITS = 12;
    constexpr s64 POSITION_HALF = s64{1} << (POSITION_BITS - 1);

    // A hair short of the middle of the pixel, so that a step landing
    // exactly between two takes the one nearer the start. It is left
    // off an axis stepping downwards, and that asymmetry is real: in
    // gpu/lines the same fan of slopes drawn steep steps across a pixel
    // later than drawn shallow it steps down.
    constexpr s64 POSITION_NUDGE = 1024;

    const s32 steps = std::max(std::abs(dx), std::abs(dy));

    // Rounded away from zero rather than truncated, so that a line
    // arrives at the end the command named instead of falling short of
    // it by whatever the division dropped.
    const auto position_step = [steps](s32 delta) -> s64 {
        if (steps == 0 || delta == 0) {
            return 0;
        }
        const s64 reach = delta > 0 ? steps - 1 : 1 - steps;
        return ((s64{delta} << POSITION_BITS) + reach) / steps;
    };
    const s64 step_x = position_step(second.x - first.x);
    const s64 step_y = position_step(second.y - first.y);

    s64 x = (s64{first.x} << POSITION_BITS) + POSITION_HALF - POSITION_NUDGE;
    s64 y = (s64{first.y} << POSITION_BITS) + POSITION_HALF -
        (step_y < 0 ? POSITION_NUDGE : 0);

    const Rgb start = from_command(first.colour);
    const Rgb finish = from_command(second.colour);

    const auto colour_step = [steps](u32 a, u32 b) -> s32 {
        if (steps == 0) {
            return 0;
        }
        const s32 delta = static_cast<s32>(b) - static_cast<s32>(a);
        return (delta << COLOUR_BITS) / steps;
    };
    const auto colour_start = [](u32 value) {
        return (static_cast<s32>(value) << COLOUR_BITS) |
            (1 << (COLOUR_BITS - 1));
    };

    s32 red = colour_start(start.r);
    s32 green = colour_start(start.g);
    s32 blue = colour_start(start.b);
    const s32 step_red = colour_step(start.r, finish.r);
    const s32 step_green = colour_step(start.g, finish.g);
    const s32 step_blue = colour_step(start.b, finish.b);

    for (s32 step = 0; step <= steps; step++) {
        const s32 px = static_cast<s32>(x >> POSITION_BITS);
        const s32 py = static_cast<s32>(y >> POSITION_BITS);
        x += step_x;
        y += step_y;

        Rgb colour = start;
        if (how.gouraud) {
            colour = {static_cast<u32>(red >> COLOUR_BITS),
                      static_cast<u32>(green >> COLOUR_BITS),
                      static_cast<u32>(blue >> COLOUR_BITS)};
        }
        red += step_red;
        green += step_green;
        blue += step_blue;

        const bool inside = px >= static_cast<s32>(state.clip_left) &&
            px <= static_cast<s32>(state.clip_right) &&
            py >= static_cast<s32>(state.clip_top) &&
            py <= static_cast<s32>(state.clip_bottom);
        if (!inside) {
            continue;
        }

        put(gpu,
            state,
            static_cast<u32>(px),
            static_cast<u32>(py),
            colour,
            how.translucent);
    }
}

void draw_rectangle(
    Gpu& gpu, const Vertex& corner, u32 width, u32 height, const Shading& how)
{
    DrawState state = prepare(gpu, how);

    // A sprite is never dithered, whatever GP0(E1h) bit 9 says. It has
    // no gradient across it to band in the first place — one flat
    // colour, or a texture walked at one texel per pixel — and the
    // hardware does not offer it the choice. gpu/rectangles is exact
    // the moment the pattern is kept off it.
    state.dither = false;

    const Rgb tint = from_command(corner.colour);

    // GP0(E1h) bits 12 and 13 turn a textured rectangle's texture round
    // without needing a second copy of it stored the other way. They
    // belong to the draw mode rather than to the command, and they are
    // a rectangle's alone: a polygon carries its own coordinates for
    // each corner and can be turned round by giving them in the other
    // order, so the hardware offers it nothing here.
    constexpr u32 FLIP_X = 1u << 12;
    constexpr u32 FLIP_Y = 1u << 13;
    const bool flip_x = (gpu.draw_mode & FLIP_X) != 0;
    const bool flip_y = (gpu.draw_mode & FLIP_Y) != 0;

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
                // from its corner, with no interpolation to get wrong —
                // backwards from it when that axis is flipped.
                //
                // The two axes are not mirror images of each other: the
                // flipped column starts one texel past the corner and
                // the flipped row starts on it. gpu/texture-flip shows
                // it plainly — a console mirrors a sprite drawn at u=0
                // as 1, 0, 255, 254, and the same sprite's rows as 0,
                // 255, 254 — and the offset is the same whether the
                // sprite is 64 texels wide or 256, so it is not the
                // width coming back the other way.
                const u32 mirrored = corner.u + 1 - column;
                const u32 u = flip_x ? mirrored : corner.u + column;
                const u32 v = flip_y ? corner.v - row : corner.v + row;
                const Texel texel = shade_texel(gpu, state, how, u, v, tint);
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
    const u16 pixel = to_pixel(from_command(colour), 0);
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
    const bool check_mask = (gpu.mask_setting & 2) != 0;
    const u16 set_mask = (gpu.mask_setting & 1) != 0 ? MASK_BIT : 0;

    // A row at a time, read whole before any of it is written: the
    // hardware holds one row and nothing more, and the two halves of
    // that show up in gpu/vram-to-vram-overlap. A copy shifted sideways
    // moves what was under it, because the row it is reading was taken
    // before the writing began — but a copy shifted one row down smears
    // its first row over the whole rectangle, because by the time the
    // second row is read, the first has already been written on top of
    // it. Reading the whole rectangle up front gets the first right and
    // the second wrong.
    std::vector<u16> row(width);
    for (u32 line = 0; line < height; line++) {
        const std::size_t source =
            std::size_t{(from_y + line) % Gpu::VRAM_HEIGHT} * Gpu::VRAM_WIDTH;
        for (u32 column = 0; column < width; column++) {
            row[column] =
                gpu.vram[source + (from_x + column) % Gpu::VRAM_WIDTH];
        }

        const std::size_t target =
            std::size_t{(to_y + line) % Gpu::VRAM_HEIGHT} * Gpu::VRAM_WIDTH;
        for (u32 column = 0; column < width; column++) {
            const std::size_t at = target + (to_x + column) % Gpu::VRAM_WIDTH;
            if (check_mask && (gpu.vram[at] & MASK_BIT) != 0) {
                continue;
            }
            gpu.vram[at] = row[column] | set_mask;
        }
    }
}
