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
constexpr u8 ERROR_SHELL_OPEN = 0x08;
constexpr u8 ERROR_INVALID_COMMAND = 0x40;
constexpr u8 ERROR_NO_DISC = 0x40;
constexpr u8 ERROR_NOT_READY = 0x80;

// What the drive says when the lid goes: an INT5 nothing asked for,
// carrying an error bit and the reason for it. The status byte in it
// is not the one the drive goes on to report — the console sends 01h
// while its Getstat still says the disc is turning and the lid is off
// it. The answer describes the drive stopping rather than the drive
// as it stands.
constexpr u8 SHELL_OPEN_ANSWER = 0x01;

// How long the disc takes to stop turning once the lid is opened.
// About a second: long enough that software polling the status sees
// the motor bit go down a moment after the lid bit goes up.
constexpr u64 SPIN_DOWN_CYCLES = CPU_CLOCK_HZ;

// What a sector says about itself when there is no sector: the mode a
// data disc is written in, and the subheader bit that marks the
// payload as data rather than as sound.
constexpr u8 MODE_2 = 2;
constexpr u8 SUBMODE_DATA = 0x08;

// What Getloc calls the run-out past the last track.
constexpr u8 LEAD_OUT_TRACK = 0xAA;

// Interrupt numbers. INT3 acknowledges a command, INT2 reports that
// what it started has finished, INT5 is a refusal, and INT1 is the one
// nothing asked for: a sector has arrived.
constexpr u8 INT_SECTOR_READY = 1;
constexpr u8 INT_COMPLETE = 2;
constexpr u8 INT_ACKNOWLEDGE = 3;
constexpr u8 INT_ERROR = 5;

// Bits of the mode register, set by Setmode.
constexpr u8 MODE_XA_FILTER = 1 << 3;
constexpr u8 MODE_WHOLE_SECTOR = 1 << 5;
constexpr u8 MODE_XA_ADPCM = 1 << 6;
constexpr u8 MODE_DOUBLE_SPEED = 1 << 7;

// The four volume registers, in the order they are held in: how much
// of each channel of the disc reaches each channel of the output.
constexpr u32 VOLUME_LEFT_TO_LEFT = 0;
constexpr u32 VOLUME_LEFT_TO_RIGHT = 1;
constexpr u32 VOLUME_RIGHT_TO_RIGHT = 2;
constexpr u32 VOLUME_RIGHT_TO_LEFT = 3;

// The register beside them: whether the compressed sound is muted,
// and the bit that says the four volumes have all been written.
constexpr u8 ADPCM_MUTE = 1 << 0;
constexpr u8 ADPCM_APPLY_VOLUME = 1 << 5;

// Bits of a sector's submode, the third byte of its subheader. A
// movie's sound is all three at once: compressed audio, written in the
// larger of the two sector forms because it needs the room and can
// afford to lose a byte, and marked as arriving in real time.
constexpr u8 SUBMODE_AUDIO = 1 << 2;
constexpr u8 SUBMODE_FORM_2 = 1 << 5;
constexpr u8 SUBMODE_REALTIME = 1 << 6;

// XA-ADPCM, which is what a movie's sound is compressed with. A Form 2
// sector carries eighteen 128-byte sound groups; each is sixteen bytes
// of filter and shift followed by twenty-eight four-byte words, and
// each word carries one sample for every block the group holds — eight
// of them at four bits, four at eight bits. So a block's samples are
// not consecutive in the sector: they are one nibble of each word in
// turn.
constexpr u32 SOUND_GROUPS = 18;
constexpr u32 SOUND_GROUP_SIZE = 128;
constexpr u32 GROUP_HEADER_SIZE = 16;
constexpr u32 SAMPLES_PER_BLOCK = 28;
constexpr u32 WORD_SIZE = 4;

// The fourth byte of the subheader says how the sound was written.
constexpr u8 CODING_STEREO = 1 << 0;
constexpr u8 CODING_HALF_RATE = 1 << 2;
constexpr u8 CODING_EIGHT_BIT = 1 << 4;

