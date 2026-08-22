// Host side of the emulator: it owns the window, the debugger UI and
// the frame loop that drives the CPU. The emulated machine itself is
// just a Bus (memory and devices) with a Cpu attached to it.

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include "cdrom.h"
#include "console.h"
#include "disasm.h"
#include "exe.h"
#include "gpu.h"
#include "sio.h"
#include "spu.h"

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

// Puts a disc in the drive the way a person does: the lid up first,
// so a game waiting for its second disc sees the drive open, the
// medium changed while nothing can reach it, and the lid down again
// after a moment. Dropping a file on the window is the whole gesture,
// and this is the half of it that cannot wait.
void open_lid_for(Console& console, const char* path)
{
    console.bus.cdrom.open_shell();
    if (!console.bus.cdrom.disc.load(path)) {
        SDL_Log("failed to load the disc from %s", path);
    }
}

// How long the lid is left standing open afterwards. A game polls the
// drive to find out that it was opened at all, so shutting it in the
// same instant would be a swap nothing had the chance to notice.
constexpr u32 LID_OPEN_FRAMES = 60;

// How much sound may be waiting to be played before the emulator is
// making it faster than the sound card is taking it. This is the
// safety valve behind the pacing below rather than the thing that
// holds the two clocks together: what is over the mark is thrown away,
// which bounds the delay when a machine falls far enough behind that
// the pacing cannot pull it back. It has to be deeper than the longest
// slice below, or a pass presented late would have most of the sound
// it made thrown away rather than played.
constexpr int MAX_QUEUED_FRAMES = Spu::SAMPLE_RATE / 4;  // 250 ms
constexpr int MAX_QUEUED_BYTES =
    MAX_QUEUED_FRAMES * static_cast<int>(sizeof(Spu::Frame));

// How much sound should be waiting to be played. A pass makes about a
// frame's worth and the card takes about a frame's worth, so left to
// itself the queue sits near empty and every hitch in a pass runs it
// dry. Silence spliced into a sound is heard as a click, and the
// louder the sound the louder the click — which is why a starving
// queue sounds like distortion rather than like a gap. Keeping three
// frames standing in front of the card is what a hitch is spent
// instead.
constexpr int TARGET_QUEUED_FRAMES = Spu::SAMPLE_RATE / 20;  // 50 ms

// The most console time one pass may run. A pass normally covers the
// time since the last one, which is a frame; this is the ceiling for
// when it is not — a window dragged, a breakpoint sat at, a machine
// waking from sleep — so that the console catches up over several
// passes rather than running minutes of a game in one.
constexpr u64 MAX_SLICE_CYCLES = CPU_CLOCK_HZ / 4;  // 250 ms

// How long a pass should take. The loop paces itself rather than
// letting the display do it: on a compositor a present waits for the
// compositor to ask for a frame, and a window you have switched away
// from is not asked for frames — for as long as it takes you to come
// back. Waiting on that is a console that stops dead, so nothing here
// ever waits on it.
constexpr u64 PASS_NS = 1'000'000'000 / 60;

// The most a pass may be lengthened or shortened to get back to that:
// fourteen frames out of a pass's seven hundred and thirty-five, which
// is two percent. Emulated time is the console's clock, so a trim is a
// change of speed and of pitch — two percent is a third of a semitone,
// heard only if it were held, and it is not: it is a hundred times the
// hundredth of a percent the two clocks drift apart by, so it converges
// in a moment and sits at nothing.
constexpr s64 MAX_TRIM_FRAMES = Spu::SAMPLE_RATE / 3000;

// The sound card, if there is one. A machine with no audio device is
// not a failure to start: the SPU runs either way, and the picture is
// still worth having.
SDL_AudioStream* open_audio()
{
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_Log("no audio: %s", SDL_GetError());
        return nullptr;
    }

    const SDL_AudioSpec spec = {
        SDL_AUDIO_S16, 2, static_cast<int>(Spu::SAMPLE_RATE)};
    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (stream == nullptr) {
        SDL_Log("no audio: %s", SDL_GetError());
        return nullptr;
    }

    SDL_ResumeAudioStreamDevice(stream);
    return stream;
}

// Everything the SPU has made since the last frame, handed to SDL to
// play. Pushing from here rather than from an audio callback means the
// device is fed on the same thread that runs the machine, so nothing
// has to be locked.
void push_audio(SDL_AudioStream* stream, Spu& spu)
{
    std::array<Spu::Frame, 1024> frames{};
    while (true) {
        const u32 count = spu.take_output(frames.data(), frames.size());
        if (count == 0) {
            return;
        }
        if (SDL_GetAudioStreamQueued(stream) > MAX_QUEUED_BYTES) {
            continue;  // drained, and dropped: the queue is too long
        }
        SDL_PutAudioStreamData(stream,
                               frames.data(),
                               static_cast<int>(count * sizeof(Spu::Frame)));
    }
}

