#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "types.h"

// Reading a game out of the zip it arrived in.
//
// A disc image is nearly always distributed compressed, and asking
// someone to unpack half a gigabyte by hand — and then to know which
// of the files that fall out is the one to open — is a worse first
// five minutes than the emulator deserves. So a zip can be handed
// straight to the drive.
//
// It is unpacked rather than read in place, and that is the whole
// design decision. A zip entry is one deflate stream with no restart
// points in it, so reading the byte at offset N means decompressing
// every byte before it; the drive seeks all over a 600 MB image, a
// couple of kilobytes at a time, and would pay that price on every
// seek.
//
// The unpacked copy goes in the system's temporary directory, and
// nothing here ever deletes it. That is not an oversight — it is why
// the temporary directory is the right place. Every system already
// empties it, at reboot or on a timer, and it is the one location a
// user is never surprised to lose something from. So the cost is one
// unpacking per game per reboot, and the cleaning up is someone else's
// job, done better than a policy here would do it: no age limit to
// pick, no size budget to argue with, and nothing of the user's ever
// deleted by this program.
namespace archive {

// Whether this path names a zip, by extension. Cheap and wrong only
// for a file that has been renamed, which the open below then reports.
bool is_zip(const std::filesystem::path& path);

// What a zip holds, in the order the archive lists it.
struct Entry {
    std::string name;
    u64 size = 0;
};

std::vector<Entry> list(const std::filesystem::path& zip);

// Unpacks the disc out of `zip` and returns the file to open — the cue
// if there is one, otherwise the only image there was. Empty if the
// archive could not be read or holds nothing that looks like a disc.
//
// Only what the drive can actually read is unpacked, which for a game
// with music on it is a small fraction of the archive: the cue, and
// the one image it names first. Unpacking is skipped entirely when the
// files are already there at the size the archive says, so a second
// launch before the next reboot costs nothing.
std::filesystem::path unpack(const std::filesystem::path& zip);

// Where unpacked games go: a directory of this program's own inside
// whatever the system uses for temporary files. Honours TMPDIR, since
// that is what temp_directory_path does.
std::filesystem::path unpack_directory();

}  // namespace archive
