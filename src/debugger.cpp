#include "debugger.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

#include "cdrom.h"
#include "console.h"
#include "disasm.h"
#include "dma.h"
#include "exe.h"
#include "gpu.h"
#include "irq.h"
#include "sio.h"
#include "spu.h"
#include "timers.h"

namespace {

// Numbers are read as hex unless they say otherwise, because almost
// every number in this domain is an address or a register value. A
// leading "0x" is accepted and ignored; a leading "#" forces decimal,
// for the counts where decimal is what anyone means.
std::optional<u64> parse_number(std::string_view text)
{
    if (text.empty()) {
        return std::nullopt;
    }

    int base = 16;
    if (text.front() == '#') {
        base = 10;
        text.remove_prefix(1);
    } else if (text.size() > 2 && text[0] == '0' &&
               (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2);
    }
    if (text.empty()) {
        return std::nullopt;
    }

    u64 value = 0;
    const char* first = text.data();
    const char* last = first + text.size();
    const auto result = std::from_chars(first, last, value, base);
    if (result.ec != std::errc{} || result.ptr != last) {
        return std::nullopt;
    }
    return value;
}

std::vector<std::string> split(const std::string& line)
{
    std::istringstream stream(line);
    std::vector<std::string> words;
    std::string word;
    while (stream >> word) {
        words.push_back(word);
    }
    return words;
}

const char* stop_name(Debugger::Stop stop)
{
    switch (stop) {
    case Debugger::Stop::Budget:
        return "budget";
    case Debugger::Stop::Breakpoint:
        return "breakpoint";
    case Debugger::Stop::Watchpoint:
        return "watchpoint";
    case Debugger::Stop::Target:
        return "target";
    case Debugger::Stop::Halted:
        return "halted";
    }
    return "?";
}

// One line saying where the machine is, printed after anything that
// moves it. Having the disassembly of the next instruction on the same
// line as the pc saves a round trip almost every time.
std::string where(Console& console)
{
    const u32 pc = console.cpu.pc;
    const u32 instr = console.bus.read32(pc);
    return std::format("pc {:08X}  cyc {}  frame {}  {}",
                       pc,
                       console.scheduler.now,
                       console.frames,
                       disassemble(instr, pc));
}

std::string format_registers(Console& console)
{
    const Cpu& cpu = console.cpu;
    std::string out;

    for (u32 i = 0; i < 32; i++) {
        out += std::format("{:>4} {:08X}", REG_NAMES[i], cpu.regs[i]);
        out += (i % 4 == 3) ? "\n" : "  ";
    }
    out += std::format(
        "pc   {:08X}  hi {:08X}  lo {:08X}\n", cpu.pc, cpu.hi, cpu.lo);
    out += std::format("sr   {:08X}  cause {:08X}  epc {:08X}  bad {:08X}\n",
                       cpu.sr,
                       cpu.cause_register(),
                       cpu.epc,
                       cpu.bad_vaddr);
    return out;
}

// The controller's buttons by name, for the command that holds one
// down. A held button is state rather than an event: the driver reads
// the pad when it likes, so pressing one means leaving it pressed
// until something says otherwise.
const std::array<std::pair<std::string_view, Sio::Button>, 16> BUTTON_NAMES = {{
    {"select", Sio::Button::Select},
    {"l3", Sio::Button::L3},
    {"r3", Sio::Button::R3},
    {"start", Sio::Button::Start},
    {"up", Sio::Button::Up},
    {"right", Sio::Button::Right},
    {"down", Sio::Button::Down},
    {"left", Sio::Button::Left},
    {"l2", Sio::Button::L2},
    {"r2", Sio::Button::R2},
    {"l1", Sio::Button::L1},
    {"r1", Sio::Button::R1},
    {"triangle", Sio::Button::Triangle},
    {"circle", Sio::Button::Circle},
    {"cross", Sio::Button::Cross},
    {"square", Sio::Button::Square},
}};

std::optional<Sio::Button> find_button(std::string_view name)
{
    for (const auto& [text, button] : BUTTON_NAMES) {
        if (text == name) {
            return button;
        }
    }
    return std::nullopt;
}

std::string button_names()
{
    std::string out;
    for (const auto& [text, button] : BUTTON_NAMES) {
        if (!out.empty()) {
            out += ", ";
        }
        out += text;
    }
    return out;
}

std::string format_devices(Console& console)
{
    const Gpu& gpu = console.bus.gpu;
    const Irq& irq = console.bus.irq;
    const Dma& dma = console.bus.dma;
    const CdRom& cdrom = console.bus.cdrom;

    std::string out;
    out += std::format("cdrom  stat {:02X}  int {:X}  enable {:02X}  "
                       "queued {}  busy {}\n",
                       cdrom.status,
                       cdrom.interrupt_flag,
                       cdrom.interrupt_enable,
                       cdrom.queued,
                       cdrom.busy ? "yes" : "no");
    out += std::format("pad  buttons {:04X}\n", console.bus.sio.buttons);
    for (u32 i = 0; i < Timers::COUNT; i++) {
        const Timers::Timer& timer = console.bus.timers.timers[i];
        if (timer.mode == 0 && timer.value == 0) {
            continue;  // never set up; not worth a line
        }
        out +=
            std::format("timer{}  value {:04X}  target {:04X}  mode {:04X}\n",
                        i,
                        timer.value,
                        timer.target,
                        timer.mode);
    }
    const Spu& spu = console.bus.spu;
    out += std::format("spu  ctrl {:04X}  endx {:06X}  playing {}  "
                       "samples {}\n",
                       spu.control(),
                       spu.ended,
                       spu.active_voices(),
                       spu.output_ready());
    out += std::format("irq  stat {:04X}  mask {:04X}  active {}\n",
                       irq.status,
                       irq.mask,
                       irq.active() ? "yes" : "no");
    out += std::format("gpu  stat {:08X}  scanline {}  field {}  vblank {}\n",
                       gpu.status(),
                       gpu.scanline,
                       gpu.odd_field ? 1 : 0,
                       gpu.in_vblank() ? "yes" : "no");
    out += std::format("gpu  mode {}  cmdwords {}  transfer {}x{} done {}\n",
                       static_cast<u32>(gpu.mode),
                       gpu.command_words,
                       gpu.transfer.width,
                       gpu.transfer.height,
                       gpu.transfer.pixels_done);
    out += std::format("dma  dpcr {:08X}  dicr {:08X}\n",
                       dma.control,
                       dma.read_register(Dma::BASE + 0x74));
    for (u32 i = 0; i < Dma::CHANNEL_COUNT; i++) {
        const Dma::Channel& channel = dma.channels[i];
        if (channel.control == 0 && channel.base == 0) {
            continue;  // never used; not worth a line
        }
        out += std::format("dma{}  madr {:08X}  bcr {:08X}  chcr {:08X}\n",
                           i,
                           channel.base,
                           channel.block,
                           channel.control);
    }
    return out;
}

// The busiest addresses first, which is what a spin loop looks like.
std::string format_histogram(const std::unordered_map<u32, u64>& counts,
                             std::size_t top,
                             Console* console)
{
    std::vector<std::pair<u32, u64>> sorted(counts.begin(), counts.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) {
            return a.second > b.second;
        }
        return a.first < b.first;  // stable output for equal counts
    });

    std::string out;
    for (std::size_t i = 0; i < sorted.size() && i < top; i++) {
        const auto [address, count] = sorted[i];
        out += std::format("  {:08X}  {:>12}", address, count);
        if (console != nullptr) {
            const u32 instr = console->bus.read32(address);
            out += std::format("  {}", disassemble(instr, address));
        }
        out += "\n";
    }
    if (sorted.empty()) {
        out += "  (nothing recorded)\n";
    }
    return out;
}

