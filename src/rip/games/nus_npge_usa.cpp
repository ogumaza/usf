// SPDX-License-Identifier: MIT

// Hey You, Pikachu! (USA): the game's music. The track titles are the names in the game's "Play music" menu, in the
// table at 0x8012BCA0 of the overlay that the game loads from 0xB3C70 in the ROM. The menu lists the 45 cues in the
// bank at 0xB140C0.
//
// Addresses come from the game's code in the image and from runs in usf's core.
//
// - The game thread selects the cue bank in the function at 0x8000BD70. The audio engine is running by then. The
//   snapshot takes over the game thread at the next instruction, 0x8000BED0.
// - The title screen sets the sequences' volume with the function at 0x8002006C and starts its song at full level.
//   The function at 0x80023244 starts a cue the same way.
// - The sequence reader's loop end is at 0x80005664. A sequence is one stream of MIDI events, with one loop.
// - From the end of the sequence's fifth pass, the reader leaves out a choice of channels 8 to 15, made from the CPU's
//   count register, in two passes of every three. A mini ends after its second pass and a fade. It never gets there.
//
// The game's scripts run on a small VM. Entry 270 of its table of native commands at 0x8012B400 is the function at
// 0x8011D768. That function passes a rate to the function at 0x800231F4. The rate is stored, and it reaches the current
// cue through the function at 0x8001FF84. That function leaves out cue 2 (Mamma mia!). The rate divides the time
// between a sequence's events. The tempo changes and the pitch doesn't.
//
// - An area's script sets the visit's time limits in its global values 1 to 5. Past the first limit, each update of the
//   script lowers the rate by 0.002 while the rate is above 0.8. The rate stops at 0.798. An area entered past that
//   limit sets 0.8. Entry 170 starts a cue at the stored rate.
// - A fishing script lowers the rate by 0.015 from 90 s into the session. The rate stops at 0.79. The script restarts
//   its cue at that rate after each catch.
//
// Each cue that a run of those areas slows has a second track at the rate where the slide stops, titled "..., slower".
// Its mini holds the same selector value and changes the sequence start at 0x8000522C to store the rate in place of
// 1.0. The driver and the library stay as they are for the 45 cues. These tracks come after them, and the numbers of
// the 45 don't change.

#include <bit>
#include <cstdint>
#include <string>
#include <vector>

#include "rip/driver.h"
#include "rip/games/games.h"

