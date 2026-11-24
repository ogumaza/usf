// SPDX-License-Identifier: MIT

// The rip of a game's tracks: each track runs from the snapshot for its length analysis and its coverage, the set is
// built from what the runs read, and each written mini is played back and compared with its run.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/psf/psf.h"
#include "xsflib/rip/length.h"
#include "xsflib/rip/length_tags.h"
#include "xsflib/sha256.h"

#include "rip/length.h"
#include "rip/profile.h"
#include "rip/snapshot.h"
#include "rip/trim.h"

namespace usf::rip
{

// Where the length tags of a mini count from: the first sound, where usf's players start their count
// (UniformRatePlayer::kLengthOrigin).
inline constexpr xsflib::rip::LengthFrom kLengthFrom = xsflib::rip::LengthFrom::kFirstSound;

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
    double unknown_length_seconds = kMaxTrackSeconds;

    // The limit of the length analysis: the long limit for music, the default for a set of sound effects.
    xsflib::rip::Limit limit = xsflib::rip::Limit::kDefault;
};

// What one track's run found.
struct TrackRun
{
    std::optional<xsflib::rip::Timing> timing;
    TrackWords words;

    // The hashes of the run's output over its length and fade (see SampleHashes), and how many frames that is.
    std::vector<xsflib::Sha256::Digest> reference;
    uint64_t check_frames = 0;

    // The CPU's reads and writes above physical 0x80000000 during the run. A console has nothing there, and neither has
    // a legacy player. An arcade board has its RAM and inputs there.
    uint64_t board_accesses = 0;
};

// Runs `track` from the snapshot. A length that the profile sets gets the first sounds of the run, and the run goes on
// at least to the first sample. A track without one doesn't run when the settings turn off the length analysis, the
// coverage and the reference.
TrackRun RunTrack(const GameProfile& profile, const Snapshot& snapshot, const Track& track,
                  const RunSettings& settings);

// Plays the mini at `path` with its library and compares its output with the run's. Returns the difference, or nothing
// when they're the same.
std::optional<std::string> CheckTrack(const std::string& path, const TrackRun& run);

// The jobs of the tracks that make a sound, each with its position in that list. A track that makes no sound gets no
// mini. A job has the members `timing`, an optional xsflib::rip::Timing, and `position`.
template <typename Job>
std::vector<Job> SoundingJobs(std::vector<Job> jobs)
{
    std::vector<Job> sounding;
    for (Job& job : jobs)
    {
        if (!job.timing || !job.timing->silent)
        {
            job.position = sounding.size();
            sounding.push_back(std::move(job));
        }
    }

    return sounding;
}

// The tags of a set's files.
struct SetTags
{
    xsflib::psf::Tags common; // Every mini's: game, artist, year, copyright, usfby, utf8.
    std::string ripper;       // The value of usfrip: the ripper's name and version.
};

// The files of a set: the library, and the mini of the track `number`, from 1. The library contains the flags and
// usfrip. A mini contains the tags `tags` that xsflib's WriteMinis gives it: the common tags, the title, and the length
// and fade of a track with a timing. It contains the flags and usfrip (`ripper`) too, with _lib (`library_name`), the
// track number and usfdriver.
std::vector<uint8_t> LibraryFile(const GameProfile& profile, const Sections& library, const SetTags& tags);
std::vector<uint8_t> MiniFile(const GameProfile& profile, const Sections& sections, uint32_t number,
                              const std::string& library_name, const std::string& ripper, xsflib::psf::Tags tags);

} // namespace usf::rip
