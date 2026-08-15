#include <format>
#include <memory>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "console.h"
#include "debugger.h"
#include "machine.h"

using namespace mips;

namespace {

constexpr u32 CODE = 0x00001000;
constexpr u32 DATA = 0x00002000;

// A console running a short program from RAM, so the tests need no
// BIOS image. Console is megabytes of arrays, hence the heap.
struct Session {
    std::unique_ptr<Console> console = std::make_unique<Console>();
    Debugger debugger;

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

    std::string operator()(const std::string& line)
    {
        return debugger.execute(*console, line);
    }
};

// A loop that writes to DATA for ever, which gives breakpoints and
// watchpoints something to catch. Returned by value: an
// initializer_list would leave the caller holding a dangling array.
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

}  // namespace

TEST_CASE("a breakpoint stops on arrival")
{
    Session session;
    session.load(writing_loop());

    session("break 1008");
    const std::string out = session("run #100");

    CHECK(out.find("breakpoint") != std::string::npos);
    CHECK(session.console->cpu.pc == CODE + 8);
    // Stopped on arrival rather than after the budget ran out.
    CHECK(session.debugger.instructions_run < 100);
}

TEST_CASE("a removed breakpoint no longer stops anything")
{
    Session session;
    session.load(writing_loop());
    session("break 1008");
    session("unbreak 1008");

    const std::string out = session("run #100");

    CHECK(out.find("budget") != std::string::npos);
    CHECK(session.debugger.instructions_run == 100);
}

TEST_CASE("a watchpoint catches the access that touches it")
{
    Session session;

    SUBCASE("a write is caught by a write watch")
    {
        session.load(writing_loop());
        session("watch w 2000");
        const std::string out = session("run #100");

        CHECK(out.find("watchpoint") != std::string::npos);
        CHECK(out.find("write at 00002000") != std::string::npos);
        // The instruction finished before the stop, so its effect is
        // there to be looked at.
        CHECK(session.console->bus.read32(DATA) == 0x55);
    }

    SUBCASE("a write is ignored by a read-only watch")
    {
        session.load(writing_loop());
        session("watch r 2000");
        const std::string out = session("run #100");

        CHECK(out.find("budget") != std::string::npos);
    }

    SUBCASE("a watch covers a range, not just its first word")
    {
        session.load({
            addiu(t1, zero, DATA),
            addiu(t0, zero, 0x55),
            sw(t0, t1, 4),  // writes DATA+4
            nop(),
        });
        session("watch w 2000 #16");
        const std::string out = session("run #10");

        CHECK(out.find("watchpoint") != std::string::npos);
        CHECK(out.find("write at 00002004") != std::string::npos);
    }
}

TEST_CASE("until runs to an address and no further")
{
    Session session;
    session.load(writing_loop());

    const std::string out = session("until 100C #100");

    CHECK(out.find("target") != std::string::npos);
    CHECK(session.console->cpu.pc == CODE + 12);
}

TEST_CASE("until gives up rather than running for ever")
{
    Session session;
    session.load(writing_loop());

    // An address the loop never reaches.
    const std::string out = session("until 9000 #50");

    CHECK(out.find("budget") != std::string::npos);
    CHECK(session.debugger.instructions_run == 50);
}

TEST_CASE("the trace records what actually ran")
{
    Session session;
    session.load(writing_loop());

    session("tracing on");
    session("run #4");
    const std::string out = session("trace #4");

    CHECK(out.find("00001000") != std::string::npos);
    CHECK(out.find("addiu   $t1, $zero, 0x2000") != std::string::npos);
    CHECK(out.find("sw      $t0, 0x0($t1)") != std::string::npos);

    // Turning it off clears it, so a later run starts clean.
    session("tracing off");
    CHECK(session("trace").find("empty") != std::string::npos);
}

TEST_CASE("the trace keeps the most recent instructions once it wraps")
{
    Session session;
    session.load(writing_loop());
    session("tracing on");

    session(std::format("run #{}", Debugger::TRACE_CAPACITY * 2));
    const std::string out = session("trace #2");

    // The ring holds a fixed amount however long the run was.
    CHECK(session.debugger.trace.size() == Debugger::TRACE_CAPACITY);
    // And what it holds is the end of the run, which is the loop.
    CHECK(out.find("00001000") == std::string::npos);
}

TEST_CASE("the profile separates instructions from the data they touch")
{
    Session session;
    session.load(writing_loop());

    const std::string out = session("profile #100 3");

    // The loop body is what ran, and the store target is what it
    // touched. If instruction fetches leaked into the data histogram
    // the two lists would be the same.
    CHECK(out.find("hottest instructions") != std::string::npos);
    CHECK(session.debugger.pc_counts[CODE + 8] > 10);
    CHECK(session.debugger.data_counts[DATA] > 10);
    CHECK(session.debugger.data_counts.count(CODE + 8) == 0);
}

