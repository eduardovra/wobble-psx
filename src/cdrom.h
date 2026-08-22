#pragma once

#include <array>
#include <initializer_list>

#include "disc.h"
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
// An empty drive is a state a real console can be in rather than a gap
// here: GetID answers INT5 with the "no disc" reason, and it is that
// answer which sends the BIOS to its shell instead of into a game.
//
// So is an open one. The lid is a switch the drive watches: lifting it
// stops the disc and sends an INT5 nothing asked for, and the status
// byte carries a bit saying the lid is, or has been, open until a
// Getstat reads it away. That bit is how a two-disc game knows it has
// been given something else to read.
//
// With a disc in it the drive also reads. A read is not a command that
// answers once: ReadN acknowledges, and then a sector arrives as an
// INT1 every time one passes under the head — 75 a second, or 150 at
// double speed — until something stops it. The sector waits in a
// buffer until software asks for it by writing the request register,
// and is then taken a byte at a time out of the data FIFO, or by the
// DMA channel that does the same thing faster.
//
// Not every sector arrives that way. A movie's sound is written among
// its pictures as compressed audio, and those sectors are the drive's
// own business: it decodes them itself and hands the samples to the
// sound processor, where they are mixed in beside its voices. Nothing
// asks for them and software never sees them — which is why a game
// starts a movie and then only ever reads its picture.
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
    // answer: what the mechanism is doing, and whether the last thing
    // asked of it went wrong.
    static constexpr u8 STATUS_ERROR = 1 << 0;
    static constexpr u8 STATUS_MOTOR = 1 << 1;
    static constexpr u8 STATUS_SEEK_ERROR = 1 << 2;
    static constexpr u8 STATUS_ID_ERROR = 1 << 3;
    static constexpr u8 STATUS_SHELL_OPEN = 1 << 4;
    static constexpr u8 STATUS_READING = 1 << 5;
    static constexpr u8 STATUS_SEEKING = 1 << 6;
    u8 status = STATUS_MOTOR;

    // The last Setmode, kept because Getparam reads it back.
    u8 mode = 0;

    // Which of the interleaved streams on the disc the drive is
    // listening to, set by Setfilter. A movie is written as one run of
    // sectors carrying several files and channels at once — picture in
    // one, sound in another — and the filter is what picks the sound
    // out of it.
    u8 filter_file = 0;
    u8 filter_channel = 0;

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

    // The disc in the drive, if there is one. Not part of a save
    // state, for the same reason the BIOS is not: it is the medium the
    // state was taken from, not state itself.
    Disc disc;

    // Where Setloc has aimed the head, and which sector a read is up
    // to. They are separate because Setloc does not move anything —
    // the seek or the read that follows it is what acts on it.
    u32 seek_lba = 0;
    u32 read_lba = 0;
    bool reading = false;

    // A read begins by moving the head, and the status byte says so:
    // seeking until the first sector arrives, reading from then on.
    bool seeking = false;

    // Whether the head has passed over a sector since the console was
    // switched on, which is the only thing that makes the header
    // Getloc reports mean anything. It survives Init — a drive that
    // has read once has a header for ever after — and is lost only
    // when the head ends up somewhere it could not read.
    bool header_valid = false;

    // Cycles left until the next sector passes under the head.
    u64 sector_remaining = 0;

    // The last sector read, whole, and how far through it software has
    // got. The two are equal when the FIFO is empty, which is the
    // state it is in until the request register asks for a sector.
    std::array<u8, Disc::RAW_SECTOR_SIZE> sector{};
    u32 data_cursor = 0;
    u32 data_end = 0;

    // The sound path, which the CPU never sees: a movie's audio
    // sectors are decoded here and handed to the SPU a sample at a
    // time, mixed in beside its voices.

    // A stereo sample of the drive's own sound, at the rate the sound
    // processor mixes at.
    struct Audio {
        s16 left = 0;
        s16 right = 0;
    };

    // Whether any of it reaches the mixer. Mute and Demute are
    // commands; the other one is a bit beside the volume registers,
    // and silences the compressed audio alone.
    bool muted = false;
    bool xa_muted = false;

    // The drive's own mixer, ahead of the SPU's: how much of each
    // channel of the disc reaches each channel of the output, in
    // 128ths, in the order left-to-left, left-to-right, right-to-right
    // and right-to-left. Software writes them one byte at a time and
    // they are adopted together when it sets the apply bit, which is
    // why there are two copies — a game fading a movie out writes four
    // bytes and means them as one change.
    static constexpr u32 VOLUME_COUNT = 4;
    static constexpr u8 VOLUME_UNITY = 0x80;
    std::array<u8, VOLUME_COUNT> volume_written = {
        VOLUME_UNITY, 0, VOLUME_UNITY, 0};
    std::array<u8, VOLUME_COUNT> volume = {VOLUME_UNITY, 0, VOLUME_UNITY, 0};

    // The decoder's memory of the two samples before the one it is
    // working out, one pair per channel. A block cannot be decoded
    // without the tail of the block in front, and the tail of a sector
    // carries into the next one — which is why this is here and not a
    // local of the decoder.
    std::array<s32, 2> adpcm_old{};
    std::array<s32, 2> adpcm_older{};

    // The resampler between the disc's rate and the mixer's. The drive
    // decodes at 37800 Hz and the SPU asks at 44100, so seven samples
    // are made out of every six by interpolating across the last
    // twenty-nine — the zigzag filter the hardware does it with, which
    // is a different one from the voices'.
    static constexpr u32 RESAMPLE_RING = 32;
    static constexpr u32 RESAMPLE_STEPS = 7;
    static constexpr u32 SIX_STEP = 6;
    std::array<s16, RESAMPLE_RING> resample_left{};
    std::array<s16, RESAMPLE_RING> resample_right{};
    u32 resample_position = 0;
    u32 six_step = SIX_STEP;

    // Decoded sound waiting to be heard. A sector is a tenth of a
    // second of it at the slowest rate, and the mixer takes it one
    // sample at a time, so several sectors' worth can be in hand at
    // once — the depth is a third of a second, which is more than the
    // drive gets ahead of itself by on a stream written to be played
    // at the speed it is read.
    static constexpr u32 AUDIO_CAPACITY = 16384;
    std::array<Audio, AUDIO_CAPACITY> audio{};
    u64 decoded = 0;
    u64 played = 0;

    // Whether the lid is open. A drive standing open reaches nothing,
    // whatever is sitting in it — which is also how a disc comes to be
    // swapped, since the only moment a game may be given a different
    // one is while the drive cannot see either.
    bool shell_open = false;

    // An INT5 the lid owes software and has not been able to give yet,
    // because an answer to something else was still in the queue.
    bool shell_report_pending = false;

    // Cycles left before the disc has stopped turning. A motor is not
    // a switch: the lid opens, and for a moment afterwards the status
    // byte still says the disc is spinning, because it is.
    u64 spin_down_remaining = 0;

    // Opens the lid, or closes it. Opening stops the drive where it
    // stands and tells software so; closing only lets it be used
    // again, and leaves the disc stopped until something asks for it.
    void open_shell();
    void close_shell();

    // Whether the drive has a disc it can read. Everything that would
    // touch the medium refuses when it does not — and an open lid is
    // one of the ways it does not.
    bool has_disc() const { return disc.loaded() && !shell_open; }

    // Whether a sector is one the sound hardware takes rather than
    // software: compressed audio, on the stream the filter names.
    // Software never sees these, which is the whole point of them —
    // a game reads a movie's picture and the sound arrives beside it
    // without anything having to ask for it.
    bool
    is_audio_sector(const std::array<u8, Disc::RAW_SECTOR_SIZE>& raw) const;

    // The next frame of it, or silence when the drive has none ready.
    // One is taken for every sample the SPU produces, and that is what
    // paces playback: a sector decodes in an instant into a tenth of a
    // second of sound, which is then heard over the tenth of a second
    // it takes the mixer to ask for all of it.
    Audio take_audio();

    // Frames decoded and not yet played. Nothing on the console
    // reports this either; it is how far ahead of the mixer the drive
    // has got, which is the one number that says whether a movie's
    // sound is arriving at the rate it is meant to be heard at.
    u32 audio_ready() const;

    // How long one sector takes at the speed the mode register asks
    // for. The drive turns at a constant 75 sectors a second, or twice
    // that, which is the one piece of its timing that is mechanical
    // and so the one games measure themselves against.
    u64 cycles_per_sector() const;

    // Takes the next byte of the current sector. Zero, and no
    // movement, once the FIFO has been read out.
    u8 read_data();

    // Whether there is anything left in it to take. It is the drive's
    // half of the DMA handshake: channel 3 is armed by software but
    // paced by this, so a transfer asked for before a sector has
    // arrived waits for it rather than reading zeroes off the end.
    bool has_data() const { return data_cursor < data_end; }

    // Which bytes of a sector the mode register exposes: the payload
    // alone, or the whole sector from its header on, which is how a
    // game reads the subheader for itself.
    u32 data_start() const;
    u32 data_size() const;