// A block's header byte: how far to shift the samples down, and which
// of the four filters to predict with. There are four here and five in
// the SPU, which is the one thing about this compression that is not
// the voices'.
constexpr u8 BLOCK_SHIFT_MASK = 0x0F;
constexpr u32 BLOCK_FILTER_COUNT = 4;

// Shifts above twelve are reserved, and the hardware reads them all as
// nine rather than as themselves.
constexpr u32 MAX_SHIFT = 12;
constexpr u32 RESERVED_SHIFT = 9;

// The filter, in sixty-fourths of the two samples before.
constexpr std::array<s32, BLOCK_FILTER_COUNT> FILTER_OLD = {0, 60, 115, 98};
constexpr std::array<s32, BLOCK_FILTER_COUNT> FILTER_OLDER = {0, 0, -52, -55};

// The zigzag filter the drive resamples with, one column per output
// sample of the seven it makes from every six, twenty-nine taps back
// through what it has decoded. It is written the way the hardware's
// tables are — a row per tap — so that it can be read against them.
constexpr u32 ZIGZAG_TAPS = 29;
constexpr s32 ZIGZAG_UNIT = 0x8000;
constexpr std::array<std::array<s32, CdRom::RESAMPLE_STEPS>, ZIGZAG_TAPS>
    ZIGZAG = {{
        {0, 0, 0, 0, -0x0001, 0x0002, -0x0005},
        {0, 0, 0, -0x0001, 0x0003, -0x0008, 0x0011},
        {0, 0, -0x0001, 0x0003, -0x0008, 0x0010, -0x0023},
        {0, -0x0002, 0x0003, -0x0008, 0x0011, -0x0023, 0x0046},
        {0, 0, -0x0002, 0x0006, -0x0010, 0x002B, -0x0017},
        {-0x0002, 0x0003, -0x0005, 0x0005, 0x000A, 0x001A, -0x0044},
        {0x000A, -0x0013, 0x001F, -0x001B, 0x006B, -0x00EB, 0x015B},
        {-0x0022, 0x003C, -0x004A, 0x00A6, -0x016D, 0x027B, -0x0347},
        {0x0041, -0x004B, 0x00B3, -0x01A8, 0x0350, -0x0548, 0x080E},
        {-0x0054, 0x00A2, -0x0192, 0x0372, -0x0623, 0x0AFA, -0x1249},
        {0x0034, -0x00E3, 0x02B1, -0x05BF, 0x0BCD, -0x16FA, 0x3C07},
        {0x0009, 0x0132, -0x039E, 0x09B8, -0x1780, 0x53E0, 0x53E0},
        {-0x010A, -0x0043, 0x04F8, -0x11B4, 0x6794, 0x3C07, -0x16FA},
        {0x0400, -0x0267, -0x05A6, 0x74BB, 0x234C, -0x1249, 0x0AFA},
        {-0x0A78, 0x0C9D, 0x7939, 0x0C9D, -0x0A78, 0x080E, -0x0548},
        {0x234C, 0x74BB, -0x05A6, -0x0267, 0x0400, -0x0347, 0x027B},
        {0x6794, -0x11B4, 0x04F8, -0x0043, -0x010A, 0x015B, -0x00EB},
        {-0x1780, 0x09B8, -0x039E, 0x0132, 0x0009, -0x0044, 0x001A},
        {0x0BCD, -0x05BF, 0x02B1, -0x00E3, 0x0034, -0x0017, 0x002B},
        {-0x0623, 0x0372, -0x0192, 0x00A2, -0x0054, 0x0046, -0x0023},
        {0x0350, -0x01A8, 0x00B3, -0x004B, 0x0041, -0x0023, 0x0010},
        {-0x016D, 0x00A6, -0x004A, 0x003C, -0x0022, 0x0011, -0x0008},
        {0x006B, -0x001B, 0x001F, -0x0013, 0x000A, -0x0005, 0x0002},
        {0x000A, 0x0005, -0x0005, 0x0003, -0x0001, 0, 0},
        {-0x0010, 0x0006, -0x0002, 0, 0, 0, 0},
        {0x0011, -0x0008, 0x0003, -0x0002, 0x0001, 0, 0},
        {-0x0008, 0x0003, -0x0001, 0, 0, 0, 0},
        {0x0003, -0x0001, 0, 0, 0, 0, 0},
        {-0x0001, 0, 0, 0, 0, 0, 0},
    }};

