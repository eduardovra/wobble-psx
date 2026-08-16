#include "gdbstub.h"

#include <algorithm>
#include <charconv>
#include <format>

#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "console.h"
#include "debugger.h"
#include "log.h"

namespace {

// gdb's idea of the MIPS register file: the 32 GPRs, six control
// registers, then an FPU. The PSX's R3000A has no FPU, so everything
// past pc reads as zero and swallows writes — but the slots must be
// there, because the g packet is a fixed-layout struct both sides
// have to agree on.
constexpr u32 REG_SR = 32;
constexpr u32 REG_LO = 33;
constexpr u32 REG_HI = 34;
constexpr u32 REG_BAD = 35;
constexpr u32 REG_CAUSE = 36;
constexpr u32 REG_PC = 37;
constexpr u32 REG_COUNT = 72;

// How many instructions to run between looks at the socket while gdb
// has said continue. Small enough that Ctrl-C in gdb answers promptly,
// large enough that the poll is nothing next to the work.
constexpr u64 RESUME_SLICE = 65536;

// What the machine is, in the form gdb asks for it. Without this a
// client has to be told `set architecture mips:3000` by hand, and it
// has to be told *before* connecting — a frontend that attaches first
// and configures afterwards reads this register file as some other
// machine's and shows a pc of zero. Answering the question the
// protocol provides for it means no client needs configuring at all.
//
// Only the architecture is declared, not the registers: that leaves
// gdb using its built-in layout for the machine, which is the one the
// g packet above is written to.
constexpr const char* TARGET_XML = "<?xml version=\"1.0\"?>"
                                   "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
                                   "<target version=\"1.0\">"
                                   "<architecture>mips:3000</architecture>"
                                   "</target>";

constexpr std::string_view FEATURES_READ = "qXfer:features:read:target.xml:";

// A number as gdb writes them in packet text: plain big-endian hex.
std::optional<u64> parse_hex(std::string_view text)
{
    u64 value = 0;
    const char* first = text.data();
    const char* last = first + text.size();
    const auto result = std::from_chars(first, last, value, 16);
    if (result.ec != std::errc{} || result.ptr != last || text.empty()) {
        return std::nullopt;
    }
    return value;
}

void append_hex_byte(std::string& out, u8 value)
{
    constexpr char DIGITS[] = "0123456789abcdef";
    out += DIGITS[value >> 4];
    out += DIGITS[value & 0xF];
}

std::optional<u8> parse_hex_byte(std::string_view text)
{
    if (text.size() < 2) {
        return std::nullopt;
    }
    const auto value = parse_hex(text.substr(0, 2));
    if (!value) {
        return std::nullopt;
    }
    return static_cast<u8>(*value);
}

// Register values travel in target byte order, and the PSX is
// little-endian — so 0xBFC00000 goes over the wire as "0000c0bf".
void append_hex_u32(std::string& out, u32 value)
{
    for (u32 i = 0; i < 4; i++) {
        append_hex_byte(out, static_cast<u8>(value >> (i * 8)));
    }
}

std::optional<u32> parse_hex_u32(std::string_view text)
{
    if (text.size() != 8) {
        return std::nullopt;
    }
    u32 value = 0;
    for (std::size_t i = 0; i < 4; i++) {
        const auto byte = parse_hex_byte(text.substr(i * 2));
        if (!byte) {
            return std::nullopt;
        }
        value |= static_cast<u32>(*byte) << (i * 8);
    }
    return value;
}

u32 read_register(const Cpu& cpu, u32 index)
{
    if (index < 32) {
        return cpu.regs[index];
    }
    switch (index) {
    case REG_SR:
        return cpu.sr;
    case REG_LO:
        return cpu.lo;
    case REG_HI:
        return cpu.hi;
    case REG_BAD:
        return cpu.bad_vaddr;
    case REG_CAUSE:
        return cpu.cause_register();
    case REG_PC:
        return cpu.pc;
    default:
        return 0;  // the FPU the PSX does not have
    }
}

// Moving pc has to move next_pc with it, or the next step would jump
// back to wherever the machine was about to go before gdb interfered.
void set_pc(Cpu& cpu, u32 address)
{
    cpu.pc = address;
    cpu.next_pc = address + 4;
}

void write_register(Cpu& cpu, u32 index, u32 value)
{
    if (index == 0) {
        return;  // $zero stays zero
    }
    if (index < 32) {
        // Both files, because regs is what the next instruction reads
        // and out_regs is what the step copies back over it.
        cpu.regs[index] = value;
        cpu.out_regs[index] = value;
        return;
    }
    switch (index) {
    case REG_SR:
        cpu.sr = value;
        break;
    case REG_LO:
        cpu.lo = value;
        break;
    case REG_HI:
        cpu.hi = value;
        break;
    case REG_BAD:
        cpu.bad_vaddr = value;
        break;
    case REG_CAUSE:
        cpu.cause = value;
        break;
    case REG_PC:
        set_pc(cpu, value);
        break;
    default:
        break;  // writes to the absent FPU vanish
    }
}

// What a stopped machine says. Breakpoints, completed steps and halts
// all come back as SIGTRAP; a watchpoint names itself and its address
// so gdb can report which one fired.
std::string stop_reply(const Debugger& debugger, Debugger::Stop stop)
{
    if (stop != Debugger::Stop::Watchpoint) {
        return "S05";
    }

    // The keyword has to match the kind of watchpoint gdb set, not
    // the kind of access that tripped it.
    const char* kind = debugger.watch_was_write ? "watch" : "rwatch";
    for (const Debugger::Watch& watch : debugger.watchpoints) {
        const bool before = debugger.watch_address < watch.address;
        const bool after =
            debugger.watch_address >= watch.address + watch.length;
        if (before || after) {
            continue;
        }
        if (watch.on_read && watch.on_write) {
            kind = "awatch";
        } else {
            kind = watch.on_write ? "watch" : "rwatch";
        }
        break;
    }
    return std::format("T05{}:{:x};", kind, debugger.watch_address);
}

}  // namespace

