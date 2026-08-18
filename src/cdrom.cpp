#include "cdrom.h"

#include <algorithm>

#include "savestate.h"
#include "scheduler.h"

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
// what it started has finished, INT5 is a refusal, and INT1 is the one
// nothing asked for: a sector has arrived.
constexpr u8 INT_SECTOR_READY = 1;
constexpr u8 INT_COMPLETE = 2;
constexpr u8 INT_ACKNOWLEDGE = 3;
constexpr u8 INT_ERROR = 5;

// Bits of the mode register, set by Setmode.
constexpr u8 MODE_WHOLE_SECTOR = 1 << 5;
constexpr u8 MODE_DOUBLE_SPEED = 1 << 7;

// A sector from the header on, which is what the mode bit above asks
// for when it wants more than the payload.
constexpr u32 WHOLE_SECTOR_SIZE = 2340;

// The drive turns at 75 sectors a second, or twice that. Everything
// else about its timing is firmware getting round to things; this part
// is a motor, and it is the part games time themselves against.
constexpr u64 SECTORS_PER_SECOND = 75;
constexpr u64 SINGLE_SPEED_CYCLES = CPU_CLOCK_HZ / SECTORS_PER_SECOND;

// How long the head takes to get somewhere. Real seek time depends on
// how far it has to go; this is the average, which is what software
// that waits for the INT2 rather than timing it will see.
constexpr u64 SEEK_CYCLES = SINGLE_SPEED_CYCLES * 20;

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
constexpr u8 STATUS_DATA_READY = 1 << 6;
constexpr u8 STATUS_BUSY = 1 << 7;

// Writing this bit to the request register copies the sector the drive
// is holding into the data FIFO; clearing it throws the FIFO away.
constexpr u8 REQUEST_WANT_DATA = 1 << 7;

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

// What GetID says about a disc it is willing to boot. The type byte
// marks it as a licensed data disc, and the four characters name the
// region the BIOS will compare against its own — an SCPH-1001 is a
// North American console, so a disc it accepts says SCEA.
constexpr u8 DISC_TYPE_LICENSED = 0x20;
constexpr std::array<u8, 4> REGION = {'S', 'C', 'E', 'A'};

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
    seek_lba = 0;
    read_lba = 0;
    reading = false;
    sector_remaining = 0;
    sector = {};
    data_cursor = 0;
    data_end = 0;
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
    state(seek_lba);
    state(read_lba);
    state(reading);
    state(sector_remaining);
    state(sector);
    state(data_cursor);
    state(data_end);
}

u64 CdRom::cycles_per_sector() const
{
    if ((mode & MODE_DOUBLE_SPEED) != 0) {
        return SINGLE_SPEED_CYCLES / 2;
    }
    return SINGLE_SPEED_CYCLES;
}

u32 CdRom::data_start() const
{
    if ((mode & MODE_WHOLE_SECTOR) != 0) {
        return Disc::HEADER_OFFSET;
    }
    return Disc::MODE2_DATA_OFFSET;
}

u32 CdRom::data_size() const
{
    if ((mode & MODE_WHOLE_SECTOR) != 0) {
        return WHOLE_SECTOR_SIZE;
    }
    return Disc::COOKED_SECTOR_SIZE;
}