// The drive's own volume registers are 128ths, and go up to twice
// unity — which is why they are applied wider than they are written
// and then clamped.
constexpr u32 VOLUME_SHIFT = 7;

s16 clamp_sample(s32 value)
{
    return static_cast<s16>(std::clamp(value, -0x8000, 0x7FFF));
}

// Twenty-eight samples of one channel, out of one block of a sound
// group. The two samples before them come in and the last two go back
// out, since the next block of the same channel continues from here.
void decode_block(const u8* group,
                  u32 block,
                  bool eight_bit,
                  s32& old,
                  s32& older,
                  std::array<s16, SAMPLES_PER_BLOCK>& out)
{
    // The sixteen header bytes are eight bytes written twice, so that
    // a drive which read one of them badly has the other to fall back
    // on. The eight that are read live at 4 to 11; the ones on either
    // side of them are the copies.
    const u8 header = group[4 + block];

    u32 shift = header & BLOCK_SHIFT_MASK;
    if (shift > MAX_SHIFT) {
        shift = RESERVED_SHIFT;
    }
    const u32 filter = (header >> 4) & (BLOCK_FILTER_COUNT - 1);

    for (u32 i = 0; i < SAMPLES_PER_BLOCK; i++) {
        // A sample is widened to sixteen bits and then shifted back
        // down by however much the block was scaled by, which is what
        // makes four bits carry a signal with any range at all.
        u16 widened = 0;
        if (eight_bit) {
            widened = static_cast<u16>(
                group[GROUP_HEADER_SIZE + block + i * WORD_SIZE] << 8);
        } else {
            const u8 packed =
                group[GROUP_HEADER_SIZE + (block / 2) + i * WORD_SIZE];
            const u8 nibble = (packed >> ((block & 1) * 4)) & 0x0F;
            widened = static_cast<u16>(nibble << 12);
        }
        const s32 scaled = static_cast<s16>(widened) >> shift;

        const s32 predicted =
            (old * FILTER_OLD[filter] + older * FILTER_OLDER[filter] + 32) / 64;
        const s32 sample = std::clamp(scaled + predicted, -0x8000, 0x7FFF);

        out[i] = static_cast<s16>(sample);
        older = old;
        old = sample;
    }
}

// One output sample: the last twenty-nine decoded ones, weighted by
// the column of the table this step of the six is up to.
s16 zigzag(const std::array<s16, CdRom::RESAMPLE_RING>& ring,
           u32 position,
           u32 step)
{
    s32 sum = 0;
    for (u32 tap = 1; tap <= ZIGZAG_TAPS; tap++) {
        const s16 sample = ring[(position - tap) % CdRom::RESAMPLE_RING];
        sum += sample * ZIGZAG[tap - 1][step] / ZIGZAG_UNIT;
    }
    return clamp_sample(sum);
}

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

// How far it can go. A disc is seventy-four minutes of medium whatever
// was written on it, and the drive will position itself anywhere on
// that; only past the edge of it is there nothing to reach. So the
// limit is the disc rather than the data — cdrom/getloc seeks to
// 74:00:00 on an image with a fraction of that on it and expects to
// arrive.
constexpr u32 SEEK_LIMIT_LBA =
    74 * 60 * static_cast<u32>(SECTORS_PER_SECOND) - Disc::LEAD_IN_SECTORS;

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

// Bringing a read to a stop is the exception, and a mechanical one:
// about thirty milliseconds, whichever speed the drive was turning at.
// cdrom/timing measures it on the console at between 1006472 and
// 1046336 cycles across ten runs.
constexpr u64 PAUSE_CYCLES = 0xFA000;

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
    // The lid is not something a power cycle closes, so a machine
    // reset while it stands open still reports it open.
    status = shell_open ? STATUS_SHELL_OPEN : STATUS_MOTOR;
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
    filter_file = 0;
    filter_channel = 0;
    seek_lba = 0;
    read_lba = 0;
    reading = false;
    seeking = false;
    header_valid = false;
    shell_report_pending = false;
    spin_down_remaining = 0;
    sector_remaining = 0;
    sector = {};
    data_cursor = 0;
    data_end = 0;
    muted = false;
    xa_muted = false;
    volume_written = {VOLUME_UNITY, 0, VOLUME_UNITY, 0};
    volume = volume_written;
    stop_audio();
}

