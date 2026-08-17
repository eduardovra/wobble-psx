#include "disc.h"

#include <algorithm>
#include <filesystem>
#include <sstream>

#include "archive.h"
#include "log.h"

namespace {

// The twelve bytes every raw sector starts with. Nothing reads them
// here, but a game that copies a sector out of the drive and looks at
// it expects them to be there.
constexpr std::array<u8, Disc::SYNC_SIZE> SYNC = {
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};

// What a synthesised header claims the sector is. Mode 2 is what a
// PlayStation disc uses, and its Form 1 subheader marks the sector as
// data rather than as audio or video to be routed elsewhere.
constexpr u8 MODE_2 = 2;
constexpr std::array<u8, 8> SUBHEADER_FORM1 = {0, 0, 0x08, 0, 0, 0, 0x08, 0};

// Lowercases and trims a CUE line, which is free-form enough that
// matching it any other way means matching every way it is written.
std::string normalised(const std::string& line)
{
    std::string out;
    for (const char c : line) {
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    const std::size_t first = out.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const std::size_t last = out.find_last_not_of(" \t\r\n");
    return out.substr(first, last - first + 1);
}

// The filename out of a FILE line, which is quoted when it has spaces
// in it and bare when it does not.
std::string quoted_field(const std::string& line)
{
    const std::size_t open = line.find('"');
    if (open != std::string::npos) {
        const std::size_t close = line.find('"', open + 1);
        if (close != std::string::npos) {
            return line.substr(open + 1, close - open - 1);
        }
    }
    std::istringstream words(line);
    std::string keyword;
    std::string name;
    words >> keyword >> name;
    return name;
}

// An mm:ss:ff timestamp as a CUE writes it — decimal, not BCD, which
// is the one place the two numberings are spelled differently.
u32 lba_from_timestamp(const std::string& text)
{
    u32 minute = 0;
    u32 second = 0;
    u32 frame = 0;
    if (std::sscanf(text.c_str(), "%u:%u:%u", &minute, &second, &frame) != 3) {
        return 0;
    }
    return (minute * 60 + second) * 75 + frame;
}

u32 sector_size_for(const std::string& mode)
{
    if (mode.find("/2048") != std::string::npos) {
        return Disc::COOKED_SECTOR_SIZE;
    }
    return Disc::RAW_SECTOR_SIZE;
}

}  // namespace

u8 to_bcd(u8 value)
{
    return static_cast<u8>(((value / 10) << 4) | (value % 10));
}

u8 from_bcd(u8 value)
{
    return static_cast<u8>(((value >> 4) * 10) + (value & 0x0F));
}

u32 lba_from_msf(Disc::Msf msf)
{
    const u32 minute = from_bcd(msf.minute);
    const u32 second = from_bcd(msf.second);
    const u32 frame = from_bcd(msf.frame);
    const u32 absolute = (minute * 60 + second) * 75 + frame;

    // Software addresses the disc from the start of the lead-in, and
    // an address inside it is not a sector anyone can read.
    if (absolute < Disc::LEAD_IN_SECTORS) {
        return 0;
    }
    return absolute - Disc::LEAD_IN_SECTORS;
}

Disc::Msf msf_from_lba(u32 lba)
{
    const u32 absolute = lba + Disc::LEAD_IN_SECTORS;
    Disc::Msf msf;
    msf.minute = to_bcd(static_cast<u8>(absolute / (60 * 75)));
    msf.second = to_bcd(static_cast<u8>((absolute / 75) % 60));
    msf.frame = to_bcd(static_cast<u8>(absolute % 75));
    return msf;
}

u32 Disc::sector_count() const
{
    u32 last = 0;
    for (const Track& track : tracks) {
        last = std::max(last, track.start_lba + track.length_sectors);
    }
    return last;
}

const Disc::Track* Disc::track_at(u32 lba) const
{
    for (const Track& track : tracks) {
        if (lba >= track.start_lba &&
            lba < track.start_lba + track.length_sectors) {
            return &track;
        }
    }
    return nullptr;
}

bool Disc::read_image(u64 offset, u8* out, u32 size) const
{
    if (compressed.is_open()) {
        return compressed.read(offset, out, size);
    }
    if (!image.is_open()) {
        return false;
    }

    image.clear();
    image.seekg(static_cast<std::streamoff>(offset));
    if (!image) {
        return false;
    }
    const auto wanted = static_cast<std::streamsize>(size);
    image.read(reinterpret_cast<char*>(out), wanted);
    return image.gcount() == wanted;
}

bool Disc::read_sector(u32 lba, std::array<u8, RAW_SECTOR_SIZE>& out) const
{
    const Track* track = track_at(lba);
    if (track == nullptr) {
        return false;
    }

    const u64 offset = track->image_offset +
        u64{lba - track->start_lba} * track->image_sector_size;

    if (track->image_data_size == RAW_SECTOR_SIZE) {
        return read_image(offset, out.data(), RAW_SECTOR_SIZE);
    }

    // A 2048-byte sector is only the payload, so the rest is rebuilt
    // around it: the sync pattern, a header saying which sector this
    // is, and a subheader marking it as data. The error correction
    // that would follow is left zero — nothing in the machine checks
    // it, and a drive that reported a sector as good is
    // indistinguishable from one whose ECC happened to agree.
    out = {};
    std::copy(SYNC.begin(), SYNC.end(), out.begin());
    const Msf msf = msf_from_lba(lba);
    out[HEADER_OFFSET] = msf.minute;
    out[HEADER_OFFSET + 1] = msf.second;
    out[HEADER_OFFSET + 2] = msf.frame;
    out[HEADER_OFFSET + 3] = MODE_2;
    std::copy(SUBHEADER_FORM1.begin(),
              SUBHEADER_FORM1.end(),
              out.begin() + SUBHEADER_OFFSET);

    return read_image(
        offset, out.data() + MODE2_DATA_OFFSET, COOKED_SECTOR_SIZE);
}

bool Disc::load_chd(const std::filesystem::path& path)
{
    if (!compressed.open(path)) {
        return false;
    }

    // A track begins where its frames do, which for a track whose gap
    // was stored with it is the gap and not the music — two seconds
    // ahead of where the cue for the same disc would put it. That
    // costs nothing while the sectors are only being read, since every
    // one of them is at the address it should be; it is the track
    // start reported to a game seeking to play the track that is
    // early, and there is no playing them yet.
    u32 lba = 0;
    for (const Chd::Track& stored : compressed.tracks) {
        // A gap the image never stored is still part of the disc, so
        // the track after it begins that much further along.
        lba += stored.pregap_frames;

        Track track;
        track.number = stored.number;
        track.audio = stored.audio;
        track.start_lba = lba;
        track.length_sectors = stored.frames;
        track.image_sector_size = Chd::FRAME_SIZE;
        track.image_data_size = stored.sector_data_size;
        track.image_offset = u64{stored.first_frame} * Chd::FRAME_SIZE;
        tracks.push_back(track);

        lba += stored.frames;
    }

    image_path = path.string();
    return true;
}

bool Disc::load(const std::string& path)
{
    tracks.clear();
    image.close();
    image.clear();
    compressed = Chd();
    image_path.clear();

    // A game usually arrives as a zip, and being handed one is not a
    // mistake to correct — it is the ordinary case. It is unpacked to
    // a cache and the disc inside it opened as if that had been given
    // in the first place.
    std::filesystem::path given(path);
    if (archive::is_zip(given)) {
        given = archive::unpack(given);
        if (given.empty()) {
            return false;
        }
    }

    // A CHD carries its own track list and is read compressed, so
    // none of the sizing below applies to it.
    if (Chd::is_chd(given)) {
        return load_chd(given);
    }

    std::filesystem::path binary = given;
    std::vector<Track> parsed;

    if (normalised(given.extension().string()) == ".cue") {
        std::ifstream cue(given);
        if (!cue) {
            log_message("disc: cannot open " + given.string());
            return false;
        }

        // Only the first FILE is followed. A multi-file cue puts each
        // audio track in its own image, which matters for playing them
        // and not for booting, so the data track is taken and the rest
        // reported rather than silently half-loaded.
        bool have_file = false;
        std::string line;
        Track current;
        bool in_track = false;
        while (std::getline(cue, line)) {
            const std::string text = normalised(line);
            if (text.starts_with("file ")) {
                if (have_file) {
                    log_message("disc: cue names more than one image; "
                                "only the first is used");
                    break;
                }
                binary = given.parent_path() / quoted_field(line);
                have_file = true;
            } else if (text.starts_with("track ")) {
                if (in_track) {
                    parsed.push_back(current);
                }
                current = {};
                in_track = true;

                std::istringstream words(text);
                std::string keyword;
                std::string number;
                std::string mode;
                words >> keyword >> number >> mode;
                current.number = static_cast<u32>(std::stoul(number));
                current.audio = mode.starts_with("audio");
                current.image_sector_size = sector_size_for(mode);
            } else if (text.starts_with("index 01") && in_track) {
                const std::size_t space = text.rfind(' ');
                current.start_lba = lba_from_timestamp(text.substr(space + 1));
            }
        }
        if (in_track) {
            parsed.push_back(current);
        }
    }

    if (parsed.empty()) {
        // A bare image, or a cue that described nothing: one data
        // track covering the whole file.
        Track only;
        only.image_sector_size = RAW_SECTOR_SIZE;
        parsed.push_back(only);
    }

    std::error_code failed;
    const auto size = std::filesystem::file_size(binary, failed);
    if (failed) {
        log_message(std::string("disc: cannot open ") + binary.string());
        return false;
    }

    // A bare .iso is normally 2048-byte sectors, and a .bin raw ones,
    // but the file itself is the better witness: whichever size
    // divides it is the one it was written with.
    if (parsed.size() == 1 && size % RAW_SECTOR_SIZE != 0 &&
        size % COOKED_SECTOR_SIZE == 0) {
        parsed[0].image_sector_size = COOKED_SECTOR_SIZE;
    }

    // Where each track sits in the image, and how long it is. A track
    // runs to the start of the next one, and the last to the end of
    // the file.
    u64 offset = 0;
    for (std::size_t i = 0; i < parsed.size(); i++) {
        Track& track = parsed[i];
        // A file holds nothing but the sectors themselves, so one
        // costs exactly as much of it as the sector is.
        track.image_data_size = track.image_sector_size;
        track.image_offset = offset;
        u32 end = 0;
        if (i + 1 < parsed.size()) {
            end = parsed[i + 1].start_lba;
        } else {
            end = static_cast<u32>(size / track.image_sector_size);
        }
        if (end < track.start_lba) {
            log_message("disc: cue tracks are not in order");
            return false;
        }
        track.length_sectors = end - track.start_lba;
        offset += u64{track.length_sectors} * track.image_sector_size;
    }

    image.open(binary, std::ios::binary);
    if (!image) {
        log_message(std::string("disc: cannot open ") + binary.string());
        return false;
    }

    tracks = std::move(parsed);
    image_path = binary.string();
    return true;
}
