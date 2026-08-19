#include "sio.h"

#include <algorithm>
#include <array>

#include "savestate.h"

namespace {

constexpr u32 DATA = 0x1F801040;
constexpr u32 STAT = 0x1F801044;
constexpr u32 MODE = 0x1F801048;
constexpr u32 CONTROL = 0x1F80104A;
constexpr u32 BAUD = 0x1F80104E;

// JOY_MODE. Nine of the halfword's bits are real — the baud divider,
// the character length, the two parity bits and the clock polarity —
// and the rest do not exist, so they read back as zero whatever is
// written over them.
constexpr u16 MODE_WRITABLE = 0x013F;

// JOY_CTRL.
constexpr u16 CONTROL_SELECT = 1 << 1;
constexpr u16 CONTROL_ACKNOWLEDGE = 1 << 4;
constexpr u16 CONTROL_RESET = 1 << 6;
constexpr u16 CONTROL_ACK_INTERRUPT = 1 << 12;
constexpr u16 CONTROL_SECOND_SOCKET = 1 << 13;

// JOY_STAT. The transmitter is never busy here, because an exchange
// finishes inside the store that started it, so both of its ready bits
// are always set.
constexpr u32 STAT_TX_READY = 1 << 0;
constexpr u32 STAT_RX_NOT_EMPTY = 1 << 1;
constexpr u32 STAT_TX_FINISHED = 1 << 2;
constexpr u32 STAT_INTERRUPT = 1 << 9;

// The byte that addresses each kind of device.
constexpr u8 ADDRESS_CONTROLLER = 0x01;
constexpr u8 ADDRESS_MEMORY_CARD = 0x81;

// The command asking a controller for its buttons. It is the only one
// a digital pad understands.
constexpr u8 COMMAND_READ = 0x42;

// A digital controller's identifier, sent back over the two bytes
// after it is addressed.
constexpr u8 IDENTIFIER_LOW = 0x41;
constexpr u8 IDENTIFIER_HIGH = 0x5A;

// What the bus reads when nothing is driving it.
constexpr u8 IDLE = 0xFF;

constexpr u64 BITS_PER_BYTE = 8;

}  // namespace

void Sio::reset()
{
    buttons = NOTHING_PRESSED;
    control = 0;
    mode = 0;
    baud = 0;
    received = 0;
    received_full = false;
    interrupt = false;
    step = 0;
    target = Target::None;
}

void Sio::visit_state(State& state)
{
    state(buttons);
    state(control);
    state(mode);
    state(baud);
    state(received);
    state(received_full);
    state(interrupt);
    state(step);
    state(target);
}

void Sio::press(Button button)
{
    buttons &= static_cast<u16>(~(1u << static_cast<u32>(button)));
}

void Sio::release(Button button)
{
    buttons |= static_cast<u16>(1u << static_cast<u32>(button));
}

bool Sio::first_socket() const
{
    return (control & CONTROL_SECOND_SOCKET) == 0;
}

u8 Sio::exchange(u8 value, bool& acknowledge)
{
    acknowledge = false;

    if (target == Target::None) {
        // The first byte of an exchange says which device is being
        // spoken to. Only a controller in the first socket answers;
        // everything else leaves the bus idle and never acknowledges,
        // which is how software finds an empty socket.
        if (value == ADDRESS_CONTROLLER && first_socket()) {
            target = Target::Controller;
            step = 1;
            acknowledge = true;
        } else if (value == ADDRESS_MEMORY_CARD) {
            target = Target::MemoryCard;
        }
        return IDLE;
    }

    if (target != Target::Controller) {
        return IDLE;
    }

    switch (step) {
    case 1:
        // Any command but "read the buttons" ends the exchange, since
        // a digital pad has nothing else to say.
        if (value != COMMAND_READ) {
            target = Target::None;
            step = 0;
            return IDLE;
        }
        step = 2;
        acknowledge = true;
        return IDENTIFIER_LOW;
    case 2:
        step = 3;
        acknowledge = true;
        return IDENTIFIER_HIGH;
    case 3:
        step = 4;
        acknowledge = true;
        return static_cast<u8>(buttons);
    default:
        // The last byte, so no acknowledge: that is what tells the
        // driver the controller has finished.
        target = Target::None;
        step = 0;
        return static_cast<u8>(buttons >> 8);
    }
}

u64 Sio::acknowledge_delay() const
{
    // The port has a divider of its own: JOY_BAUD sets it and JOY_MODE
    // multiplies it, which is how the same wires carry a controller at
    // 250 kHz and slower devices below that. A bit costs one whole
    // divider period, so a byte costs eight.
    constexpr std::array<u64, 4> FACTORS = {1, 1, 16, 64};
    const u64 period = u64{baud} * FACTORS[mode & 3];
    return std::max<u64>(period, 1) * BITS_PER_BYTE + ACKNOWLEDGE_CYCLES;
}

bool Sio::deliver_acknowledge()
{
    interrupt = true;
    return (control & CONTROL_ACK_INTERRUPT) != 0;
}

u32 Sio::read_register(u32 phys)
{
    switch (phys) {
    case DATA: {
        const u8 value = received_full ? received : IDLE;
        received_full = false;
        return value;
    }
    case STAT: {
        u32 value = STAT_TX_READY | STAT_TX_FINISHED;
        if (received_full) {
            value |= STAT_RX_NOT_EMPTY;
        }
        if (interrupt) {
            value |= STAT_INTERRUPT;
        }
        return value;
    }
    case MODE:
        return mode;
    case CONTROL:
        return control;
    case BAUD:
        return baud;
    default:
        return 0;
    }
}

bool Sio::write_register(u32 phys, u32 value)
{
    switch (phys) {
    case DATA: {
        bool acknowledge = false;
        received = exchange(static_cast<u8>(value), acknowledge);
        received_full = true;
        return acknowledge;
    }

    case CONTROL: {
        control = static_cast<u16>(value);
        if ((control & CONTROL_ACKNOWLEDGE) != 0) {
            interrupt = false;
        }
        if ((control & CONTROL_RESET) != 0) {
            reset();
        }
        // Dropping the select line ends whatever exchange was in
        // progress, which is how a driver starts one over after an
        // unexpected answer.
        if ((control & CONTROL_SELECT) == 0) {
            target = Target::None;
            step = 0;
        }
        // Neither bit stays set: they are actions, not settings.
        control &= static_cast<u16>(~(CONTROL_ACKNOWLEDGE | CONTROL_RESET));
        return false;
    }

    case MODE:
        mode = static_cast<u16>(value) & MODE_WRITABLE;
        return false;
    case BAUD:
        baud = static_cast<u16>(value);
        return false;
    default:
        return false;
    }
}