u8 CdRom::read_data()
{
    if (data_cursor >= data_end) {
        return 0;
    }
    return sector[data_cursor++];
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

void CdRom::advance_read(u64 cycles)
{
    if (!reading) {
        return;
    }
    if (sector_remaining > cycles) {
        sector_remaining -= cycles;
        return;
    }

    // The queue is two deep, and a read that has got ahead of software
    // must not overwrite the sector it has not collected yet. So the
    // drive waits with the sector under its head rather than dropping
    // it: a game that falls behind is served late, not served wrong.
    if (queued >= QUEUE_CAPACITY) {
        sector_remaining = 0;
        return;
    }

    if (!disc.read_sector(read_lba, sector)) {
        // Off the end of the disc. The drive stops rather than
        // spinning against nothing.
        reading = false;
        status = static_cast<u8>(status & ~STATUS_READING);
        answer(INT_ERROR,
               {static_cast<u8>(status | STATUS_ERROR), ERROR_SEEK_FAILED},
               0);
        return;
    }

    read_lba++;
    sector_remaining = cycles_per_sector();
    answer(INT_SECTOR_READY, {status}, 0);
}

bool CdRom::tick(u64 cycles)
{
    advance_read(cycles);

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
    case MUTE:
    case DEMUTE:
    case SET_FILTER:
        answer(INT_ACKNOWLEDGE, {status}, ACKNOWLEDGE_CYCLES);
        break;

    case SET_LOC:
        // Aims the head without moving it: the seek or read that comes
        // next is what acts on this. The address is in minutes,
        // seconds and frames, BCD, counted from the start of the
        // lead-in rather than of the data.
        if (parameter_count >= 3) {
            seek_lba =
                lba_from_msf({parameters[0], parameters[1], parameters[2]});
        }
        answer(INT_ACKNOWLEDGE, {status}, ACKNOWLEDGE_CYCLES);
        break;

    case SET_MODE:
        mode = parameter_count > 0 ? parameters[0] : 0;
        answer(INT_ACKNOWLEDGE, {status}, ACKNOWLEDGE_CYCLES);
        break;

    case GET_PARAM:
        answer(INT_ACKNOWLEDGE, {status, mode, 0, 0, 0}, ACKNOWLEDGE_CYCLES);
        break;

    case GET_LOC_P: {
        // Where the drive is on the disc. Reported from the sector
        // last read rather than from anything the mechanism knows,
        // which is also all a real drive has to go on.
        const Disc::Msf absolute = msf_from_lba(read_lba);
        const Disc::Track* track = disc.track_at(read_lba);
        const u32 track_number = track != nullptr ? track->number : 1;
        const u32 start = track != nullptr ? track->start_lba : 0;
        const Disc::Msf relative = msf_from_lba(read_lba - start);
        answer(INT_ACKNOWLEDGE,
               {to_bcd(static_cast<u8>(track_number)),
                1,
                relative.minute,
                relative.second,
                relative.frame,
                absolute.minute,
                absolute.second,
                absolute.frame},
               ACKNOWLEDGE_CYCLES);
        break;
    }

    case GET_LOC_L:
        // The header and subheader of the sector last read, straight
        // out of it — the twelve bytes of sync are skipped, and the
        // eight after them are what this answers with.
        if (!has_disc()) {
            answer(INT_ERROR, {failed, ERROR_SEEK_FAILED}, ACKNOWLEDGE_CYCLES);
            break;
        }
        answer(INT_ACKNOWLEDGE,
               {sector[Disc::HEADER_OFFSET],
                sector[Disc::HEADER_OFFSET + 1],
                sector[Disc::HEADER_OFFSET + 2],
                sector[Disc::HEADER_OFFSET + 3],
                sector[Disc::SUBHEADER_OFFSET],
                sector[Disc::SUBHEADER_OFFSET + 1],
                sector[Disc::SUBHEADER_OFFSET + 2],
                sector[Disc::SUBHEADER_OFFSET + 3]},
               ACKNOWLEDGE_CYCLES);
        break;

    case INIT:
        mode = 0;
        reading = false;
        status = static_cast<u8>((status & ~STATUS_READING) | STATUS_MOTOR);
        answer(INT_ACKNOWLEDGE, {status}, ACKNOWLEDGE_CYCLES);
        answer(INT_COMPLETE, {status}, INIT_CYCLES);
        break;

    case STOP:
    case PAUSE:
        // Both stop a read; Stop also parks the mechanism. The first
        // answer reports what the drive was doing, the second what it
        // is doing now, which is why the status byte is read twice.
        answer(INT_ACKNOWLEDGE, {status}, ACKNOWLEDGE_CYCLES);
        reading = false;
        status = static_cast<u8>(status & ~STATUS_READING);
        if (command == STOP) {
            status = static_cast<u8>(status & ~STATUS_MOTOR);
        }
        answer(INT_COMPLETE, {status}, SECOND_ANSWER_CYCLES);
        break;

    case READ_N:
    case READ_S:
        if (!has_disc()) {
            answer(INT_ACKNOWLEDGE, {status}, ACKNOWLEDGE_CYCLES);
            answer(
                INT_ERROR, {failed, ERROR_SEEK_FAILED}, SECOND_ANSWER_CYCLES);
            break;
        }
        // A read acknowledges once and then keeps answering: the
        // sectors arrive as INT1s from the tick, not from here.
        read_lba = seek_lba;
        reading = true;
        sector_remaining = SEEK_CYCLES + cycles_per_sector();
        status = static_cast<u8>(status | STATUS_READING);
        answer(INT_ACKNOWLEDGE, {status}, ACKNOWLEDGE_CYCLES);
        break;

    case SEEK_L:
    case SEEK_P:
        if (!has_disc() || seek_lba >= disc.sector_count()) {
            answer(INT_ACKNOWLEDGE, {status}, ACKNOWLEDGE_CYCLES);
            answer(
                INT_ERROR, {failed, ERROR_SEEK_FAILED}, SECOND_ANSWER_CYCLES);
            break;
        }
        reading = false;
        read_lba = seek_lba;
        status = static_cast<u8>(status & ~STATUS_READING);
        answer(INT_ACKNOWLEDGE, {status}, ACKNOWLEDGE_CYCLES);
        answer(INT_COMPLETE, {status}, SEEK_CYCLES);
        break;

    case READ_TOC:
        if (!has_disc()) {
            answer(INT_ACKNOWLEDGE, {status}, ACKNOWLEDGE_CYCLES);
            answer(
                INT_ERROR, {failed, ERROR_SEEK_FAILED}, SECOND_ANSWER_CYCLES);
            break;
        }
        // The table of contents is already known from the cue; the
        // command exists to make the drive go and read it, which takes
        // time and changes nothing here.
        answer(INT_ACKNOWLEDGE, {status}, ACKNOWLEDGE_CYCLES);
        answer(INT_COMPLETE, {status}, INIT_CYCLES);
        break;

    case GET_TN:
        if (!has_disc()) {
            answer(INT_ERROR, {failed, ERROR_SEEK_FAILED}, ACKNOWLEDGE_CYCLES);
            break;
        }
        answer(INT_ACKNOWLEDGE,
               {status,
                to_bcd(static_cast<u8>(disc.tracks.front().number)),
                to_bcd(static_cast<u8>(disc.tracks.back().number))},
               ACKNOWLEDGE_CYCLES);
        break;

    case GET_TD: {
        if (!has_disc()) {
            answer(INT_ERROR, {failed, ERROR_SEEK_FAILED}, ACKNOWLEDGE_CYCLES);
            break;
        }
        // Track zero is not a track but the lead-out, which is how
        // software asks how long the disc is.
        const u8 wanted = parameter_count > 0 ? from_bcd(parameters[0]) : 0;
        u32 start = disc.sector_count();
        for (const Disc::Track& track : disc.tracks) {
            if (track.number == wanted) {
                start = track.start_lba;
            }
        }
        const Disc::Msf msf = msf_from_lba(start);
        answer(INT_ACKNOWLEDGE,
               {status, msf.minute, msf.second},
               ACKNOWLEDGE_CYCLES);
        break;
    }

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
        // the reason, and the BIOS stops looking for a game; a disc
        // answers with its type and the region it was pressed for,
        // which the BIOS compares against its own before running
        // anything off it.
        answer(INT_ACKNOWLEDGE, {status}, ACKNOWLEDGE_CYCLES);
        if (!has_disc()) {
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
        }
        answer(INT_COMPLETE,
               {status,
                0,
                DISC_TYPE_LICENSED,
                0,
                REGION[0],
                REGION[1],
                REGION[2],
                REGION[3]},
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
        if (data_cursor < data_end) {
            value |= STATUS_DATA_READY;
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
        return read_data();

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
        if (index == 0) {
            // The request register. Asking for data presents the
            // sector the drive is holding; withdrawing the request
            // throws away whatever of it was left. Software that reads
            // a sector twice without asking again gets nothing, which
            // is what makes the request the thing that paces a read.
            //
            // Asking again while bytes are still in hand does nothing
            // at all — the sector is not presented afresh. Games rely
            // on that: reading a sector's header and its data as two
            // separate transfers, each preceded by a request, only
            // works because the second one leaves the first where it
            // finished.
            if ((value & REQUEST_WANT_DATA) != 0) {
                if (data_cursor >= data_end) {
                    data_cursor = data_start();
                    data_end = data_start() + data_size();
                }
            } else {
                data_cursor = 0;
                data_end = 0;
            }
        } else if (index == 1) {
            // Acknowledging is writing a one to the bit, which is the
            // opposite of I_STAT next door. Doing so releases whatever
            // answer the drive has been holding back.
            interrupt_flag &= static_cast<u8>(~(value & FLAG_MASK));
            if ((value & FLAG_RESET_PARAMETERS) != 0) {
                parameter_count = 0;
            }
        }
        // Indices 2 and 3 are more of the audio path.
        return;

    default:
        return;
    }
}
