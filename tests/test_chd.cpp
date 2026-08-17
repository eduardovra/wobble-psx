#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "chd.h"
#include "disc.h"

namespace {

// A CHD the test writes itself, so what the reader is handed is a real
// file in the format rather than a stand-in for one. It is written
// with no compression, which the format allows and which is what makes
// writing one here reasonable: the map becomes a plain list of where
// each hunk went, and the hunks themselves are the frames as they are.
struct ChdImage {
    static constexpr u32 FRAMES_PER_HUNK = 4;
    static constexpr u32 HUNK_BYTES = Chd::FRAME_SIZE * FRAMES_PER_HUNK;

    // The v5 header, and the metadata entry that precedes each track's
    // line of text: a tag, a length with flags in its top byte, and
    // where the next entry is.
    static constexpr u32 HEADER_SIZE = 124;
    static constexpr u32 ENTRY_SIZE = 16;
    static constexpr u32 TRACK_TAG = 0x43485432;  // 'CHT2'

    struct Track {
        u32 number = 1;
        std::string type = "MODE2_RAW";
        u32 frames = 0;
        u32 pregap = 0;
        std::string pregap_type = "NONE";
    };

    std::filesystem::path directory;

    ChdImage()
    {
        directory = std::filesystem::temp_directory_path() /
            ("wobble-chd-" + std::to_string(counter++));
        std::filesystem::create_directories(directory);
    }

    ~ChdImage() { std::filesystem::remove_all(directory); }

    ChdImage(const ChdImage&) = delete;
    ChdImage& operator=(const ChdImage&) = delete;

    // Writes a CHD of `tracks`, every frame filled with the number of
    // the frame it is, so a sector read back says where in the file it
    // came from — which is the whole difficulty with a format whose
    // bug is an offset rather than a wrong byte.
    std::string write(const std::string& name,
                      const std::vector<Track>& tracks) const
    {
        // Where each track's frames land, padded out to a group as the
        // format stores them.
        std::vector<u32> first_frame;
        u32 frames = 0;
        for (const Track& track : tracks) {
            first_frame.push_back(frames);
            frames += (track.frames + Chd::TRACK_PADDING - 1) /
                Chd::TRACK_PADDING * Chd::TRACK_PADDING;
        }

        const u64 logical_bytes = u64{frames} * Chd::FRAME_SIZE;
        const auto hunks =
            static_cast<u32>((logical_bytes + HUNK_BYTES - 1) / HUNK_BYTES);

        // The first hunk of the file is the header, the map and the
        // metadata: hunk data has to start at a multiple of the hunk
        // size, and that is the first one it can start at.
        std::vector<u8> file(u64{hunks + 1} * HUNK_BYTES, 0);
        const u32 map_offset = HEADER_SIZE;
        const u32 metadata_offset = map_offset + hunks * 4;

        std::copy_n("MComprHD", 8, file.begin());
        put_be32(file, 8, HEADER_SIZE);
        put_be32(file, 12, 5);
        put_be64(file, 32, logical_bytes);
        put_be64(file, 40, map_offset);
        put_be64(file, 48, metadata_offset);
        put_be32(file, 56, HUNK_BYTES);
        put_be32(file, 60, Chd::FRAME_SIZE);

        // Uncompressed, so a map entry is only which hunk of the file
        // holds this hunk of the image.
        for (u32 hunk = 0; hunk < hunks; hunk++) {
            put_be32(file, map_offset + hunk * 4, hunk + 1);
        }

        u32 at = metadata_offset;
        for (std::size_t i = 0; i < tracks.size(); i++) {
            const std::string text = metadata_for(tracks[i]);
            const auto length = static_cast<u32>(text.size() + 1);
            const bool last = i + 1 == tracks.size();

            put_be32(file, at, TRACK_TAG);
            put_be32(file, at + 4, length);
            put_be64(file, at + 8, last ? 0 : at + ENTRY_SIZE + length);
            std::copy(text.begin(), text.end(), file.begin() + at + ENTRY_SIZE);
            at += ENTRY_SIZE + length;
        }
        REQUIRE(at <= HUNK_BYTES);

        for (u32 frame = 0; frame < frames; frame++) {
            const u64 offset = u64{HUNK_BYTES} + u64{frame} * Chd::FRAME_SIZE;
            const auto start = static_cast<std::ptrdiff_t>(offset);
            std::fill_n(file.begin() + start,
                        Chd::RAW_SECTOR_SIZE,
                        static_cast<u8>(frame));
        }

        const std::filesystem::path path = directory / name;
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(file.data()),
                  static_cast<std::streamsize>(file.size()));
        return path.string();
    }

    std::string write_text(const std::string& name,
                           const std::string& text) const
    {
        const std::filesystem::path path = directory / name;
        std::ofstream file(path);
        file << text;
        return path.string();
    }

    static std::string metadata_for(const Track& track)
    {
        return "TRACK:" + std::to_string(track.number) + " TYPE:" + track.type +
            " SUBTYPE:NONE FRAMES:" + std::to_string(track.frames) +
            " PREGAP:" + std::to_string(track.pregap) +
            " PGTYPE:" + track.pregap_type + " PGSUB:NONE POSTGAP:0";
    }

    static void put_be32(std::vector<u8>& out, u64 at, u32 value)
    {
        for (u32 byte = 0; byte < 4; byte++) {
            out[at + byte] = static_cast<u8>(value >> (24 - byte * 8));
        }
    }

