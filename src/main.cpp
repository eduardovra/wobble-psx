// Host side of the emulator: it owns the window, the debugger UI and
// the frame loop that drives the CPU. The emulated machine itself is
// just a Bus (memory and devices) with a Cpu attached to it.

#include <algorithm>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include "console.h"
#include "disasm.h"
#include "exe.h"
#include "gpu.h"
#include "sio.h"

namespace {

// The keyboard as a controller. The face buttons are laid out the way
// they sit on the pad rather than by name, so the shape on screen is
// the shape under the fingers.
struct PadKey {
    SDL_Keycode key;
    Sio::Button button;
};

constexpr std::array<PadKey, 14> PAD_KEYS = {{
    {SDLK_UP, Sio::Button::Up},
    {SDLK_DOWN, Sio::Button::Down},
    {SDLK_LEFT, Sio::Button::Left},
    {SDLK_RIGHT, Sio::Button::Right},
    {SDLK_RETURN, Sio::Button::Start},
    {SDLK_RSHIFT, Sio::Button::Select},
    {SDLK_X, Sio::Button::Cross},
    {SDLK_Z, Sio::Button::Square},
    {SDLK_S, Sio::Button::Circle},
    {SDLK_A, Sio::Button::Triangle},
    {SDLK_Q, Sio::Button::L1},
    {SDLK_W, Sio::Button::L2},
    {SDLK_E, Sio::Button::R1},
    {SDLK_R, Sio::Button::R2},
}};

void handle_pad_key(Sio& sio, const SDL_Event& event)
{
    const bool down = event.type == SDL_EVENT_KEY_DOWN;
    if (!down && event.type != SDL_EVENT_KEY_UP) {
        return;
    }
    // A key the debugger UI is using is not a button press.
    if (ImGui::GetIO().WantCaptureKeyboard) {
        return;
    }
    for (const PadKey& mapping : PAD_KEYS) {
        if (mapping.key != event.key.key) {
            continue;
        }
        if (down) {
            sio.press(mapping.button);
        } else {
            sio.release(mapping.button);
        }
        return;
    }
}

// Emulated time to run per host frame. The renderer is vsynced to
// 60 Hz, so running one sixtieth of a second of console time per pass
// keeps the emulator at roughly real speed.
constexpr u64 CYCLES_PER_HOST_FRAME = CPU_CLOCK_HZ / 60;

// The console's picture on its way to the window. The texture is made
// once at the largest size any display mode reaches, and only the part
// the current mode uses is ever uploaded — so a mode change costs
// nothing and needs no reallocation.
struct Display {
    SDL_Texture* texture = nullptr;
    std::vector<u32> pixels;

    bool create(SDL_Renderer* renderer)
    {
        texture = SDL_CreateTexture(renderer,
                                    SDL_PIXELFORMAT_XRGB8888,
                                    SDL_TEXTUREACCESS_STREAMING,
                                    MAX_WIDTH,
                                    MAX_HEIGHT);
        if (texture == nullptr) {
            return false;
        }
        // The console's pixels are square-ish and few, and smoothing
        // them is not what they looked like.
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
        pixels.resize(std::size_t{MAX_WIDTH} * MAX_HEIGHT);
        return true;
    }