void CdRom::visit_state(State& state)
{
    state(index);
    state(status);
    state(mode);
    state(filter_file);
    state(filter_channel);
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
    state(seeking);
    state(header_valid);
    // The lid itself is left out for the same reason the disc is: it
    // is the drive someone is sitting at, not state the machine holds.
    // What the controller made of it — the bit in the status byte, the
    // answer it still owes, the motor winding down — is state.
    state(shell_report_pending);
    state(spin_down_remaining);
    state(sector_remaining);
    state(sector);
    state(data_cursor);
    state(data_end);
    state(muted);
    state(xa_muted);
    state(volume_written);
    state(volume);
    state(adpcm_old);
    state(adpcm_older);
    state(resample_left);
    state(resample_right);
    state(resample_position);
    state(six_step);
    state(audio);
    state(decoded);
    state(played);
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

    // Read somewhere of its own first. A sector meant for the sound
    // hardware never reaches the buffer software takes its data from,
    // and one arriving mid-transfer must not disturb what is being
    // taken out of it.
    std::array<u8, Disc::RAW_SECTOR_SIZE> incoming{};
    if (!disc.read_sector(read_lba, incoming)) {
        // Off the end of the disc. The drive stops rather than
        // spinning against nothing, and forgets where it was: there
        // was no header where it ended up to tell it.
        lose_position();
        answer(INT_ERROR, {status, ERROR_SEEK_FAILED}, 0);
        return;
    }

    // The first sector is also the end of the seek the read began
    // with, which is what the status byte has been reporting until
    // now.
    seeking = false;
    header_valid = true;
    status = static_cast<u8>((status & ~STATUS_SEEKING) | STATUS_READING);

    read_lba++;
    sector_remaining = cycles_per_sector();

    // A movie's sound is written between its pictures, in the same run
    // of sectors, and goes straight to the sound hardware: no
    // interrupt, and nothing for software to collect. A game reading
    // the picture would be handed the sound as though it were more
    // picture otherwise, which is what makes a movie fail to decode
    // rather than merely fail to play.
    if (is_audio_sector(incoming)) {
        if (matches_filter(incoming)) {
            decode_audio(incoming);
        }
        return;
    }

    sector = incoming;
    answer(INT_SECTOR_READY, {status}, 0);
}

bool CdRom::is_audio_sector(
    const std::array<u8, Disc::RAW_SECTOR_SIZE>& raw) const
{
    if ((mode & MODE_XA_ADPCM) == 0) {
        // Without the mode bit there is no sound path at all, and
        // every sector is data whatever it says about itself.
        return false;
    }

    // Sound on a stream the filter does not name is still the sound
    // hardware's rather than software's: it is dropped there instead
    // of being played, which is what `matches_filter` decides.
    const u8 submode = raw[Disc::SUBHEADER_OFFSET + 2];
    const u8 wanted = SUBMODE_AUDIO | SUBMODE_REALTIME | SUBMODE_FORM_2;
    return (submode & wanted) == wanted;
}

bool CdRom::matches_filter(
    const std::array<u8, Disc::RAW_SECTOR_SIZE>& raw) const
{
    if ((mode & MODE_XA_FILTER) == 0) {
        return true;
    }
    const u8 file = raw[Disc::SUBHEADER_OFFSET];
    const u8 channel = raw[Disc::SUBHEADER_OFFSET + 1];
    return file == filter_file && channel == filter_channel;
}

