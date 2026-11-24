// SPDX-License-Identifier: MIT

// The game profiles, without their images: every image's sets have names, and no two sets of an image are alike; every
// set titles its tracks differently; and the drivers of Paper Mario, GoldenEye 007 and Gex 64 make the calls that their
// tracks require, with the start effects and the track orders that runs of the games' code gave.

#include "rip/profiles.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/testing/check.h"

#include "rip/mips.h"
#include "rip/profile.h"

namespace
{

namespace mips = usf::rip::mips;

using mips::kA0;
using mips::kS0;
using mips::kS1;
using mips::kT0;
using mips::kT1;

// An image with more than one set names each set, and no two of its sets alike.
void ImageSets()
{
    std::set<std::string> images;
    for (const usf::rip::GameProfile& profile : usf::rip::Profiles())
    {
        images.insert(profile.sha1);
    }

    for (const std::string& sha1 : images)
    {
        const std::vector<const usf::rip::GameProfile*> found = usf::rip::FindProfiles(sha1);
        XSFLIB_CHECK(!found.empty());
        if (found.size() > 1)
        {
            std::set<std::string> names;
            for (const usf::rip::GameProfile* profile : found)
            {
                XSFLIB_CHECK(!profile->set.empty() && names.insert(profile->set).second);
            }
        }
    }

    // Super Mario 64 (USA): the music first, then the sound effects. --bgm leaves the sound effects out.
    const std::vector<const usf::rip::GameProfile*> sm64 =
        usf::rip::FindProfiles("9bef1128717f958171a4afac3ed78ee2bb4e86ce");
    XSFLIB_CHECK(sm64.size() == 2 && !sm64[0]->sound_effects && sm64[1]->sound_effects);
    XSFLIB_CHECK(usf::rip::FindProfiles("0000000000000000000000000000000000000000").empty());
}

// The positions in `code` at which the words of `part` start.
std::vector<size_t> Positions(const std::vector<uint32_t>& code, const std::vector<uint32_t>& part)
{
    std::vector<size_t> positions;
    for (auto at = std::search(code.begin(), code.end(), part.begin(), part.end()); at != code.end();
         at = std::search(at + 1, code.end(), part.begin(), part.end()))
    {
        positions.push_back(static_cast<size_t>(at - code.begin()));
    }

    return positions;
}

// Whether `code` contains the words of each of `parts` in turn, with two words between each part and the next, exactly
// once. Each of a driver's tests of a call ends with a branch past the call and its delay slot: the two words between
// the parts.
bool HasCallOnce(const std::vector<uint32_t>& code, const std::vector<std::vector<uint32_t>>& parts)
{
    int found = 0;
    for (size_t at : Positions(code, parts.front()))
    {
        size_t next = at + parts.front().size();
        bool matches = true;
        for (size_t i = 1; i < parts.size() && matches; i++)
        {
            next += 2;
            matches = next + parts[i].size() <= code.size() &&
                      std::equal(parts[i].begin(), parts[i].end(), code.begin() + static_cast<std::ptrdiff_t>(next));
            next += parts[i].size();
        }
        if (matches)
        {
            found++;
        }
    }

    return found == 1;
}

// The words of a driver's test of the track number. It masks the number in s0 by `mask` into t1 and loads `value` into
// t0. A branch past the call follows, taken when they differ.
std::vector<uint32_t> TrackTest(uint32_t mask, uint32_t value)
{
    mips::Assembler test(0);
    test.LoadImmediate(kT0, mask);
    test.Emit(mips::And(kT1, kS0, kT0));
    test.LoadImmediate(kT0, value);

    return test.Words();
}

// The words of a driver's test of the frame for a call made only in the first `frames` frames. s1 counts the frames. A
// branch past the call follows, taken from frame `frames` on.
std::vector<uint32_t> FrameLimit(uint32_t frames)
{
    mips::Assembler test(0);
    test.LoadImmediate(kT0, frames);
    test.Emit(mips::Sltu(kT1, kS1, kT0));

    return test.Words();
}

// The words of a call of `function`: the words of `arguments`, then the load of the function into t9 and the jump to
// it through t9.
std::vector<uint32_t> CallWords(const mips::Assembler& arguments, uint32_t function)
{
    mips::Assembler call(0);
    for (uint32_t word : arguments.Words())
    {
        call.Emit(word);
    }
    call.LoadImmediate(mips::kT9, function);
    call.Emit(mips::Jalr(mips::kRa, mips::kT9));

    return call.Words();
}

// Whether Paper Mario's driver, for the track numbered `number` alone, calls `function` with Toad Town's song name and
// `mix`. The driver loads the track's number into t0 and branches past the call for every other track. It then loads
// the arguments into a0 and a1 and the function into t9, and calls it.
bool CallsMix(const std::vector<uint32_t>& code, uint32_t number, uint32_t function, uint32_t mix)
{
    constexpr uint32_t kToadTownName = 0x32313020;
    mips::Assembler head(0);
    head.LoadImmediate(kT0, number);
    mips::Assembler arguments(0);
    arguments.LoadImmediate(kA0, kToadTownName);
    arguments.LoadImmediate(mips::kA1, mix);

    return HasCallOnce(code, {head.Words(), CallWords(arguments, function)});
}

// Whether a driver calls `function` with the one argument that `argument` loads into a0, for the tracks whose number,
// masked by `mask`, equals `value`. The driver masks the track number in s0 into t1, loads `value` into t0 and branches
// past the call when they differ.
bool CallsForMaskedTracks(const std::vector<uint32_t>& code, uint32_t mask, uint32_t value,
                          const mips::Assembler& argument, uint32_t function)
{
    return HasCallOnce(code, {TrackTest(mask, value), CallWords(argument, function)});
}

// Paper Mario (USA): Toad Town's tracks in their order, with their numbers, titles and mix calls. The five buildings
// and the floor of Rowf's Badge Shop each have a track at the full level and one at the near level. Mixes 4 and 5 and
// the unused mix 6 have one at the near level. Song 0x01 follows. Each title after the first of song 0x00 adds its
// version.
void ToadTownTracks()
{
    constexpr uint32_t kNear = 0x80056044;
    constexpr uint32_t kFull = 0x80056068;
    struct Expected
    {
        uint32_t number;
        uint32_t function; // 0 for a track without a mix.
        uint32_t mix;
    };
    constexpr Expected kToadTown[] = {
        {0x0, 0, 0},          // Toad Town.
        {0x100, 0, 0},        // Toad Town, the second version.
        {0x10000, kFull, 1},  // Toad Town, Tayce T.'s House.
        {0x20000, kFull, 2},  // Toad Town, Merlon's House.
        {0x30000, kFull, 3},  // Toad Town, the Dojo.
        {0x40000, kNear, 4},  // Toad Town, the pipe to the Tunnels.
        {0x50000, kNear, 5},  // Toad Town, Minh T.'s Garden.
        {0x70000, kFull, 7},  // Toad Town, Russ T.'s House.
        {0x80000, kFull, 8},  // Toad Town, Rowf's Badge Shop.
        {0x90000, kFull, 9},  // Toad Town, the Warehouse.
        {0x110000, kNear, 1}, // Toad Town, outside Tayce T.'s House.
        {0x120000, kNear, 2}, // Toad Town, outside Merlon's House.
        {0x130000, kNear, 3}, // Toad Town, outside the Dojo.
        {0x170000, kNear, 7}, // Toad Town, outside Russ T.'s House.
        {0x190000, kNear, 9}, // Toad Town, outside the Warehouse.
        {0x180000, kNear, 8}, // Toad Town, outside Rowf's Badge Shop.
        {0x60000, kNear, 6},  // Toad Town, mix 6 (unused).
        {0x1, 0, 0},          // Goomba Village.
    };

    const std::vector<const usf::rip::GameProfile*> found =
        usf::rip::FindProfiles("3837f44cda784b466c9a2d99df70d77c322b97a0");
    XSFLIB_REQUIRE(found.size() == 1 && found[0]->driver && found[0]->tracks.size() > std::size(kToadTown));
    for (size_t i = 0; i < std::size(kToadTown); i++)
    {
        const Expected& expected = kToadTown[i];
        const usf::rip::Track& track = found[0]->tracks[i];
        // The first and the last track are the first tracks of their songs.
        const bool last = i + 1 == std::size(kToadTown);
        const std::string song = last ? "Song 0x01" : "Song 0x00";
        const std::string title = i == 0 || last ? song : song + ", version " + std::to_string(i + 1);
        XSFLIB_CHECK(track.number == expected.number && track.name == title);
        if (expected.function != 0)
        {
            XSFLIB_CHECK(CallsMix(found[0]->driver->code, expected.number, expected.function, expected.mix));
        }
    }

    // Each track of the set has a number of its own.
    std::set<uint32_t> numbers;
    for (const usf::rip::Track& track : found[0]->tracks)
    {
        XSFLIB_CHECK(numbers.insert(track.number).second);
    }
}

// Paper Mario (USA): the effect that each track requests for the first bus at its start, and the order of the tracks of
// Normal Battle (0x02) and Raphael Raven (0x28). A request word contains the effect in its top byte and the request
// flag below it. Runs of the game's music functions in usf's core give the effects: Raphael Raven after Deep Jungle,
// Yoshi's Village and the file select's song, the battle's pop with no effect, and the one song before 0x79 and 0x83.
void PaperMarioStartEffects()
{
    constexpr uint32_t kEffectRequest = 0x801D57E0;
    constexpr uint32_t kRequested = 0x00010000;
    // The track's song sets its effect at the start, and the track requests none.
    constexpr uint32_t kSongSets = 0xFF;
    struct Expected
    {
        uint32_t number;
        uint32_t effect;
    };
    constexpr Expected kNormalBattle[] = {
        {0x2, 1},        {0x102, 1},     {0x202, 1},     {0x302, 1},     {0x5000002, 5},  {0x5000102, 5},
        {0x5000202, 5},  {0x2000002, 2}, {0x2000102, 2}, {0x2000202, 2}, {0x80000002, 0}, {0x80000102, 0},
        {0x80000202, 0}, {0x3000002, 3}, {0x3000102, 3}, {0x3000202, 3},
    };
    constexpr Expected kRaphaelRaven[] = {
        {0x28, 2}, {0x128, kSongSets}, {0x228, 2}, {0x1000028, 1}, {0x3000028, 3}, {0x80000028, 0},
    };
    constexpr Expected kLatePhrases[] = {{0x170, 10}, {0x79, 10}, {0x183, 1}, {0x191, 5}};

    const std::vector<const usf::rip::GameProfile*> found =
        usf::rip::FindProfiles("3837f44cda784b466c9a2d99df70d77c322b97a0");
    XSFLIB_REQUIRE(found.size() == 1);
    const std::vector<usf::rip::Track>& tracks = found[0]->tracks;

    // Checks each track's request word. With `in_order`, the tracks also come one after another in the set.
    const auto check = [&](std::span<const Expected> expected, bool in_order)
    {
        const auto has_number = [&](uint32_t number)
        {
            return [number](const usf::rip::Track& track)
            {
                return track.number == number;
            };
        };
        const auto first = std::find_if(tracks.begin(), tracks.end(), has_number(expected[0].number));
        for (size_t i = 0; i < expected.size(); i++)
        {
            const auto track = std::find_if(tracks.begin(), tracks.end(), has_number(expected[i].number));
            XSFLIB_REQUIRE(track != tracks.end());
            if (in_order)
            {
                XSFLIB_CHECK(track - first == static_cast<std::ptrdiff_t>(i));
            }

            std::vector<uint32_t> requests;
            for (const usf::rip::RdramPatch& patch : track->rdram_patches)
            {
                if (patch.address == kEffectRequest)
                {
                    requests.insert(requests.end(), patch.words.begin(), patch.words.end());
                }
            }
            const std::vector<uint32_t> wanted = expected[i].effect == kSongSets
                                                     ? std::vector<uint32_t>{}
                                                     : std::vector<uint32_t>{(expected[i].effect << 24) | kRequested};
            XSFLIB_CHECK(requests == wanted);
        }
    };

    check(kNormalBattle, true);
    check(kRaphaelRaven, true);
    check(kLatePhrases, false);
}

// GoldenEye 007 (USA): the track of Surface 2's stage music, 0x1C with the background 0x35, takes the place of 0x1C in
// the set. 0x1C alone, as multiplayer plays it, is the last track. The driver plays the tracks without a background
// byte through the play function. It plays the stage music track through the stage start, with the stage number 0x2B.
// The numbers come from the stage table of the game at 0x8004EB10.
void GoldenEyeSurface2()
{
    constexpr uint32_t kPlayMusic = 0x70006E7C;
    constexpr uint32_t kStartStageMusic = 0x7F0C11FC;
    constexpr uint32_t kBackgroundMask = 0xFF00;
    constexpr uint32_t kSurface2Stage = 0x2B;
    constexpr uint32_t kSurface2Track = 0x351C;
    constexpr double kSurface2Seconds = 307.2;
    // The tracks of 0x01 to 0x1B, apart from the silent 0x14, come first.
    constexpr size_t kSurface2Index = 26;

    const std::vector<const usf::rip::GameProfile*> found =
        usf::rip::FindProfiles("abe01e4aeb033b6c0836819f549c791b26cfde83");
    XSFLIB_REQUIRE(found.size() == 1 && found[0]->driver && found[0]->tracks.size() > kSurface2Index + 1);

    const std::vector<usf::rip::Track>& tracks = found[0]->tracks;
    const usf::rip::Track& stage = tracks[kSurface2Index];
    XSFLIB_CHECK(stage.number == kSurface2Track && stage.name == "Sequence 0x1C" &&
                 stage.length_seconds == kSurface2Seconds);
    const usf::rip::Track& alone = tracks.back();
    XSFLIB_CHECK(alone.number == 0x1C && alone.name == "Sequence 0x1C, version 2" &&
                 alone.length_seconds == kSurface2Seconds);

    // The play function takes the track number as it is. The stage start takes the stage number.
    mips::Assembler track_argument(0);
    track_argument.Emit(mips::Sll(kA0, kS0, 0));
    mips::Assembler stage_argument(0);
    stage_argument.LoadImmediate(kA0, kSurface2Stage);
    const std::vector<uint32_t>& code = found[0]->driver->code;
    XSFLIB_CHECK(CallsForMaskedTracks(code, kBackgroundMask, 0, track_argument, kPlayMusic));
    XSFLIB_CHECK(CallsForMaskedTracks(code, kBackgroundMask, kSurface2Track & kBackgroundMask, stage_argument,
                                      kStartStageMusic));
}

// The functions among `functions` that `code` calls, in the order of the code. Each call loads its function into t9
// and jumps to it through t9.
std::vector<uint32_t> CalledFunctions(const std::vector<uint32_t>& code, const std::vector<uint32_t>& functions)
{
    const mips::Assembler no_arguments(0);
    std::vector<std::pair<size_t, uint32_t>> calls;
    for (uint32_t function : functions)
    {
        for (size_t at : Positions(code, CallWords(no_arguments, function)))
        {
            calls.emplace_back(at, function);
        }
    }

    std::sort(calls.begin(), calls.end());
    std::vector<uint32_t> called;
    for (const auto& call : calls)
    {
        called.push_back(call.second);
    }

    return called;
}

// Gex 64 (USA): the driver makes the music update with interrupts off, in the frames of a level's song and in the
// first four frames of a short song. It makes the load with interrupts off only for the track of song 8 of looney69,
// whose song starts in the load. Each restore sets the interrupt enable bit again. In usf's core, the audio thread's
// tick otherwise comes in the middle of the start of a song, and the song plays wrongly.
void Gex64Updates()
{
    constexpr uint32_t kDisableInterrupts = 0x8005DD00;
    constexpr uint32_t kLoadLevelMusic = 0x800327C0;
    constexpr uint32_t kMusicUpdate = 0x80032BD0;
    constexpr uint32_t kRestoreInterrupts = 0x8005DD20;
    constexpr uint32_t kShortSong = 0x02000000;
    constexpr uint32_t kShortSongFrames = 4;
    // The track of song 8 of looney69, level 8, in place of the level's song. Its patch writes the song's cartridge
    // offset into the song start of level 8's entry in the table of levels.
    constexpr uint32_t kLooney69Song8 = 0x04000008;
    constexpr uint32_t kLevel8SongStart = 0x8006F6E0;
    constexpr uint32_t kSong8Offset = 0x459FF0;

    const std::vector<const usf::rip::GameProfile*> found =
        usf::rip::FindProfiles("16042cd0dfa5439ca436f1bf05ebfb7e9f730cda");
    XSFLIB_REQUIRE(found.size() == 1 && found[0]->driver);
    const std::vector<uint32_t>& code = found[0]->driver->code;

    // The guard of the load's mask names the track of song 8 of looney69, and only that track.
    int song8_tracks = 0;
    for (const usf::rip::Track& track : found[0]->tracks)
    {
        if (track.number != kLooney69Song8)
        {
            continue;
        }

        song8_tracks++;
        const auto writes_song8 = [&](const usf::rip::RdramPatch& patch)
        {
            return patch.address == kLevel8SongStart && !patch.words.empty() && patch.words[0] == kSong8Offset;
        };
        XSFLIB_CHECK(std::any_of(track.rdram_patches.begin(), track.rdram_patches.end(), writes_song8));
    }
    XSFLIB_CHECK(song8_tracks == 1);

    // The load and the update take the level, the low byte of the track number. The restore takes 1.
    const mips::Assembler no_arguments(0);
    mips::Assembler level(0);
    level.Emit(mips::Srl(kA0, kS0, 0));
    level.Emit(mips::Andi(kA0, kA0, 0xFF));
    level.Emit(mips::Sll(kA0, kA0, 0));
    mips::Assembler one(0);
    one.LoadImmediate(kA0, 1);
    const std::vector<uint32_t> disable = CallWords(no_arguments, kDisableInterrupts);
    const std::vector<uint32_t> load = CallWords(level, kLoadLevelMusic);
    const std::vector<uint32_t> update = CallWords(level, kMusicUpdate);
    const std::vector<uint32_t> restore = CallWords(one, kRestoreInterrupts);

    // The start calls mask the load. Each kind of frame then masks its update.
    const std::vector<uint32_t> expected = {kDisableInterrupts, kLoadLevelMusic,    kRestoreInterrupts,
                                            kDisableInterrupts, kDisableInterrupts, kMusicUpdate,
                                            kMusicUpdate,       kRestoreInterrupts, kRestoreInterrupts};
    XSFLIB_CHECK(CalledFunctions(code, {kDisableInterrupts, kLoadLevelMusic, kMusicUpdate, kRestoreInterrupts}) ==
                 expected);

    // Only the track of song 8 of looney69 masks the load. Every track makes it: the branch past the first disable
    // lands on the load, after the call's delay slot.
    const std::vector<uint32_t> load_start = TrackTest(0xFFFFFFFF, kLooney69Song8);
    XSFLIB_CHECK(HasCallOnce(code, {load_start, disable}) && HasCallOnce(code, {load_start, restore}));
    const std::vector<size_t> loads = Positions(code, load);
    const std::vector<size_t> disables = Positions(code, disable);
    XSFLIB_CHECK(loads.size() == 1 && !disables.empty() && loads[0] == disables[0] + disable.size() + 1);

    // Each call of a frame is made once in a level's frames and once in a short song's first four frames.
    const std::vector<uint32_t> level_frames = TrackTest(kShortSong, 0);
    const std::vector<uint32_t> short_frames = TrackTest(kShortSong, kShortSong);
    const std::vector<uint32_t> first_frames = FrameLimit(kShortSongFrames);
    for (const std::vector<uint32_t>& call : {disable, update, restore})
    {
        XSFLIB_CHECK(HasCallOnce(code, {level_frames, call}));
        XSFLIB_CHECK(HasCallOnce(code, {short_frames, first_frames, call}));
    }
}

// Every set titles each of its tracks differently. A title is the song's number, with a version after the first track
// of a song.
void TrackTitles()
{
    for (const usf::rip::GameProfile& profile : usf::rip::Profiles())
    {
        std::set<std::string> titles;
        for (const usf::rip::Track& track : profile.tracks)
        {
            XSFLIB_CHECK(!track.name.empty() && titles.insert(track.name).second);
        }
    }
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("profiles",
                                    []
                                    {
                                        ImageSets();
                                        ToadTownTracks();
                                        PaperMarioStartEffects();
                                        GoldenEyeSurface2();
                                        Gex64Updates();
                                        TrackTitles();
                                    });
}
