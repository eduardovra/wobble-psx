#include "chd.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>

#include <libchdr/chd.h>

#include "log.h"

namespace {

// The most tracks a CD can hold, and so the point at which looking for
// another one stops even if the file keeps answering.
constexpr u32 MAX_TRACKS = 99;

// A track's metadata is one line of text, and 256 bytes is comfortably
// more than the longest of the two formats can come to.
constexpr u32 METADATA_SIZE = 256;

constexpr u32 NO_HUNK = 0xFFFFFFFF;

// How much of a frame is the sector, which depends on how the track
// was written. The raw types keep the sync and header with it; the
// cooked ones keep only the payload and leave the rest to be rebuilt.
u32 sector_data_size_for(const std::string& type)
{
    if (type == "MODE1" || type == "MODE2_FORM1") {
        return Chd::COOKED_SECTOR_SIZE;
    }
    if (type == "MODE1_RAW" || type == "MODE2_RAW" || type == "AUDIO") {
        return Chd::RAW_SECTOR_SIZE;
    }

    // Mode 2 written without its sync but with its subheader, and the
    // Form 2 sectors of a Video CD. Neither is what a PlayStation game
    // is dumped as, and taking the frame whole at least gives the drive
    // the bytes that are there rather than refusing the disc.
    log_message("chd: track type " + type +
                " is not one this understands; its sectors are "
                "served as they are stored");
    return Chd::RAW_SECTOR_SIZE;
}

// One track's line of metadata, in either of the two forms it is
// written in — the second says what the gaps are, the first predates
// them being recorded at all.
bool parse_track(const char* text, Chd::Track& track)
{
    char type[32] = {};
    char subtype[32] = {};
    char pregap_type[32] = {};
    char pregap_subtype[32] = {};
    int number = 0;
    int frames = 0;
    int pregap = 0;
    int postgap = 0;

    const int fields = std::sscanf(text,
                                   "TRACK:%d TYPE:%31s SUBTYPE:%31s "
                                   "FRAMES:%d PREGAP:%d PGTYPE:%31s "
                                   "PGSUB:%31s POSTGAP:%d",
                                   &number,
                                   type,
                                   subtype,
                                   &frames,
                                   &pregap,
                                   pregap_type,
                                   pregap_subtype,
                                   &postgap);
    if (fields < 4 || number <= 0 || frames <= 0) {
        return false;
    }

    track.number = static_cast<u32>(number);
    track.audio = std::string(type) == "AUDIO";
    track.frames = static_cast<u32>(frames);
    track.sector_data_size = sector_data_size_for(type);

    // A gap written as "V" is inside the image and already counted in
    // the frames above. Any other kind was described by the cue and
    // then left out, so it exists only in the numbering.
    const bool pregap_stored = pregap_type[0] == 'V';
    if (fields >= 6 && pregap > 0 && !pregap_stored) {
        track.pregap_frames = static_cast<u32>(pregap);
    }
    return true;
}

}  // namespace

// The open file and the last hunk taken out of it. A hunk holds
// several frames, and the drive reads sectors in order, so the one
// just decompressed is nearly always the one wanted next.
struct Chd::File {
    chd_file* handle = nullptr;
    u32 hunk_bytes = 0;
    u32 hunk_count = 0;
    std::vector<u8> hunk;
    u32 cached_hunk = NO_HUNK;

    ~File()
    {
        if (handle != nullptr) {
            chd_close(handle);
        }
    }
};

Chd::Chd() = default;
Chd::~Chd() = default;
Chd::Chd(Chd&&) noexcept = default;
Chd& Chd::operator=(Chd&&) noexcept = default;