void CdRom::decode_audio(const std::array<u8, Disc::RAW_SECTOR_SIZE>& raw)
{
    const u8 coding = raw[Disc::SUBHEADER_OFFSET + 3];
    const bool stereo = (coding & CODING_STEREO) != 0;
    const bool eight_bit = (coding & CODING_EIGHT_BIT) != 0;
    const bool half_rate = (coding & CODING_HALF_RATE) != 0;

    // Eight blocks to a group at four bits, four at eight, and in
    // stereo they alternate between the channels: a pair of blocks is
    // one stretch of time, not two.
    const u32 blocks = eight_bit ? 4 : 8;
    const u32 step = stereo ? 2 : 1;

    std::array<s16, SAMPLES_PER_BLOCK> left{};
    std::array<s16, SAMPLES_PER_BLOCK> right{};

    for (u32 group_index = 0; group_index < SOUND_GROUPS; group_index++) {
        const u8* group =
            &raw[Disc::MODE2_DATA_OFFSET + group_index * SOUND_GROUP_SIZE];

        for (u32 block = 0; block < blocks; block += step) {
            decode_block(
                group, block, eight_bit, adpcm_old[0], adpcm_older[0], left);
            if (stereo) {
                decode_block(group,
                             block + 1,
                             eight_bit,
                             adpcm_old[1],
                             adpcm_older[1],
                             right);
            }

            for (u32 i = 0; i < SAMPLES_PER_BLOCK; i++) {
                // Mono is played down both channels rather than down
                // one: a movie recorded in mono is not a movie that
                // comes out of the left speaker.
                const s16 other = stereo ? right[i] : left[i];
                resample(left[i], other);
                if (half_rate) {
                    resample(left[i], other);
                }
            }
        }
    }
}

void CdRom::resample(s16 left, s16 right)
{
    resample_left[resample_position % RESAMPLE_RING] = left;
    resample_right[resample_position % RESAMPLE_RING] = right;
    resample_position++;

    // Six samples in, seven out: the drive holds its decoded output in
    // a ring and interpolates across it whenever six more have gone
    // in, which is what turns the disc's rate into the mixer's.
    six_step--;
    if (six_step != 0) {
        return;
    }
    six_step = SIX_STEP;

    for (u32 step = 0; step < RESAMPLE_STEPS; step++) {
        queue_audio(zigzag(resample_left, resample_position, step),
                    zigzag(resample_right, resample_position, step));
    }
}

void CdRom::queue_audio(s16 left, s16 right)
{
    // A drive further ahead of the mixer than the queue is deep is one
    // being fed sound faster than it can be heard, which no stream
    // written to be played at the speed it is read does. Dropping what
    // will not fit keeps what is already queued playing in order
    // rather than skipping backwards through it.
    if (decoded - played >= AUDIO_CAPACITY) {
        return;
    }
    audio[decoded % AUDIO_CAPACITY] = {left, right};
    decoded++;
}

CdRom::Audio CdRom::take_audio()
{
    if (played >= decoded) {
        return {};
    }

    const Audio frame = audio[played % AUDIO_CAPACITY];
    played++;

    // Muting silences the output rather than stopping the decoder, so
    // a movie muted halfway through comes back where it has got to and
    // not where it left off.
    if (muted || xa_muted) {
        return {};
    }

    // The drive's own mixer: each channel of the disc reaches each
    // channel of the output by however much software asked for, which
    // is how a game plays a stereo movie in mono, or fades one out
    // without touching the SPU.
    const s32 left =
        (frame.left * volume[0] + frame.right * volume[3]) >> VOLUME_SHIFT;
    const s32 right =
        (frame.right * volume[2] + frame.left * volume[1]) >> VOLUME_SHIFT;
    return {clamp_sample(left), clamp_sample(right)};
}

u32 CdRom::audio_ready() const { return static_cast<u32>(decoded - played); }

void CdRom::stop_audio()
{
    decoded = 0;
    played = 0;
    adpcm_old = {};
    adpcm_older = {};
    resample_left = {};
    resample_right = {};
    resample_position = 0;
    six_step = SIX_STEP;
}

