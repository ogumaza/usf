// SPDX-License-Identifier: MIT

// Chameleon Twist 2 (USA): libultra's compressed sequence player under the game's music code.
//
// Addresses come from the game's code in the image and from runs in usf's core. The main segment is copied from
// cartridge offset 0x1000 to 0x80025C00.
//
// - The sequence file at cartridge offset 0x120190 holds 24 sequences.
// - The song-select function at 0x8003BE10 takes the song in a0 and the player in a1. It stops the player, sets bit 1
//   of the player's flags at offset 0x17C and leaves the song in the request words at 0x80169088. The players'
//   structures start at 0x801A0A10, 0x188 bytes each.
// - The audio thread (0x8018FF00) calls the sound update at 0x8003BFE0 from 0x800CD4E0 in each audio frame. The update
//   starts a requested song once the player has stopped.
// - The game makes no sound until it gets a press of A. In a run that pressed A at intervals, the first song to play
//   was sequence 0x0D. Sequence 0x10 followed, from the code of six menu screens.
// - A stage's song comes from the table of halfwords at 0x800EC430, through 0x800286D4.
// - The image holds two copies of libultra's sequence reader. The players use the one that stores the jump back to a
//   loop at 0x800CF48C, with the new read pointer in t7 and the track in t3. The other, with its store at 0x800E8C4C,
//   plays nothing here.

#include "rip/games/games.h"
#include "rip/games/tracks.h"
#include "rip/mips.h"

namespace usf::rip::games
{
namespace
{

// The audio thread's frame calls the sound update from here.
constexpr uint32_t kAudioFrameUpdate = 0x800CD4E0;

// The first sequence player's flags (bit 1: a song waits to start) and its requested song.
constexpr uint32_t kPlayerFlags = 0x801A0B8C;
constexpr uint32_t kSongRequest = 0x80169088;

constexpr uint32_t kGameThread = 0x80102B98;
constexpr uint32_t kLoopJump = 0x800CF48C;

constexpr uint32_t kSequences = 24;

// The titles of the tracks that have a name.
constexpr NamedNumber kTitles[] = {
    {0x0D, "Sequence 0x0D: after the logos"},
    {0x10, "Sequence 0x10: menus"},
};

} // namespace

GameProfile NusN2veUsa()
{
    GameProfile profile;
    profile.game = "Chameleon Twist 2";
    profile.serial = "NUS-N2VE-USA";
    profile.sha1 = "9fa379d66b218228da3cbf386c91d857c677f489";
    profile.year = "1999";
    profile.cic = n64::Cic::k6102;
    profile.sound_driver = kCompressedSequencePlayer;
    profile.controller = true;

    // No song plays before a press of A, so the snapshot comes in the audio thread during the first screens. The game
    // thread stops, so that the game requests no other song. The patch and the selector make the update start the
    // track's song in that audio frame, as the song-select function would.
    profile.snapshot_address = kAudioFrameUpdate;
    profile.snapshot_hit = 100;
    profile.quiet_tries = 120;
    profile.boot_seconds = 20.0;
    profile.selector.kind = TrackSelector::Kind::kRdram;
    profile.selector.address = kSongRequest;
    profile.selector.size = 2;
    profile.rdram_patches = {{.address = kPlayerFlags, .words = {2}}};
    profile.stop_threads = {kGameThread};

    profile.loop_watches.push_back({.address = kLoopJump, .target_register = mips::kT7, .track_register = mips::kT3});

    for (uint32_t index = 0; index < kSequences; index++)
    {
        profile.tracks.push_back(NamedOrNumberedTrack(index, kTitles, "Sequence", TitleBase::kHex));
    }

    return profile;
}

} // namespace usf::rip::games