    static void put_be64(std::vector<u8>& out, u64 at, u64 value)
    {
        for (u32 byte = 0; byte < 8; byte++) {
            out[at + byte] = static_cast<u8>(value >> (56 - byte * 8));
        }
    }

    static int counter;
};

int ChdImage::counter = 0;

}  // namespace

TEST_CASE("a chd is opened as the disc it was made from")
{
    const ChdImage image;
    const std::string path = image.write(
        "game.chd",
        {{1, "MODE2_RAW", 21, 0, "NONE"}, {2, "AUDIO", 10, 150, "NONE"}});

    Disc disc;
    REQUIRE(disc.load(path));
    REQUIRE(disc.tracks.size() == 2);

    CHECK(disc.tracks[0].number == 1);
    CHECK_FALSE(disc.tracks[0].audio);
    CHECK(disc.tracks[0].start_lba == 0);
    CHECK(disc.tracks[0].length_sectors == 21);

    // The gap ahead of the second track is described by the file and
    // not stored in it, so it takes up room in the numbering only.
    CHECK(disc.tracks[1].number == 2);
    CHECK(disc.tracks[1].audio);
    CHECK(disc.tracks[1].start_lba == 21 + 150);
    CHECK(disc.tracks[1].length_sectors == 10);
}

TEST_CASE("a sector of a chd comes back whole")
{
    const ChdImage image;
    const std::string path =
        image.write("game.chd", {{1, "MODE2_RAW", 21, 0, "NONE"}});

    Disc disc;
    REQUIRE(disc.load(path));

    std::array<u8, Disc::RAW_SECTOR_SIZE> sector{};
    REQUIRE(disc.read_sector(5, sector));

    // All 2352 bytes of it, and none of the 96 bytes of subcode that
    // follow it in the file.
    CHECK(sector[0] == 5);
    CHECK(sector[Disc::RAW_SECTOR_SIZE - 1] == 5);

    CHECK_FALSE(disc.read_sector(21, sector));
}

TEST_CASE("a track of a chd is read from where its padding put it")
{
    // Each track's frames are padded out to a group of four, so the
    // second track of a 21-frame first one starts at frame 24 and not
    // at 21. Reading it at the wrong offset is the mistake the format
    // invites, and the one nothing downstream would report.
    const ChdImage image;
    const std::string path = image.write(
        "game.chd",
        {{1, "MODE2_RAW", 21, 0, "NONE"}, {2, "MODE2_RAW", 10, 0, "NONE"}});

    Disc disc;
    REQUIRE(disc.load(path));
    REQUIRE(disc.tracks.size() == 2);
    CHECK(disc.tracks[1].start_lba == 21);

    std::array<u8, Disc::RAW_SECTOR_SIZE> sector{};
    REQUIRE(disc.read_sector(21, sector));
    CHECK(sector[0] == 24);

    REQUIRE(disc.read_sector(20, sector));
    CHECK(sector[0] == 20);
}

TEST_CASE("a cooked track of a chd has its sync and header rebuilt")
{
    const ChdImage image;
    const std::string path =
        image.write("game.chd", {{1, "MODE1", 8, 0, "NONE"}});

    Disc disc;
    REQUIRE(disc.load(path));

    std::array<u8, Disc::RAW_SECTOR_SIZE> sector{};
    REQUIRE(disc.read_sector(3, sector));

    CHECK(sector[0] == 0x00);
    CHECK(sector[1] == 0xFF);
    const Disc::Msf msf = msf_from_lba(3);
    CHECK(sector[Disc::HEADER_OFFSET] == msf.minute);
    CHECK(sector[Disc::HEADER_OFFSET + 1] == msf.second);
    CHECK(sector[Disc::HEADER_OFFSET + 2] == msf.frame);

    // The 2048 bytes the track stores, where a Mode 2 Form 1 sector
    // keeps its payload, and nothing of the frame past them.
    CHECK(sector[Disc::MODE2_DATA_OFFSET] == 3);
    CHECK(sector[Disc::MODE2_DATA_OFFSET + Disc::COOKED_SECTOR_SIZE - 1] == 3);
    CHECK(sector[Disc::MODE2_DATA_OFFSET + Disc::COOKED_SECTOR_SIZE] == 0);
}

TEST_CASE("bytes are taken from across the hunks they fall in")
{
    const ChdImage image;
    const std::string path =
        image.write("game.chd", {{1, "MODE2_RAW", 12, 0, "NONE"}});

    Chd chd;
    REQUIRE(chd.open(path));

    // A hunk is four frames, so this run starts in one and ends in the
    // next.
    const u64 offset = u64{3} * Chd::FRAME_SIZE;
    std::vector<u8> taken(std::size_t{Chd::FRAME_SIZE} * 2, 0xAA);
    REQUIRE(chd.read(offset, taken.data(), static_cast<u32>(taken.size())));

    CHECK(taken[0] == 3);
    CHECK(taken[Chd::RAW_SECTOR_SIZE - 1] == 3);
    CHECK(taken[Chd::FRAME_SIZE] == 4);

    // And past the end of the image is nothing, not zeroes.
    CHECK_FALSE(chd.read(u64{12} * Chd::FRAME_SIZE * 4, taken.data(), 16));
}

TEST_CASE("a file that is not a chd is refused rather than loaded empty")
{
    const ChdImage image;
    const std::string path = image.write_text("game.chd", "not a chd at all");

    Disc disc;
    CHECK_FALSE(disc.load(path));
    CHECK_FALSE(disc.loaded());

    std::array<u8, Disc::RAW_SECTOR_SIZE> sector{};
    CHECK_FALSE(disc.read_sector(0, sector));
}
