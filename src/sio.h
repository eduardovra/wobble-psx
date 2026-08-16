#pragma once

#include "types.h"

struct State;

// The controller and memory card port, SIO0 — a synchronous serial
// bus with two identical sockets on the front of the console. (The
// serial port on the back is SIO1, a different device at the next
// block of addresses, and is not here.)
//
//   0x1F801040  JOY_DATA  a byte out and, in the same instant, a byte
//                         back: the bus shifts both ways at once
//   0x1F801044  JOY_STAT  whether the shift register is ready, whether
//                         a byte is waiting, and the acknowledge line
//   0x1F801048  JOY_MODE  baud divider and character length
//   0x1F80104A  JOY_CTRL  transmit and receive enables, which socket
//                         is selected, and which interrupts are wanted
//   0x1F80104E  JOY_BAUD  the reload value for the baud timer
//
// A device on the bus is addressed by the first byte of an exchange
// and answers from the second onwards, so reading a controller is a
// conversation rather than a register read: 01h to say "controller in
// this slot", 42h to ask for its state, and then a byte written for
// each byte wanted back. Between bytes the device pulls /ACK low to
// say it has more to send, and that is what raises the interrupt the
// driver waits on. A socket with nothing in it never acknowledges,
// which is exactly how software tells that nothing is plugged in.
//
// One digital controller is plugged into the first socket. The second
// socket is empty, and so is every memory card slot.
struct Sio {
    static constexpr u32 BASE = 0x1F801040;
    static constexpr u32 END = 0x1F801050;

    // Buttons, in the order the controller reports them, active low —
    // a pressed button is a zero. The names are the pad's rather than
    // any game's.
    enum class Button : u32 {
        Select = 0,
        L3 = 1,
        R3 = 2,
        Start = 3,
        Up = 4,
        Right = 5,
        Down = 6,
        Left = 7,
        L2 = 8,
        R2 = 9,
        L1 = 10,
        R1 = 11,
        Triangle = 12,
        Circle = 13,
        Cross = 14,
        Square = 15,
    };

    void reset();

    void visit_state(State& state);

    // How long after the last bit of a byte the device pulls /ACK low.
    // It matters that the acknowledge is not instant: the driver sends
    // a byte, clears the interrupt the last one left behind, and only
    // then waits for this one — so an acknowledge delivered inside the
    // store that sent the byte is thrown away before anything looks
    // for it, and the driver decides the socket is empty.
    static constexpr u64 ACKNOWLEDGE_CYCLES = 338;

    // Cycles until the acknowledge for a byte written now: the eight
    // bits going out on the wire, and then the device's answer.
    u64 acknowledge_delay() const;

    // Reads are not const: taking the received byte out of JOY_DATA is
    // what empties it.
    u32 read_register(u32 phys);

    // Returns whether the device answered and so will acknowledge,
    // which the caller schedules for ACKNOWLEDGE_CYCLES from now.
    bool write_register(u32 phys, u32 value);

    // The acknowledge arriving. Returns whether it brings the CPU's
    // interrupt line up, which it does only if the port was asked for
    // one.
    bool deliver_acknowledge();

    void press(Button button);
    void release(Button button);

    // All ones: nothing pressed.
    static constexpr u16 NOTHING_PRESSED = 0xFFFF;
    u16 buttons = NOTHING_PRESSED;

    u16 control = 0;
    u16 mode = 0;
    u16 baud = 0;

    u8 received = 0;
    bool received_full = false;

    // Set from the acknowledge until software clears it, which is what
    // JOY_STAT reports and what the interrupt was raised for.
    bool interrupt = false;

    // How far into the current exchange the addressed device is. Zero
    // means the next byte written picks a device rather than being
    // data for one.
    u32 step = 0;

    // Which device the exchange addressed, or None while no exchange
    // is in progress. A memory card is addressed the same way and gets
    // as far as being asked for, since there is none to answer.
    enum class Target : u32 { None, Controller, MemoryCard };
    Target target = Target::None;

private:
    // Sends one byte and returns what came back, setting `acknowledge`
    // when the device has more to say.
    u8 exchange(u8 value, bool& acknowledge);

    // Whether the selected socket is the one with the controller in
    // it. JOY_CTRL picks between the two.
    bool first_socket() const;
};
