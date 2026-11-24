// SPDX-License-Identifier: MIT

#include "rip/rip.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <span>

#include "ultralib/machine/save_state.h"
#include "ultralib/machine/system.h"
#include "xsflib/bytes.h"
#include "xsflib/rip/length_tags.h"

#include "rip/length.h"
#include "rip/player.h"
#include "usf/usf_file.h"

namespace usf::rip
{
namespace
{

constexpr double kMsPerSecond = 1000.0;

// The two flags' names, in the format's spelling.
constexpr const char* kEnableCompare = "_enablecompare";
constexpr const char* kEnableFifoFull = "_enableFIFOfull";

void AddFlags(const GameProfile& profile, xsflib::psf::Tags& tags)
{
    if (profile.enable_compare)
    {
        tags[kEnableCompare] = "1";
    }
    if (profile.enable_fifo_full)
    {
        tags[kEnableFifoFull] = "1";
    }
}

std::string TimeText(uint64_t frames, uint32_t rate)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.3f s", static_cast<double>(frames) / rate);

    return text;
}

// How long a track runs, in milliseconds.
struct RunSpan
{
    uint64_t play_ms = 0; // The length and the fade. The reference covers them.
    uint64_t end_ms = 0;  // Where the run stops.
};

// The time from the start of a track to the end of its fade, as a player runs it: the end of its length tags
// (xsflib::rip::LengthTags::end_ms). A timing that the profile sets has no first sample before its run. It plays its
// length, then its fade, or the fade of a track that ends.
uint64_t PlayMs(const xsflib::rip::Timing& timing)
{
    if (const std::optional<xsflib::rip::LengthTags> tags = xsflib::rip::TagsOfTiming(timing, kLengthFrom, 0.0))
    {
        return static_cast<uint64_t>(tags->end_ms);
    }

    return static_cast<uint64_t>(timing.length_ms + (timing.fade_ms > 0 ? timing.fade_ms : xsflib::rip::kEndFadeMs));
}

// The coverage runs on past the end of the fade: for a looping track, half its length more, at least one pass of its
// loop; and the margin for every track. The reference ends with the fade.
RunSpan SpanOf(const std::optional<xsflib::rip::Timing>& timing, const RunSettings& settings)
{
    uint64_t play_ms = static_cast<uint64_t>(settings.unknown_length_seconds * kMsPerSecond);
    uint64_t run_ms = play_ms;
    if (timing)
    {
        play_ms = PlayMs(*timing);
        run_ms = play_ms + (timing->fade_ms > 0 ? static_cast<uint64_t>(timing->length_ms) / 2 : 0);
    }

    run_ms += static_cast<uint64_t>(settings.margin_seconds * kMsPerSecond);
    const uint64_t end_ms = settings.coverage ? run_ms : settings.reference ? play_ms : 0;

    return {.play_ms = play_ms, .end_ms = end_ms};
}

// Completes the reference: renders on until `hashes` covers the frames of the first `play_ms` milliseconds, then keeps
// the hashes of those frames in `run`.
template <typename OnSamples>
void FinishReference(MsRenderer& renderer, const OnSamples& on_samples, const SampleHashes& hashes,
                     const std::vector<uint64_t>& frames_at_ms, uint64_t play_ms, TrackRun& run)
{
    run.check_frames = play_ms == 0 ? 0 : frames_at_ms[play_ms - 1];
    const uint64_t blocks = (run.check_frames + SampleHashes::kBlockFrames - 1) / SampleHashes::kBlockFrames;
    while (hashes.Blocks().size() < blocks)
    {
        on_samples(renderer.Next());
    }

    run.reference.assign(hashes.Blocks().begin(), hashes.Blocks().begin() + static_cast<std::ptrdiff_t>(blocks));
}

// The sounds of a run that complete a timing that the profile sets, as the length analysis finds them in the timings
// that it measures: the first sample that isn't 0, the first millisecond with a sample over -72 dB, and the end of the
// last such millisecond up to the profile's length.
class RunSounds
{
public:
    explicit RunSounds(long long length_ms) : length_ms_(length_ms)
    {
    }

    void Add(const MsRenderer& renderer, std::span<const int16_t> samples)
    {
        if (!sample_ms_)
        {
            sample_ms_ = renderer.FirstSampleMs();
        }

        const auto ms = static_cast<long long>(renderer.Ms() - 1);
        if (ms < length_ms_ && xsflib::rip::Audible(samples))
        {
            first_sound_ms_ = first_sound_ms_.value_or(ms);
            end_ms_ = ms + 1;
        }
    }

    bool Found() const
    {
        return sample_ms_.has_value();
    }

