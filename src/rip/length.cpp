// SPDX-License-Identifier: MIT

#include "rip/length.h"

#include <cstddef>
#include <map>
#include <optional>
#include <utility>

namespace usf::rip
{
namespace
{

constexpr uint64_t kMsPerSecond = 1000;

// The adapter that MeasureLength plays (see xsflib/rip/length.h). It follows the output alone. It reads no voices and
// no sequence player. The sequence is therefore always busy and never steady. It leaves out the tests from Ringing to
// ActiveChannels. MeasureLength then takes the answers of a model that follows only its output. The first sample that
// isn't 0 comes from the samples that a player renders too.
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

    // Whether every track other than `track` that has looped since the first sound has played its loop kLengthLoops
    // times.
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

    // The milliseconds from the start of the frame just run to its first sample that isn't 0.
    std::optional<double> FirstSampleMs() const
    {
        const std::optional<double> ms = renderer_.FirstSampleMs();
        if (!ms)
        {
            return std::nullopt;
        }

        return *ms - static_cast<double>(renderer_.Ms() - 1);
    }

    template <typename F>
    void OnLoop(F on_loop)
    {
        on_loop_ = std::move(on_loop);
    }

private:
    // A track's loop and the passes it has made since the first sound. A track that moves on to another loop starts its
    // count again.
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

    // The track whose loops MeasureLength counts, once has_lead_ is set. A track register can contain any value, such
    // as a channel's address in KSEG0. No track number can therefore mark a missing lead.
    int lead_ = 0;
    bool has_lead_ = false;

    std::map<int, Passes> passes_;
};

} // namespace

xsflib::rip::LengthRules GameLengthRules(xsflib::rip::Limit limit)
{
    xsflib::rip::LengthRules rules;
    rules.frame_ms = 1.0;
    rules.max_ms = kMaxTrackSeconds * static_cast<double>(kMsPerSecond);
    rules.max_frames = static_cast<uint64_t>(rules.max_ms / rules.frame_ms);
    rules.silent_frames = 180'000;

    return limit == xsflib::rip::Limit::kLong ? xsflib::rip::WithLongLimit(rules) : rules;
}

MsRenderer::MsRenderer(TrackPlayer& player) : player_(player)
{
}

std::span<const int16_t> MsRenderer::Next()
{
    start_frame_ = player_.Frames();
    rate_ = player_.SampleRate();
    remainder_ += rate_;
    const uint64_t frames = remainder_ / kMsPerSecond;
    remainder_ %= kMsPerSecond;
    samples_.resize(2 * frames);
    player_.Render(samples_);
    ms_++;

    // A millisecond whose last frame plays at another rate starts a run at that rate. The frames before it played at
    // the old rate.
    const uint32_t rate = player_.SampleRate();
    if (rate != run_rate_)
    {
        if (run_rate_ != 0)
        {
            run_start_ms_ += static_cast<double>(start_frame_ - run_start_frame_) * static_cast<double>(kMsPerSecond) /
                             static_cast<double>(run_rate_);
        }

        run_start_frame_ = start_frame_;
        run_rate_ = rate;
    }

    return samples_;
}

uint64_t MsRenderer::Ms() const
{
    return ms_;
}

std::optional<double> MsRenderer::FirstSampleMs() const
{
    for (size_t i = 0; i < samples_.size(); i++)
    {
        if (samples_[i] != 0)
        {
            const uint64_t frame = start_frame_ - run_start_frame_ + i / 2;
            return run_start_ms_ +
                   static_cast<double>(frame) * static_cast<double>(kMsPerSecond) / static_cast<double>(run_rate_);
        }
    }

    return std::nullopt;
}

TrackPlayer& MsRenderer::Player()
{
    return player_;
}

std::optional<xsflib::rip::Timing> MeasureTrack(MsRenderer& renderer,
                                                const std::function<void(std::span<const int16_t>)>& on_samples,
                                                xsflib::rip::Limit limit)
{
    OutputModel model(renderer, on_samples);

    return xsflib::rip::MeasureLength(model, GameLengthRules(limit));
}

} // namespace usf::rip