    static constexpr int MAX_WIDTH = 640;
    static constexpr int MAX_HEIGHT = 480;
};

// The debugger lives in a strip down the left and the picture gets
// everything to the right of it. The panels are pinned there rather
// than left floating, because a panel that can be dragged over the
// machine's output eventually is — and this build of ImGui has no
// docking to arrange them with instead.
float sidebar_width(float window_width)
{
    return std::clamp(window_width * 0.3f, 320.0f, 460.0f);
}

constexpr ImGuiWindowFlags PANEL_FLAGS = ImGuiWindowFlags_NoMove |
    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;

// ImGui measures in logical points and the renderer in pixels, and on
// a high-density display those are not the same number, so the strip's
// width is converted rather than assumed.
SDL_FRect picture_region(SDL_Renderer* renderer, float sidebar)
{
    int output_width = 0;
    int output_height = 0;
    SDL_GetCurrentRenderOutputSize(renderer, &output_width, &output_height);

    const float logical = ImGui::GetIO().DisplaySize.x;
    float scale = 1.0f;
    if (logical > 0) {
        scale = static_cast<float>(output_width) / logical;
    }
    const float left = sidebar * scale;
    return {left,
            0,
            static_cast<float>(output_width) - left,
            static_cast<float>(output_height)};
}

// Every display mode was shown at 4:3 whatever its pixel count — 256
// across and 640 across filled the same television — so the picture is
// fitted to that shape rather than to its own dimensions, and centred
// in whatever room is left over.
SDL_FRect fit_into(const SDL_FRect& region)
{
    constexpr float ASPECT = 4.0f / 3.0f;
    float width = region.w;
    float height = width / ASPECT;
    if (height > region.h) {
        height = region.h;
        width = height * ASPECT;
    }
    return {region.x + (region.w - width) / 2,
            region.y + (region.h - height) / 2,
            width,
            height};
}

void present_display(SDL_Renderer* renderer,
                     Display& display,
                     const Gpu& gpu,
                     const SDL_FRect& region)
{
    const SDL_FRect area = fit_into(region);

    // A blanked GPU puts out no picture at all, which is a black
    // screen rather than the last thing that was drawn.
    if (gpu.display_disabled) {
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderFillRect(renderer, &area);
        return;
    }

    const int width = std::min<int>(static_cast<int>(gpu.display_width()),
                                    Display::MAX_WIDTH);
    const int height = std::min<int>(static_cast<int>(gpu.display_height()),
                                     Display::MAX_HEIGHT);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const Gpu::Colour colour =
                gpu.display_pixel(static_cast<u32>(x), static_cast<u32>(y));
            display.pixels[std::size_t{static_cast<u32>(y)} * width + x] =
                (u32{colour.r} << 16) | (u32{colour.g} << 8) | colour.b;
        }
    }

    const SDL_Rect uploaded = {0, 0, width, height};
    SDL_UpdateTexture(display.texture,
                      &uploaded,
                      display.pixels.data(),
                      width * static_cast<int>(sizeof(u32)));

    const SDL_FRect source = {
        0, 0, static_cast<float>(width), static_cast<float>(height)};
    SDL_RenderTexture(renderer, display.texture, &source, &area);
}

// Debugger panel: run/pause/step controls and the full register file.
void draw_cpu_window(Console& console, bool& emu_running, float sidebar)
{
    Cpu& cpu = console.cpu;

    const float height = ImGui::GetIO().DisplaySize.y * 0.6f;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(sidebar, height));
    ImGui::Begin("CPU", nullptr, PANEL_FLAGS);

    if (cpu.halted) {
        emu_running = false;
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                           "HALTED: %s",
                           cpu.halt_reason.c_str());
    } else {
        ImGui::TextUnformatted(emu_running ? "running" : "paused");
    }

    if (ImGui::Button(emu_running ? "Pause" : "Run")) {
        emu_running = !emu_running;
    }
    ImGui::SameLine();
    if (ImGui::Button("Step")) {
        emu_running = false;
        // Single-stepping still moves the clock, so events stay in
        // step with the instruction stream while debugging.
        console.step();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        console.reset();
    }

    ImGui::Text("cycle %llu  frame %llu",
                static_cast<unsigned long long>(console.scheduler.now),
                static_cast<unsigned long long>(console.frames));
    ImGui::Text("pc %08X  hi %08X  lo %08X", cpu.pc, cpu.hi, cpu.lo);
    ImGui::Text("sr %08X  cause %08X  epc %08X  bad %08X",
                cpu.sr,
                cpu.cause_register(),
                cpu.epc,
                cpu.bad_vaddr);
    ImGui::Text("i_stat %04X  i_mask %04X  scanline %u",
                console.bus.irq.status,
                console.bus.irq.mask,
                console.bus.gpu.scanline);
    ImGui::Separator();
    // Two to a row rather than four: the strip is narrower than the
    // window used to be, and a register that wraps is worse than one
    // more row of them.
    for (int i = 0; i < 32; i++) {
        if (i % 2 != 0) {
            ImGui::SameLine();
        }
        ImGui::Text("%-4s %08X ", REG_NAMES[i], cpu.regs[i]);
    }

    ImGui::End();
}

