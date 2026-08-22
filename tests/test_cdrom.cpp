#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "cdrom.h"
#include "console.h"
#include "irq.h"

namespace {

// The drive talked to through the bus, exactly as the BIOS does it, so
// the register banking and the byte-wide decode are part of what is
// tested. Time is moved by hand rather than by running the CPU: what
// matters here is that an answer arrives after the right interval, not
// what the processor was doing meanwhile.
struct Drive {
    std::unique_ptr<Console> console = std::make_unique<Console>();

    Drive()
    {
        console->reset();
        set_index(1);
        write(2, 0x1F);  // let every interrupt through
        set_index(0);
    }

    void set_index(u8 index) const { write(0, index); }

    void write(u32 offset, u8 value) const
    {
        console->bus.write8(CdRom::BASE + offset, value);
    }

    u8 read(u32 offset) const
    {
        return console->bus.read8(CdRom::BASE + offset);
    }

    void command(u8 code, const std::vector<u8>& parameters = {}) const
    {
        set_index(0);
        for (const u8 parameter : parameters) {
            write(2, parameter);
        }
        write(1, code);
    }

    void advance(u64 cycles) const
    {
        console->scheduler.advance(cycles);
        console->dispatch_due_events();
    }

    // The interrupt the drive is holding out, or zero when it has none.
    u8 pending() const
    {
        set_index(1);
        return read(3) & 0x07;
    }

    void acknowledge() const
    {
        set_index(1);
        write(3, 0x07);
    }

    std::vector<u8> answer() const
    {
        set_index(0);
        std::vector<u8> bytes;
        while ((read(0) & 0x20) != 0) {
            bytes.push_back(read(1));
        }
        return bytes;
    }

    bool raised() const
    {
        const u32 line = 1u << static_cast<u32>(Interrupt::CdRom);
        return (console->bus.irq.status & line) != 0;
    }
};

// A disc for the cases that need the drive to find something. Every
// byte of a sector is that sector's own number, so an answer taken
// from the medium says which sector it came off.
//
// Sectors named in `audio` are written as a movie's sound instead:
// the larger of the two sector forms, marked as compressed audio
// arriving in real time, which is what a drive tells them apart by.
struct Image {
    std::filesystem::path directory;

    explicit Image(u32 sectors, const std::vector<u32>& audio = {})
    {
        directory = std::filesystem::temp_directory_path() /
            ("wobble-cdrom-" + std::to_string(counter++));
        std::filesystem::create_directories(directory);

        path = (directory / "disc.bin").string();
        std::ofstream file(path, std::ios::binary);
        for (u32 lba = 0; lba < sectors; lba++) {
            std::vector<char> raw(Disc::RAW_SECTOR_SIZE,
                                  static_cast<char>(lba));

            const bool sound =
                std::find(audio.begin(), audio.end(), lba) != audio.end();
            raw[Disc::SUBHEADER_OFFSET] = FILE_NUMBER;
            raw[Disc::SUBHEADER_OFFSET + 1] = CHANNEL;
            raw[Disc::SUBHEADER_OFFSET + 2] =
                sound ? SUBMODE_SOUND : SUBMODE_DATA;
            raw[Disc::SUBHEADER_OFFSET + 3] = sound ? CODING_STEREO : 0;

            if (sound) {
                write_sound(raw);
            }

            file.write(raw.data(), static_cast<std::streamsize>(raw.size()));
        }
    }

    static constexpr char FILE_NUMBER = 1;
    static constexpr char CHANNEL = 2;
    static constexpr char SUBMODE_DATA = 0x08;
    static constexpr char SUBMODE_SOUND = 0x64;

    // Stereo, 37800 Hz, four bits to the sample: the way a movie's
    // sound is nearly always written.
    static constexpr char CODING_STEREO = 0x01;