    // Sets the first sounds of `timing`. A track that ends ends at its last sound. A track that made no sound is
    // silent.
    void SetIn(xsflib::rip::Timing& timing) const
    {
        if (!sample_ms_)
        {
            timing.silent = true;
            return;
        }

        timing.first_sample_ms = static_cast<long long>(*sample_ms_ + 0.5);
        timing.first_sound_ms = first_sound_ms_;
        if (timing.fade_ms == 0 && end_ms_ > 0)
        {
            timing.length_ms = std::min(timing.length_ms, end_ms_);
        }
    }

private:
    long long length_ms_ = 0;
    std::optional<double> sample_ms_;
    std::optional<long long> first_sound_ms_;
    long long end_ms_ = 0;
};

} // namespace

TrackRun RunTrack(const GameProfile& profile, const Snapshot& snapshot, const Track& track, const RunSettings& settings)
{
    TrackRun run;
    if (track.length_seconds)
    {
        run.timing = xsflib::rip::Timing{std::llround(*track.length_seconds * kMsPerSecond),
                                         std::llround(track.fade_seconds.value_or(0.0) * kMsPerSecond)};
    }

    // A timing that the profile sets gets its first sounds from the run.
    const bool set_timing = run.timing.has_value();
    const bool analyze = !set_timing && settings.lengths;
    if (!set_timing && !analyze && !settings.coverage && !settings.reference)
    {
        return run;
    }

    PlayerOptions options;
    options.enable_compare = profile.enable_compare;
    options.enable_fifo_full = profile.enable_fifo_full;
    options.coverage = settings.coverage;
    options.loop_watches = profile.loop_watches;

    const std::vector<uint8_t> state = TrackState(snapshot, track);
    TrackPlayer player(snapshot.rom, state, options);
    MsRenderer renderer(player);

    // For the reference, every millisecond is hashed, and the frames at its end are kept. The check then covers the
    // frames of the length and the fade exactly.
    SampleHashes hashes;
    std::vector<uint64_t> frames_at_ms;
    RunSounds run_sounds(set_timing ? run.timing->length_ms : 0);
    const auto on_samples = [&](std::span<const int16_t> samples)
    {
        run_sounds.Add(renderer, samples);
        if (settings.reference)
        {
            hashes.Add(samples);
            frames_at_ms.push_back(player.Frames());
        }
    };

    if (analyze)
    {
        run.timing = MeasureTrack(renderer, on_samples, settings.limit);
    }

    // A timing that the profile sets runs at least to the end of its length, and on to its first sample, up to the end
    // of its fade.
    const RunSpan span = SpanOf(run.timing, settings);
    const auto set_timing_runs = [&]
    {
        const auto ms = static_cast<long long>(renderer.Ms());
        return set_timing && (ms < run.timing->length_ms || (!run_sounds.Found() && renderer.Ms() < span.play_ms));
    };
    while (renderer.Ms() < span.end_ms || set_timing_runs())
    {
        on_samples(renderer.Next());
    }

    if (set_timing)
    {
        run_sounds.SetIn(*run.timing);
    }

    if (settings.reference)
    {
        FinishReference(renderer, on_samples, hashes, frames_at_ms, span.play_ms, run);
    }

    run.board_accesses = player.System().BoardAccesses();

    if (settings.coverage)
    {
        run.words = WordsOf(*player.System().GetCoverage(), ultralib::machine::RomImageSize(snapshot.rom.size()),
                            xsflib::LoadLe32(snapshot.state.data() + ultralib::machine::kStateRdramSizeOffset));
    }

    return run;
}

std::optional<std::string> CheckTrack(const std::string& path, const TrackRun& run)
{
    LoadedUsf loaded;
    if (auto error = LoadUsf(path, xsflib::psf::ReadWholeFile, loaded))
    {
        return error;
    }

    PlayerOptions options;
    options.enable_compare = loaded.enable_compare;
    options.enable_fifo_full = loaded.enable_fifo_full;
    TrackPlayer player(loaded.rom, loaded.state, options);

    SampleHashes hashes;
    std::vector<int16_t> samples(2 * SampleHashes::kBlockFrames);
    for (size_t block = 0; block < run.reference.size(); block++)
    {
        player.Render(samples);
        hashes.Add(samples);
        if (hashes.Blocks()[block] != run.reference[block])
        {
            return "its output differs from the untrimmed state's from " +
                   TimeText(block * SampleHashes::kBlockFrames, player.SampleRate()) + " on";
        }
    }

    return std::nullopt;
}

std::vector<uint8_t> LibraryFile(const GameProfile& profile, const Sections& library, const SetTags& tags)
{
    xsflib::psf::Tags library_tags;
    AddFlags(profile, library_tags);
    library_tags["usfrip"] = tags.ripper;

    return WriteUsf(library, library_tags);
}

std::vector<uint8_t> MiniFile(const GameProfile& profile, const Sections& sections, uint32_t number,
                              const std::string& library_name, const std::string& ripper, xsflib::psf::Tags tags)
{
    tags["_lib"] = library_name;
    AddFlags(profile, tags);
    tags["track"] = std::to_string(number);
    tags["usfrip"] = ripper;
    if (!profile.sound_driver.empty())
    {
        tags["usfdriver"] = profile.sound_driver;
    }

    return WriteUsf(sections, tags);
}

} // namespace usf::rip
