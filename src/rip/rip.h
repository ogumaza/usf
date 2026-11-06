// SPDX-License-Identifier: MIT

// The rip of a game's tracks: each track runs from the snapshot for its length analysis and its coverage, the set is
// built from what the runs read, and each written mini is played back and compared with its run.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "xsflib/psf/psf.h"
#include "xsflib/rip/length.h"
#include "xsflib/sha256.h"

#include "rip/profile.h"
#include "rip/snapshot.h"
#include "rip/trim.h"

namespace usf::rip
{

struct RunSettings
{
    // Analyze the length of each track that the profile gives no length.
    bool lengths = true;

    // Record coverage, for a trimmed set.
    bool coverage = true;

    // Hash the output over the length and the fade, for the check of the written minis (CheckTrack).
    bool reference = true;

    // Seconds of play after the end of a track's fade that the coverage still covers. A looping track also runs for
    // half its length more: at least one more pass of its loop.
    double margin_seconds = 10.0;

    // How long a track without a length runs for its coverage.
    double unknown_length_seconds = 600.0;
};

// What one track's run found.
struct TrackRun
{
    std::optional<xsflib::rip::Timing> timing;
    TrackWords words;

    // The hashes of the run's output over its length and fade (see SampleHashes), and how many frames that is.
    std::vector<xsflib::Sha256::Digest> reference;
    uint64_t check_frames = 0;
};

// Runs `track` from the snapshot. A track that needs no length analysis, no coverage and no reference doesn't run.
TrackRun RunTrack(const GameProfile& profile, const Snapshot& snapshot, const Track& track,
                  const RunSettings& settings);

// Plays the mini at `path` with its library and compares its output with the run's. Returns the difference, or
// nothing when they're the same.
std::optional<std::string> CheckTrack(const std::string& path, const TrackRun& run);

// The tags of a set's files.
struct SetTags
{
    xsflib::psf::Tags common; // Every mini's: game, artist, year, copyright, usfby, utf8.
    std::string ripper;       // The value of usfrip: the ripper's name and version.
};

// The files of a set: the library, and the mini of the track `track`, the `index`th of the set from 0. The library
// holds the flags and usfrip. A mini holds them too, with _lib (`library_name`), the common tags, the title, the
// track number, the length and fade of `timing`, and usfdriver.
std::vector<uint8_t> LibraryFile(const GameProfile& profile, const Sections& library, const SetTags& tags);
std::vector<uint8_t> MiniFile(const GameProfile& profile, const Sections& sections, const Track& track, size_t index,
                              const std::optional<xsflib::rip::Timing>& timing, const std::string& library_name,
                              const SetTags& tags);

} // namespace usf::rip