constexpr const char* HELP =
    "run [n]            run n instructions (default 1)\n"
    "runc <n>           run n cycles\n"
    "frames [n]         run n frames (default 1)\n"
    "until <addr> [n]   run until pc reaches addr, at most n instructions\n"
    "break [addr]       set a breakpoint, or list them\n"
    "unbreak <addr>     remove one\n"
    "watch <rw> <addr> [len]   report reads/writes to a range\n"
    "unwatch <addr>     remove one\n"
    "regs               the register file and COP0\n"
    "disas [addr] [n]   disassemble n instructions (default at pc)\n"
    "mem <addr> [n]     dump n words\n"
    "dev                device state: irq, gpu, dma, cdrom, pad, timers, spu\n"
    "pad <down|up> <button>   hold a controller button, or let go\n"
    "trace [n]          the last n instructions retired\n"
    "tracing <on|off>   record the instruction trace\n"
    "profile <n> [top]  run n instructions, then the busiest addresses\n"
    "disc <file>        put a disc in the drive (.zip, .cue, .bin)\n"
    "exe <file>         boot the BIOS, then run a PS-EXE on it\n"
    "screen <file>      write what the display shows, as a PPM\n"
    "vram <file>        write the whole of VRAM, as a PPM\n"
    "audio <on|off>     record what the SPU plays\n"
    "audio <file>       write what has been recorded, as a WAV\n"
    "save <file>        write a save state\n"
    "load <file>        restore one\n"
    "reset              power-cycle the machine\n"
    "tty                what the BIOS has printed\n"
    "quit\n"
    "\n"
    "Numbers are hex; prefix with # for decimal.\n";

