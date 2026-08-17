#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include "types.h"

// Reading a disc out of the .chd it was compressed into.
//
// A CHD is the other way a game arrives, and the better one. Where a
// zip has to be unpacked before the drive can seek in it — one deflate
// stream with no way into the middle — a CHD is cut into hunks of a
// few sectors each, compressed separately, with an index in front
// saying where every one of them landed. So a sector in the middle
// costs decompressing the one hunk that holds it, and the image can be
// read where it lies, at about a third of the size of the bin it was
// made from.
//
// Underneath, the disc is a flat run of frames in track order: 2352
// bytes of sector followed by 96 of subcode, whatever the track's own
// sectors are. Nothing here reads the subcode; it is stepped over.
//
// The decompression is libchdr's, which is also what MAME and every
// other emulator that reads the format uses. Only the parts a
// PlayStation disc needs are wrapped: the track list, and bytes at an
// offset.
struct Chd {
    // What one sector occupies in the file, subcode included.
    static constexpr u32 FRAME_SIZE = 2448;

    // How the sector itself is stored when the track keeps it whole,
    // and when it keeps only a Mode 1 or Mode 2 Form 1 payload.
    static constexpr u32 RAW_SECTOR_SIZE = 2352;
    static constexpr u32 COOKED_SECTOR_SIZE = 2048;

    // Tracks are stored in groups of this many frames, each one padded
    // out to the boundary, so where a track's frames begin is not the
    // sum of the lengths before it.
    static constexpr u32 TRACK_PADDING = 4;

    struct Track {
        u32 number = 1;
        bool audio = false;

        // Frames the track holds, and where the first of them sits in
        // the file's run of frames.
        u32 frames = 0;
        u32 first_frame = 0;

        // A gap before the track that the cue described and the image
        // never stored. It counts in the disc's numbering all the same,
        // so the tracks after it sit that much further along.
        u32 pregap_frames = 0;

        // How much of each frame is the sector: the whole 2352 bytes,
        // or only the payload of a track written cooked.
        u32 sector_data_size = RAW_SECTOR_SIZE;
    };

    Chd();
    ~Chd();
    Chd(Chd&&) noexcept;
    Chd& operator=(Chd&&) noexcept;

    // Whether this path names a CHD, by extension.
    static bool is_chd(const std::filesystem::path& path);

    // Opens the file and reads its track list. False if it is not a
    // CHD, could not be decompressed, or is not a CD at all.
    bool open(const std::filesystem::path& path);

    bool is_open() const;

    // Fills `out` with `size` bytes at `offset` of the decompressed
    // image. False past the end of it, or if a hunk would not decode.
    bool read(u64 offset, u8* out, u32 size) const;

    std::vector<Track> tracks;

private:
    // The open file and the hunk last decompressed out of it, which
    // libchdr's types are kept inside of so its header stops here.
    struct File;
    std::unique_ptr<File> file;
};