void CdRom::load_header()
{
    // A seek ends with the head over a sector, and reading that
    // sector's header is how the drive knows it arrived — which is
    // also what leaves Getloc with something to answer.
    //
    // Past the end of what was written the disc is still there and
    // still turning, with no header anywhere on it. The console's
    // drive answers from where its own servo says it is rather than
    // refusing, so that is what stands in here: the address, and the
    // mode a data disc is written in.
    if (!disc.read_sector(read_lba, sector)) {
        const Disc::Msf msf = msf_from_lba(read_lba);
        sector = {};
        sector[Disc::HEADER_OFFSET] = msf.minute;
        sector[Disc::HEADER_OFFSET + 1] = msf.second;
        sector[Disc::HEADER_OFFSET + 2] = msf.frame;
        sector[Disc::HEADER_OFFSET + 3] = MODE_2;
        sector[Disc::SUBHEADER_OFFSET + 2] = SUBMODE_DATA;
        sector[Disc::SUBHEADER_OFFSET + 6] = SUBMODE_DATA;
    }
    header_valid = true;
}

void CdRom::set_status(u8 bits)
{
    // Everything a command has to say about the drive is said by
    // replacing the status byte, and none of it has any bearing on the
    // lid: that bit is where the drive is rather than what it is
    // doing, and only Getstat takes it away.
    status = static_cast<u8>((status & STATUS_SHELL_OPEN) | bits);
}

void CdRom::open_shell()
{
    if (shell_open) {
        return;
    }
    shell_open = true;

    // Whatever the drive was doing it is not doing now, and it no
    // longer knows where its head is: the disc that comes back may not
    // be the one that went away.
    reading = false;
    seeking = false;
    header_valid = false;
    data_cursor = 0;
    data_end = 0;
    stop_audio();

    // The disc is still turning for a moment yet, and the status byte
    // says so until it has wound down.
    status = STATUS_MOTOR | STATUS_SHELL_OPEN;
    spin_down_remaining = SPIN_DOWN_CYCLES;
    shell_report_pending = true;
}

void CdRom::close_shell()
{
    // Closing the lid only makes the drive usable again. Nothing spins
    // up until something asks it to, and the bit stays up until a
    // Getstat reads it: a game that was not watching when the lid
    // moved still finds out that it did.
    shell_open = false;
    spin_down_remaining = 0;
    status = STATUS_SHELL_OPEN;
}

void CdRom::lose_position()
{
    // What a drive that cannot find a header does: it gives up on the
    // read, stops the motor, and has nothing left to answer Getloc
    // with. The seek-error bit is how it says so, and it stands in
    // place of the general error bit rather than beside it.
    reading = false;
    seeking = false;
    header_valid = false;
    stop_audio();
    set_status(STATUS_SEEK_ERROR);
}