// Writes what the display is showing as a binary PPM: a three-line
// header and then the pixels, three bytes each. It is the plainest
// image format there is, which is the point — a picture of the machine
// should not need a library to produce or anything unusual to open.
std::string write_screen(const Gpu& gpu, const std::string& path)
{
    const u32 width = gpu.display_width();
    const u32 height = gpu.display_height();

    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return "could not open that file\n";
    }
    file << "P6\n" << width << " " << height << "\n255\n";

    std::string pixels;
    pixels.reserve(std::size_t{width} * height * 3);
    for (u32 y = 0; y < height; y++) {
        for (u32 x = 0; x < width; x++) {
            const Gpu::Colour colour = gpu.display_pixel(x, y);
            pixels += static_cast<char>(colour.r);
            pixels += static_cast<char>(colour.g);
            pixels += static_cast<char>(colour.b);
        }
    }
    file.write(pixels.data(), static_cast<std::streamsize>(pixels.size()));

    return std::format("wrote {}x{}{}\n",
                       width,
                       height,
                       gpu.display_disabled ? " (display is blanked)" : "");
}

// The same, for the whole of VRAM rather than the part of it being
// shown. A picture of the display says whether something looks right;
// a picture of VRAM says what was actually drawn, textures and palettes
// and back buffers included, which is what a test with a reference
// image of its own is comparing against. The mask bit has no colour and
// is not in here.
std::string write_vram(const Gpu& gpu, const std::string& path)
{
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return "could not open that file\n";
    }
    file << "P6\n" << Gpu::VRAM_WIDTH << " " << Gpu::VRAM_HEIGHT << "\n255\n";

    // Five bits to eight the same way the display does it, so a pixel
    // looks the same whichever picture it is found in.
    const auto stretch = [](u32 value) {
        return static_cast<char>((value << 3) | (value >> 2));
    };

    std::string pixels;
    pixels.reserve(Gpu::VRAM_PIXELS * 3);
    for (const u16 pixel : gpu.vram) {
        pixels += stretch(pixel & 0x1F);
        pixels += stretch((pixel >> 5) & 0x1F);
        pixels += stretch((pixel >> 10) & 0x1F);
    }
    file.write(pixels.data(), static_cast<std::streamsize>(pixels.size()));

    return std::format("wrote {}x{}\n", Gpu::VRAM_WIDTH, Gpu::VRAM_HEIGHT);
}