// Whether there is any point handing a picture over. A window that is
// covered, shrunk to the taskbar, or simply not the one being typed at
// is a window the compositor has stopped asking for frames — and a
// present it has not asked for is one that waits until it does, which
// may be until you come back. Nothing is presented to such a window,
// so nothing waits on it. Read immediately before presenting rather
// than at the top of the pass: switching away is something that
// happens between one and the other.
bool worth_presenting(SDL_Window* window)
{
    const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
    const SDL_WindowFlags out_of_sight =
        SDL_WINDOW_MINIMIZED | SDL_WINDOW_OCCLUDED;
    if ((flags & out_of_sight) != 0) {
        return false;
    }
    return (flags & SDL_WINDOW_INPUT_FOCUS) != 0;
}

// The end of a pass: whenever the last one ended, plus a frame. Timing
// the wait from a deadline rather than from now keeps the work a pass
// does from being added to the wait, and a machine that has fallen
// behind takes its next deadline from the clock rather than trying to
// make up passes it has missed.
void wait_for_pass(u64& deadline)
{
    const u64 now = SDL_GetTicksNS();
    if (deadline <= now) {
        deadline = now + PASS_NS;
        return;
    }
    SDL_DelayNS(deadline - now);
    deadline += PASS_NS;
}

// Console time owed for the real time that has passed since the last
// pass. A pass is normally one refresh of the display, but a window
// nobody is looking at is a window a compositor presents a few times a
// second or not at all — and a fixed frame's worth per pass would then
// run the console a few frames a second too, which is not heard as a
// slow picture but as the sound stopping and starting again in time
// with them. What the console owes is time, not passes.
u64 elapsed_cycles(u64& since)
{
    const u64 now = SDL_GetTicksNS();
    const u64 elapsed = now - since;
    since = now;
    return std::min(elapsed * CPU_CLOCK_HZ / 1'000'000'000, MAX_SLICE_CYCLES);
}

// That slice, trimmed by how far the sound queue is from where it
// should be. Wall time and the sound card's crystal are two clocks and
// never quite the same speed, so left alone the queue drifts one way
// or the other until it empties or fills; the trim runs the console at
// the speed the card actually plays at. A machine with no sound card
// has only the clock to go on, and keeps the slice as it is.
u64 cycles_for_pass(SDL_AudioStream* stream, u64 slice)
{
    if (stream == nullptr) {
        return slice;
    }

    const int queued =
        SDL_GetAudioStreamQueued(stream) / static_cast<int>(sizeof(Spu::Frame));
    const s64 short_by = TARGET_QUEUED_FRAMES - queued;

    // An eighth of the shortfall at a time: correcting all of it in
    // one pass would overshoot and set the queue swinging, and the
    // console would audibly speed up and slow down with it.
    const s64 trim =
        std::clamp(short_by / 8, -MAX_TRIM_FRAMES, MAX_TRIM_FRAMES);
    const s64 cycles_a_frame = static_cast<s64>(Spu::TICK_CYCLES);
    const s64 trimmed = static_cast<s64>(slice) + trim * cycles_a_frame;
    return static_cast<u64>(std::max<s64>(trimmed, 0));
}

// The console's picture on its way to the window. The texture is made
// once at the largest size any display mode reaches, and only the part
// the current mode uses is ever uploaded — so a mode change costs
// nothing and needs no reallocation.
struct Display {
    SDL_Texture* texture = nullptr;
    std::vector<u32> pixels;

    // The picture held in `pixels`: its shape, and whether there is
    // one at all. A blanked GPU puts out no picture, which is a black
    // screen rather than the last thing that was drawn.
    int width = 0;
    int height = 0;
    bool blank = true;

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

// Takes a copy of what the GPU is putting out. Kept apart from
// showing it because when the copy is taken is the whole question: a
// game draws over the frame it is displaying and has it finished by
// vertical blank, so a copy taken part-way through one is part-drawn.
// The BIOS fades its logo in through a single buffer, and copying it
// wherever a host frame happened to land caught it cleared but not yet
// redrawn — a black frame every dozen or so, which is a flicker.
void capture_display(Display& display, const Gpu& gpu)
{
    display.blank = gpu.display_disabled;
    if (display.blank) {
        return;
    }

    display.width = std::min<int>(static_cast<int>(gpu.display_width()),
                                  Display::MAX_WIDTH);
    display.height = std::min<int>(static_cast<int>(gpu.display_height()),
                                   Display::MAX_HEIGHT);

    for (int y = 0; y < display.height; y++) {
        for (int x = 0; x < display.width; x++) {
            const Gpu::Colour colour =
                gpu.display_pixel(static_cast<u32>(x), static_cast<u32>(y));
            const std::size_t index =
                std::size_t{static_cast<u32>(y)} * display.width + x;
            display.pixels[index] =
                (u32{colour.r} << 16) | (u32{colour.g} << 8) | colour.b;
        }
    }
}

void present_display(SDL_Renderer* renderer,
                     Display& display,
                     const SDL_FRect& region)
{
    const SDL_FRect area = fit_into(region);

    if (display.blank || display.width == 0 || display.height == 0) {
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderFillRect(renderer, &area);
        return;
    }

    const SDL_Rect uploaded = {0, 0, display.width, display.height};
    SDL_UpdateTexture(display.texture,
                      &uploaded,
                      display.pixels.data(),
                      display.width * static_cast<int>(sizeof(u32)));

    const SDL_FRect source = {0,
                              0,
                              static_cast<float>(display.width),
                              static_cast<float>(display.height)};
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

// What the program is and how to ask it for something, for a reader
// who has only the binary. The controls are part of it because a
// keyboard is the only pad there is, and nothing on screen says so.
void print_usage()
{
    std::puts(R"(wobble - a PlayStation 1 emulator

Usage: wobble [bios] [game] [options]

  bios              a BIOS image to boot (default: SCPH1001.BIN)
  game              a .zip, .cue or .chd to put in the drive

Options:
  --disc PATH       put PATH in the drive, whatever it is named
                    (.bin and .iso images need this, since a .bin is
                    as likely to be the BIOS as a disc)
  --exe PATH        boot the BIOS, then side-load a PS-EXE in place of
                    a disc's program
  -h, --help        show this and exit

Controls (the keyboard is the pad in the first socket):
  arrow keys        d-pad
  X  S  Z  A        cross, circle, square, triangle
  Q  W  E  R        L1, L2, R1, R2
  Enter  RShift     start, select

Dropping a disc image on the window swaps it in: the lid opens, the
disc changes, and the lid shuts a second later, which is what a
two-disc game is waiting to see.

Examples:
  wobble SCPH1001.BIN
  wobble SCPH1001.BIN "Ridge Racer (USA).zip"
  wobble SCPH1001.BIN --exe program.exe)");
}

}  // namespace

int main(int argc, char** argv)
{
    std::string bios_path = "SCPH1001.BIN";
    std::string exe_path;
    std::string disc_path;

    for (int i = 1; i < argc; i++) {
        const std::string argument = argv[i];
        if (argument == "-h" || argument == "--help") {
            print_usage();
            return 0;
        }
        if (argument == "--exe" && i + 1 < argc) {
            exe_path = argv[++i];
        } else if (argument == "--disc" && i + 1 < argc) {
            disc_path = argv[++i];
        } else if (argument.ends_with(".zip") || argument.ends_with(".cue") ||
                   argument.ends_with(".chd")) {
            // A game named without the flag, since that is what
            // anyone would try first. Only the extensions a disc is
            // unambiguously named with are taken this way: a .bin is
            // as likely to be the BIOS as a disc, so that one still
            // has to say which it is.
            disc_path = argument;
        } else if (argument.starts_with("-")) {
            // Anything else beginning with a dash is a mistyped
            // option, not a file to boot: taking it for one would
            // fail much later and say the wrong thing about why.
            std::fprintf(stderr, "unknown option: %s\n", argument.c_str());
            print_usage();
            return 1;
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
    // The loop keeps its own time (see wait_for_pass), so a present
    // hands the picture over and returns. Waiting for a refresh is
    // waiting on the compositor, and the compositor stops asking a
    // window you have switched away from for anything at all.
    SDL_SetRenderVSync(renderer, 0);

    SDL_AudioStream* audio = open_audio();

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
    u32 lid_open_frames = 0;
    u64 last_pass = SDL_GetTicksNS();
    u64 pass_deadline = last_pass + PASS_NS;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }
            if (event.type == SDL_EVENT_DROP_FILE) {
                open_lid_for(console, event.drop.data);
                lid_open_frames = LID_OPEN_FRAMES;
            }
            handle_pad_key(console.bus.sio, event);
        }

        if (lid_open_frames > 0) {
            lid_open_frames--;
            if (lid_open_frames == 0) {
                console.bus.cdrom.close_shell();
            }
        }

        const u64 slice = elapsed_cycles(last_pass);
        if (emu_running) {
            // The console owes the time that has passed, which is what
            // keeps the machine running at its own speed whatever the
            // window is doing. Where in the console's frame that slice
            // ends is nobody's business but the picture's, so the
            // slice is run a frame at a time and the picture taken
            // each time one is finished. A slice ending mid-frame
            // leaves the last finished picture standing, which is what
            // a television would still be showing.
            u64 owed = cycles_for_pass(audio, slice);
            while (owed > 0 && !console.cpu.halted) {
                const u64 before = console.scheduler.now;
                const bool finished = console.run_until_frame(owed);
                owed -= std::min(owed, console.scheduler.now - before);
                if (finished) {
                    capture_display(display, console.bus.gpu);
                }
            }
        } else {
            // Paused, the picture is whatever is there now: someone
            // stepping an instruction at a time is watching for what
            // that instruction did.
            capture_display(display, console.bus.gpu);
        }
        if (audio != nullptr) {
            push_audio(audio, console.bus.spu);
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
        present_display(renderer, display, region);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);

        if (worth_presenting(window)) {
            SDL_RenderPresent(renderer);
        }
        wait_for_pass(pass_deadline);
    }

    SDL_DestroyAudioStream(audio);
    SDL_DestroyTexture(display.texture);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
