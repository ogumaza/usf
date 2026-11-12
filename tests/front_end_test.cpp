// SPDX-License-Identifier: MIT

// The path from a file to the frames that the front ends get: the loader, usf::UniformRatePlayer, xsflib's Playback and
// RenderAhead, on a set made from small programs (test_set.h). The anchor of the checks is the arithmetic of the
// programs and of the AI's registers: the ramp that a program sends, a rate of the video clock over DACRATE + 1, and a
// length of the tags' seconds at that rate.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "xsflib/player/playback.h"
#include "xsflib/player/render_ahead.h"
#include "xsflib/psf/psf.h"
#include "xsflib/testing/check.h"
#include "xsflib/testing/sets.h"

#include "test_set.h"
#include "usf/uniform_rate_player.h"
#include "usf/usf_file.h"

namespace
{

using Playback = xsflib::player::Playback<usf::UniformRatePlayer>;
using usf::testing::TestRate;
using usf::testing::TestTrack;
using xsflib::testing::ExactReader;

constexpr uint32_t kFirstDacRate = 1520;
constexpr uint32_t kSecondDacRate = 2204;

// Files in memory, by path.
using Files = std::map<std::string, std::vector<uint8_t>>;

// A set of one library and one mini with `tags`, at set/track.miniusf.
Files MakeSet(const TestTrack& track, xsflib::psf::Tags tags)
{
    Files files;
    files["set/lib.usflib"] = usf::testing::MakeTestLibrary({{"_enablecompare", "1"}, {"_enablefifofull", "1"}});
    tags["_lib"] = "lib.usflib";
    files["set/track.miniusf"] = usf::testing::MakeTestMini(track, tags);

    return files;
}

bool Open(Playback& playback, const Files& files)
{
    return playback.Open("set/track.miniusf", ExactReader(files)) && playback.Start();
}

// Whether `pcm` holds the track's ramp from `first`, frame after frame.
bool HoldsRamp(const std::vector<int16_t>& pcm, const TestTrack& track, uint64_t first = 0)
{
    for (size_t i = 0; i < pcm.size() / 2; i++)
    {
        if (pcm[2 * i] != usf::testing::TestLeft(track, first + i) ||
            pcm[2 * i + 1] != usf::testing::TestRight(track, first + i))
        {
            return false;
        }
    }

    return true;
}

// The track's ramp for its first `frames` frames: the model of what the track plays.
std::vector<int16_t> Ramp(const TestTrack& track, size_t frames)
{
    std::vector<int16_t> pcm(2 * frames);
    for (size_t i = 0; i < frames; i++)
    {
        pcm[2 * i] = usf::testing::TestLeft(track, i);
        pcm[2 * i + 1] = usf::testing::TestRight(track, i);
    }

    return pcm;
}

// The whole playback's frames.
std::vector<int16_t> RenderAll(Playback& playback)
{
    return xsflib::testing::RenderFrames([&](int16_t* out, size_t frames) { return playback.Render(out, frames); });
}

void PlaysAtTheTracksRate()
{
    TestTrack track;
    track.dac_rate = kFirstDacRate;
    const uint32_t rate = TestRate(kFirstDacRate);
    const Files files = MakeSet(track, {{"length", "1"}});

    Playback playback;
    const bool opened = Open(playback, files);
    const std::vector<int16_t> pcm = RenderAll(playback);

    XSFLIB_CHECK(opened);
    XSFLIB_CHECK(playback.SampleRate() == static_cast<int>(rate));
    XSFLIB_CHECK(playback.LengthFrames() == rate);
    XSFLIB_CHECK(pcm.size() == 2 * size_t{rate});
    XSFLIB_CHECK(HoldsRamp(pcm, track));
}

void LengthFollowsEachTracksRate()
{
    TestTrack track;
    track.dac_rate = kSecondDacRate;
    const uint32_t rate = TestRate(kSecondDacRate);
    const Files files = MakeSet(track, {{"length", "2"}, {"fade", "1"}});
    const std::vector<int16_t> model = Ramp(track, 3 * size_t{rate});

    Playback playback;
    const bool opened = Open(playback, files);
    const std::vector<int16_t> pcm = RenderAll(playback);

    XSFLIB_CHECK(opened);
    XSFLIB_CHECK(rate != TestRate(kFirstDacRate));
    XSFLIB_CHECK(playback.SampleRate() == static_cast<int>(rate));
    XSFLIB_CHECK(playback.PlayFrames() == 2 * uint64_t{rate});
    XSFLIB_CHECK(playback.FadeFrames() == rate);

    // The fade starts after the play time. The fade's first frame is the ramp as it stands.
    xsflib::testing::CheckLengthAndFade(pcm, model, 2 * size_t{rate}, rate);
}

// A game that changes its rate mid-track plays on at the track's rate. The player converts the later frames. The second
// rate is the lower one. The ramp then rises more slowly by the ratio of the two rates, within the rounding of the
// interpolation.
void ConvertsALaterRate()
{
    TestTrack track;
    track.dac_rate = kFirstDacRate;
    track.change_after = 4;
    track.second_dac_rate = kSecondDacRate;
    const uint32_t first = TestRate(kFirstDacRate);
    const uint32_t second = TestRate(kSecondDacRate);
    const Files files = MakeSet(track, {{"length", "1"}});

    Playback playback;
    const bool opened = Open(playback, files);
    const std::vector<int16_t> pcm = RenderAll(playback);

    XSFLIB_CHECK(opened);
    XSFLIB_CHECK(playback.SampleRate() == static_cast<int>(first));
    XSFLIB_REQUIRE(pcm.size() == 2 * size_t{first});

    // A buffer takes the rate at the moment it starts. When the rate changes, the fourth buffer is still waiting behind
    // the third. The first three buffers are therefore the ramp itself.
    const uint32_t unchanged = 3 * usf::testing::kTestBufferFrames;
    const std::vector<int16_t> before(pcm.begin(), pcm.begin() + 2 * static_cast<std::ptrdiff_t>(unchanged));
    XSFLIB_CHECK(HoldsRamp(before, track));

    // After the change, output frame n is source frame change + (n - change) × second / first, or just before it.
    bool follows = true;
    for (uint64_t n = unchanged + 2; n < first; n++)
    {
        const uint64_t source = unchanged + (n - unchanged) * second / first;
        const int64_t difference = int64_t{static_cast<uint16_t>(pcm[2 * n])} - static_cast<int64_t>(source);
        follows = follows && std::llabs(difference) <= 2;
    }
    XSFLIB_CHECK(follows);
}

// A seek on and then one back land where a render from the start does.
void SeeksBackAndOn()
{
    TestTrack track;
    const Files files = MakeSet(track, {{"length", "2"}});
    const std::vector<int16_t> model = Ramp(track, 30000 + 256);

    xsflib::testing::CheckSeeks<Playback>("set/track.miniusf", ExactReader(files), model, {30000, 1000}, 256);
}

void RendersAhead()
{
    TestTrack track;
    const uint32_t rate = TestRate(track.dac_rate);
    const Files files = MakeSet(track, {{"length", "1"}});

    auto playback = std::make_unique<Playback>();
    const bool opened = Open(*playback, files);
    xsflib::player::RenderAhead ahead(std::move(playback));
    const auto read = [&ahead](int16_t* out, size_t frames)
    {
        return ahead.Read(out, frames);
    };

    // A read returns what the worker has rendered so far, at least one frame.
    ahead.Seek(5000);
    const std::vector<int16_t> pcm = xsflib::testing::RenderFrames(read, 256);
    ahead.Seek(0);
    const std::vector<int16_t> all = xsflib::testing::RenderFrames(read);

    XSFLIB_CHECK(opened);
    XSFLIB_CHECK(pcm.size() == 2 * 256 && HoldsRamp(pcm, track, 5000));
    XSFLIB_CHECK(all.size() == 2 * size_t{rate});
    XSFLIB_CHECK(HoldsRamp(all, track));
    XSFLIB_CHECK(ahead.Error().empty());
}

// The work loops run, and they leave the frames as they were.
void LoadsLeaveTheFramesAlone()
{
    TestTrack track;
    track.cpu_loops = 2000;
    track.rsp_loops = 2000;
    const Files files = MakeSet(track, {{"length", "0.5"}});

    Playback playback;
    const bool opened = Open(playback, files);
    const std::vector<int16_t> pcm = RenderAll(playback);

    XSFLIB_CHECK(opened);
    XSFLIB_CHECK(pcm.size() == 2 * size_t{TestRate(track.dac_rate) / 2});
    XSFLIB_CHECK(HoldsRamp(pcm, track));
}

// A game that sends no buffers at all still plays for its length. Its frames are silence. A render never waits for
// ever.
void NoBuffersGiveSilence()
{
    TestTrack track;
    track.stop_after = 1;
    const Files files = MakeSet(track, {{"length", "2"}});

    Playback playback;
    const bool opened = Open(playback, files);
    const std::vector<int16_t> pcm = RenderAll(playback);

    XSFLIB_CHECK(opened);
    XSFLIB_CHECK(playback.SampleRate() > 0);
    XSFLIB_CHECK(pcm.size() == 4 * size_t(playback.SampleRate()));
    XSFLIB_CHECK(std::all_of(pcm.begin(), pcm.end(), [](int16_t sample) { return sample == 0; }));
}

// A game that stops sending buffers goes on with silence, and the time goes on too.
void StoppedBuffersGiveSilence()
{
    constexpr uint32_t kBuffers = 8;
    TestTrack track;
    track.stop_after = kBuffers + 1;
    const uint32_t rate = TestRate(track.dac_rate);
    const Files files = MakeSet(track, {{"length", "2"}});

    Playback playback;
    const bool opened = Open(playback, files);
    const std::vector<int16_t> pcm = RenderAll(playback);

    const uint32_t sent = kBuffers * usf::testing::kTestBufferFrames;
    XSFLIB_CHECK(opened);
    XSFLIB_REQUIRE(pcm.size() == 4 * size_t{rate});
    XSFLIB_CHECK(
        HoldsRamp(std::vector<int16_t>(pcm.begin(), pcm.begin() + 2 * static_cast<std::ptrdiff_t>(sent)), track));
    XSFLIB_CHECK(std::all_of(pcm.begin() + 2 * static_cast<std::ptrdiff_t>(sent), pcm.end(),
                             [](int16_t sample) { return sample == 0; }));
}

// A file in an archive: foobar2000 gives a path whose last folder ends at a bar. The library is found after that bar.
void FindsALibraryInAnArchive()
{
    TestTrack track;
    Files files = MakeSet(track, {{"length", "1"}});
    Files archive;
    archive["unpack://zip|5|a.zip|lib.usflib"] = files["set/lib.usflib"];
    archive["unpack://zip|5|a.zip|track.miniusf"] = files["set/track.miniusf"];

    usf::LoadedUsf set;
    const auto error = usf::LoadUsf("unpack://zip|5|a.zip|track.miniusf", ExactReader(archive), set, "/\\|");

    XSFLIB_CHECK(!error);
    XSFLIB_CHECK(set.sources.size() == 2);
}

// A set that doesn't load fails in Open, with a reason.
void ReportsAMissingLibrary()
{
    TestTrack track;
    Files files = MakeSet(track, {{"length", "1"}});
    files.erase("set/lib.usflib");

    xsflib::testing::CheckRefusals<Playback>({{"set/track.miniusf", "lib.usflib"}}, ExactReader(files));
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("front_end",
                                    []
                                    {
                                        PlaysAtTheTracksRate();
                                        LengthFollowsEachTracksRate();
                                        ConvertsALaterRate();
                                        SeeksBackAndOn();
                                        RendersAhead();
                                        LoadsLeaveTheFramesAlone();
                                        NoBuffersGiveSilence();
                                        StoppedBuffersGiveSilence();
                                        FindsALibraryInAnArchive();
                                        ReportsAMissingLibrary();
                                    });
}
