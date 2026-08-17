#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "disc.h"

namespace {

// A disc built for the test to read back. Every sector says which one
// it is, so an assertion about the data FIFO knows what it should have
// got — which is the whole difficulty with an image format whose bug
// is usually an offset rather than a wrong byte.
struct Image {
    std::filesystem::path directory;

    Image()
    {
        directory = std::filesystem::temp_directory_path() /
            ("wobble-disc-" + std::to_string(counter++));
        std::filesystem::create_directories(directory);
    }

    ~Image() { std::filesystem::remove_all(directory); }

    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    // Writes `sectors` raw sectors, each filled with its own number.
    std::string write_raw(const std::string& name, u32 sectors) const
    {
        const std::filesystem::path path = directory / name;
        std::ofstream file(path, std::ios::binary);
        for (u32 lba = 0; lba < sectors; lba++) {
            std::vector<u8> sector(Disc::RAW_SECTOR_SIZE, static_cast<u8>(lba));
            file.write(reinterpret_cast<const char*>(sector.data()),
                       static_cast<std::streamsize>(sector.size()));
        }
        return path.string();
    }

    // The same, as bare 2048-byte blocks with no sync or header.
    std::string write_cooked(const std::string& name, u32 sectors) const
    {
        const std::filesystem::path path = directory / name;
        std::ofstream file(path, std::ios::binary);
        for (u32 lba = 0; lba < sectors; lba++) {
            std::vector<u8> block(Disc::COOKED_SECTOR_SIZE,
                                  static_cast<u8>(lba));
            file.write(reinterpret_cast<const char*>(block.data()),
                       static_cast<std::streamsize>(block.size()));
        }
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

    static int counter;
};

int Image::counter = 0;

}  // namespace

TEST_CASE("an address in minutes and seconds is the sector two seconds on")
{
    // The numbering starts after a two-second lead-in, so the first
    // readable sector is at 00:02:00 and not at zero.
    CHECK(lba_from_msf({0x00, 0x02, 0x00}) == 0);
    CHECK(lba_from_msf({0x00, 0x02, 0x01}) == 1);
    CHECK(lba_from_msf({0x00, 0x03, 0x00}) == 75);
    CHECK(lba_from_msf({0x01, 0x00, 0x00}) == 60 * 75 - 150);

    // The digits are BCD, so the tens are a nibble and not a decade.
    CHECK(lba_from_msf({0x00, 0x12, 0x00}) == 10 * 75);
}

TEST_CASE("the two numberings are inverses of each other")
{
    for (const u32 lba : {0u, 1u, 74u, 75u, 4499u, 100000u}) {
        CHECK(lba_from_msf(msf_from_lba(lba)) == lba);
    }
}

TEST_CASE("a raw image is served a sector at a time")
{
    const Image image;
    const std::string path = image.write_raw("disc.bin", 8);

    Disc disc;
    REQUIRE(disc.load(path));
    CHECK(disc.loaded());
    CHECK(disc.sector_count() == 8);
    CHECK(disc.tracks.size() == 1);

    std::array<u8, Disc::RAW_SECTOR_SIZE> sector{};
    REQUIRE(disc.read_sector(5, sector));
    CHECK(sector[0] == 5);
    CHECK(sector[Disc::RAW_SECTOR_SIZE - 1] == 5);

    // Past the end of the disc is not a sector of zeroes; it is
    // nothing, and the drive has to be able to tell the difference.
    CHECK_FALSE(disc.read_sector(8, sector));
}

TEST_CASE("a 2048-byte image has its sync and header rebuilt")
{
    const Image image;
    const std::string path = image.write_cooked("disc.iso", 4);

    Disc disc;
    REQUIRE(disc.load(path));
    CHECK(disc.sector_count() == 4);

    std::array<u8, Disc::RAW_SECTOR_SIZE> sector{};
    REQUIRE(disc.read_sector(2, sector));

    // The sync pattern, then a header saying which sector this is.
    CHECK(sector[0] == 0x00);
    CHECK(sector[1] == 0xFF);
    CHECK(sector[Disc::SYNC_SIZE - 1] == 0x00);

    const Disc::Msf msf = msf_from_lba(2);
    CHECK(sector[Disc::HEADER_OFFSET] == msf.minute);
    CHECK(sector[Disc::HEADER_OFFSET + 1] == msf.second);
    CHECK(sector[Disc::HEADER_OFFSET + 2] == msf.frame);

    // And the payload where a Mode 2 Form 1 sector keeps it.
    CHECK(sector[Disc::MODE2_DATA_OFFSET] == 2);
}

TEST_CASE("a cue names the image and describes its tracks")
{
    const Image image;
    image.write_raw("game.bin", 100);
    const std::string cue = image.write_text("game.cue",
                                             "FILE \"game.bin\" BINARY\n"
                                             "  TRACK 01 MODE2/2352\n"
                                             "    INDEX 01 00:00:00\n"
                                             "  TRACK 02 AUDIO\n"
                                             "    INDEX 01 00:00:60\n");

    Disc disc;
    REQUIRE(disc.load(cue));
    REQUIRE(disc.tracks.size() == 2);

    CHECK(disc.tracks[0].number == 1);
    CHECK_FALSE(disc.tracks[0].audio);
    CHECK(disc.tracks[0].start_lba == 0);
    CHECK(disc.tracks[0].length_sectors == 60);

    CHECK(disc.tracks[1].number == 2);
    CHECK(disc.tracks[1].audio);
    CHECK(disc.tracks[1].start_lba == 60);

    // The second track's sectors come from further into the same file.
    std::array<u8, Disc::RAW_SECTOR_SIZE> sector{};
    REQUIRE(disc.read_sector(60, sector));
    CHECK(sector[0] == 60);
}

TEST_CASE("a cue whose tracks are each in their own file keeps the first")
{
    // How a dump with Redbook audio is usually written: the data track
    // in one file and every audio track in another. Only the data
    // track is needed to boot, and taking it is better than refusing
    // the disc.
    const Image image;
    image.write_raw("track01.bin", 40);
    image.write_raw("track02.bin", 90);
    const std::string cue = image.write_text("game.cue",
                                             "FILE \"track01.bin\" BINARY\n"
                                             "  TRACK 01 MODE2/2352\n"
                                             "    INDEX 01 00:00:00\n"
                                             "FILE \"track02.bin\" BINARY\n"
                                             "  TRACK 02 AUDIO\n"
                                             "    INDEX 00 00:00:00\n"
                                             "    INDEX 01 00:02:00\n");

    Disc disc;
    REQUIRE(disc.load(cue));
    REQUIRE(disc.tracks.size() == 1);
    CHECK(disc.tracks[0].number == 1);
    CHECK(disc.sector_count() == 40);
}

TEST_CASE("a disc that is not there is refused rather than loaded empty")
{
    Disc disc;
    CHECK_FALSE(disc.load("/nonexistent/game.cue"));
    CHECK_FALSE(disc.loaded());

    std::array<u8, Disc::RAW_SECTOR_SIZE> sector{};
    CHECK_FALSE(disc.read_sector(0, sector));
}