namespace usf::rip::games
{

namespace
{

constexpr uint32_t kBankSelected = 0x8000BED0;
constexpr uint32_t kSetCategoryVolume = 0x8002006C;
constexpr uint32_t kPlayCue = 0x80023244;
constexpr uint32_t kLoopEnd = 0x80005664;

constexpr uint32_t kSequences = 2;
constexpr uint32_t kFullVolume = 0x7FFF;

// The end of RDRAM. A run of the game holds zeros there.
constexpr uint32_t kDriverAddress = 0x803FF000;

constexpr const char* kCues[] = {
    "Yours ever",  "Camp night",   "Mamma mia!",    "Joyful",      "Home at last", "Prairie wind",  "Wood music",
    "Hello!",      "Daring days",  "Discovery",     "Play days",   "Misty lake",   "My bedroom",    "Pi-pikachu!",
    "Go shopping", "Quiz time",    "Who am I?",     "Feel lonely", "Over the sea", "Gone fishing!", "Opening 1",
    "Opening 2",   "Success",      "See you later", "Picnic",      "What's up?",   "Boo",           "Call",
    "Counting",    "A good thing", "Evolved",       "Evolving",    "Eye icon",     "Eye mode",      "Failure",
    "Harmonica 1", "Harmonica 2",  "Out of tune",   "In tune",     "Fly around",   "Correct",       "Trumpet 1",
    "Trumpet 2",   "Sea noise",    "Doing well",
};

struct SlowCue
{
    uint32_t cue;
    double rate;
};

// The rates in single precision, as the scripts compute them and as runs of the areas pass them to 0x8001FF84.
constexpr double kVisitLimitRate = 0.7980026006698608;
constexpr double kFishingRate = 0.7900002002716064;

constexpr SlowCue kSlowCues[] = {
    {5, kVisitLimitRate},  // Prairie wind: Springleaf entrance, Oddish meadow and Springleaf field.
    {6, kVisitLimitRate},  // Wood music: Viridian deep and the playground.
    {11, kVisitLimitRate}, // Misty lake: Olivine lakeshore and Olivine lake.
    {14, kVisitLimitRate}, // Go shopping: the shopping errand.
    {18, kVisitLimitRate}, // Over the sea: Cobalt island and Cobalt coast.
    {19, kFishingRate},    // Gone fishing!: the three fishing areas.
    {24, kVisitLimitRate}, // Picnic: Ochre deep and Ochre entrance.
};

// lui $1, the upper half of `value`, for the addiu that follows.
constexpr uint32_t LoadUpper(uint32_t value)
{
    return 0x3C010000 | (((value + 0x8000) >> 16) & 0xFFFF);
}

// addiu $1, $1, the lower half of `value`.
constexpr uint32_t AddLower(uint32_t value)
{
    return 0x24210000 | (value & 0xFFFF);
}

// The sequence start loads 1.0 into two FPU registers and stores them at offset 0x450 of the sequence's state. These
// words store `rate` with $1 instead, in the same instructions' places. They replace the call of an empty function
// after the store and the saving and reloading of $a0 around it.
std::vector<RdramPatch> StartRatePatches(double rate)
{
    constexpr uint32_t kStoreHigh = 0xAC810450; // sw $1, 0x450($a0)
    constexpr uint32_t kStoreLow = 0xAC810454;  // sw $1, 0x454($a0)
    constexpr uint32_t kNop = 0;

    const auto bits = std::bit_cast<uint64_t>(rate);
    const auto high = static_cast<uint32_t>(bits >> 32);
    const auto low = static_cast<uint32_t>(bits);
    return {{.address = 0x80005260, .words = {LoadUpper(high)}},
            {.address = 0x8000526C, .words = {AddLower(high)}},
            {.address = 0x8000527C, .words = {kStoreHigh}},
            {.address = 0x80005290, .words = {LoadUpper(low), AddLower(low), kStoreLow, kNop}}};
}

} // namespace

GameProfile NusNpgeUsa()
{
    GameProfile profile;
    profile.game = "Hey You, Pikachu!";
    profile.serial = "NUS-NPGE-USA";
    profile.sha1 = "da1a1e47a86720f9d54fb2d2d247480041bda824";
    profile.year = "2000";
    profile.sound_driver = "Hey You, Pikachu! sequence engine on n_audio";
    profile.controller = true;

    profile.snapshot_address = kBankSelected;
    profile.snapshot_hit = 1;
    profile.quiet_tries = 1;
    profile.boot_seconds = 3.0;

    CallDriverOptions driver;
    driver.address = kDriverAddress;
    driver.start_calls = {
        {.function = kSetCategoryVolume, .arguments = {{.value = kSequences}, {.value = kFullVolume}}},
        {.function = kPlayCue, .arguments = {{.track = true}}}};
    profile.driver = BuildCallDriver(driver);

    profile.loop_watches.push_back({.address = kLoopEnd});

    uint32_t number = 0;
    for (const char* name : kCues)
    {
        Track track;
        track.number = number++;
        track.name = name;
        profile.tracks.push_back(track);
    }

    for (const SlowCue& slow : kSlowCues)
    {
        Track track;
        track.number = slow.cue;
        track.name = std::string(kCues[slow.cue]) + ", slower";
        track.rdram_patches = StartRatePatches(slow.rate);
        profile.tracks.push_back(track);
    }

    return profile;
}

} // namespace usf::rip::games