// Writes what has been recorded as a WAV: a forty-four byte header and
// then the samples, which is the audio equivalent of the PPM above —
// the plainest container the frames will go in, and one that every
// player and every plotting script already opens.
std::string write_audio(const std::vector<Spu::Frame>& frames,
                        const std::string& path)
{
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return "could not open that file\n";
    }

    const u32 data_bytes = static_cast<u32>(frames.size() * sizeof(Spu::Frame));
    const u32 byte_rate = Spu::SAMPLE_RATE * sizeof(Spu::Frame);

    const auto put = [&](const char* text) { file.write(text, 4); };
    const auto put32 = [&](u32 value) {
        const std::array<char, 4> bytes = {static_cast<char>(value),
                                           static_cast<char>(value >> 8),
                                           static_cast<char>(value >> 16),
                                           static_cast<char>(value >> 24)};
        file.write(bytes.data(), bytes.size());
    };
    const auto put16 = [&](u16 value) {
        const std::array<char, 2> bytes = {static_cast<char>(value),
                                           static_cast<char>(value >> 8)};
        file.write(bytes.data(), bytes.size());
    };

    put("RIFF");
    put32(36 + data_bytes);
    put("WAVE");
    put("fmt ");
    put32(16);  // the length of this chunk
    put16(1);   // uncompressed
    put16(2);   // stereo
    put32(Spu::SAMPLE_RATE);
    put32(byte_rate);
    put16(sizeof(Spu::Frame));  // bytes per frame
    put16(16);                  // bits per sample
    put("data");
    put32(data_bytes);
    file.write(reinterpret_cast<const char*>(frames.data()), data_bytes);

    return std::format("wrote {} frames, {:.2f} seconds\n",
                       frames.size(),
                       static_cast<double>(frames.size()) / Spu::SAMPLE_RATE);
}

}  // namespace

void Debugger::take_recording(Spu& spu)
{
    std::array<Spu::Frame, 1024> frames{};
    while (true) {
        const u32 count = spu.take_output(frames.data(), frames.size());
        if (count == 0) {
            return;
        }
        recorded.insert(recorded.end(), frames.begin(), frames.begin() + count);
    }
}

void Debugger::note_access(u32 address, u32 length, bool write)
{
    const bool is_fetch = fetch_pending && !write && address == executing_pc;
    if (is_fetch) {
        fetch_pending = false;
    }

    if (profiling && !is_fetch) {
        data_counts[address]++;
    }

    for (const Watch& watch : watchpoints) {
        const bool wanted = write ? watch.on_write : watch.on_read;
        if (!wanted) {
            continue;
        }
        // Overlap, not equality: a watch covers a range and an access
        // has a width, so either can straddle the other.
        const bool before = address + length <= watch.address;
        const bool after = address >= watch.address + watch.length;
        if (before || after) {
            continue;
        }
        watch_hit = true;
        watch_address = address;
        watch_was_write = write;
        return;
    }
}

Debugger::Stop
Debugger::run(Console& console, u64 limit, std::optional<u32> target)
{
    console.bus.debug = this;
    watch_hit = false;
    instructions_run = 0;

    Stop stop = Stop::Budget;
    for (u64 i = 0; i < limit; i++) {
        if (console.cpu.halted) {
            stop = Stop::Halted;
            break;
        }

        const u32 pc = console.cpu.pc;
        executing_pc = pc;
        fetch_pending = true;
        if (trace_enabled) {
            // Read around the hook: the debugger looking at the
            // instruction is not the machine accessing memory.
            console.bus.debug = nullptr;
            const u32 instr = console.bus.read32(pc);
            console.bus.debug = this;

            if (trace.size() < TRACE_CAPACITY) {
                trace.push_back({pc, instr});
            } else {
                trace[trace_next] = {pc, instr};
            }
            trace_next = (trace_next + 1) % TRACE_CAPACITY;
        }
        if (profiling) {
            pc_counts[pc]++;
        }

        console.step();
        instructions_run++;

        // The SPU's buffer holds a fifth of a second, so it has to be
        // emptied while the machine runs rather than after it. Once
        // every few thousand instructions is often enough — that is a
        // fraction of a millisecond of console time.
        if (recording && (i % 1024) == 0) {
            take_recording(console.bus.spu);
        }

        if (watch_hit) {
            stop = Stop::Watchpoint;
            break;
        }
        if (target.has_value() && console.cpu.pc == *target) {
            stop = Stop::Target;
            break;
        }
        const auto hit =
            std::find(breakpoints.begin(), breakpoints.end(), console.cpu.pc);
        if (hit != breakpoints.end()) {
            stop = Stop::Breakpoint;
            break;
        }
    }
    if (console.cpu.halted && stop == Stop::Budget) {
        stop = Stop::Halted;
    }

    console.bus.debug = nullptr;
    return stop;
}