std::optional<std::string> GdbStub::handle_packet(std::string_view payload)
{
    if (payload.empty()) {
        return "";
    }
    Cpu& cpu = console.cpu;
    const std::string_view rest = payload.substr(1);

    switch (payload[0]) {
    case '?':
        return "S05";

    case 'g': {
        std::string out;
        for (u32 i = 0; i < REG_COUNT; i++) {
            append_hex_u32(out, read_register(cpu, i));
        }
        return out;
    }

    case 'G': {
        if (rest.size() < std::size_t{REG_COUNT} * 8) {
            return "E01";
        }
        for (u32 i = 0; i < REG_COUNT; i++) {
            const auto value =
                parse_hex_u32(rest.substr(std::size_t{i} * 8, 8));
            if (!value) {
                return "E01";
            }
            write_register(cpu, i, *value);
        }
        return "OK";
    }

    case 'p': {
        const auto index = parse_hex(rest);
        if (!index || *index >= REG_COUNT) {
            return "E01";
        }
        std::string out;
        append_hex_u32(out, read_register(cpu, static_cast<u32>(*index)));
        return out;
    }

    case 'P': {
        const auto equals = rest.find('=');
        if (equals == std::string_view::npos) {
            return "E01";
        }
        const auto index = parse_hex(rest.substr(0, equals));
        const auto value = parse_hex_u32(rest.substr(equals + 1));
        if (!index || *index >= REG_COUNT || !value) {
            return "E01";
        }
        write_register(cpu, static_cast<u32>(*index), *value);
        return "OK";
    }

    case 'm': {
        const auto comma = rest.find(',');
        if (comma == std::string_view::npos) {
            return "E01";
        }
        const auto address = parse_hex(rest.substr(0, comma));
        const auto length = parse_hex(rest.substr(comma + 1));
        if (!address || !length || *length > 4096) {
            return "E01";
        }
        std::string out;
        for (u64 i = 0; i < *length; i++) {
            const auto byte = console.bus.peek8(static_cast<u32>(*address + i));
            if (!byte) {
                // Partial answers confuse more than they help; if any
                // of the range is off the map, say so about all of it.
                return "E01";
            }
            append_hex_byte(out, *byte);
        }
        return out;
    }

    case 'M': {
        const auto comma = rest.find(',');
        const auto colon = rest.find(':');
        if (comma == std::string_view::npos ||
            colon == std::string_view::npos || colon < comma) {
            return "E01";
        }
        const auto address = parse_hex(rest.substr(0, comma));
        const auto length =
            parse_hex(rest.substr(comma + 1, colon - comma - 1));
        const std::string_view data = rest.substr(colon + 1);
        if (!address || !length || data.size() < *length * 2) {
            return "E01";
        }
        for (u64 i = 0; i < *length; i++) {
            const auto byte = parse_hex_byte(data.substr(i * 2));
            if (!byte) {
                return "E01";
            }
            if (!console.bus.poke8(static_cast<u32>(*address + i), *byte)) {
                return "E01";
            }
        }
        return "OK";
    }

    case 'c':
        if (!rest.empty()) {
            const auto address = parse_hex(rest);
            if (!address) {
                return "E01";
            }
            set_pc(cpu, static_cast<u32>(*address));
        }
        return std::nullopt;  // the caller runs the machine

    case 's': {
        if (!rest.empty()) {
            const auto address = parse_hex(rest);
            if (!address) {
                return "E01";
            }
            set_pc(cpu, static_cast<u32>(*address));
        }
        const Debugger::Stop stop = debugger.run(console, 1, std::nullopt);
        return stop_reply(debugger, stop);
    }

    case 'Z':
    case 'z': {
        // "Z0,addr,kind": type, address, then the breakpoint's size —
        // which for a watchpoint is the length of the watched range.
        const auto first = rest.find(',');
        const auto second = rest.find(',', first + 1);
        if (rest.empty() || first == std::string_view::npos ||
            second == std::string_view::npos) {
            return "E01";
        }
        const char type = rest[0];
        const auto parsed_address =
            parse_hex(rest.substr(first + 1, second - first - 1));
        const auto parsed_length = parse_hex(rest.substr(second + 1));
        if (!parsed_address || !parsed_length) {
            return "E01";
        }
        const u32 address = static_cast<u32>(*parsed_address);
        const bool inserting = payload[0] == 'Z';

        if (type == '0' || type == '1') {  // software or hardware break;
                                           // this machine has one kind
            auto& points = debugger.breakpoints;
            const auto found = std::find(points.begin(), points.end(), address);
            if (inserting && found == points.end()) {
                points.push_back(address);
            }
            if (!inserting && found != points.end()) {
                points.erase(found);
            }
            return "OK";
        }
        if (type == '2' || type == '3' || type == '4') {
            if (!inserting) {
                auto& points = debugger.watchpoints;
                const auto removed =
                    std::remove_if(points.begin(),
                                   points.end(),
                                   [&](const Debugger::Watch& watch) {
                                       return watch.address == address;
                                   });
                points.erase(removed, points.end());
                return "OK";
            }
            Debugger::Watch watch;
            watch.address = address;
            watch.length = static_cast<u32>(*parsed_length);
            watch.on_write = type == '2' || type == '4';
            watch.on_read = type == '3' || type == '4';
            debugger.watchpoints.push_back(watch);
            return "OK";
        }
        return "";  // a breakpoint type this stub does not have
    }

    case 'D':
    case 'k':
        client_done = true;
        return "OK";

    case 'H':  // set thread for later operations: there is one thread
    case 'T':  // thread alive check: it is
        return "OK";

    default:
        break;
    }

    if (payload.starts_with("qSupported")) {
        return "PacketSize=4000;qXfer:features:read+;swbreak+;hwbreak+";
    }
    if (payload.starts_with(FEATURES_READ)) {
        // "…:target.xml:offset,length" — the object is served in
        // whatever sized pieces the client asks for, 'm' when more
        // follows and 'l' when this is the last of it.
        const std::string_view request = payload.substr(FEATURES_READ.size());
        const auto comma = request.find(',');
        if (comma == std::string_view::npos) {
            return "E01";
        }
        const auto offset = parse_hex(request.substr(0, comma));
        const auto length = parse_hex(request.substr(comma + 1));
        if (!offset || !length) {
            return "E01";
        }

        const std::string_view xml = TARGET_XML;
        if (*offset >= xml.size()) {
            return "l";
        }
        const auto start = static_cast<std::size_t>(*offset);
        const std::size_t remaining = xml.size() - start;
        const std::size_t count =
            std::min<std::size_t>(remaining, static_cast<std::size_t>(*length));
        const char* marker = (count < remaining) ? "m" : "l";
        return marker + std::string(xml.substr(start, count));
    }
    if (payload == "qAttached") {
        return "1";  // attached to an existing process, not spawned
    }
    if (payload == "qC") {
        return "QC1";
    }
    if (payload == "qfThreadInfo") {
        return "m1";
    }
    if (payload == "qsThreadInfo") {
        return "l";
    }

    // The empty reply is the protocol's "not supported", after which
    // gdb uses its fallback for whatever it was asking.
    return "";
}

