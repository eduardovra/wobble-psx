// Host side of the emulator: it owns the window, the debugger UI and
// the frame loop that drives the CPU. The emulated machine itself is
// just a Bus (memory and devices) with a Cpu attached to it.

#include <algorithm>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include "bus.h"
#include "cpu.h"
#include "scheduler.h"

namespace {

// Conventional MIPS register names, in register-number order. The
// hardware only numbers them; the names are an ABI convention, but
// they are what disassembly and BIOS documentation use.
// clang-format off
constexpr const char* REG_NAMES[32] = {
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0",   "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0",   "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8",   "t9", "k0", "k1", "gp", "sp", "fp", "ra",
};
// clang-format on

// Emulated time to run per host frame. The renderer is vsynced to
// 60 Hz, so running one sixtieth of a second of console time per pass
// keeps the emulator at roughly real speed.
constexpr u64 CYCLES_PER_HOST_FRAME = CPU_CLOCK_HZ / 60;

// One VBlank per NTSC field. A placeholder until there is a GPU: the
// true rate is 59.94 Hz and comes from the video clock and scanline
// count, not from dividing the CPU clock.
constexpr u64 VBLANK_INTERVAL = CPU_CLOCK_HZ / 60;

// Machine state that is neither CPU nor memory — for now just the
// frames VBlank has counted, which is the only visible sign that the
// scheduler is keeping time.
struct Timing {
    u64 frames = 0;
};

void reset_machine(Cpu& cpu, Scheduler& scheduler, Timing& timing)
{
    cpu.reset();
    scheduler.reset();
    timing = Timing{};
    scheduler.schedule_in(EventKind::VBlank, VBLANK_INTERVAL);
}

void dispatch_due_events(Scheduler& scheduler, Timing& timing)
{
    while (const std::optional<DueEvent> event = scheduler.next_due()) {
        switch (event->kind) {
        case EventKind::VBlank:
            timing.frames++;
            // Nothing is periodic on its own; a repeating event asks
            // for its next occurrence as it fires. Counting from the
            // deadline rather than from now keeps it exactly on rate.
            scheduler.schedule_at(EventKind::VBlank,
                                  event->deadline + VBLANK_INTERVAL);
            break;
        case EventKind::Count:
            break;  // sentinel, never returned
        }
    }
}

// Runs the console for a slice of emulated time. The CPU is let loose
// only as far as the next deadline, so a device's event lands on the
// exact cycle it asked for rather than whenever the loop next checks.
void run_cycles(Cpu& cpu, Scheduler& scheduler, Timing& timing, u64 budget)
{
    const u64 end = scheduler.now + budget;
    while (scheduler.now < end && !cpu.halted) {
        const u64 deadline = std::min(end, scheduler.next_deadline());
        while (scheduler.now < deadline && !cpu.halted) {
            scheduler.advance(cpu.step());
        }
        dispatch_due_events(scheduler, timing);
    }
}

// Debugger panel: run/pause/step controls and the full register file.
void draw_cpu_window(Cpu& cpu,
                     Scheduler& scheduler,
                     Timing& timing,
                     bool& emu_running)
{
    ImGui::Begin("CPU");

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
        scheduler.advance(cpu.step());
        dispatch_due_events(scheduler, timing);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        reset_machine(cpu, scheduler, timing);
    }

    ImGui::Text("cycle %llu  frame %llu",
                static_cast<unsigned long long>(scheduler.now),
                static_cast<unsigned long long>(timing.frames));
    ImGui::Text("pc %08X  hi %08X  lo %08X", cpu.pc, cpu.hi, cpu.lo);
    ImGui::Text("sr %08X  cause %08X  epc %08X  bad %08X",
                cpu.sr,
                cpu.cause,
                cpu.epc,
                cpu.bad_vaddr);
    ImGui::Separator();
    for (int i = 0; i < 32; i++) {
        if (i % 4 != 0) {
            ImGui::SameLine();
        }
        ImGui::Text("%-4s %08X ", REG_NAMES[i], cpu.regs[i]);
    }

    ImGui::End();
}

// What the BIOS has printed, captured by the CPU's putchar hook.
// It is the main sign of life before there is a GPU to draw with.
void draw_tty_window(const Cpu& cpu)
{
    ImGui::Begin("TTY");
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
    const char* bios_path = (argc > 1) ? argv[1] : "SCPH1001.BIN";

    static Bus bus;
    if (!bus.load_bios(bios_path)) {
        SDL_Log("failed to load BIOS from %s", bios_path);
        return 1;
    }
    Cpu cpu(bus);
    Scheduler scheduler;
    Timing timing;
    reset_machine(cpu, scheduler, timing);

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
        }

        if (emu_running) {
            run_cycles(cpu, scheduler, timing, CYCLES_PER_HOST_FRAME);
        }
        if (cpu.halted && !was_halted) {
            SDL_Log("cpu halted: %s", cpu.halt_reason.c_str());
        }
        was_halted = cpu.halted;
        log_new_tty_lines(cpu, tty_logged_upto);

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        draw_cpu_window(cpu, scheduler, timing, emu_running);
        draw_tty_window(cpu);

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
