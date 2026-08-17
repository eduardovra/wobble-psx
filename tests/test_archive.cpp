#include <array>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <doctest/doctest.h>
#include <miniz.h>

#include "archive.h"
#include "disc.h"

namespace {

// A zip built for the test to open again, with the temporary
// directory redirected so a run leaves nothing behind in the place a
// real one would use.
struct Archive {
    std::filesystem::path directory;
    std::string previous_tmpdir;
    bool had_tmpdir = false;

    Archive()
    {
        directory = std::filesystem::temp_directory_path() /
            ("wobble-archive-" + std::to_string(counter++));
        std::filesystem::create_directories(directory);

        const std::filesystem::path temporary = directory / "tmp";
        std::filesystem::create_directories(temporary);
        if (const char* existing = std::getenv("TMPDIR")) {
            previous_tmpdir = existing;
            had_tmpdir = true;
        }
        setenv("TMPDIR", temporary.string().c_str(), 1);
    }

    ~Archive()
    {
        if (had_tmpdir) {
            setenv("TMPDIR", previous_tmpdir.c_str(), 1);
        } else {
            unsetenv("TMPDIR");
        }
        std::filesystem::remove_all(directory);
    }

    Archive(const Archive&) = delete;
    Archive& operator=(const Archive&) = delete;

    // Writes a zip holding each of `files` as name and contents.
    std::filesystem::path write(
        const std::string& name,
        const std::vector<std::pair<std::string, std::vector<u8>>>& files) const
    {
        const std::filesystem::path path = directory / name;
        mz_zip_archive writer{};
        REQUIRE(mz_zip_writer_init_file(&writer, path.string().c_str(), 0));
        for (const auto& [entry, bytes] : files) {
            REQUIRE(mz_zip_writer_add_mem(&writer,
                                          entry.c_str(),
                                          bytes.data(),
                                          bytes.size(),
                                          MZ_DEFAULT_COMPRESSION));
        }
        REQUIRE(mz_zip_writer_finalize_archive(&writer));
        mz_zip_writer_end(&writer);
        return path;
    }

    // Sectors that say which one they are, as the disc tests use.
    static std::vector<u8> sectors(u32 count)
    {
        std::vector<u8> bytes;
        for (u32 lba = 0; lba < count; lba++) {
            bytes.insert(
                bytes.end(), Disc::RAW_SECTOR_SIZE, static_cast<u8>(lba));
        }
        return bytes;
    }

    static int counter;
};

int Archive::counter = 0;

std::vector<u8> text_of(const std::string& text)
{
    return {text.begin(), text.end()};
}

}  // namespace

TEST_CASE("a zip is recognised by its name")
{
    CHECK(archive::is_zip("game.zip"));
    CHECK(archive::is_zip("Game (USA).ZIP"));
    CHECK_FALSE(archive::is_zip("game.cue"));
}

TEST_CASE("what is in an archive can be listed without unpacking it")
{
    const Archive fixture;
    const auto zip =
        fixture.write("game.zip",
                      {{"game.cue", text_of("FILE \"game.bin\" BINARY\n")},
                       {"game.bin", Archive::sectors(4)}});

    const std::vector<archive::Entry> entries = archive::list(zip);
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].name == "game.cue");
    CHECK(entries[1].size == 4 * Disc::RAW_SECTOR_SIZE);
}

TEST_CASE("a zipped game is unpacked and the cue inside it opened")
{
    const Archive fixture;
    const auto zip = fixture.write("Some Game (USA).zip",
                                   {{"game.bin", Archive::sectors(10)},
                                    {"game.cue",
                                     text_of("FILE \"game.bin\" BINARY\n"
                                             "  TRACK 01 MODE2/2352\n"
                                             "    INDEX 01 00:00:00\n")}});

    // The drive is given the zip and finds the disc inside it.
    Disc disc;
    REQUIRE(disc.load(zip.string()));
    CHECK(disc.loaded());
    CHECK(disc.sector_count() == 10);

    std::array<u8, Disc::RAW_SECTOR_SIZE> sector{};
    REQUIRE(disc.read_sector(7, sector));
    CHECK(sector[0] == 7);
}