bool Chd::is_chd(const std::filesystem::path& path)
{
    std::string extension = path.extension().string();
    std::transform(extension.begin(),
                   extension.end(),
                   extension.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return extension == ".chd";
}

bool Chd::is_open() const { return file != nullptr && file->handle != nullptr; }

bool Chd::open(const std::filesystem::path& path)
{
    tracks.clear();
    file.reset();

    chd_file* handle = nullptr;
    const chd_error opened =
        chd_open(path.string().c_str(), CHD_OPEN_READ, nullptr, &handle);
    if (opened != CHDERR_NONE) {
        log_message("chd: cannot open " + path.string() + ": " +
                    chd_error_string(opened));
        return false;
    }

    auto opened_file = std::make_unique<File>();
    opened_file->handle = handle;

    const chd_header* header = chd_get_header(handle);
    opened_file->hunk_bytes = header->hunkbytes;
    opened_file->hunk_count = header->totalhunks;
    opened_file->hunk.resize(header->hunkbytes);

    // A CD's hunks are whole frames. A CHD of anything else — a hard
    // disk, a laserdisc — is not a disc this drive can take, and says
    // so here rather than by handing out sectors cut at the wrong
    // place. That it also has no track metadata is caught below.
    if (header->hunkbytes == 0 || header->hunkbytes % FRAME_SIZE != 0) {
        log_message("chd: " + path.string() + " is not a CD image");
        return false;
    }

    // Tracks are asked for one at a time until the file runs out of
    // them, since nothing in the header says how many there are.
    u32 first_frame = 0;
    for (u32 index = 0; index < MAX_TRACKS; index++) {
        char metadata[METADATA_SIZE] = {};
        chd_error found = chd_get_metadata(handle,
                                           CDROM_TRACK_METADATA2_TAG,
                                           index,
                                           metadata,
                                           METADATA_SIZE - 1,
                                           nullptr,
                                           nullptr,
                                           nullptr);
        if (found != CHDERR_NONE) {
            found = chd_get_metadata(handle,
                                     CDROM_TRACK_METADATA_TAG,
                                     index,
                                     metadata,
                                     METADATA_SIZE - 1,
                                     nullptr,
                                     nullptr,
                                     nullptr);
        }
        if (found != CHDERR_NONE) {
            break;
        }

        Track track;
        if (!parse_track(metadata, track)) {
            log_message("chd: track " + std::to_string(index + 1) + " of " +
                        path.string() + " could not be read");
            return false;
        }

        // Where the track's frames start. Each track is padded out to
        // a group boundary, so this is not the sum of the lengths
        // before it.
        track.first_frame = first_frame;
        const u32 padded =
            (track.frames + TRACK_PADDING - 1) / TRACK_PADDING * TRACK_PADDING;
        first_frame += padded;

        tracks.push_back(track);
    }

    if (tracks.empty()) {
        log_message("chd: " + path.string() + " holds no tracks");
        return false;
    }

    file = std::move(opened_file);
    return true;
}

bool Chd::read(u64 offset, u8* out, u32 size) const
{
    if (!is_open()) {
        return false;
    }

    // A sector is smaller than a hunk but need not sit inside one, so
    // what is wanted is taken a hunk at a time.
    while (size > 0) {
        const u64 hunk = offset / file->hunk_bytes;
        if (hunk >= file->hunk_count) {
            return false;
        }

        const auto wanted = static_cast<u32>(hunk);
        if (file->cached_hunk != wanted) {
            const chd_error decoded =
                chd_read(file->handle, wanted, file->hunk.data());
            if (decoded != CHDERR_NONE) {
                file->cached_hunk = NO_HUNK;
                log_message("chd: hunk " + std::to_string(wanted) +
                            " would not decode: " + chd_error_string(decoded));
                return false;
            }
            file->cached_hunk = wanted;
        }

        const auto within = static_cast<u32>(offset % file->hunk_bytes);
        const u32 taken = std::min(size, file->hunk_bytes - within);
        std::copy_n(file->hunk.begin() + within, taken, out);

        offset += taken;
        out += taken;
        size -= taken;
    }
    return true;
}
