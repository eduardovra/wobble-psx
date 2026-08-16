#include <memory>
#include <vector>

#include <doctest/doctest.h>

#include "bus.h"
#include "console.h"
#include "irq.h"
#include "sio.h"

namespace {

// The port driven through the bus of a whole console, because the
// acknowledge that follows a byte is a scheduled event: nothing here
// happens without time passing.
struct Port {
    std::unique_ptr<Console> console = std::make_unique<Console>();
    Bus* bus = &console->bus;

    // Long enough for the acknowledge, and short enough to be sure
    // nothing else brought it about.
    void wait() const
    {
        console->scheduler.advance(bus->sio.acknowledge_delay());
        console->dispatch_due_events();
    }

    void select() const
    {
        // Transmit and receive on, the first socket selected, and the
        // acknowledge interrupt wanted — what a driver sets before it
        // says anything.
        bus->write16(0x1F80104A, 0x1005);
    }

    void deselect() const { bus->write16(0x1F80104A, 0x0000); }

    // Sends one byte and returns what the device sent back.
    u8 send(u8 value) const
    {
        bus->write8(0x1F801040, value);
        return bus->read8(0x1F801040);
    }

    u32 status() const { return bus->read32(0x1F801044); }

    bool raised() const
    {
        const u32 line = 1u << static_cast<u32>(Interrupt::Controller);
        return (bus->irq.status & line) != 0;
    }

    // The whole conversation a driver has with a digital pad.
    std::vector<u8> read_pad() const
    {
        select();
        std::vector<u8> answer;
        answer.push_back(send(0x01));
        answer.push_back(send(0x42));
        answer.push_back(send(0x00));
        answer.push_back(send(0x00));
        answer.push_back(send(0x00));
        deselect();
        return answer;
    }
};

}  // namespace

TEST_CASE("the transmitter is always ready to be given a byte")
{
    const Port port;
    CHECK((port.status() & 1) != 0);
}

TEST_CASE("a digital controller answers with its identifier and buttons")
{
    const Port port;
    const std::vector<u8> answer = port.read_pad();

    CHECK(answer[0] == 0xFF);  // nothing drives the bus for the address
    CHECK(answer[1] == 0x41);  // digital pad, low half of the id
    CHECK(answer[2] == 0x5A);  // and the high half
    CHECK(answer[3] == 0xFF);  // no button pressed is all ones
    CHECK(answer[4] == 0xFF);
}

TEST_CASE("a pressed button shows up in the bytes the pad sends back")
{
    const Port port;
    port.bus->sio.press(Sio::Button::Start);
    port.bus->sio.press(Sio::Button::Cross);

    const std::vector<u8> answer = port.read_pad();
    CHECK(answer[3] == 0xF7);  // Start is bit 3 of the low byte
    CHECK(answer[4] == 0xBF);  // Cross is bit 6 of the high byte

    port.bus->sio.release(Sio::Button::Start);
    port.bus->sio.release(Sio::Button::Cross);
    CHECK(port.read_pad()[3] == 0xFF);
}

TEST_CASE("the acknowledge comes after the byte, not with it")
{
    const Port port;
    port.select();
    CHECK_FALSE(port.raised());

    // The driver clears the last interrupt between sending a byte and
    // waiting for this one, so an acknowledge inside the store that
    // sent it would be thrown away before anything looked.
    port.send(0x01);
    CHECK_FALSE(port.raised());

    port.wait();
    CHECK(port.raised());
    CHECK((port.status() & (1 << 9)) != 0);
}

TEST_CASE("the last byte of an exchange is not acknowledged")
{
    const Port port;
    port.select();
    port.send(0x01);
    port.send(0x42);
    port.send(0x00);
    port.send(0x00);
    port.wait();

    // Clear the flag, then send the byte that ends the conversation.
    port.bus->write16(0x1F80104A, 0x1015);
    CHECK((port.status() & (1 << 9)) == 0);

    port.send(0x00);
    port.wait();
    CHECK((port.status() & (1 << 9)) == 0);
}

TEST_CASE("an empty socket never answers")
{
    const Port port;
    // The same conversation, addressed to the second socket.
    port.bus->write16(0x1F80104A, 0x3005);

    CHECK(port.send(0x01) == 0xFF);
    port.wait();
    CHECK_FALSE(port.raised());
    CHECK(port.send(0x42) == 0xFF);
    port.wait();
    CHECK_FALSE(port.raised());
}

TEST_CASE("a memory card slot is empty too")
{
    const Port port;
    port.select();

    CHECK(port.send(0x81) == 0xFF);
    port.wait();
    CHECK_FALSE(port.raised());
}

TEST_CASE("deselecting abandons a half-finished exchange")
{
    const Port port;
    port.select();
    port.send(0x01);
    port.send(0x42);
    port.deselect();

    // Starting again gets the identifier again rather than continuing
    // where the last conversation left off.
    port.select();
    port.send(0x01);
    CHECK(port.send(0x42) == 0x41);
}

TEST_CASE("the receive flag clears when the byte is taken")
{
    const Port port;
    port.select();
    port.bus->write8(0x1F801040, 0x01);

    CHECK((port.status() & (1 << 1)) != 0);
    port.bus->read8(0x1F801040);
    CHECK((port.status() & (1 << 1)) == 0);
}
