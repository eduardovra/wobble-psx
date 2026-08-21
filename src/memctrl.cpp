#include "memctrl.h"

#include "savestate.h"

namespace {

// The fields of a delay/size register that bear on timing. The rest of
// it — the window size, the auto-increment bit, the DMA overrides —
// says nothing about how long a CPU access takes.
constexpr u32 ACCESS_TIME = 0xF << 4;  // how long the device holds the bus
constexpr u32 USE_RECOVERY = 1 << 8;
constexpr u32 USE_FLOATING = 1 << 10;
constexpr u32 BUS_16_BIT = 1 << 12;

u32 common_delay(u32 common, u32 index)
{
    return (common >> (index * 4)) & 0xF;
}

}  // namespace

// How many cycles one access to this device takes, and how many each
// access after it in the same transfer takes.
//
// The first access is the plain one: the delay the device asks for,
// plus four cycles of address setup and decode. What the common
// periods buy is the gap between one access and the next — a recovery
// period before the device may be strobed again, and a floating period
// for the last one to let go of the data lines — so they land on the
// accesses that follow rather than on the first. That is why a
// sequential access can cost more than the first one, which it does on
// the two devices asking for a recovery period: the SPU's halfword
// costs 18 cycles and the second halfword of a word costs 21.
//
// Two of the four periods buy nothing, which is why only two of the
// four bits that ask for one are named above. A device may ask for a
// hold period (bit 9) or a pre-strobe one (bit 11) — expansion 2 asks
// for the first and the CD-ROM for the second — and the console
// charges neither: both come out at exactly the delay they asked for.
// The bits are stored, and obeyed to the extent the console obeys
// them, which is not at all.
void MemControl::recalculate()
{
    // The two window base addresses come first in the map and the
    // common delays last, so the six delay registers are what lies
    // between them.
    const u32 common = registers.back();
    for (u32 device = 0; device < static_cast<u32>(Device::Count); device++) {
        const u32 delay = registers[device + 2];
        const u32 access_time = (delay & ACCESS_TIME) >> 4;

        u32 sequential = access_time + 2;
        if (delay & USE_RECOVERY) {
            sequential += common_delay(common, 0);
        }
        if (delay & USE_FLOATING) {
            sequential += common_delay(common, 2);
        }
        const u32 bus_shift = (delay & BUS_16_BIT) ? 1u : 0u;
        timing[device] = {access_time + 4, sequential, bus_shift};
    }
}

u32 MemControl::access_cycles(Device device, u32 bytes) const
{
    const Timing& device_timing = timing[static_cast<u32>(device)];
    const u32 bus_bytes = 1u << device_timing.bus_shift;
    const u32 accesses = (bytes + bus_bytes - 1) >> device_timing.bus_shift;
    return device_timing.first + device_timing.sequential * (accesses - 1);
}

u32 MemControl::read_register(u32 phys) const
{
    return registers[(phys - BASE) / 4];
}

void MemControl::write_register(u32 phys, u32 value)
{
    registers[(phys - BASE) / 4] = value;
    recalculate();
}

void MemControl::visit_state(State& state)
{
    state(registers);
    recalculate();
}
