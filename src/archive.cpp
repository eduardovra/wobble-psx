#include "archive.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <string_view>

#include <miniz.h>

#include "log.h"

namespace {

constexpr std::string_view CUE = ".cue";

std::string lowercased(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

std::string extension_of(const std::string& name)
{
    return lowercased(std::filesystem::path(name).extension().string());
}

bool is_cue(const std::string& name) { return extension_of(name) == CUE; }

bool is_disc_image(const std::string& name)
{
    const std::string extension = extension_of(name);
    return extension == ".bin" || extension == ".iso" || extension == ".img";
}

// A zip entry names a path, and nothing stops it naming one outside
// where it is being unpacked. Only the last component is kept, so an
// archive cannot write anywhere but the directory it was given.
std::string safe_name(const std::string& name)
{
    return std::filesystem::path(name).filename().string();
}

// Whether `path` already holds `size` bytes, which is what lets a
// second launch skip the unpacking.
bool already_there(const std::filesystem::path& path, u64 size)
{
    std::error_code failed;
    const auto actual = std::filesystem::file_size(path, failed);
    return !failed && actual == size;
}

// The image named by the first FILE line of a cue.
//
// Deliberately the first and not all of them: Disc::load follows the
// first FILE too, because the rest of a multi-file cue is the Redbook
// audio, which nothing here can play yet. The two choices have to
// agree — unpacking less than the drive reads would break it — so if
// one of them learns to follow every track, so must the other.
std::string first_image_in(const std::filesystem::path& cue)
{
    std::ifstream file(cue);
    std::string line;
    while (std::getline(file, line)) {
        if (!lowercased(line).starts_with("file ")) {
            continue;
        }
        const std::size_t open = line.find('"');
        if (open == std::string::npos) {
            continue;
        }
        const std::size_t close = line.find('"', open + 1);
        if (close != std::string::npos) {
            return line.substr(open + 1, close - open - 1);
        }
    }
    return "";
}

}  // namespace

namespace archive {

bool is_zip(const std::filesystem::path& path)
{
    return lowercased(path.extension().string()) == ".zip";
}

std::vector<Entry> list(const std::filesystem::path& zip)
{
    mz_zip_archive reader{};
    if (!mz_zip_reader_init_file(&reader, zip.string().c_str(), 0)) {
        return {};
    }

    std::vector<Entry> entries;
    const mz_uint count = mz_zip_reader_get_num_files(&reader);
    for (mz_uint i = 0; i < count; i++) {
        mz_zip_archive_file_stat info{};
        if (!mz_zip_reader_file_stat(&reader, i, &info)) {
            continue;
        }
        if (mz_zip_reader_is_file_a_directory(&reader, i)) {
            continue;
        }
        entries.push_back({info.m_filename, info.m_uncomp_size});
    }

    mz_zip_reader_end(&reader);
    return entries;
}

std::filesystem::path unpack_directory()
{
    std::error_code failed;
    const auto temporary = std::filesystem::temp_directory_path(failed);
    if (failed) {
        return "wobble-psx";
    }
    return temporary / "wobble-psx";
}

std::filesystem::path unpack(const std::filesystem::path& zip)
{
    const std::vector<Entry> entries = list(zip);
    if (entries.empty()) {
        log_message(std::format("archive: nothing readable in {}",
                                zip.filename().string()));
        return {};
    }

    // Where this game goes. Named for the archive, so two games do not
    // share a directory and the same game finds its own again.
    const std::filesystem::path directory =
        unpack_directory() / zip.stem().string();
    std::error_code failed;
    std::filesystem::create_directories(directory, failed);
    if (failed) {
        log_message(
            std::format("archive: cannot write to {}", directory.string()));
        return {};
    }

    mz_zip_archive reader{};
    if (!mz_zip_reader_init_file(&reader, zip.string().c_str(), 0)) {
        return {};
    }

    bool said_so = false;
    auto extract = [&](const Entry& entry) {
        const std::filesystem::path target = directory / safe_name(entry.name);
        if (already_there(target, entry.size)) {
            return true;
        }
        if (!said_so) {
            log_message(std::format("archive: unpacking {} — once, until the "
                                    "system next clears its temporary files",
                                    zip.filename().string()));
            said_so = true;
        }
        if (mz_zip_reader_extract_file_to_file(
                &reader, entry.name.c_str(), target.string().c_str(), 0)) {
            return true;
        }
        log_message(std::format("archive: could not unpack {}", entry.name));
        return false;
    };

    // The cues first, and all of them: they are a few hundred bytes
    // each and one of them says which image is worth the disk.
    const Entry* cue = nullptr;
    for (const Entry& entry : entries) {
        if (!is_cue(entry.name)) {
            continue;
        }
        if (!extract(entry)) {
            mz_zip_reader_end(&reader);
            return {};
        }
        if (cue == nullptr) {
            cue = &entry;
        }
    }

    // Then the one image the drive will read. A game with music on it
    // keeps its audio tracks in the archive, where they take no room
    // and nothing yet can play them from anyway.
    std::string wanted;
    if (cue != nullptr) {
        wanted = first_image_in(directory / safe_name(cue->name));
    }

    bool have_image = false;
    for (const Entry& entry : entries) {
        if (!is_disc_image(entry.name)) {
            continue;
        }
        const bool named =
            !wanted.empty() && safe_name(entry.name) == safe_name(wanted);
        // With no cue to name one, the archive holds a bare image and
        // the first is as good as it gets.
        if (!named && (cue != nullptr || have_image)) {
            continue;
        }
        if (!extract(entry)) {
            mz_zip_reader_end(&reader);
            return {};
        }
        have_image = true;
    }
    mz_zip_reader_end(&reader);

    // The cue describes the whole disc and names the image beside it,
    // so it is what the drive should be given when there is one.
    if (cue != nullptr) {
        return directory / safe_name(cue->name);
    }
    for (const Entry& entry : entries) {
        if (is_disc_image(entry.name)) {
            return directory / safe_name(entry.name);
        }
    }

    log_message(
        std::format("archive: no disc image in {}", zip.filename().string()));
    return {};
}

}  // namespace archive
