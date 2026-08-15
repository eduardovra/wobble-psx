#include "irq.h"

void Irq::reset()
{
    status = 0;
    mask = 0;
}

void Irq::raise(Interrupt line)
{
    status |= static_cast<u16>(1u << static_cast<u32>(line));
}

void Irq::acknowledge(u16 value) { status &= value; }
