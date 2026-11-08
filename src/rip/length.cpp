// SPDX-License-Identifier: MIT

#include "rip/length.h"

#include <map>
#include <utility>

namespace usf::rip
{

namespace
{

constexpr uint64_t kMsPerSecond = 1000;

// The adapter that MeasureLength plays (see xsflib/rip/length.h). It follows the output alone. It sees no voices and
// no sequence player, so the sequence is always busy and never steady.
//
// A sequence player may loop each of its tracks separately. libultra's compressed sequence player is one. Each track
// that loops then jumps back once a pass. The jumps of one pass come one after another. MeasureLength would start its
// count again at each of them. The model therefore passes on the loops of one track alone: the first track that loops
// after the first sound. The other tracks' loops count for OthersLooped. A loop watch without a track register gives
// every loop to track 0. The model then passes them all on.
class OutputModel
{
public:
    OutputModel(MsRenderer& renderer, const std::function<void(std::span<const int16_t>)>& on_samples)
        : renderer_(renderer), on_samples_(on_samples)
    {
    }

    bool RunFrame()
    {
        const std::span<const int16_t> samples = renderer_.Next();
        on_samples_(samples);
        sounded_ = xsflib::rip::Audible(samples);

        TrackPlayer& player = renderer_.Player();
        for (const LoopEvent& loop : player.TakeLoops(player.Frames()))
        {
            CountLoop(loop.track, loop.target);
        }

        heard_ = heard_ || sounded_;

        return true;
    }

    // Whether every track other than `track` that has looped since the first sound has played its loop
    // kLengthLoops times.
    bool OthersLooped(int track) const
    {
        for (const auto& [other, passes] : passes_)
        {
            if (other != track && passes.count < xsflib::rip::kLengthLoops)
            {
                return false;
            }
        }

        return true;
    }

    bool Sounded() const
    {
        return sounded_;
    }

    bool Ringing() const
    {
        return false;
    }

    std::size_t VoiceCount() const
    {
        return 0;
    }

    xsflib::rip::VoicePlace Voice(std::size_t) const
    {
        return {};
    }

    bool Active() const
    {
        return false;
    }

    bool Settled() const
    {
        return false;
    }

    bool Stuck() const
    {
        return false;
    }

    bool Moving() const
    {
        return false;
    }

    uint64_t ChangedAt() const
    {
        return 0;
    }

    bool Busy() const
    {
        return true;
    }

    bool ChannelsChanging() const
    {
        return false;
    }

    int ActiveChannels() const
    {
        return 0;
    }

    template <typename F>
    void OnLoop(F on_loop)
    {
        on_loop_ = std::move(on_loop);
    }

private:
    // A track's loop and the passes it has made since the first sound. A track that moves on to another loop starts
    // its count again.
    struct Passes
    {
        uint32_t target = 0;
        int count = 0;
    };

    void CountLoop(int track, uint32_t target)
    {
        // MeasureLength ignores the loops that go round before the first sound. They choose no track either.
        if (heard_)
        {
            Passes& passes = passes_[track];
            passes.count = passes.target == target ? passes.count + 1 : 1;
            passes.target = target;
            if (!has_lead_)
            {
                lead_ = track;
                has_lead_ = true;
            }
        }

        if (on_loop_ && (!has_lead_ || track == lead_))
        {
            on_loop_(track, target);
        }
    }

    MsRenderer& renderer_;
    const std::function<void(std::span<const int16_t>)>& on_samples_;
    std::function<void(int, uint32_t)> on_loop_;
    bool sounded_ = false;
    bool heard_ = false; // Whether an earlier frame sounded.
    // The track whose loops MeasureLength counts, once has_lead_ is set. A track register can hold any value, such as a
    // channel's address in KSEG0. No track number can therefore mark a missing lead.
    int lead_ = 0;
    bool has_lead_ = false;
    std::map<int, Passes> passes_;
};

} // namespace

xsflib::rip::LengthRules GameLengthRules()
{
    xsflib::rip::LengthRules rules;
    rules.frame_ms = 1.0;
    rules.max_ms = 600'000.0;
    rules.max_frames = 600'000;
    rules.silent_frames = 180'000;
    rules.fade_ms = 10'000.0;
    rules.tail_ms = 500.0;

    return rules;
}

MsRenderer::MsRenderer(TrackPlayer& player) : player_(player)
{
}

std::span<const int16_t> MsRenderer::Next()
{
    remainder_ += player_.SampleRate();
    const uint64_t frames = remainder_ / kMsPerSecond;
    remainder_ %= kMsPerSecond;
    samples_.resize(2 * frames);
    player_.Render(samples_);
    ms_++;

    return samples_;
}

uint64_t MsRenderer::Ms() const
{
    return ms_;
}

TrackPlayer& MsRenderer::Player()
{
    return player_;
}

std::optional<xsflib::rip::Timing> MeasureTrack(MsRenderer& renderer,
                                                const std::function<void(std::span<const int16_t>)>& on_samples)
{
    OutputModel model(renderer, on_samples);

    return xsflib::rip::MeasureLength(model, GameLengthRules());
}

} // namespace usf::rip
