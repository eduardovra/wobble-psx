#include "spu.h"

#include "savestate.h"

namespace {

// The control block, by address. Everything below 0x1F801D80 is the
// twenty-four voices, sixteen bytes each.
constexpr u32 KEY_ON = 0x1F801D88;
constexpr u32 KEY_OFF = 0x1F801D8C;
constexpr u32 ENDX = 0x1F801D9C;
constexpr u32 IRQ_ADDRESS = 0x1F801DA4;
constexpr u32 TRANSFER_ADDRESS = 0x1F801DA6;
constexpr u32 TRANSFER_FIFO = 0x1F801DA8;
constexpr u32 CONTROL = 0x1F801DAA;
constexpr u32 STATUS = 0x1F801DAE;

// Bits of the control register. The transfer mode is the pair that
// matters most here: it is what the status register has to echo back
// before the sound library will go on.
constexpr u16 CONTROL_TRANSFER_MODE = 0x0030;
constexpr u16 CONTROL_IRQ_ENABLE = 0x0040;

// The low six bits of the control register are the "current mode",
// which the status register reports back once the SPU has adopted it.
constexpr u16 STATUS_MODE_MASK = 0x003F;
constexpr u16 STATUS_IRQ_FLAG = 1 << 6;
constexpr u16 STATUS_TRANSFER_REQUEST = 1 << 7;
constexpr u16 STATUS_DMA_WRITE_REQUEST = 1 << 8;
constexpr u16 STATUS_DMA_READ_REQUEST = 1 << 9;

// Transfer modes, in the two bits above: stop, manual, DMA write and
// DMA read, in that order. Only the two DMA modes are named, since
// they are the only ones the status register answers differently for —
// the other two ask the DMA controller for nothing.
constexpr u16 TRANSFER_DMA_WRITE = 2;
constexpr u16 TRANSFER_DMA_READ = 3;

// Software sets the transfer and interrupt addresses in units of eight
// bytes, since sample data is written in blocks of sixteen and never
// starts anywhere finer than this.
constexpr u32 ADDRESS_UNIT = 8;

}  // namespace

void Spu::reset()
{
    registers = {};
    ram = {};
    transfer_address = 0;
    key_on = 0;
    key_off = 0;
    ended = 0;
    irq_flag = false;
}

void Spu::visit_state(State& state)
{
    state(registers);
    state(ram);
    state(transfer_address);
    state(key_on);
    state(key_off);
    state(ended);
    state(irq_flag);
}

u16 Spu::control() const { return registers[index_of(CONTROL)]; }

u16 Spu::status() const
{
    // The mode the SPU is in, which is simply the mode it was asked
    // for: there is nothing here that takes time to adopt one, and
    // software waits on these six bits before it will go on.
    u16 value = control() & STATUS_MODE_MASK;

    if (irq_flag) {
        value |= STATUS_IRQ_FLAG;
    }

    // Which way the SPU wants the DMA controller to move data, which
    // follows from the transfer mode and nothing else. The busy bit
    // above them stays clear: a transfer here finishes inside the
    // store that asked for it, so there is never a moment when
    // software could see one in progress.
    switch ((control() & CONTROL_TRANSFER_MODE) >> 4) {
    case TRANSFER_DMA_WRITE:
        value |= STATUS_TRANSFER_REQUEST | STATUS_DMA_WRITE_REQUEST;
        break;
    case TRANSFER_DMA_READ:
        value |= STATUS_TRANSFER_REQUEST | STATUS_DMA_READ_REQUEST;
        break;
    default:
        break;
    }
    return value;
}

bool Spu::interrupt_pending() const { return irq_flag; }

void Spu::transfer(u16 value)
{
    const u32 address = transfer_address % RAM_SIZE;
    ram[address] = static_cast<u8>(value);
    ram[address + 1] = static_cast<u8>(value >> 8);

    const u32 armed = u32{registers[index_of(IRQ_ADDRESS)]} * ADDRESS_UNIT;
    if ((control() & CONTROL_IRQ_ENABLE) != 0 && address == armed) {
        irq_flag = true;
    }

    transfer_address = (transfer_address + 2) % RAM_SIZE;
}

void Spu::write_dma(u32 word)
{
    transfer(static_cast<u16>(word));
    transfer(static_cast<u16>(word >> 16));
}

u32 Spu::read_dma()
{
    u32 word = 0;
    for (u32 i = 0; i < 2; i++) {
        const u32 address = transfer_address % RAM_SIZE;
        const u32 half = u32{ram[address]} | (u32{ram[address + 1]} << 8);
        word |= half << (i * 16);
        transfer_address = (transfer_address + 2) % RAM_SIZE;
    }
    return word;
}

u16 Spu::read_register(u32 phys)
{
    switch (phys) {
    case STATUS:
        return status();
    case ENDX:
        return static_cast<u16>(ended);
    case ENDX + 2:
        return static_cast<u16>(ended >> 16);
    default:
        break;
    }

    // Everything else reads back what was written to it, which is what
    // the hardware does for the voice registers and the volumes. The
    // per-voice current volume and repeat address would answer for a
    // voice being played, and there is none.
    return registers[index_of(phys)];
}

void Spu::write_register(u32 phys, u16 value)
{
    registers[index_of(phys)] = value;

    switch (phys) {
    case TRANSFER_ADDRESS:
        transfer_address = u32{value} * ADDRESS_UNIT;
        break;

    case TRANSFER_FIFO:
        // Hardware queues these thirty-two deep and moves them when
        // the control register says to. Writing straight through is
        // the same thing to anything that cannot see the FIFO's depth,
        // and nothing can: there is no register that reports it.
        transfer(value);
        break;

    case CONTROL:
        // Turning the interrupt off is how software acknowledges one.
        if ((value & CONTROL_IRQ_ENABLE) == 0) {
            irq_flag = false;
        }
        break;

    case KEY_ON:
    case KEY_ON + 2: {
        const u32 shift = (phys == KEY_ON) ? 0 : 16;
        const u32 started = u32{value} << shift;
        key_on |= started;
        key_off &= ~started;

        // A voice that is started reports at once that it has reached
        // the end of its sample. That is a lie — there is no decoder
        // to reach anything — but it is the useful direction to lie
        // in: software that starts a sound and waits for it to finish
        // carries on, where the truthful answer of "still playing,
        // forever" would stop it dead.
        ended |= started;
        break;
    }

    case KEY_OFF:
    case KEY_OFF + 2: {
        const u32 shift = (phys == KEY_OFF) ? 0 : 16;
        const u32 stopped = u32{value} << shift;
        key_off |= stopped;
        key_on &= ~stopped;
        break;
    }

    default:
        break;
    }
}