std::string GdbStub::resume(const std::function<bool()>& interrupted)
{
    for (;;) {
        const Debugger::Stop stop =
            debugger.run(console, RESUME_SLICE, std::nullopt);
        if (stop != Debugger::Stop::Budget) {
            return stop_reply(debugger, stop);
        }
        if (interrupted()) {
            return "S02";  // SIGINT, which is what gdb's Ctrl-C means
        }
    }
}

// Everything from here down is framing and sockets: the protocol wraps
// each payload as $payload#xx, where xx is a two-digit checksum, and
// each side acknowledges with '+' or asks again with '-'.
namespace {

std::string frame(std::string_view payload)
{
    u8 sum = 0;
    for (const char c : payload) {
        sum = static_cast<u8>(sum + static_cast<u8>(c));
    }
    return std::format("${}#{:02x}", payload, sum);
}

bool send_all(int fd, std::string_view data)
{
    while (!data.empty()) {
        const ssize_t sent = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
        if (sent <= 0) {
            return false;
        }
        data.remove_prefix(static_cast<std::size_t>(sent));
    }
    return true;
}

// Whether gdb has sent its interrupt byte while the machine runs. A
// closed connection counts as one: there is nobody left to run for.
bool interrupt_pending(int fd)
{
    pollfd entry{fd, POLLIN, 0};
    if (::poll(&entry, 1, 0) <= 0) {
        return false;
    }
    char byte = 0;
    const ssize_t received = ::recv(fd, &byte, 1, 0);
    if (received <= 0) {
        return true;
    }
    return byte == '\x03';
}

// Takes one complete, verified packet payload off the front of the
// buffer, acknowledging it on the way. Nullopt when the buffer does
// not hold a whole packet yet; '+' acknowledgements and stray bytes
// are dropped, and '-' asks for the last reply again.
std::optional<std::string>
extract_packet(int fd, std::string& buffer, const std::string& last_reply)
{
    while (!buffer.empty()) {
        if (buffer.front() == '-') {
            send_all(fd, last_reply);
            buffer.erase(0, 1);
            continue;
        }
        if (buffer.front() != '$') {
            buffer.erase(0, 1);  // '+', or noise between packets
            continue;
        }
        const auto hash = buffer.find('#');
        if (hash == std::string::npos || buffer.size() < hash + 3) {
            return std::nullopt;  // still arriving
        }
        std::string payload = buffer.substr(1, hash - 1);
        const auto expected =
            parse_hex_byte(std::string_view(buffer).substr(hash + 1, 2));
        buffer.erase(0, hash + 3);

        u8 sum = 0;
        for (const char c : payload) {
            sum = static_cast<u8>(sum + static_cast<u8>(c));
        }
        if (!expected || *expected != sum) {
            send_all(fd, "-");
            continue;
        }
        send_all(fd, "+");
        return payload;
    }
    return std::nullopt;
}

void run_session(GdbStub& stub, int fd)
{
    stub.client_done = false;
    std::string buffer;
    std::string last_reply;

    while (!stub.client_done) {
        char chunk[4096];
        const ssize_t received = ::recv(fd, chunk, sizeof(chunk), 0);
        if (received <= 0) {
            return;
        }
        buffer.append(chunk, static_cast<std::size_t>(received));

        while (const auto payload = extract_packet(fd, buffer, last_reply)) {
            const auto reply = stub.handle_packet(*payload);
            if (reply.has_value()) {
                last_reply = frame(*reply);
            } else {
                const std::string stop =
                    stub.resume([fd] { return interrupt_pending(fd); });
                last_reply = frame(stop);
            }
            if (!send_all(fd, last_reply)) {
                return;
            }
        }
    }
}

}  // namespace

bool GdbStub::serve(u16 port)
{
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        return false;
    }
    const int reuse = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    // Loopback only: the stub gives whoever connects the whole machine,
    // so it should not be reachable from anywhere but this host.
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const auto* bind_address = reinterpret_cast<const sockaddr*>(&address);
    if (::bind(listener, bind_address, sizeof(address)) < 0 ||
        ::listen(listener, 1) < 0) {
        ::close(listener);
        return false;
    }

    // Clients are taken one at a time, and a detach or disconnect just
    // means waiting for the next one — the machine keeps its state, so
    // gdb can drop off and come back to the same session.
    for (;;) {
        const int client = ::accept(listener, nullptr, nullptr);
        if (client < 0) {
            continue;
        }
        log_message("gdb: client connected");
        run_session(*this, client);
        ::close(client);
        log_message("gdb: client disconnected");
    }
}
