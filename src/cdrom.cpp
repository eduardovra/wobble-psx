#include "cdrom.h"

#include <algorithm>

#include "savestate.h"

namespace {

// The commands the BIOS sends while it works out what, if anything, is
// in the drive. Everything else answers with "invalid command", which
// is what the controller itself does.
enum Command : u8 {
    GET_STAT = 0x01,
    SET_LOC = 0x02,
    READ_N = 0x06,
    STOP = 0x08,
    PAUSE = 0x09,
    INIT = 0x0A,
    MUTE = 0x0B,
    DEMUTE = 0x0C,
    SET_FILTER = 0x0D,
    SET_MODE = 0x0E,
    GET_PARAM = 0x0F,
    GET_LOC_L = 0x10,
    GET_LOC_P = 0x11,
    GET_TN = 0x13,
    GET_TD = 0x14,
    SEEK_L = 0x15,
    SEEK_P = 0x16,
    TEST = 0x19,
    GET_ID = 0x1A,
    READ_S = 0x1B,
    READ_TOC = 0x1E,
};

// The second byte of an INT5, saying why the command failed.
constexpr u8 ERROR_SEEK_FAILED = 0x04;
constexpr u8 ERROR_INVALID_COMMAND = 0x40;
constexpr u8 ERROR_NO_DISC = 0x40;

// Interrupt numbers. INT3 acknowledges a command, INT2 reports that
// what it started has finished, and INT5 is a refusal.
constexpr u8 INT_COMPLETE = 2;
constexpr u8 INT_ACKNOWLEDGE = 3;
constexpr u8 INT_ERROR = 5;

// Bits 0..4 of the interrupt flag register are the ones software can
// see and acknowledge; the rest read back as ones.
constexpr u8 FLAG_MASK = 0x1F;
constexpr u8 FLAG_UNUSED = 0xE0;

// Writing this bit to the flag register empties the parameter FIFO,
// which is how software recovers from a half-written command.
constexpr u8 FLAG_RESET_PARAMETERS = 0x40;

// Bits of the status register at 0x1F801800. The index sits in the low
// two, and the rest report what the two FIFOs and the controller are
// doing.
constexpr u8 STATUS_PARAMETERS_EMPTY = 1 << 3;
constexpr u8 STATUS_PARAMETERS_READY = 1 << 4;
constexpr u8 STATUS_RESPONSE_READY = 1 << 5;
constexpr u8 STATUS_BUSY = 1 << 7;

// How long the drive takes to answer, in CPU cycles. Nearly every
// command acknowledges after about the same interval — a millisecond
// and a half — because the wait is the controller's firmware getting
// round to it rather than anything mechanical. Init takes longer
// because it really does reinitialise the drive, and the second answer
// to a command that failed comes quickly, since failing is all it did.
constexpr u64 ACKNOWLEDGE_CYCLES = 0xC4E1;
constexpr u64 INIT_CYCLES = 0x13CCE;
constexpr u64 SECOND_ANSWER_CYCLES = 0x4A00;

// The version of the controller's own firmware, which Test(20h) reads
// back: 19 September 1994, revision C0, as fitted to an SCPH-1001.
constexpr u8 TEST_VERSION = 0x20;
constexpr std::array<u8, 4> CONTROLLER_VERSION = {0x94, 0x09, 0x19, 0xC0};

}  // namespace

void CdRom::reset()
{
    index = 0;
    status = STATUS_MOTOR;
    mode = 0;
    parameters = {};
    parameter_count = 0;
    response = {};
    response_length = 0;
    response_read = 0;
    interrupt_flag = 0;
    interrupt_enable = 0;
    busy = false;
    queue = {};
    queued = 0;
}

void CdRom::visit_state(State& state)
{
    state(index);
    state(status);
    state(mode);
    state(parameters);
    state(parameter_count);
    state(response);
    state(response_length);
    state(response_read);
    state(interrupt_flag);
    state(interrupt_enable);
    state(busy);
    state(queue);
    state(queued);
}

bool CdRom::line_active() const
{
    return (interrupt_flag & interrupt_enable & FLAG_MASK) != 0;
}

void CdRom::answer(u8 interrupt, std::initializer_list<u8> bytes, u64 delay)
{
    if (queued >= QUEUE_CAPACITY) {
        return;
    }
    Response& slot = queue[queued];
    slot = {};
    slot.interrupt = interrupt;
    slot.length = static_cast<u8>(bytes.size());
    std::copy(bytes.begin(), bytes.end(), slot.bytes.begin());
    slot.remaining = delay;
    queued++;
}

void CdRom::deliver()
{
    const Response& head = queue[0];
    response = head.bytes;
    response_length = head.length;
    response_read = 0;
    interrupt_flag = head.interrupt;
    busy = false;

    for (u32 i = 1; i < queued; i++) {
        queue[i - 1] = queue[i];
    }
    queued--;
}

bool CdRom::tick(u64 cycles)
{
    if (queued == 0) {
        return false;
    }
    Response& head = queue[0];
    if (head.remaining > cycles) {
        head.remaining -= cycles;
        return false;
    }
    head.remaining = 0;

    // An answer that is ready still waits its turn: the drive hands
    // over nothing while software has an unacknowledged interrupt.
    if (interrupt_flag != 0) {
        return false;
    }
    deliver();
    return line_active();
}