TEST_CASE("a zip holding only an image and no cue still opens")
{
    const Archive fixture;
    const auto zip =
        fixture.write("bare.zip", {{"bare.bin", Archive::sectors(3)}});

    Disc disc;
    REQUIRE(disc.load(zip.string()));
    CHECK(disc.sector_count() == 3);
}

TEST_CASE("unpacking twice does not unpack twice")
{
    const Archive fixture;
    const auto zip =
        fixture.write("again.zip", {{"again.bin", Archive::sectors(5)}});

    const std::filesystem::path first = archive::unpack(zip);
    REQUIRE(!first.empty());
    const auto written = std::filesystem::last_write_time(first);

    // The second call finds the file already there at the right size
    // and leaves it alone, which is what makes a second launch quick.
    const std::filesystem::path second = archive::unpack(zip);
    CHECK(second == first);
    CHECK(std::filesystem::last_write_time(first) == written);
}

TEST_CASE("an entry naming a path outside the directory cannot escape it")
{
    const Archive fixture;
    const auto zip = fixture.write(
        "nasty.zip", {{"../../escaped.bin", Archive::sectors(2)}});

    const std::filesystem::path unpacked = archive::unpack(zip);
    REQUIRE(!unpacked.empty());

    // Only the last component of the name is used, so the file lands
    // in the unpack directory and nowhere above it.
    CHECK(unpacked.filename() == "escaped.bin");
    CHECK(std::filesystem::exists(unpacked));
    CHECK(unpacked.string().find("..") == std::string::npos);
}

TEST_CASE("a zip with nothing that looks like a disc is refused")
{
    const Archive fixture;
    const auto zip =
        fixture.write("notes.zip", {{"readme.txt", text_of("nothing here")}});

    Disc disc;
    CHECK_FALSE(disc.load(zip.string()));
    CHECK_FALSE(disc.loaded());
}

TEST_CASE("only the tracks the drive reads are unpacked")
{
    // A game with music on it keeps its audio in the archive. Nothing
    // can play those tracks yet, and the drive follows only the first
    // FILE of a cue, so unpacking them would be several hundred
    // megabytes spent on nothing.
    const Archive fixture;
    const auto zip =
        fixture.write("Musical (USA).zip",
                      {{"Musical (Track 01).bin", Archive::sectors(4)},
                       {"Musical (Track 02).bin", Archive::sectors(50)},
                       {"Musical (Track 03).bin", Archive::sectors(50)},
                       {"Musical.cue",
                        text_of("FILE \"Musical (Track 01).bin\" BINARY\n"
                                "  TRACK 01 MODE2/2352\n"
                                "    INDEX 01 00:00:00\n"
                                "FILE \"Musical (Track 02).bin\" BINARY\n"
                                "  TRACK 02 AUDIO\n"
                                "    INDEX 01 00:00:00\n"
                                "FILE \"Musical (Track 03).bin\" BINARY\n"
                                "  TRACK 03 AUDIO\n"
                                "    INDEX 01 00:00:00\n")}});

    const std::filesystem::path cue = archive::unpack(zip);
    REQUIRE(!cue.empty());
    const std::filesystem::path directory = cue.parent_path();

    CHECK(std::filesystem::exists(directory / "Musical (Track 01).bin"));
    CHECK_FALSE(std::filesystem::exists(directory / "Musical (Track 02).bin"));
    CHECK_FALSE(std::filesystem::exists(directory / "Musical (Track 03).bin"));

    // And the disc still reads, which is the point of leaving them.
    Disc disc;
    REQUIRE(disc.load(zip.string()));
    CHECK(disc.sector_count() == 4);
}

TEST_CASE("unpacked games go under the system temporary directory")
{
    const Archive fixture;
    const auto zip =
        fixture.write("Where.zip", {{"where.bin", Archive::sectors(2)}});

    const std::filesystem::path unpacked = archive::unpack(zip);
    REQUIRE(!unpacked.empty());

    const std::string temporary =
        std::filesystem::temp_directory_path().string();
    CHECK(unpacked.string().starts_with(temporary));
}
