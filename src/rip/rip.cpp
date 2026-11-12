// SPDX-License-Identifier: MIT

#include "rip/rip.h"

#include <cmath>
#include <cstddef>
#include <cstdio>

#include "xsflib/bytes.h"

#include "n64/save_state.h"
#include "n64/system.h"
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

// The coverage runs on past the end of the fade: for a looping track, half its length more, at least one pass of its
// loop; and the margin for every track. The reference ends with the fade.
RunSpan SpanOf(const std::optional<xsflib::rip::Timing>& timing, const RunSettings& settings)
{
    uint64_t play_ms = static_cast<uint64_t>(settings.unknown_length_seconds * kMsPerSecond);
    uint64_t run_ms = play_ms;
    if (timing)
    {
        play_ms = static_cast<uint64_t>(timing->length_ms + timing->fade_ms);
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

} // namespace

TrackRun RunTrack(const GameProfile& profile, const Snapshot& snapshot, const Track& track, const RunSettings& settings)
{
    TrackRun run;
    if (track.length_seconds)
    {
        run.timing = xsflib::rip::Timing{std::llround(*track.length_seconds * kMsPerSecond),
                                         std::llround(track.fade_seconds.value_or(0.0) * kMsPerSecond)};
    }

    const bool analyze = !run.timing && settings.lengths;
    if (!analyze && !settings.coverage && !settings.reference)
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
    const auto on_samples = [&](std::span<const int16_t> samples)
    {
        if (settings.reference)
        {
            hashes.Add(samples);
            frames_at_ms.push_back(player.Frames());
        }
    };

    if (analyze)
    {
        run.timing = MeasureTrack(renderer, on_samples);
    }

    const RunSpan span = SpanOf(run.timing, settings);
    while (renderer.Ms() < span.end_ms)
    {
        on_samples(renderer.Next());
    }

    if (settings.reference)
    {
        FinishReference(renderer, on_samples, hashes, frames_at_ms, span.play_ms, run);
    }

    run.board_accesses = player.System().BoardAccesses();

    if (settings.coverage)
    {
        run.words = WordsOf(*player.System().GetCoverage(), n64::RomImageSize(snapshot.rom.size()),
                            xsflib::LoadLe32(snapshot.state.data() + n64::kStateRdramSizeOffset));
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
