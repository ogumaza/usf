// SPDX-License-Identifier: MIT

// The player: a USF track with its libraries, running on the core with the legacy players' flags and timing, as a
// stream of stereo frames at the game's sample rate.
//
//     usf::Player player;
//     if (!player.Load(path, xsflib::psf::ReadWholeFile) || !player.Start())
//     {
//         return player.Error();
//     }
//     const uint32_t rate = player.SampleRate();
//     const size_t got = player.Render(frames, count);
//
// Player is a PlayerBase of xsflib, so it has Load, Start, GetState, Error, GetTags, LengthMs, FadeMs and Volume from
// there. The caller applies the length, the fade and the volume. xsflib's Playback does that for a player whose rate
// stays the same from one Start to the next: a constant kSampleRate, or a kSampleRate of 0 with SampleRate(). A USF
// track's rate can change mid-stream. Player therefore has no kSampleRate. UniformRatePlayer gives Playback one rate
// for each track.
//
// The rate is the game's native rate. A game can change it mid-track, at the start of a buffer. Render stops there, so
// all the frames that one call returns have one rate.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "xsflib/player/player.h"
#include "xsflib/psf/psf.h"

#include "n64/system.h"
#include "usf/track.h"
#include "usf/usf_file.h"

namespace usf
{

class Player : public xsflib::player::PlayerBase<Player, LoadedUsf>
{
public:
    using Base = xsflib::player::PlayerBase<Player, LoadedUsf>;

    static constexpr const char* kNotPlayable = "the set holds no save state";
    static constexpr const char* kStartFailure = "the emulation failed to start: ";
    static constexpr const char* kStepFailure = "the emulation failed: ";

    // Loads the track at `path` with its libraries, as LoadUsf does. Bad sets are rejected here. The error tells why.
    static std::optional<std::string> LoadFile(const std::string& path, const xsflib::psf::FileReader& reader,
                                               LoadedUsf& set);

    Player();
    ~Player();

    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // Renders up to `frames` stereo frames (interleaved left and right) into `out`, which has room for `frames` frames.
    // Returns the frames written. All of them have the rate that SampleRate gave before the call. The count is less
    // than `frames` when the rate changes at that point, and 0 after an error or before Start. A track never ends by
    // itself, and a game that stops sending audio gives silence.
    std::size_t Render(int16_t* out, std::size_t frames);

    // The rate of the frames that the next Render returns, in Hz. It may run the emulation until the game sends audio.
    // Returns 0 after an error or before Start.
    uint32_t SampleRate();

    // The frames rendered since Start.
    uint64_t Position() const;

private:
    friend Base;

    struct RateChange
    {
        uint64_t frame = 0;
        uint32_t rate = 0;
    };

    // The hooks of PlayerBase.
    void Unload();
    bool Playable() const;
    bool StartSource();
    bool RunStep();
    bool Finished() const;
    std::vector<int16_t>& Output();

    // Runs the emulation until `frames` frames wait to be rendered. Returns false after an error.
    bool Buffer(std::size_t frames);

    uint64_t ProducedFrames() const;
    void AddSamples(std::span<const int16_t> samples, uint32_t rate);
    void DropPastRates(uint64_t frame);

    std::unique_ptr<n64::System> system_;
    std::vector<int16_t> samples_;
    std::vector<RateChange> rates_; // The rate of the next frame, then any changes after it.
    OutputClock clock_;
    uint32_t silence_rate_ = 0;
};

} // namespace usf
