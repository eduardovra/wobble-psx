#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include "bus.h"
#include "cpu.h"

namespace {

constexpr const char* REG_NAMES[32] = {
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0",   "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0",   "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8",   "t9", "k0", "k1", "gp", "sp", "fp", "ra",
};

// Not cycle-accurate pacing — just a budget per video frame that
// keeps the UI responsive while the BIOS executes.
constexpr int INSTRUCTIONS_PER_FRAME = 100000;

void draw_cpu_window(Cpu& cpu, bool& emu_running)
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
        cpu.step();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        cpu.reset();
    }

    ImGui::Text("pc %08X  hi %08X  lo %08X", cpu.pc, cpu.hi, cpu.lo);
    ImGui::Text("sr %08X  cause %08X  epc %08X",
                cpu.sr,
                cpu.cause,
                cpu.epc);
    ImGui::Separator();
    for (int i = 0; i < 32; i++) {
        if (i % 4 != 0) {
            ImGui::SameLine();
        }
        ImGui::Text("%-4s %08X ", REG_NAMES[i], cpu.regs[i]);
    }

    ImGui::End();
}

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
        const auto line = cpu.tty.substr(logged_upto,
                                         newline - logged_upto);
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

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "wobble",
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
            for (int i = 0; i < INSTRUCTIONS_PER_FRAME; i++) {
                cpu.step();
                if (cpu.halted) {
                    break;
                }
            }
        }
        if (cpu.halted && !was_halted) {
            SDL_Log("cpu halted: %s", cpu.halt_reason.c_str());
        }
        was_halted = cpu.halted;
        log_new_tty_lines(cpu, tty_logged_upto);

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        draw_cpu_window(cpu, emu_running);
        draw_tty_window(cpu);

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(
            ImGui::GetDrawData(), renderer);
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