std::string Debugger::execute(Console& console, const std::string& line)
{
    const std::vector<std::string> words = split(line);
    if (words.empty() || words[0].front() == '#') {
        return "";  // blank line or comment
    }
    const std::string& command = words[0];

    // A number argument, if the command was given one.
    auto argument = [&](std::size_t index) -> std::optional<u64> {
        if (words.size() <= index) {
            return std::nullopt;
        }
        return parse_number(words[index]);
    };

    // Everything after the command word, for the commands whose
    // argument is a filename. A path is not a list of words however
    // many spaces are in it, and disc images are named for the game
    // they hold, so most of them have several.
    auto filename = [&]() -> std::string {
        const std::size_t start = line.find_first_not_of(" \t");
        const std::size_t after = line.find_first_of(" \t", start);
        if (after == std::string::npos) {
            return "";
        }
        const std::size_t first = line.find_first_not_of(" \t", after);
        if (first == std::string::npos) {
            return "";
        }
        return line.substr(first, line.find_last_not_of(" \t\r\n") - first + 1);
    };

    auto report_stop = [&](Stop stop) {
        std::string out = std::format("stopped: {} after {} instructions\n",
                                      stop_name(stop),
                                      instructions_run);
        if (stop == Stop::Watchpoint) {
            out += std::format("  {} at {:08X}\n",
                               watch_was_write ? "write" : "read",
                               watch_address);
        }
        if (console.cpu.halted) {
            out += std::format("  halt: {}\n", console.cpu.halt_reason);
        }
        return out + where(console) + "\n";
    };

    if (command == "help") {
        return HELP;
    }

    if (command == "reset") {
        console.reset();
        return where(console) + "\n";
    }

    if (command == "run") {
        const u64 count = argument(1).value_or(1);
        return report_stop(run(console, count, std::nullopt));
    }

    if (command == "runc") {
        const auto cycles = argument(1);
        if (!cycles) {
            return "runc needs a cycle count\n";
        }
        const u64 end = console.scheduler.now + *cycles;
        Stop stop = Stop::Budget;
        // Cycles are not instructions, so this runs in slices and
        // checks the clock between them; a breakpoint still stops it.
        while (console.scheduler.now < end && !console.cpu.halted) {
            stop = run(console, 1024, std::nullopt);
            if (stop != Stop::Budget) {
                break;
            }
        }
        return report_stop(stop);
    }

    if (command == "frames") {
        const u64 count = argument(1).value_or(1);
        const u64 target_frame = console.frames + count;
        Stop stop = Stop::Budget;
        while (console.frames < target_frame && !console.cpu.halted) {
            stop = run(console, 4096, std::nullopt);
            if (stop != Stop::Budget) {
                break;
            }
        }
        return report_stop(stop);
    }

    if (command == "until") {
        const auto address = argument(1);
        if (!address) {
            return "until needs an address\n";
        }
        const u64 limit = argument(2).value_or(100'000'000);
        return report_stop(run(console, limit, static_cast<u32>(*address)));
    }

    if (command == "break") {
        const auto address = argument(1);
        if (!address) {
            if (breakpoints.empty()) {
                return "no breakpoints\n";
            }
            std::string out;
            for (const u32 breakpoint : breakpoints) {
                out += std::format("  {:08X}\n", breakpoint);
            }
            return out;
        }
        breakpoints.push_back(static_cast<u32>(*address));
        return std::format("breakpoint at {:08X}\n", *address);
    }

    if (command == "unbreak") {
        const auto address = argument(1);
        if (!address) {
            return "unbreak needs an address\n";
        }
        const auto removed = std::remove(
            breakpoints.begin(), breakpoints.end(), static_cast<u32>(*address));
        const bool found = removed != breakpoints.end();
        breakpoints.erase(removed, breakpoints.end());
        return found ? "removed\n" : "no such breakpoint\n";
    }

    if (command == "watch") {
        if (words.size() < 3) {
            return "watch needs a mode (r, w or rw) and an address\n";
        }
        const std::string& mode = words[1];
        const auto address = parse_number(words[2]);
        if (!address) {
            return "watch needs an address\n";
        }
        Watch watch;
        watch.address = static_cast<u32>(*address);
        watch.length = static_cast<u32>(argument(3).value_or(4));
        watch.on_read = mode.find('r') != std::string::npos;
        watch.on_write = mode.find('w') != std::string::npos;
        if (!watch.on_read && !watch.on_write) {
            return "watch mode must contain r, w or both\n";
        }
        watchpoints.push_back(watch);
        return std::format("watching {:08X}+{:X} for {}{}\n",
                           watch.address,
                           watch.length,
                           watch.on_read ? "r" : "",
                           watch.on_write ? "w" : "");
    }

    if (command == "unwatch") {
        const auto address = argument(1);
        if (!address) {
            return "unwatch needs an address\n";
        }
        const auto removed = std::remove_if(
            watchpoints.begin(), watchpoints.end(), [&](const Watch& watch) {
                return watch.address == *address;
            });
        const bool found = removed != watchpoints.end();
        watchpoints.erase(removed, watchpoints.end());
        return found ? "removed\n" : "no such watchpoint\n";
    }

    if (command == "regs") {
        return format_registers(console);
    }

    if (command == "dev") {
        return format_devices(console);
    }

    if (command == "pad") {
        if (words.size() < 3) {
            return "pad needs down or up, and a button name\n";
        }
        const bool down = words[1] == "down";
        if (!down && words[1] != "up") {
            return "pad takes down or up\n";
        }
        const auto button = find_button(words[2]);
        if (!button) {
            return "no such button; try " + button_names() + "\n";
        }
        if (down) {
            console.bus.sio.press(*button);
        } else {
            console.bus.sio.release(*button);
        }
        return std::format("buttons {:04X}\n", console.bus.sio.buttons);
    }

    if (command == "disas") {
        const u32 address =
            static_cast<u32>(argument(1).value_or(console.cpu.pc));
        const u64 count = argument(2).value_or(16);
        std::string out;
        for (u64 i = 0; i < count; i++) {
            const u32 at = address + static_cast<u32>(i * 4);
            const u32 instr = console.bus.read32(at);
            // The arrow makes the current instruction findable in a
            // long dump without counting lines.
            const char* marker = (at == console.cpu.pc) ? "->" : "  ";
            out += std::format("{} {:08X}  {:08X}  {}\n",
                               marker,
                               at,
                               instr,
                               disassemble(instr, at));
        }
        return out;
    }

    if (command == "mem") {
        const auto address = argument(1);
        if (!address) {
            return "mem needs an address\n";
        }
        const u64 count = argument(2).value_or(16);
        std::string out;
        for (u64 i = 0; i < count; i += 4) {
            const u32 at = static_cast<u32>(*address + i * 4);
            out += std::format("{:08X} ", at);
            for (u64 j = 0; j < 4 && i + j < count; j++) {
                out += std::format(
                    " {:08X}",
                    console.bus.read32(at + static_cast<u32>(j * 4)));
            }
            out += "\n";
        }
        return out;
    }

    if (command == "tracing") {
        if (words.size() < 2) {
            return std::format("tracing is {}\n", trace_enabled ? "on" : "off");
        }
        trace_enabled = words[1] == "on";
        if (!trace_enabled) {
            trace.clear();
            trace_next = 0;
        }
        return std::format("tracing {}\n", trace_enabled ? "on" : "off");
    }

    if (command == "trace") {
        if (trace.empty()) {
            return "trace is empty (tracing on)\n";
        }
        const u64 count = std::min<u64>(argument(1).value_or(32), trace.size());
        std::string out;
        // The ring's oldest entry is wherever the next write would go,
        // once it has wrapped.
        const std::size_t total = trace.size();
        for (u64 i = 0; i < count; i++) {
            const std::size_t offset = total - count + i;
            const std::size_t index = (total == TRACE_CAPACITY)
                ? (trace_next + offset) % total
                : offset;
            const TraceEntry& entry = trace[index];
            out += std::format("{:08X}  {:08X}  {}\n",
                               entry.pc,
                               entry.instr,
                               disassemble(entry.instr, entry.pc));
        }
        return out;
    }

    if (command == "profile") {
        const auto count = argument(1);
        if (!count) {
            return "profile needs an instruction count\n";
        }
        const std::size_t top =
            static_cast<std::size_t>(argument(2).value_or(10));

        pc_counts.clear();
        data_counts.clear();
        profiling = true;
        const Stop stop = run(console, *count, std::nullopt);
        profiling = false;

        std::string out = report_stop(stop);
        out += "hottest instructions:\n";
        out += format_histogram(pc_counts, top, &console);
        out += "most accessed addresses:\n";
        out += format_histogram(data_counts, top, nullptr);
        return out;
    }

    if (command == "save") {
        const std::string path = filename();
        if (path.empty()) {
            return "save needs a filename\n";
        }
        const std::vector<u8> bytes = console.save_state();
        std::ofstream file(path, std::ios::binary);
        if (!file) {
            return "could not open that file\n";
        }
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        return std::format("saved {} bytes\n", bytes.size());
    }

    if (command == "load") {
        const std::string path = filename();
        if (path.empty()) {
            return "load needs a filename\n";
        }
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            return "could not open that file\n";
        }
        const std::vector<u8> bytes((std::istreambuf_iterator<char>(file)),
                                    std::istreambuf_iterator<char>());
        if (!console.load_state(bytes)) {
            return "not a save state this build can read\n";
        }
        return where(console) + "\n";
    }

    if (command == "disc") {
        const std::string path = filename();
        if (path.empty()) {
            return "disc needs a filename\n";
        }
        if (!console.bus.cdrom.disc.load(path)) {
            return "could not read that disc\n";
        }
        const Disc& disc = console.bus.cdrom.disc;
        return std::format(
            "{} tracks, {} sectors\n", disc.tracks.size(), disc.sector_count());
    }

    if (command == "exe") {
        const std::string path = filename();
        if (path.empty()) {
            return "exe needs a filename\n";
        }
        const std::string failure = sideload_exe(console, path);
        if (!failure.empty()) {
            return failure + "\n";
        }
        return where(console) + "\n";
    }

    if (command == "screen") {
        const std::string path = filename();
        if (path.empty()) {
            return "screen needs a filename\n";
        }
        return write_screen(console.bus.gpu, path);
    }

    if (command == "vram") {
        const std::string path = filename();
        if (path.empty()) {
            return "vram needs a filename\n";
        }
        return write_vram(console.bus.gpu, path);
    }

    if (command == "audio") {
        if (words.size() < 2) {
            return "audio needs on, off or a filename\n";
        }
        if (words[1] == "on" || words[1] == "off") {
            recording = words[1] == "on";
            // Whatever the SPU is holding was made before anyone asked
            // to listen, so it is dropped rather than recorded.
            take_recording(console.bus.spu);
            recorded.clear();
            return recording ? "recording\n" : "stopped\n";
        }
        take_recording(console.bus.spu);
        return write_audio(recorded, filename());
    }

    if (command == "tty") {
        return console.cpu.tty + "\n";
    }

    if (command == "where") {
        return where(console) + "\n";
    }

    return std::format("unknown command: {} (try help)\n", command);
}
