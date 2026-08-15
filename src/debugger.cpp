#include "debugger.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>
#include <sstream>

#include "console.h"
#include "disasm.h"
#include "dma.h"
#include "gpu.h"
#include "irq.h"

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

std::string format_devices(Console& console)
{
    const Gpu& gpu = console.bus.gpu;
    const Irq& irq = console.bus.irq;
    const Dma& dma = console.bus.dma;

    std::string out;
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
    "dev                device state: irq, gpu, dma\n"
    "trace [n]          the last n instructions retired\n"
    "tracing <on|off>   record the instruction trace\n"
    "profile <n> [top]  run n instructions, then the busiest addresses\n"
    "screen <file>      write what the display shows, as a PPM\n"
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

}  // namespace

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
        if (words.size() < 2) {
            return "save needs a filename\n";
        }
        const std::vector<u8> bytes = console.save_state();
        std::ofstream file(words[1], std::ios::binary);
        if (!file) {
            return "could not open that file\n";
        }
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        return std::format("saved {} bytes\n", bytes.size());
    }

    if (command == "load") {
        if (words.size() < 2) {
            return "load needs a filename\n";
        }
        std::ifstream file(words[1], std::ios::binary);
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

    if (command == "screen") {
        if (words.size() < 2) {
            return "screen needs a filename\n";
        }
        return write_screen(console.bus.gpu, words[1]);
    }

    if (command == "tty") {
        return console.cpu.tty + "\n";
    }

    if (command == "where") {
        return where(console) + "\n";
    }

    return std::format("unknown command: {} (try help)\n", command);
}
