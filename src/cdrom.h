#pragma once

#include <array>
#include <initializer_list>

#include "types.h"

struct State;

// The CD-ROM controller: less a disc interface than a second computer,
// spoken to through four bytes of address space.
//
//   0x1F801800  status, and the index that says what the other three
//               ports mean — the controller has more registers than it
//               has addresses, so they are banked behind this one
//   0x1F801801  the command byte going out, response bytes coming back
//   0x1F801802  command parameters going out, sector data coming back
//   0x1F801803  interrupt enable, and the interrupt that is pending
//
// Nothing the drive is asked to do finishes inside the store that asked
// for it. Every answer arrives later as an interrupt numbered 1 to 5,
// with a few bytes attached, and only one can be outstanding at a time:
// the drive holds the next answer until software has acknowledged the
// last. That is why a command which both starts something and finishes
// it — Init, or a seek — answers twice, an INT3 saying it was heard and
// an INT2 saying it is done.
//
// The drive is empty. That is a state a real console can be in rather
// than a gap here: GetID answers INT5 with the "no disc" reason, and it
// is that answer which sends the BIOS to its shell. A disc would fill
// the data FIFO, which is why reads of it return nothing yet.
struct CdRom {
    static constexpr u32 BASE = 0x1F801800;
    static constexpr u32 END = 0x1F801804;

    // How often the drive is looked at. It is a mechanism running to
    // its own timing rather than to the CPU's, and the shortest thing
    // it does takes tens of thousands of cycles, so resolving its
    // deadlines to a couple of thousand is under a tenth of a percent
    // — far finer than anything software can measure, and a great deal
    // simpler than scheduling each answer individually.
    static constexpr u64 TICK_CYCLES = 2048;

    // Both FIFOs the CPU writes are sixteen bytes deep. No command
    // takes more than a handful of parameters; the depth is the
    // hardware's.
    static constexpr u32 FIFO_CAPACITY = 16;

    // The longest answer any command gives is GetID's eight bytes.
    static constexpr u32 RESPONSE_CAPACITY = 16;

    // A command produces at most a first answer and a second one, so
    // there are never more than two waiting.
    static constexpr u32 QUEUE_CAPACITY = 2;

    void reset();

    void visit_state(State& state);

    // Reads are not const: taking a byte out of the response FIFO is
    // what advances it.
    u8 read_register(u32 phys);
    void write_register(u32 phys, u8 value);

    // Lets `cycles` of drive time pass, and reports whether an answer
    // came due in it that has brought the interrupt line up.
    bool tick(u64 cycles);

    // An answer the drive has prepared but not yet handed over.
    struct Response {
        u8 interrupt = 0;
        u8 length = 0;
        std::array<u8, RESPONSE_CAPACITY> bytes{};

        // Cycles left before the drive is ready to give it. Only the
        // answer at the front of the queue counts down, because the one
        // behind it does not start until the one in front has been
        // taken.
        u64 remaining = 0;
    };

    // Which of the banked register sets 0x1F801801..3 currently reach.
    u8 index = 0;

    // The drive's own status byte, the first byte of nearly every
    // answer. Only the spindle bit is ever set here: there is no disc
    // to seek on, read from or play.
    static constexpr u8 STATUS_ERROR = 1 << 0;
    static constexpr u8 STATUS_MOTOR = 1 << 1;
    static constexpr u8 STATUS_ID_ERROR = 1 << 3;
    u8 status = STATUS_MOTOR;

    // The last Setmode, kept because Getparam reads it back.
    u8 mode = 0;

    std::array<u8, FIFO_CAPACITY> parameters{};
    u32 parameter_count = 0;

    std::array<u8, RESPONSE_CAPACITY> response{};
    u32 response_length = 0;
    u32 response_read = 0;

    // Bits 0..4 of the interrupt flag register. The low three are the
    // number of the answer waiting to be acknowledged, and while they
    // are non-zero the drive holds everything else back.
    u8 interrupt_flag = 0;
    u8 interrupt_enable = 0;

    // Set from the store that writes a command until its first answer
    // is delivered, which is the whole of what software can see of the
    // controller being busy.
    bool busy = false;

    std::array<Response, QUEUE_CAPACITY> queue{};
    u32 queued = 0;

private:
    void execute(u8 command);

    // Queues an answer `delay` cycles out. The parameters are read
    // during the command, not when the answer is given, so a command
    // can queue both of its answers up front and be done.
    void answer(u8 interrupt, std::initializer_list<u8> bytes, u64 delay);

    // Hands the front of the queue to software: its bytes become the
    // response FIFO and its number becomes the pending interrupt.
    void deliver();

    bool line_active() const;
};