void CdRom::execute(u8 command)
{
    // Every command that succeeds says what the drive is doing, and
    // every one that fails says so in the same first byte with the
    // error bit added.
    const u8 failed = static_cast<u8>(status | STATUS_ERROR);

    switch (command) {
    case GET_STAT:
    case SET_LOC:
    case MUTE:
    case DEMUTE:
    case SET_FILTER:
        answer(INT_ACKNOWLEDGE, {status}, ACKNOWLEDGE_CYCLES);
        break;

    case SET_MODE:
        mode = parameter_count > 0 ? parameters[0] : 0;
        answer(INT_ACKNOWLEDGE, {status}, ACKNOWLEDGE_CYCLES);
        break;

    case GET_PARAM:
        answer(INT_ACKNOWLEDGE, {status, mode, 0, 0, 0}, ACKNOWLEDGE_CYCLES);
        break;

    case GET_LOC_P:
        // Where the drive is on the disc: track one, the very start.
        answer(INT_ACKNOWLEDGE, {1, 1, 0, 0, 0, 0, 0, 0}, ACKNOWLEDGE_CYCLES);
        break;

    case INIT:
        mode = 0;
        answer(INT_ACKNOWLEDGE, {status}, ACKNOWLEDGE_CYCLES);
        answer(INT_COMPLETE, {status}, INIT_CYCLES);
        break;

    case STOP:
    case PAUSE:
        answer(INT_ACKNOWLEDGE, {status}, ACKNOWLEDGE_CYCLES);
        answer(INT_COMPLETE, {status}, SECOND_ANSWER_CYCLES);
        break;

    case READ_N:
    case READ_S:
    case SEEK_L:
    case SEEK_P:
    case READ_TOC:
        // Heard, then refused: there is nothing under the head to seek
        // to or read from.
        answer(INT_ACKNOWLEDGE, {status}, ACKNOWLEDGE_CYCLES);
        answer(INT_ERROR, {failed, ERROR_SEEK_FAILED}, SECOND_ANSWER_CYCLES);
        break;

    case GET_LOC_L:
    case GET_TN:
    case GET_TD:
        answer(INT_ERROR, {failed, ERROR_SEEK_FAILED}, ACKNOWLEDGE_CYCLES);
        break;

    case TEST:
        if (parameter_count > 0 && parameters[0] == TEST_VERSION) {
            answer(INT_ACKNOWLEDGE,
                   {CONTROLLER_VERSION[0],
                    CONTROLLER_VERSION[1],
                    CONTROLLER_VERSION[2],
                    CONTROLLER_VERSION[3]},
                   ACKNOWLEDGE_CYCLES);
        } else {
            answer(
                INT_ERROR, {failed, ERROR_INVALID_COMMAND}, ACKNOWLEDGE_CYCLES);
        }
        break;

    case GET_ID:
        // The answer the whole boot turns on. An empty drive reports
        // the identification error in its status byte and "no disc" as
        // the reason, and the BIOS stops looking for a game.
        answer(INT_ACKNOWLEDGE, {status}, ACKNOWLEDGE_CYCLES);
        answer(INT_ERROR,
               {static_cast<u8>(status | STATUS_ID_ERROR),
                ERROR_NO_DISC,
                0,
                0,
                0,
                0,
                0,
                0},
               SECOND_ANSWER_CYCLES);
        break;

    default:
        answer(INT_ERROR, {failed, ERROR_INVALID_COMMAND}, ACKNOWLEDGE_CYCLES);
        break;
    }

    parameter_count = 0;
}

u8 CdRom::read_register(u32 phys)
{
    switch (phys - BASE) {
    case 0: {
        u8 value = index;
        if (parameter_count == 0) {
            value |= STATUS_PARAMETERS_EMPTY;
        }
        if (parameter_count < FIFO_CAPACITY) {
            value |= STATUS_PARAMETERS_READY;
        }
        if (response_read < response_length) {
            value |= STATUS_RESPONSE_READY;
        }
        if (busy) {
            value |= STATUS_BUSY;
        }
        return value;
    }

    case 1:
        // The response FIFO, whichever index is selected. Reading past
        // the end of an answer gives nothing, and the ready bit above
        // has already gone low to say so.
        if (response_read >= response_length) {
            return 0;
        }
        return response[response_read++];

    case 2:
        return 0;  // the data FIFO, which only a disc could fill

    case 3:
        // Bits the register does not have read back as ones, and the
        // enable and flag registers are each mirrored at two indices.
        if ((index & 1) == 0) {
            return static_cast<u8>(interrupt_enable | FLAG_UNUSED);
        }
        return static_cast<u8>(interrupt_flag | FLAG_UNUSED);

    default:
        return 0;
    }
}

void CdRom::write_register(u32 phys, u8 value)
{
    switch (phys - BASE) {
    case 0:
        index = value & 3;
        return;

    case 1:
        if (index == 0) {
            busy = true;
            execute(value);
        }
        // The other indices are the audio path into the SPU, which
        // there is nothing to hear from yet.
        return;

    case 2:
        if (index == 0 && parameter_count < FIFO_CAPACITY) {
            parameters[parameter_count++] = value;
        } else if (index == 1) {
            interrupt_enable = value & FLAG_MASK;
        }
        return;

    case 3:
        if (index == 1) {
            // Acknowledging is writing a one to the bit, which is the
            // opposite of I_STAT next door. Doing so releases whatever
            // answer the drive has been holding back.
            interrupt_flag &= static_cast<u8>(~(value & FLAG_MASK));
            if ((value & FLAG_RESET_PARAMETERS) != 0) {
                parameter_count = 0;
            }
        }
        // Index 0 is the request register, which asks for sector data,
        // and 2 and 3 are more of the audio path.
        return;

    default:
        return;
    }
}