    // The sound itself, which is as simple as XA-ADPCM gets: every
    // block scaled by nothing and predicted from nothing, so each
    // sample is the nibble it was written as, shifted up to fill a
    // halfword. The left channel's nibble is 7 and the right's is 2,
    // which makes a steady tone in each and tells the two apart.
    static constexpr char GROUP_HEADER = 0x00;  // shift 0, filter 0
    static constexpr char PACKED_SAMPLES = 0x27;
    static constexpr s16 LEFT_SAMPLE = 0x7000;
    static constexpr s16 RIGHT_SAMPLE = 0x2000;

    // Eighteen 128-byte groups to a sector, each sixteen bytes of
    // header and 112 of samples.
    static constexpr u32 SOUND_GROUPS = 18;
    static constexpr u32 SOUND_GROUP_SIZE = 128;
    static constexpr u32 GROUP_HEADER_SIZE = 16;

    // What that decodes to at the rate the mixer asks at: a sector is
    // 2016 frames of 37800 Hz sound, and seven come out of every six.
    static constexpr u32 FRAMES_PER_SECTOR = 2352;

    static void write_sound(std::vector<char>& raw)
    {
        for (u32 group = 0; group < SOUND_GROUPS; group++) {
            const u32 start =
                Disc::MODE2_DATA_OFFSET + group * SOUND_GROUP_SIZE;
            for (u32 i = 0; i < GROUP_HEADER_SIZE; i++) {
                raw[start + i] = GROUP_HEADER;
            }
            for (u32 i = GROUP_HEADER_SIZE; i < SOUND_GROUP_SIZE; i++) {
                raw[start + i] = PACKED_SAMPLES;
            }
        }
    }

    ~Image() { std::filesystem::remove_all(directory); }

    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    std::string path;

    static int counter;
};

int Image::counter = 0;

// Comfortably longer than any answer takes, for the cases that care
// only that one eventually arrived.
constexpr u64 LONG_ENOUGH = 0x40000;

// The same again for the one thing that is mechanical rather than
// firmware getting round to something: moving the head.
constexpr u64 SEEK_LONG = 0x1000000;

// Less than a sector takes, for the cases that must see them arrive
// one at a time: a drive left to run reads ahead into the next.
constexpr u64 UNDER_A_SECTOR = 0x40000;

}  // namespace

TEST_CASE("the drive answers a command, but not straight away")
{
    const Drive drive;
    drive.command(0x01);  // GetStat

    drive.advance(1024);
    CHECK(drive.pending() == 0);
    CHECK_FALSE(drive.raised());

    drive.advance(LONG_ENOUGH);
    CHECK(drive.pending() == 3);
    CHECK(drive.raised());
    CHECK(drive.answer() == std::vector<u8>{0x02});
}

TEST_CASE("the controller reports its own firmware version")
{
    const Drive drive;
    drive.command(0x19, {0x20});  // Test, sub-function 20h

    drive.advance(LONG_ENOUGH);
    CHECK(drive.pending() == 3);
    CHECK(drive.answer() == std::vector<u8>{0x94, 0x09, 0x19, 0xC0});
}

