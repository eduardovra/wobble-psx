#pragma once

#include <array>
#include <fstream>
#include <string>
#include <vector>

#include "types.h"

// A disc in the drive.
//
// A PlayStation disc is not a file of 2048-byte blocks. It is a track
// of 2352-byte sectors, each carrying a sync pattern, a header saying
// where on the disc it is, and only then its payload — which is why
// the canonical dump is a BIN/CUE pair and not an ISO. Games rely on
// the difference: the subheader distinguishes a data sector from an
// audio one interleaved with it, and the CUE is the only place the
// audio tracks are described at all.
//
// So sectors are served raw, at their full 2352 bytes, whatever the
// image on disk actually holds. An ISO of bare 2048-byte blocks is
// accepted and its missing sync and header synthesised, because a
// homebrew image built that way should still boot.
//
// The image is read from the file as the drive asks for sectors rather
// than held in memory: a full disc is several hundred megabytes, and
// the drive never wants more than one sector at a time.
struct Disc {
    static constexpr u32 RAW_SECTOR_SIZE = 2352;
    static constexpr u32 COOKED_SECTOR_SIZE = 2048;

    // Where the parts of a raw sector begin. A Mode 2 Form 1 sector —
    // which is what a PlayStation disc is made of — puts an 8-byte
    // subheader between the header and the payload, so its data starts
    // eight bytes later than a Mode 1 sector's does.
    static constexpr u32 SYNC_SIZE = 12;
    static constexpr u32 HEADER_OFFSET = 12;
    static constexpr u32 SUBHEADER_OFFSET = 16;
    static constexpr u32 MODE1_DATA_OFFSET = 16;
    static constexpr u32 MODE2_DATA_OFFSET = 24;

    // The lead-in the numbering starts after: sector zero of the data
    // is two seconds in, so an address in minutes and seconds is 150
    // sectors ahead of the index it reads from.
    static constexpr u32 LEAD_IN_SECTORS = 150;

    // A minute, second and frame as the drive speaks them: three
    // BCD bytes, which is how Setloc arrives and how a sector's own
    // header describes itself.
    struct Msf {
        u8 minute = 0;
        u8 second = 0;
        u8 frame = 0;
    };

    struct Track {
        u32 number = 1;
        bool audio = false;

        // Where the track begins, counted from the start of the data.
        u32 start_lba = 0;
        u32 length_sectors = 0;

        // How the image stores it, which is not always how the drive
        // serves it — a 2048-byte image is padded back up to 2352 on
        // the way out.
        u32 image_sector_size = RAW_SECTOR_SIZE;
        u64 image_offset = 0;
    };

    // Opens a .cue and the image it names, or a bare .bin/.iso as a
    // single data track. False if nothing could be opened or the image
    // is not a whole number of sectors.
    bool load(const std::string& path);

    bool loaded() const { return !tracks.empty(); }

    // Sectors on the disc, lead-in excluded.
    u32 sector_count() const;

    // The track an address falls in, or null past the end of the disc.
    const Track* track_at(u32 lba) const;

    // Fills `out` with the sector at `lba`, synthesising the sync and
    // header for an image that does not carry them. False past the end
    // of the disc, leaving `out` untouched.
    bool read_sector(u32 lba, std::array<u8, RAW_SECTOR_SIZE>& out) const;

    std::vector<Track> tracks;

    // Kept open for the life of the disc. Mutable because reading a
    // sector is a seek and a read on it, and a disc being read is not
    // a disc being changed.
    mutable std::ifstream image;
    std::string image_path;
};

// The two numberings the drive uses, and the conversion between them.
// Both take and return BCD, since that is the only form software ever
// sees them in.
u32 lba_from_msf(Disc::Msf msf);
Disc::Msf msf_from_lba(u32 lba);

u8 to_bcd(u8 value);
u8 from_bcd(u8 value);