TEST_CASE("running under the debugger does not change what the machine does")
{
    // The whole tool is worthless if observing perturbs; this is the
    // property everything else rests on.
    Session watched;
    Session plain;
    watched.load(writing_loop());
    plain.load(writing_loop());

    watched("tracing on");
    // A watch on memory the program never touches: the hook runs on
    // every access and checks the range, without ever stopping the
    // run, which is exactly the path being tested for side effects.
    watched("watch rw 9000");
    watched("profile #200");

    for (int i = 0; i < 200; i++) {
        plain.console->step();
    }

    CHECK(watched.console->cpu.pc == plain.console->cpu.pc);
    CHECK(watched.console->cpu.regs == plain.console->cpu.regs);
    CHECK(watched.console->scheduler.now == plain.console->scheduler.now);
    CHECK(watched.console->frames == plain.console->frames);
}

TEST_CASE("the memory hook is detached once a run ends")
{
    Session session;
    session.load(writing_loop());
    session("run #10");

    // Left attached, every access outside a run would pay for it.
    CHECK(session.console->bus.debug == nullptr);
}

TEST_CASE("a save state restores the machine exactly")
{
    Session session;
    session.load(writing_loop());
    session("run #50");

    const std::vector<u8> saved = session.console->save_state();
    const u32 pc_at_save = session.console->cpu.pc;
    const u64 cycles_at_save = session.console->scheduler.now;

    session("run #50");
    REQUIRE(session.console->cpu.pc != pc_at_save);

    REQUIRE(session.console->load_state(saved));
    CHECK(session.console->cpu.pc == pc_at_save);
    CHECK(session.console->scheduler.now == cycles_at_save);

    // And it carries on from there identically.
    session("run #50");
    const u32 pc_after = session.console->cpu.pc;
    REQUIRE(session.console->load_state(saved));
    session("run #50");
    CHECK(session.console->cpu.pc == pc_after);
}

TEST_CASE("a save state carries device state, not just memory")
{
    Session session;
    session.load(writing_loop());
    session.console->bus.gpu.write_gp1(0x08000000 | (1u << 5));
    session.console->bus.gpu.vram[100] = 0xBEEF;
    session.console->bus.irq.raise(Interrupt::VBlank);

    const std::vector<u8> saved = session.console->save_state();

    // Not reset(): VRAM deliberately survives that, as it does on
    // hardware, so it would not prove the state carried anything.
    session.console->bus.gpu.vram[100] = 0;
    session.console->bus.gpu.write_gp1(0x08000000);
    session.console->bus.irq.reset();
    REQUIRE(session.console->bus.gpu.vram[100] != 0xBEEF);

    REQUIRE(session.console->load_state(saved));
    CHECK(session.console->bus.gpu.vram[100] == 0xBEEF);
    CHECK(session.console->bus.gpu.interlaced());
    CHECK(session.console->bus.irq.status != 0);
}

TEST_CASE("a state from another build is refused rather than restored")
{
    Session session;
    session.load(writing_loop());
    const std::vector<u8> saved = session.console->save_state();

    SUBCASE("a truncated file")
    {
        const std::vector<u8> damaged(saved.begin(), saved.begin() + 64);
        CHECK_FALSE(session.console->load_state(damaged));
    }
    SUBCASE("something that is not a save state at all")
    {
        const std::vector<u8> nonsense(4096, 0xAB);
        CHECK_FALSE(session.console->load_state(nonsense));
    }
    SUBCASE("a version this build does not know")
    {
        std::vector<u8> future = saved;
        future[4] = 0xFF;
        CHECK_FALSE(session.console->load_state(future));
    }
}

TEST_CASE("numbers are hex unless they are marked decimal")
{
    Session session;
    session.load(writing_loop());

    // 20 hex is 32 decimal: the two must not agree.
    session("run 20");
    const u64 as_hex = session.debugger.instructions_run;
    session("run #20");
    const u64 as_decimal = session.debugger.instructions_run;

    CHECK(as_hex == 32);
    CHECK(as_decimal == 20);
}

TEST_CASE("a bad command is reported rather than being fatal")
{
    Session session;
    session.load(writing_loop());

    CHECK(session("wibble").find("unknown command") != std::string::npos);
    CHECK(session("until").find("needs an address") != std::string::npos);
    CHECK(session("break zzz").find("no breakpoints") != std::string::npos);
    CHECK(session("").empty());
    CHECK(session("# a comment").empty());

    // And the machine is still usable afterwards.
    CHECK(session("run #1").find("budget") != std::string::npos);
}
