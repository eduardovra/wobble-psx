#include <format>
#include <memory>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "console.h"
#include "debugger.h"
#include "gdbstub.h"
#include "machine.h"

using namespace mips;

namespace {

constexpr u32 CODE = 0x00001000;
constexpr u32 DATA = 0x00002000;

// A console with a stub attached, spoken to in decoded packet
// payloads — the framing and checksums live with the socket, so the
// protocol is testable without one.
struct Session {
    std::unique_ptr<Console> console = std::make_unique<Console>();
    Debugger debugger;
    GdbStub stub{*console, debugger};

    void load(const std::vector<u32>& program) const
    {
        console->reset();
        u32 address = CODE;
        for (const u32 instruction : program) {
            console->bus.write32(address, instruction);
            address += 4;
        }
        console->cpu.pc = CODE;
        console->cpu.next_pc = CODE + 4;
        console->cpu.current_pc = CODE;
    }

    // A packet that must answer immediately, i.e. anything but c.
    std::string operator()(const std::string& payload)
    {
        const auto reply = stub.handle_packet(payload);
        REQUIRE(reply.has_value());
        return *reply;
    }
};

std::vector<u32> writing_loop()
{
    return {
        addiu(t1, zero, DATA),  // CODE+0
        addiu(t0, zero, 0x55),  // CODE+4
        sw(t0, t1, 0),          // CODE+8
        beq(zero, zero, -3),    // CODE+12, back to CODE+4
        nop(),                  // CODE+16, the delay slot
    };
}

// A value as it travels in a packet: hex bytes in target order, and
// the PSX is little-endian.
std::string hex_le(u32 value)
{
    std::string out;
    for (u32 i = 0; i < 4; i++) {
        out += std::format("{:02x}", (value >> (i * 8)) & 0xFF);
    }
    return out;
}

}  // namespace

TEST_CASE("g reports every register, with pc where the cpu is")
{
    Session session;
    session.load(writing_loop());

    const std::string reply = session("g");
    CHECK(reply.size() == 72 * 8);
    CHECK(reply.substr(std::size_t{37} * 8, 8) == hex_le(CODE));
}

TEST_CASE("P writes a register and p reads it back")
{
    Session session;
    session.load(writing_loop());

    CHECK(session("P8=" + hex_le(0xDEADBEEF)) == "OK");
    CHECK(session("p8") == hex_le(0xDEADBEEF));
    CHECK(session.console->cpu.regs[8] == 0xDEADBEEF);
}

TEST_CASE("writing pc moves next_pc with it")
{
    Session session;
    session.load(writing_loop());

    CHECK(session("P25=" + hex_le(CODE + 8)) == "OK");
    CHECK(session.console->cpu.pc == CODE + 8);
    CHECK(session.console->cpu.next_pc == CODE + 12);
}

TEST_CASE("m reads the program back out of memory")
{
    Session session;
    session.load(writing_loop());

    const std::string reply = session(std::format("m{:x},4", CODE));
    CHECK(reply == hex_le(writing_loop()[0]));
}

TEST_CASE("m refuses hardware registers rather than touching them")
{
    Session session;
    session.load(writing_loop());

    CHECK(session("m1f801070,4") == "E01");
}

TEST_CASE("M patches memory")
{
    Session session;
    session.load(writing_loop());

    const std::string packet =
        std::format("M{:x},4:{}", DATA, hex_le(0x12345678));
    CHECK(session(packet) == "OK");
    CHECK(session.console->bus.read32(DATA) == 0x12345678);
}

TEST_CASE("s steps one instruction")
{
    Session session;
    session.load(writing_loop());

    CHECK(session("s") == "S05");
    CHECK(session.console->cpu.pc == CODE + 4);
}

TEST_CASE("Z0 sets a breakpoint that continue stops at")
{
    Session session;
    session.load(writing_loop());

    CHECK(session(std::format("Z0,{:x},4", CODE + 8)) == "OK");
    const auto reply = session.stub.handle_packet("c");
    REQUIRE(!reply.has_value());

    const std::string stop = session.stub.resume([] { return false; });
    CHECK(stop == "S05");
    CHECK(session.console->cpu.pc == CODE + 8);
}

TEST_CASE("z0 removes a breakpoint")
{
    Session session;
    session.load(writing_loop());

    session(std::format("Z0,{:x},4", CODE + 8));
    CHECK(session(std::format("z0,{:x},4", CODE + 8)) == "OK");
    CHECK(session.debugger.breakpoints.empty());
}

TEST_CASE("Z2 sets a write watchpoint that names itself when it fires")
{
    Session session;
    session.load(writing_loop());

    CHECK(session(std::format("Z2,{:x},4", DATA)) == "OK");
    const std::string stop = session.stub.resume([] { return false; });
    CHECK(stop == std::format("T05watch:{:x};", DATA));
}

TEST_CASE("an interrupt stops a run that would not stop itself")
{
    Session session;
    session.load(writing_loop());

    const std::string stop = session.stub.resume([] { return true; });
    CHECK(stop == "S02");
}

TEST_CASE("the stub describes the machine so no client needs telling")
{
    Session session;
    session.load(writing_loop());

    // A client that attaches before it configures anything reads the
    // register file as some other machine's without this, and shows a
    // pc of zero.
    CHECK(session("qSupported:multiprocess+").find("qXfer:features:read+") !=
          std::string::npos);

    const std::string xml = session("qXfer:features:read:target.xml:0,1000");
    CHECK(xml.starts_with("l"));  // the whole object, nothing to follow
    CHECK(xml.find("mips:3000") != std::string::npos);
}

TEST_CASE("the description is served in pieces when asked for pieces")
{
    Session session;
    session.load(writing_loop());

    // Offsets and lengths are hex, so this asks for the first 16.
    const std::string whole = session("qXfer:features:read:target.xml:0,1000");
    const std::string head = session("qXfer:features:read:target.xml:0,10");
    CHECK(head.starts_with("m"));  // more to come
    CHECK(head.size() == 17);
    CHECK(whole.substr(1, 16) == head.substr(1));

    // Reading from the end says so rather than running off it.
    CHECK(session("qXfer:features:read:target.xml:ffff,10") == "l");
}

TEST_CASE("the stub answers the handshake gdb opens with")
{
    Session session;
    session.load(writing_loop());

    CHECK(session("qSupported:multiprocess+").find("PacketSize") !=
          std::string::npos);
    CHECK(session("?") == "S05");
    CHECK(session("Hg0") == "OK");
    CHECK(session("vMustReplyEmpty") == "");
}