bool CdRom::tick(u64 cycles)
{
    if (spin_down_remaining > 0) {
        if (spin_down_remaining <= cycles) {
            spin_down_remaining = 0;
            status = static_cast<u8>(status & ~STATUS_MOTOR);
        } else {
            spin_down_remaining -= cycles;
        }
    }

    // The lid's own answer waits for room in the queue rather than
    // being dropped, the same as a sector does. Software that was
    // between commands when the lid moved must still be told.
    if (shell_report_pending && queued < QUEUE_CAPACITY) {
        shell_report_pending = false;
        answer(INT_ERROR, {SHELL_OPEN_ANSWER, ERROR_SHELL_OPEN}, 0);
    }

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
        // The one command that puts the lid's bit down, which is what
        // makes "is or was open" worth reporting: a game learns that
        // the disc may have been changed under it, and says it has
        // noticed by asking. A lid still open cannot be acknowledged
        // away, so the bit only goes once it is shut.
        answer(INT_ACKNOWLEDGE, {status}, ACKNOWLEDGE_CYCLES);
        if (!shell_open) {
            status = static_cast<u8>(status & ~STATUS_SHELL_OPEN);
        }
        break;

    case MUTE:
    case DEMUTE:
        // Both the disc's own tracks and a movie's compressed sound,
        // which is the difference between this and the mute bit
        // beside the volume registers.
        muted = command == MUTE;
        answer(INT_ACKNOWLEDGE, {status}, ACKNOWLEDGE_CYCLES);
        break;

    case SET_FILTER:
        // Which of the streams woven into one run of sectors the
        // sound is on. It only matters once the mode register asks
        // for the sound to be played at all.
        if (parameter_count >= 2) {
            filter_file = parameters[0];
            filter_channel = parameters[1];
        }
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
        // which is also all a real drive has to go on — so a head
        // that ended up somewhere unreadable cannot answer either.
        if ((status & STATUS_SEEK_ERROR) != 0) {
            answer(INT_ERROR, {status, ERROR_SEEK_FAILED}, ACKNOWLEDGE_CYCLES);
            break;
        }
        const Disc::Msf absolute = msf_from_lba(read_lba);
        const Disc::Track* track = disc.track_at(read_lba);

        // Past the last track the head is over the lead-out, which is
        // numbered rather than named: AA, and not in BCD, because AA
        // is not a number that BCD can hold.
        const u8 number = track != nullptr
            ? to_bcd(static_cast<u8>(track->number))
            : LEAD_OUT_TRACK;
        const u32 start = track != nullptr ? track->start_lba : 0;
        const Disc::Msf relative = msf_from_lba(read_lba - start);
        answer(INT_ACKNOWLEDGE,
               {number,
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
        //
        // Which means there has to have been one. A drive that has not
        // passed over a sector since it was switched on has no header
        // to give and refuses, and it refuses with its status as it
        // stands rather than with the error bit added: nothing went
        // wrong, there is simply nothing there yet.
        if (!has_disc() || !header_valid) {
            answer(INT_ERROR, {status, ERROR_NOT_READY}, ACKNOWLEDGE_CYCLES);
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
        // Everything the drive was doing stops and the motor comes
        // back on. What survives is the header: a drive that has read
        // a sector once can answer Getloc for the rest of the time it
        // is powered, and Init is not a power cycle.
        mode = 0;
        reading = false;
        seeking = false;
        stop_audio();
        set_status(STATUS_MOTOR);
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
        seeking = false;
        // Sound already decoded is dropped rather than played out: a
        // movie the game has stopped must stop being heard, and a
        // third of a second of it is long enough to notice.
        stop_audio();
        status = static_cast<u8>(status & ~(STATUS_READING | STATUS_SEEKING));
        if (command == STOP) {
            status = static_cast<u8>(status & ~STATUS_MOTOR);
        }
        answer(INT_COMPLETE, {status}, PAUSE_CYCLES);
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
        //
        // It begins where the last Setloc pointed, which is somewhere
        // else, so the head has to get there first. Until it does the
        // drive reports itself as seeking rather than reading — the
        // two bits are never both up.
        read_lba = seek_lba;
        reading = true;
        seeking = true;
        status = static_cast<u8>((status & ~STATUS_READING) | STATUS_SEEKING);
        sector_remaining = SEEK_CYCLES + cycles_per_sector();
        answer(INT_ACKNOWLEDGE, {status}, ACKNOWLEDGE_CYCLES);
        break;

    case SEEK_L:
    case SEEK_P:
        if (!has_disc() || seek_lba > SEEK_LIMIT_LBA) {
            answer(INT_ACKNOWLEDGE, {status}, ACKNOWLEDGE_CYCLES);
            lose_position();
            answer(
                INT_ERROR, {status, ERROR_SEEK_FAILED}, SECOND_ANSWER_CYCLES);
            break;
        }
        reading = false;
        seeking = false;
        read_lba = seek_lba;
        status = static_cast<u8>(status & ~(STATUS_READING | STATUS_SEEKING));
        load_header();
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
        } else if (index == 3) {
            volume_written[VOLUME_RIGHT_TO_RIGHT] = value;
        }
        return;

    case 2:
        if (index == 0 && parameter_count < FIFO_CAPACITY) {
            parameters[parameter_count++] = value;
        } else if (index == 1) {
            interrupt_enable = value & FLAG_MASK;
        } else if (index == 2) {
            volume_written[VOLUME_LEFT_TO_LEFT] = value;
        } else if (index == 3) {
            volume_written[VOLUME_RIGHT_TO_LEFT] = value;
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
        } else if (index == 2) {
            volume_written[VOLUME_LEFT_TO_RIGHT] = value;
        } else if (index == 3) {
            // The volume registers are four separate bytes and one
            // change: nothing software writes to them is heard until
            // it says here that it has finished writing them.
            xa_muted = (value & ADPCM_MUTE) != 0;
            if ((value & ADPCM_APPLY_VOLUME) != 0) {
                volume = volume_written;
            }
        }
        return;

    default:
        return;
    }
}