// What the BIOS has printed, captured by the CPU's putchar hook.
// It is the main sign of life before there is a GPU to draw with.
void draw_tty_window(const Cpu& cpu, float sidebar)
{
    const float top = ImGui::GetIO().DisplaySize.y * 0.6f;
    ImGui::SetNextWindowPos(ImVec2(0, top));
    ImGui::SetNextWindowSize(
        ImVec2(sidebar, ImGui::GetIO().DisplaySize.y - top));
    ImGui::Begin("TTY", nullptr, PANEL_FLAGS);
    ImGui::TextUnformatted(cpu.tty.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::End();
}

void log_new_tty_lines(const Cpu& cpu, size_t& logged_upto)
{
    while (true) {
        const size_t newline = cpu.tty.find('\n', logged_upto);
        if (newline == std::string::npos) {
            return;
        }
        const auto line = cpu.tty.substr(logged_upto, newline - logged_upto);
        SDL_Log("tty: %s", line.c_str());
        logged_upto = newline + 1;
    }
}

}  // namespace

int main(int argc, char** argv)
{
    std::string bios_path = "SCPH1001.BIN";
    std::string exe_path;
    std::string disc_path;

    for (int i = 1; i < argc; i++) {
        const std::string argument = argv[i];
        if (argument == "--exe" && i + 1 < argc) {
            exe_path = argv[++i];
        } else if (argument == "--disc" && i + 1 < argc) {
            disc_path = argv[++i];
        } else if (argument.ends_with(".zip") || argument.ends_with(".cue")) {
            // A game named without the flag, since that is what
            // anyone would try first. Only the two extensions a disc
            // is unambiguously named with are taken this way: a .bin
            // is as likely to be the BIOS as a disc, so that one still
            // has to say which it is.
            disc_path = argument;
        } else {
            bios_path = argument;
        }
    }

    // Megabytes of arrays, too big for the stack.
    static Console console;
    if (!console.bus.load_bios(bios_path)) {
        SDL_Log("failed to load BIOS from %s", bios_path.c_str());
        return 1;
    }
    // Before the reset, so the drive has the disc in it from the
    // moment the BIOS first asks what is there.
    if (!disc_path.empty() && !console.bus.cdrom.disc.load(disc_path)) {
        SDL_Log("failed to load the disc from %s", disc_path.c_str());
        return 1;
    }
    console.reset();

    // Before the window, since the boot it runs first shows nothing
    // and a window that appeared only to sit blank would be worse
    // than one that appears when there is something in it.
    if (!exe_path.empty()) {
        const std::string failure = sideload_exe(console, exe_path);
        if (!failure.empty()) {
            SDL_Log("%s", failure.c_str());
            return 1;
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window* window =
        SDL_CreateWindow("wobble",
                         1280,
                         720,
                         SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window == nullptr) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (renderer == nullptr) {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);

    Display display;
    if (!display.create(renderer)) {
        SDL_Log("could not create the display texture: %s", SDL_GetError());
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    bool emu_running = true;
    bool was_halted = false;
    size_t tty_logged_upto = 0;
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }
            handle_pad_key(console.bus.sio, event);
        }

        if (emu_running) {
            console.run_cycles(CYCLES_PER_HOST_FRAME);
        }
        if (console.cpu.halted && !was_halted) {
            SDL_Log("cpu halted: %s", console.cpu.halt_reason.c_str());
        }
        was_halted = console.cpu.halted;
        log_new_tty_lines(console.cpu, tty_logged_upto);

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        const float sidebar = sidebar_width(ImGui::GetIO().DisplaySize.x);
        draw_cpu_window(console, emu_running, sidebar);
        draw_tty_window(console.cpu, sidebar);
        const SDL_FRect region = picture_region(renderer, sidebar);

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
        SDL_RenderClear(renderer);
        present_display(renderer, display, console.bus.gpu, region);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    SDL_DestroyTexture(display.texture);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