TEST_CASE("an empty drive reports no disc, which is what sends the "
          "BIOS to its shell")
{
    const Drive drive;
    drive.command(0x1A);  // GetID

    drive.advance(LONG_ENOUGH);
    CHECK(drive.pending() == 3);
    CHECK(drive.answer() == std::vector<u8>{0x02});

    // The second answer is held back until the first is taken.
    drive.advance(LONG_ENOUGH);
    CHECK(drive.pending() == 3);

    drive.acknowledge();
    drive.advance(LONG_ENOUGH);
    CHECK(drive.pending() == 5);
    CHECK(drive.answer() ==
          std::vector<u8>{0x0A, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
}

TEST_CASE("a masked-off interrupt leaves the CPU's line alone")
{
    const Drive drive;
    drive.set_index(1);
    drive.write(2, 0x00);  // interrupt enable, all off
    drive.command(0x01);

    drive.advance(LONG_ENOUGH);
    CHECK(drive.pending() == 3);
    CHECK_FALSE(drive.raised());
}

TEST_CASE("an unknown command is refused rather than ignored")
{
    const Drive drive;
    drive.command(0x7F);

    drive.advance(LONG_ENOUGH);
    CHECK(drive.pending() == 5);
    CHECK(drive.answer() == std::vector<u8>{0x03, 0x40});
}

TEST_CASE("parameters reach the command that was written after them")
{
    const Drive drive;
    drive.command(0x0E, {0x80});  // Setmode
    drive.advance(LONG_ENOUGH);
    CHECK(drive.pending() == 3);
    drive.acknowledge();

    drive.command(0x0F);  // Getparam reads the mode back
    drive.advance(LONG_ENOUGH);
    CHECK(drive.answer() == std::vector<u8>{0x02, 0x80, 0x00, 0x00, 0x00});
}

TEST_CASE("asking for a sector already in hand does not rewind it")
{
    const Drive drive;
    CdRom& cdrom = drive.console->bus.cdrom;

    // A sector whose every byte says where in the sector it came from,
    // so a FIFO that restarted is told apart from one that carried on.
    for (u32 i = 0; i < cdrom.sector.size(); i++) {
        cdrom.sector[i] = static_cast<u8>(i);
    }

    drive.command(0x0E, {0x20});  // Setmode, whole sectors
    drive.advance(LONG_ENOUGH);
    drive.acknowledge();

    // A game reads the twelve bytes of header and subheader first, and
    // asks again before taking the data behind them. The second ask
    // must leave the first where it finished.
    drive.set_index(0);
    drive.write(3, 0x80);
    for (u8 offset = 12; offset < 24; offset++) {
        CHECK(drive.read(2) == offset);
    }

    drive.write(3, 0x80);
    CHECK(drive.read(2) == 24);
    CHECK(drive.read(2) == 25);

    // Withdrawing the request does throw the rest away, and asking
    // afresh then presents the sector from its start.
    drive.write(3, 0x00);
    CHECK(drive.read(2) == 0);
    drive.write(3, 0x80);
    CHECK(drive.read(2) == 12);
}

TEST_CASE("the status register tracks both FIFOs")
{
    const Drive drive;
    drive.set_index(0);
    CHECK((drive.read(0) & 0x08) != 0);  // parameters empty
    CHECK((drive.read(0) & 0x10) != 0);  // room for more
    CHECK((drive.read(0) & 0x20) == 0);  // no answer waiting

    drive.write(2, 0x20);
    CHECK((drive.read(0) & 0x08) == 0);

    drive.write(1, 0x19);
    CHECK((drive.read(0) & 0x80) != 0);  // busy until it answers
    drive.advance(LONG_ENOUGH);
    CHECK((drive.read(0) & 0x80) == 0);
    CHECK((drive.read(0) & 0x20) != 0);
}

TEST_CASE("the drive has no header to report until it has read one")
{
    const Image image(64);
    const Drive drive;
    REQUIRE(drive.console->bus.cdrom.disc.load(image.path));

    // Nothing has passed under the head since the console was switched
    // on, so there is no header to answer with — and the refusal does
    // not claim anything went wrong, because nothing did.
    drive.command(0x10);  // GetlocL
    drive.advance(LONG_ENOUGH);
    CHECK(drive.pending() == 5);
    CHECK(drive.answer() == std::vector<u8>{0x02, 0x80});
    drive.acknowledge();

    // A seek ends with the head over a sector, and from then on the
    // header is that sector's own bytes.
    drive.command(0x02, {0x00, 0x02, 0x16});  // Setloc, sector 16
    drive.advance(LONG_ENOUGH);
    drive.acknowledge();

    drive.command(0x15);  // SeekL
    drive.advance(LONG_ENOUGH);
    CHECK(drive.pending() == 3);
    drive.acknowledge();
    drive.advance(SEEK_LONG);
    CHECK(drive.pending() == 2);
    drive.acknowledge();

    drive.command(0x10);
    drive.advance(LONG_ENOUGH);
    CHECK(drive.pending() == 3);
    CHECK(drive.answer() == std::vector<u8>{16, 16, 16, 16, 1, 2, 0x08, 0});
}

TEST_CASE("a read says it is seeking until its first sector arrives")
{
    const Image image(64);
    const Drive drive;
    REQUIRE(drive.console->bus.cdrom.disc.load(image.path));

    drive.command(0x02, {0x00, 0x02, 0x16});  // Setloc, sector 16
    drive.advance(LONG_ENOUGH);
    drive.acknowledge();

    // The read begins somewhere else, so the head has to get there
    // first, and the two bits are never both up.
    drive.command(0x06);  // ReadN
    drive.advance(LONG_ENOUGH);
    CHECK(drive.pending() == 3);
    CHECK(drive.answer() == std::vector<u8>{0x02 | 0x40});
    drive.acknowledge();

    drive.advance(SEEK_LONG);
    CHECK(drive.pending() == 1);
    CHECK(drive.answer() == std::vector<u8>{0x02 | 0x20});
}

TEST_CASE("the head reaches past the end of the data but not past the disc")
{
    const Image image(64);
    const Drive drive;
    REQUIRE(drive.console->bus.cdrom.disc.load(image.path));

    // Sixty-four sectors of data on seventy-four minutes of medium.
    // The drive positions itself against the disc, not against what
    // happens to be written on it, so this arrives.
    drive.command(0x02, {0x74, 0x00, 0x00});
    drive.advance(LONG_ENOUGH);
    drive.acknowledge();

    drive.command(0x15);  // SeekL
    drive.advance(LONG_ENOUGH);
    drive.acknowledge();
    drive.advance(SEEK_LONG);
    CHECK(drive.pending() == 2);
    drive.acknowledge();

    // Half a minute further on there is no disc left to reach. The
    // drive gives up, stops the motor, and reports the seek error in
    // place of the general one.
    drive.command(0x02, {0x74, 0x30, 0x00});
    drive.advance(LONG_ENOUGH);
    drive.acknowledge();

    drive.command(0x15);
    drive.advance(LONG_ENOUGH);
    CHECK(drive.pending() == 3);
    drive.acknowledge();
    drive.advance(LONG_ENOUGH);
    CHECK(drive.pending() == 5);
    CHECK(drive.answer() == std::vector<u8>{0x04, 0x04});
    drive.acknowledge();

    // With the head somewhere it could not read, neither Getloc has
    // anything left to say.
    drive.command(0x11);  // GetlocP
    drive.advance(LONG_ENOUGH);
    CHECK(drive.pending() == 5);
}

TEST_CASE("a movie's sound never reaches the software reading its picture")
{
    // Sound written between the pictures, as a movie on a disc is.
    const Image image(64, {9, 11});
    const Drive drive;
    REQUIRE(drive.console->bus.cdrom.disc.load(image.path));

    // Reading for the picture, with the drive told to play the sound
    // as it goes. That makes the sound sectors its own business, and
    // software is never offered them.
    drive.command(0x0E, {0x40});  // Setmode, XA-ADPCM
    drive.advance(LONG_ENOUGH);
    drive.acknowledge();

    drive.command(0x02, {0x00, 0x02, 0x08});  // Setloc, sector 8
    drive.advance(LONG_ENOUGH);
    drive.acknowledge();

    drive.command(0x1B);  // ReadS
    drive.advance(LONG_ENOUGH);
    drive.acknowledge();

    // Time is let past in less than a sector at a time so the drive
    // never gets a sector ahead of the one being collected.
    std::vector<u8> delivered;
    for (u32 wanted = 0; wanted < 3; wanted++) {
        for (u32 tries = 0; tries < 128 && drive.pending() != 1; tries++) {
            drive.advance(UNDER_A_SECTOR);
        }
        REQUIRE(drive.pending() == 1);

        drive.set_index(0);
        drive.write(3, 0x80);
        delivered.push_back(drive.read(2));
        drive.acknowledge();
    }

    CHECK(delivered == std::vector<u8>{8, 10, 12});
}

TEST_CASE("opening the lid stops the drive, and it says so unasked")
{
    const Image image(64);
    const Drive drive;
    REQUIRE(drive.console->bus.cdrom.disc.load(image.path));

    drive.console->bus.cdrom.open_shell();
    drive.advance(LONG_ENOUGH);

    // An answer to nothing: the drive has stopped, and why.
    CHECK(drive.pending() == 5);
    CHECK(drive.answer() == std::vector<u8>{0x01, 0x08});
    drive.acknowledge();

    // The disc is still turning for a moment after the lid goes.
    drive.command(0x01);  // GetStat
    drive.advance(LONG_ENOUGH);
    CHECK(drive.answer() == std::vector<u8>{0x12});
    drive.acknowledge();

    drive.advance(CPU_CLOCK_HZ);
    drive.command(0x01);
    drive.advance(LONG_ENOUGH);
    CHECK(drive.answer() == std::vector<u8>{0x10});
}

TEST_CASE("the lid's bit stands after it is shut, until a Getstat takes it")
{
    const Image image(64);
    const Drive drive;
    REQUIRE(drive.console->bus.cdrom.disc.load(image.path));

    drive.console->bus.cdrom.open_shell();
    drive.advance(LONG_ENOUGH);
    drive.acknowledge();
    drive.console->bus.cdrom.close_shell();

    // A game that was not watching when the lid moved still finds out
    // that it did, and only the asking puts the bit down.
    drive.command(0x01);  // GetStat
    drive.advance(LONG_ENOUGH);
    CHECK(drive.answer() == std::vector<u8>{0x10});
    drive.acknowledge();

    drive.command(0x01);
    drive.advance(LONG_ENOUGH);
    CHECK(drive.answer() == std::vector<u8>{0x00});
}

TEST_CASE("a disc changed while the lid is open is the one read after it")
{
    // Two discs of different lengths, which is the cheapest thing to
    // ask the drive that only the medium can answer.
    const Image first(64);
    const Image second(200);
    const Drive drive;
    REQUIRE(drive.console->bus.cdrom.disc.load(first.path));

    drive.command(0x14, {0x00});  // GetTD of the lead-out: how long it is
    drive.advance(LONG_ENOUGH);
    CHECK(drive.answer() == std::vector<u8>{0x02, 0x00, 0x02});
    drive.acknowledge();

    drive.console->bus.cdrom.open_shell();
    drive.advance(LONG_ENOUGH);
    REQUIRE(drive.pending() == 5);
    drive.acknowledge();

    // A drive standing open reaches nothing, whatever is sitting in it.
    drive.command(0x1A);  // GetID
    drive.advance(LONG_ENOUGH);
    REQUIRE(drive.pending() == 3);
    drive.answer();
    drive.acknowledge();
    drive.advance(LONG_ENOUGH);
    REQUIRE(drive.pending() == 5);
    CHECK(drive.answer().at(1) == 0x40);  // no disc
    drive.acknowledge();

    REQUIRE(drive.console->bus.cdrom.disc.load(second.path));
    drive.console->bus.cdrom.close_shell();

    drive.command(0x14, {0x00});
    drive.advance(LONG_ENOUGH);
    CHECK(drive.answer() == std::vector<u8>{0x10, 0x00, 0x04});
}

namespace {

// Starts a movie: the mode bit that sends sound to the hardware, the
// head aimed at the first sector of it, and a read running.
void play_movie(const Drive& drive, u8 mode)
{
    drive.command(0x0E, {mode});  // Setmode
    drive.advance(LONG_ENOUGH);
    drive.acknowledge();

    drive.command(0x02, {0x00, 0x02, 0x08});  // Setloc, sector 8
    drive.advance(LONG_ENOUGH);
    drive.acknowledge();

    drive.command(0x1B);  // ReadS
    drive.advance(LONG_ENOUGH);
    drive.acknowledge();
}

// Lets the drive read for long enough to pass over the sound sectors,
// collecting the sectors software is offered so the read keeps moving.
void read_past_the_sound(const Drive& drive, u32 sectors)
{
    for (u32 collected = 0; collected < sectors;) {
        drive.advance(UNDER_A_SECTOR);
        if (drive.pending() == 1) {
            drive.acknowledge();
            collected++;
        }
    }
}

// Everything the drive has decoded and not yet been asked for, in the
// order it would be heard.
std::vector<CdRom::Audio> take_sound(const Drive& drive)
{
    CdRom& cdrom = drive.console->bus.cdrom;
    std::vector<CdRom::Audio> frames;
    while (cdrom.audio_ready() > 0) {
        frames.push_back(cdrom.take_audio());
    }
    return frames;
}

// Sound sectors woven through the sectors from 9 on, the way a movie
// is written: one in every two, so a read of any length passes some.
std::vector<u32> woven_sound()
{
    std::vector<u32> sectors;
    for (u32 lba = 9; lba < 40; lba += 2) {
        sectors.push_back(lba);
    }
    return sectors;
}

// The steady part of a decoded tone, past the samples the resampler
// spends catching up with a signal that starts out of nothing.
constexpr u32 SETTLED = 128;

// What one of the image's samples comes back as. The drive's filter
// loses about a tenth of a constant signal, which is the filter being
// the hardware's rather than an ideal one.
constexpr double RESAMPLE_GAIN = 0.906;

}  // namespace

TEST_CASE("a movie's sound is decoded and handed to the mixer")
{
    const Image image(64, {9, 11, 13});
    const Drive drive;
    REQUIRE(drive.console->bus.cdrom.disc.load(image.path));

    play_movie(drive, 0x40);  // XA-ADPCM, no filter
    read_past_the_sound(drive, 4);

    const std::vector<CdRom::Audio> frames = take_sound(drive);
    REQUIRE(frames.size() > SETTLED);

    // A steady tone in each channel, and a different one in each: the
    // decoder read the nibbles it was given, and read the two channels
    // out of the halves of the same byte rather than mixing them.
    const double left = Image::LEFT_SAMPLE * RESAMPLE_GAIN;
    const double right = Image::RIGHT_SAMPLE * RESAMPLE_GAIN;
    for (u32 i = SETTLED; i < frames.size(); i++) {
        CHECK(frames[i].left == doctest::Approx(left).epsilon(0.01));
        CHECK(frames[i].right == doctest::Approx(right).epsilon(0.01));
    }
}

TEST_CASE("a sound sector is a sector of sound, however long it takes to hear")
{
    // The rate is the mixer's and not the drive's: a sector holds a
    // twentieth of a second of sound however fast it was read, and it
    // is heard over that twentieth.
    const Image image(64, {9});
    const Drive drive;
    REQUIRE(drive.console->bus.cdrom.disc.load(image.path));

    play_movie(drive, 0x40);
    read_past_the_sound(drive, 2);

    // The mixer has been taking a frame a sample all along, so what
    // the drive has decoded is counted rather than what is left.
    CHECK(drive.console->bus.cdrom.decoded == Image::FRAMES_PER_SECTOR);
}

TEST_CASE("sound on another stream is dropped rather than played")
{
    const Image image(64, {9, 11});
    const Drive drive;
    REQUIRE(drive.console->bus.cdrom.disc.load(image.path));

    // The filter on, naming a channel the disc does not carry. The
    // sectors are still the sound hardware's — software is never
    // offered them — but they are not this movie's sound.
    drive.command(0x0D, {Image::FILE_NUMBER, Image::CHANNEL + 1});
    drive.advance(LONG_ENOUGH);
    drive.acknowledge();

    play_movie(drive, 0x48);  // XA-ADPCM with the filter
    read_past_the_sound(drive, 3);

    CHECK(drive.console->bus.cdrom.audio_ready() == 0);
    CHECK(drive.console->bus.cdrom.played == 0);
}

TEST_CASE("the stream the filter names is the one that is played")
{
    const Image image(64, {9, 11});
    const Drive drive;
    REQUIRE(drive.console->bus.cdrom.disc.load(image.path));

    drive.command(0x0D, {Image::FILE_NUMBER, Image::CHANNEL});
    drive.advance(LONG_ENOUGH);
    drive.acknowledge();

    play_movie(drive, 0x48);
    read_past_the_sound(drive, 3);

    CHECK(drive.console->bus.cdrom.audio_ready() > 0);
}

TEST_CASE("muting the drive silences it without stopping its decoder")
{
    const Image image(64, woven_sound());
    const Drive drive;
    REQUIRE(drive.console->bus.cdrom.disc.load(image.path));

    drive.command(0x0B);  // Mute
    drive.advance(LONG_ENOUGH);
    drive.acknowledge();

    play_movie(drive, 0x40);
    read_past_the_sound(drive, 3);

    // Still decoding — the sound is there, it is simply not heard.
    std::vector<CdRom::Audio> frames = take_sound(drive);
    REQUIRE(frames.size() > SETTLED);
    for (const CdRom::Audio& frame : frames) {
        CHECK(frame.left == 0);
        CHECK(frame.right == 0);
    }

    // And demuting picks the movie up where it has got to rather than
    // where it was silenced.
    drive.command(0x0C);  // Demute
    drive.advance(LONG_ENOUGH);
    drive.acknowledge();
    read_past_the_sound(drive, 3);

    frames = take_sound(drive);
    REQUIRE(frames.size() > SETTLED);
    CHECK(frames[SETTLED].left != 0);
}

TEST_CASE("the drive's own volume registers are written apart and applied "
          "together")
{
    const Image image(64, woven_sound());
    const Drive drive;
    REQUIRE(drive.console->bus.cdrom.disc.load(image.path));

    // Half of each channel into each, which is how a game asked for
    // mono output plays a stereo movie.
    constexpr u8 HALF = 0x40;
    drive.set_index(2);
    drive.write(2, HALF);  // left to left
    drive.write(3, HALF);  // left to right
    drive.set_index(3);
    drive.write(1, HALF);  // right to right
    drive.write(2, HALF);  // right to left

    play_movie(drive, 0x40);
    read_past_the_sound(drive, 3);

    // Nothing has been applied yet, so the movie is still in stereo.
    std::vector<CdRom::Audio> frames = take_sound(drive);
    REQUIRE(frames.size() > SETTLED);
    CHECK(frames[SETTLED].left != frames[SETTLED].right);

    drive.set_index(3);
    drive.write(3, 0x20);  // apply the four of them
    read_past_the_sound(drive, 3);

    frames = take_sound(drive);
    REQUIRE(frames.size() > SETTLED);

    const double mono =
        (Image::LEFT_SAMPLE + Image::RIGHT_SAMPLE) * RESAMPLE_GAIN / 2;
    for (u32 i = SETTLED; i < frames.size(); i++) {
        CHECK(frames[i].left == frames[i].right);
        CHECK(frames[i].left == doctest::Approx(mono).epsilon(0.01));
    }
}

TEST_CASE("a movie that is stopped stops being heard")
{
    const Image image(64, {9, 11});
    const Drive drive;
    REQUIRE(drive.console->bus.cdrom.disc.load(image.path));

    play_movie(drive, 0x40);
    read_past_the_sound(drive, 3);
    REQUIRE(drive.console->bus.cdrom.audio_ready() > 0);

    // A third of a second of decoded sound can be in hand, and it must
    // not go on playing after the game has stopped the drive.
    drive.command(0x09);  // Pause
    drive.advance(LONG_ENOUGH);
    CHECK(drive.console->bus.cdrom.audio_ready() == 0);
}