private:
    void execute(u8 command);

    // Whether a sound sector is on the stream Setfilter named, which
    // is what decides between playing it and dropping it. Either way
    // it is the sound hardware's and not software's.
    bool matches_filter(const std::array<u8, Disc::RAW_SECTOR_SIZE>& raw) const;

    // Turns one sector of XA-ADPCM into sound: eighteen groups of
    // eight blocks, each a filter and a shift over twenty-eight
    // packed samples, at whatever rate and width the subheader says.
    void decode_audio(const std::array<u8, Disc::RAW_SECTOR_SIZE>& raw);

    // One decoded frame on its way through the resampler, at the
    // disc's own rate. A half-rate sector hands each frame over twice,
    // which is what makes 18900 Hz into the 37800 the filter works at.
    void resample(s16 left, s16 right);

    // One frame at the mixer's rate, into the queue above.
    void queue_audio(s16 left, s16 right);

    // Throws away what has been decoded and forgets where the decoder
    // was. What a drive that has stopped reading owes the mixer is
    // nothing: a movie that is over must not go on being heard.
    void stop_audio();

    // Moves the read on by one sector, if one is due. Split out
    // because it is the only part of the drive that runs without
    // having been asked to.
    void advance_read(u64 cycles);

    // Reads the header of the sector the head has arrived at, which
    // is what a seek does last and what Getloc answers from.
    void load_header();

    // Replaces what the drive is doing without disturbing what the
    // lid is: the shell bit is kept whatever a command sets the rest
    // of the status byte to.
    void set_status(u8 bits);

    // Gives up on where the head is: the state a drive is left in by
    // a seek that found nothing to read.
    void lose_position();

    // Queues an answer `delay` cycles out. The parameters are read
    // during the command, not when the answer is given, so a command
    // can queue both of its answers up front and be done.
    void answer(u8 interrupt, std::initializer_list<u8> bytes, u64 delay);

    // Hands the front of the queue to software: its bytes become the
    // response FIFO and its number becomes the pending interrupt.
    void deliver();

    bool line_active() const;
};
