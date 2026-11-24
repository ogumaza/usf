// SPDX-License-Identifier: MIT

// Super Mario 64 (USA): the game's sound effects, played one at a time through the game's sound-effect interface.
// nus_nsme_usa_music.cpp has the game's music, in a set of its own.
//
// Addresses come from the game's code in the image and from runs in usf's core.
//
// - The play-sound function at 0x8031EB00 takes a sound's code in a0 and a pointer to its source's position in a1. It
//   queues a request. The global source at 0x803331F0 plays a sound at the listener, with no pan or distance.
// - The game-loop tick at 0x8031FD84 takes no arguments. It sets the word at 0x80332E54. The sound thread then updates
//   the effects at its next audio frame. The game calls the tick once a frame, every second video field.
// - A code contains the bank in bits 28 to 31, flags in bits 24 to 27, the sound's number in bits 16 to 23 and its
//   priority in bits 8 to 15. Bit 7 marks a sound that plays once. Bit 0 marks a new request. A sound without bit 7
//   plays only while the game keeps requesting it.
// - The sound reset at 0x80322230 returns to 0x802491D8 in the game thread. The first return comes before any music and
//   before the first effect.
// - The sound-player sequence has a channel table for each bank. Banks 3 and 8 share one, and so do banks 5 and 9. The
//   tables contain 542 entries. An entry whose script is the same as an earlier entry's plays the same sound. The
//   profile leaves those out. A sound with variants by ground (default, grass, water, stone, creaky floor, snow, ice
//   and sand) adds the variant to its number. A variant is a track of its own only where its script differs. That
//   leaves 431 sounds. One of them, menu sound 0x10, makes no sound when played alone. The profile leaves it out too.
//   That leaves 430 tracks, 89 of them sounds that play only while requested.
// - The lengths come from runs of each track in usf's core: the last sound and 0.5 s more. The rip runs each track that
//   long and ends it at its last sound. A sound that plays only while requested is requested for 5 s. Sound 0x60 of
//   bank 8 plays once but keeps its level until the game stops it. Its track plays for 5 s and fades out as a loop
//   does, over 7 s.
// - A track's number is the sound's code. Its title gives the bank and the sound's number. The comments in kSounds
//   describe the sounds where a description is known.
// - The game's main thread (0x8033A8E0, priority 100) sends a message to the queue at 0x8033B010 at each retrace, at
//   0x802467DC, through the handler that the word at 0x8032D564 points to. It sends it whether the game draws or not.
//   The queue has room for one message, and it contains one at the snapshot. The game thread (0x8033AA90, priority 10)
//   waits for two of them in each frame, at 0x8024810C and 0x80248154, through osRecvMesg at 0x80322800. Only the idle
//   thread (0x8033A730, priority 0) has a lower priority. Its loop at 0x80246DD8 is a branch to itself.

#include <cstdio>
#include <string>
#include <vector>

#include "rip/driver.h"
#include "rip/games/games.h"

namespace usf::rip::games
{
namespace
{

constexpr uint32_t kPlaySound = 0x8031EB00;
constexpr uint32_t kGlobalSource = 0x803331F0;
constexpr uint32_t kGameLoopTick = 0x8031FD84;
constexpr uint32_t kAfterSoundReset = 0x802491D8;
constexpr uint32_t kRecvMesg = 0x80322800;
constexpr uint32_t kRetraceQueue = 0x8033B010;

// The last lines of the third frame buffer (0x803DA800 to 0x80400000). The game's set-up stores the three frame
// buffers' addresses at 0x802489C4 to 0x80248A20: 0x8038F800, 0x803B5000 and 0x803DA800. Only the RDP writes there, and
// it draws nothing after the snapshot.
constexpr uint32_t kDriverAddress = 0x803FF000;

constexpr uint32_t kDiscrete = 0x80;

// A sound that plays only while requested is requested for 150 frames, 5 s.
constexpr uint32_t kHeldFrames = 150;

// The bank and the sound's number in a code.
constexpr int kBankShift = 28;
constexpr int kSoundShift = 16;
constexpr uint32_t kSoundMask = 0xFF;

struct Sound
{
    uint32_t code;
    double seconds; // The track's length.
    double fade;    // The track's fade.
};

// clang-format off
const Sound kSounds[] = {
    {0x04008081, 0.8, 0.0}, // jump (default)
    {0x04018081, 0.8, 0.0}, // jump (grass)
    {0x04028081, 1.0, 0.0}, // jump (water)
    {0x04038081, 0.8, 0.0}, // jump (stone)
    {0x04048081, 0.9, 0.0}, // jump (creaky floor)
    {0x04058081, 0.9, 0.0}, // jump (snow)
    {0x04068081, 0.8, 0.0}, // jump (ice)
    {0x04078081, 1.0, 0.0}, // jump (sand)
    {0x04088081, 0.8, 0.0}, // landing (default)
    {0x04098081, 0.9, 0.0}, // landing (grass)
    {0x040A8081, 1.0, 0.0}, // landing (water)
    {0x040B8081, 0.9, 0.0}, // landing (stone)
    {0x040C8081, 1.0, 0.0}, // landing (creaky floor)
    {0x040D8081, 1.0, 0.0}, // landing (snow)
    {0x040E8081, 0.9, 0.0}, // landing (ice)
    {0x040F8081, 1.0, 0.0}, // landing (sand)
    {0x06108081, 0.8, 0.0}, // step (default)
    {0x06118081, 1.0, 0.0}, // step (grass)
    {0x06128081, 1.1, 0.0}, // step (water)
    {0x06138081, 0.9, 0.0}, // step (stone)
    {0x06148081, 1.1, 0.0}, // step (creaky floor)
    {0x06158081, 1.1, 0.0}, // step (snow)
    {0x06168081, 0.9, 0.0}, // step (ice)
    {0x06178081, 1.1, 0.0}, // step (sand)
    {0x04188081, 0.8, 0.0}, // body hit ground (default)
    {0x04198081, 1.0, 0.0}, // body hit ground (grass)
    {0x041A8081, 1.0, 0.0}, // body hit ground (water)
    {0x041B8081, 0.9, 0.0}, // body hit ground (stone)
    {0x041C8081, 1.0, 0.0}, // body hit ground (creaky floor)
    {0x041D8081, 1.0, 0.0}, // body hit ground (snow)
    {0x041E8081, 0.9, 0.0}, // body hit ground (ice)
    {0x041F8081, 1.0, 0.0}, // body hit ground (sand)
    {0x06208081, 0.8, 0.0}, // step tiptoe (default)
    {0x06218081, 0.9, 0.0}, // step tiptoe (grass)
    {0x06228081, 1.0, 0.0}, // step tiptoe (water)
    {0x06238081, 0.8, 0.0}, // step tiptoe (stone)
    {0x06248081, 0.9, 0.0}, // step tiptoe (creaky floor)
    {0x06258081, 0.9, 0.0}, // step tiptoe (snow)
    {0x06268081, 0.9, 0.0}, // step tiptoe (ice)
    {0x06278081, 1.1, 0.0}, // step tiptoe (sand)
    {0x04289081, 1.0, 0.0}, // metal jump
    {0x04299081, 1.1, 0.0}, // metal landing
    {0x042A9081, 0.9, 0.0}, // metal step
    {0x042B9081, 1.1, 0.0}, // metal heavy landing
    {0x062C0081, 1.0, 0.0}, // clap hands cold
    {0x042DA081, 0.7, 0.0}, // hanging step
    {0x042E0081, 0.8, 0.0}, // quicksand step
    {0x042F9081, 0.9, 0.0}, // metal step tiptoe
    {0x0430C081, 2.0, 0.0},
    {0x04316081, 1.3, 0.0},
    {0x04328081, 1.3, 0.0},
    {0x04338081, 1.3, 0.0}, // swim
    {0x04348081, 2.0, 0.0},
    {0x04358081, 0.8, 0.0}, // throw
    {0x04368081, 0.8, 0.0}, // key swish
    {0x04378081, 0.8, 0.0}, // spin
    {0x043A8081, 1.1, 0.0}, // climb up tree
    {0x003B0001, 5.6, 0.0}, // climb down tree
    {0x003C0001, 5.6, 0.0},
    {0x043D8081, 0.8, 0.0},
    {0x043E8081, 0.8, 0.0},
    {0x043F8081, 0.8, 0.0}, // pat back
    {0x04408081, 0.8, 0.0}, // brush hair
    {0x04418081, 1.0, 0.0}, // climb up pole
    {0x04428081, 1.1, 0.0}, // metal bonk
    {0x04438081, 1.2, 0.0}, // unstuck from ground
    {0x0444C081, 1.7, 0.0}, // hit
    {0x0445A081, 0.8, 0.0}, // bonk
    {0x0446A081, 2.8, 0.0}, // shrink into the haunted house
    {0x0447A081, 1.7, 0.0}, // swim fast
    {0x04488081, 0.7, 0.0}, // stuck in ground (default)
    {0x044D8081, 0.8, 0.0}, // stuck in ground (snow)
    {0x044E8081, 0.8, 0.0}, // stuck in ground (ice)
    {0x04509081, 1.1, 0.0}, // metal jump water
    {0x04519081, 1.2, 0.0}, // metal land water
    {0x04529081, 1.0, 0.0}, // metal step water
    {0x00530001, 5.7, 0.0},
    {0x00540001, 5.7, 0.0},
    {0x00550001, 5.7, 0.0},
    {0x04568081, 1.8, 0.0}, // flying fast
    {0x0457C081, 1.1, 0.0}, // teleport
    {0x0458A081, 0.8, 0.0},
    {0x0459B081, 1.0, 0.0}, // bounce off object
    {0x045A8081, 0.9, 0.0}, // side flip (other)
    {0x045BFF81, 1.5, 0.0}, // read sign
    {0x045C8081, 1.0, 0.0},
    {0x005D0001, 5.7, 0.0},
    {0x04608081, 1.1, 0.0}, // heavy landing (default)
    {0x04618081, 1.1, 0.0}, // heavy landing (grass)
    {0x04628081, 1.1, 0.0}, // heavy landing (water)
    {0x04638081, 1.1, 0.0}, // heavy landing (stone)
    {0x04648081, 1.1, 0.0}, // heavy landing (creaky floor)
    {0x04658081, 1.1, 0.0}, // heavy landing (snow)
    {0x04668081, 1.1, 0.0}, // heavy landing (ice)
    {0x04678081, 1.1, 0.0}, // heavy landing (sand)
    {0x14000001, 5.6, 0.0}, // slide (default)
    {0x14010001, 5.6, 0.0}, // slide (grass)
    {0x14020001, 5.6, 0.0}, // slide (water)
    {0x14030001, 5.6, 0.0}, // slide (stone)
    {0x14040001, 5.6, 0.0}, // slide (creaky floor)
    {0x14050001, 5.6, 0.0}, // slide (snow)
    {0x14060001, 5.6, 0.0}, // slide (ice)
    {0x14070001, 5.6, 0.0}, // slide (sand)
    {0x14100001, 5.7, 0.0}, // lava burn
    {0x14110001, 5.7, 0.0}, // slide down pole
    {0x14128001, 5.6, 0.0}, // slide down tree
    {0x38118081, 1.6, 0.0}, // coin
    {0x14140001, 5.7, 0.0}, // quicksand death
    {0x14160001, 5.7, 0.0}, // shocked
    {0x14170001, 5.7, 0.0}, // flying
    {0x1C180001, 6.2, 0.0}, // almost drowning
    {0x1D192001, 5.7, 0.0}, // aim cannon
    {0x101A0001, 5.6, 0.0},
    {0x14200001, 5.6, 0.0}, // riding shell (default)
    {0x14210001, 5.7, 0.0}, // riding shell (grass)
    {0x14220001, 5.7, 0.0}, // riding shell (water)
    {0x14230001, 5.6, 0.0}, // riding shell (stone)
    {0x14240001, 5.6, 0.0}, // riding shell (creaky floor)
    {0x14250001, 5.6, 0.0}, // riding shell (snow)
    {0x14260001, 5.6, 0.0}, // riding shell (ice)
    {0x14270001, 5.7, 0.0}, // riding shell (sand)
    {0x14280001, 5.7, 0.0}, // riding shell lava
    {0x24008081, 0.9, 0.0}, // Mario: yah wah hoo
    {0x20018081, 0.9, 0.0}, // Mario: yah wah hoo (variant)
    {0x20028081, 0.8, 0.0}, // Mario: yah wah hoo (variant)
    {0x24038081, 1.1, 0.0}, // Mario: hoo-hoo
    {0x24048081, 1.6, 0.0}, // Mario: yahoo
    {0x24058081, 1.0, 0.0}, // Mario: uh
    {0x24068081, 1.0, 0.0}, // Mario: hrmm
    {0x24078081, 0.9, 0.0}, // Mario: wah 2
    {0x2408C081, 1.1, 0.0}, // Mario: whoa
    {0x24098081, 1.4, 0.0}, // Mario: eeuh
    {0x240AFF81, 1.1, 0.0}, // Mario: attacked
    {0x240B8081, 1.1, 0.0}, // Mario: ooof
    {0x240C8081, 2.0, 0.0}, // Mario: here we go
    {0x240D8081, 2.0, 0.0}, // Mario: yawning
    {0x240E8081, 1.6, 0.0}, // Mario: snoring 1
    {0x240F8081, 1.0, 0.0}, // Mario: snoring 2
    {0x2410C081, 2.4, 0.0}, // Mario: waaaooow
    {0x24118081, 1.4, 0.0}, // Mario: ha-ha
    {0x24188081, 1.3, 0.0}, // Mario: panting
    {0x2413D081, 0.8, 0.0}, // Mario: uh 2
    {0x2414A081, 2.7, 0.0}, // Mario: on fire
    {0x2415FF81, 2.1, 0.0}, // Mario: dying
    {0x24168081, 1.1, 0.0}, // Mario: panting cold
    {0x241D8081, 0.9, 0.0}, // Mario: coughing 3
    {0x20198081, 1.3, 0.0}, // Mario: panting (variant)
    {0x201A8081, 1.3, 0.0}, // Mario: panting (variant)
    {0x241B8081, 0.8, 0.0}, // Mario: coughing 1
    {0x241C8081, 0.9, 0.0}, // Mario: coughing 2
    {0x241E8081, 0.9, 0.0}, // Mario: punch yah
    {0x241F8081, 1.1, 0.0}, // Mario: punch hoo
    {0x24208081, 1.6, 0.0}, // Mario: mamma mia
    {0x20210001, 5.7, 0.0}, // Mario: okey dokey
    {0x2423F081, 2.0, 0.0}, // Mario: drowning
    {0x24248081, 0.9, 0.0}, // Mario: punch wah
    {0x30008081, 2.0, 0.0}, // activate cap switch
    {0x71198081, 1.2, 0.0}, // enter hole
    {0x30038081, 2.0, 0.0}, // flame out
    {0x3004C081, 1.7, 0.0}, // open wood door
    {0x3005C081, 1.0, 0.0}, // close wood door
    {0x3006C081, 1.4, 0.0}, // open iron door
    {0x3007C081, 1.1, 0.0}, // close iron door
    {0x30080001, 5.5, 0.0}, // bubbles
    {0x30090081, 2.2, 0.0}, // moving water
    {0x300A0081, 1.2, 0.0}, // swish water
    {0x300B0081, 0.8, 0.0}, // quiet bubble
    {0x300C8081, 2.5, 0.0}, // volcano explosion
    {0x300D0081, 0.9, 0.0}, // quiet bubble 2
    {0x300E8081, 1.3, 0.0}, // castle trap open
    {0x300F0081, 2.0, 0.0}, // wall explosion
    {0x38128081, 1.7, 0.0}, // coin water
    {0x30160091, 1.1, 0.0}, // short star
    {0x30170081, 0.9, 0.0}, // big clock
    {0x30180001, 5.6, 0.0}, // loud pound
    {0x301A0001, 5.7, 0.0}, // short pound 1
    {0x31208081, 2.4, 0.0}, // open chest
    {0x31228081, 2.0, 0.0}, // clam shell 1
    {0x30240081, 0.8, 0.0}, // box landing
    {0x30250081, 1.6, 0.0},
    {0x30264081, 2.0, 0.0}, // clam shell 2
    {0x30274081, 1.7, 0.0}, // clam shell 3
    {0x39280081, 1.8, 0.0}, // painting eject
    {0x701EFF81, 2.0, 0.0}, // star sound
    {0x302D8081, 2.2, 0.0}, // platform
    {0x302E2081, 2.1, 0.0}, // donut platform explosion
    {0x312F0081, 2.4, 0.0}, // Bowser bomb explosion
    {0x38300081, 1.2, 0.0}, // coin spurt
    {0x30310001, 5.6, 0.0}, // explosion 6
    {0x30320001, 5.7, 0.0},
    {0x30344081, 1.8, 0.0}, // boat tilt 1
    {0x30354081, 1.8, 0.0}, // boat tilt 2
    {0x30364081, 1.4, 0.0}, // coin drop
    {0x38378081, 1.5, 0.0},
    {0x30380081, 2.5, 0.0}, // pendulum swing
    {0x30390081, 1.4, 0.0}, // chain chomp 1
    {0x303A0081, 1.7, 0.0}, // chain chomp 2
    {0x303B0081, 0.9, 0.0}, // door turn key
    {0x303C0081, 1.3, 0.0}, // moving in sand
    {0x303D0081, 1.6, 0.0},
    {0x303E0081, 1.1, 0.0}, // moving platform switch
    {0x303FA081, 2.9, 0.0}, // cage open
    {0x30400081, 1.6, 0.0}, // quiet pound 1 low priority
    {0x3041C081, 2.1, 0.0}, // break box
    {0x30420081, 0.8, 0.0}, // door insert key
    {0x30430081, 3.2, 0.0}, // quiet pound 2
    {0x30440081, 1.9, 0.0}, // big pound
    {0x30450081, 2.7, 0.0},
    {0x30468081, 1.5, 0.0},
    {0x30478081, 3.3, 0.0}, // cannon up
    {0x30480081, 1.8, 0.0}, // Grindel roll
    {0x30490001, 5.6, 0.0}, // explosion 7
    {0x304A0001, 5.7, 0.0}, // shake coffin
    {0x814BE081, 6.8, 0.0}, // pyramid top spin
    {0x814CF081, 4.0, 0.0}, // pyramid top explosion
    {0x314D4081, 1.5, 0.0}, // race gun shot
    {0x304EC081, 1.4, 0.0}, // star door open
    {0x304FC081, 1.4, 0.0}, // star door close
    {0x80504001, 4.8, 0.0}, // bird chirp 2
    {0x50514001, 5.7, 0.0}, // bird chirp 3
    {0x90524001, 4.3, 0.0}, // bird chirp 1
    {0x60104001, 5.9, 0.0}, // castle outdoors ambient
    {0x8054F011, 5.9, 0.0}, // switch tick fast
    {0x8055F011, 5.9, 0.0}, // switch tick slow
    {0x30560081, 1.6, 0.0}, // pound rock
    {0x3057FF91, 2.0, 0.0}, // star appears
    {0x3058FF81, 1.6, 0.0}, // collect 1-up
    {0x80590081, 1.0, 0.0}, // rotating block alert
    {0x305A0081, 0.9, 0.0}, // button press low priority
    {0x305B0081, 0.7, 0.0}, // elevator move
    {0x305C0081, 0.9, 0.0}, // swish air
    {0x305D0081, 1.9, 0.0}, // haunted chair
    {0x305E0081, 0.8, 0.0}, // soft landing
    {0x305F0081, 0.9, 0.0}, // haunted chair move
    {0x80600081, 5.0, 7.0}, // Bowser explode
    {0x80610081, 4.3, 0.0}, // Bowser key
    {0x30628081, 2.3, 0.0}, // Bowser platform
    {0x8063D081, 3.4, 0.0}, // 1-up appear
    {0x3064C081, 1.9, 0.0}, // heart spin
    {0x3065C081, 1.2, 0.0}, // pound wood post
    {0x30668081, 1.4, 0.0}, // water level trigger
    {0x3067A081, 1.0, 0.0}, // switch door open
    {0x30689081, 1.6, 0.0}, // red coin
    {0x30690081, 5.9, 0.0}, // birds fly away
    {0x806AA081, 1.6, 0.0}, // right answer
    {0x306B8081, 1.4, 0.0}, // metal pound
    {0x306C4081, 1.1, 0.0}, // boing 1
    {0x306D2081, 1.2, 0.0}, // boing 2 low priority
    {0x30703081, 1.1, 0.0}, // Yoshi talk
    {0x30713081, 1.0, 0.0}, // splattering
    {0x30720001, 6.1, 0.0}, // boing 3
    {0x30730081, 3.8, 0.0}, // grand star
    {0x30740081, 1.4, 0.0}, // grand star jump
    {0x40000001, 6.2, 0.0}, // waterfall 1
    {0x40020001, 5.6, 0.0}, // elevator 1
    {0x41030001, 5.6, 0.0}, // droning 1
    {0x40050001, 6.0, 0.0}, // wind 1
    {0x40060001, 5.7, 0.0}, // moving sand snow
    {0x40070001, 5.7, 0.0},
    {0x40090001, 5.8, 0.0}, // water
    {0x400A0001, 5.7, 0.0},
    {0x400B0001, 5.8, 0.0}, // boat rocking 1
    {0x400C0001, 6.2, 0.0}, // elevator 3
    {0x400D0001, 5.7, 0.0}, // elevator 4
    {0x400E0001, 6.6, 0.0}, // moving sand
    {0x400F4001, 5.8, 0.0}, // merry go round creaking
    {0x40108001, 6.0, 0.0}, // wind 2
    {0x60050001, 5.6, 0.0}, // rough slide
    {0x40120001, 5.6, 0.0},
    {0x40130001, 6.0, 0.0}, // sliding
    {0x40140011, 5.7, 0.0}, // star
    {0x41150001, 6.6, 0.0}, // moving big platform
    {0x41160001, 5.9, 0.0}, // water drain
    {0x40178001, 5.7, 0.0}, // metal box push
    {0x40188001, 5.7, 0.0}, // sink quicksand
    {0x600B4001, 6.2, 0.0}, // Peach twinkle
    {0x50008081, 1.7, 0.0}, // Sushi shark water sound
    {0x50010081, 1.0, 0.0}, // Mr. I shoot
    {0x50020081, 0.8, 0.0}, // baby penguin walk
    {0x50030081, 1.9, 0.0}, // Bowser walk
    {0x90040081, 2.4, 0.0}, // Bowser roar
    {0x50050081, 1.7, 0.0}, // Bowser tail pickup
    {0x50060081, 3.8, 0.0}, // Bowser defeated
    {0x50070081, 1.2, 0.0}, // Bowser spinning
    {0x50080081, 1.8, 0.0}, // Bowser inhaling
    {0x50098081, 0.8, 0.0}, // big penguin walk
    {0x500A0081, 0.9, 0.0}, // Boo bounce top
    {0x500B0081, 0.9, 0.0}, // Boo laugh short
    {0x500CA081, 1.8, 0.0}, // Thwomp
    {0x500DF081, 2.9, 0.0}, // cannon 1
    {0x500EF081, 2.9, 0.0}, // cannon 2
    {0x500FF081, 1.1, 0.0}, // cannon 3
    {0x90105081, 0.8, 0.0}, // Piranha plant bite
    {0x90116081, 1.5, 0.0}, // Piranha plant dying
    {0x50120001, 5.7, 0.0}, // jump walk water
    {0x50130081, 1.5, 0.0},
    {0x50140081, 0.9, 0.0}, // Mr. I death
    {0x50155081, 1.0, 0.0}, // pounding 1
    {0x50166081, 1.1, 0.0}, // Whomp
    {0x50178081, 1.0, 0.0}, // Bully metal
    {0x5018A081, 1.6, 0.0}, // Bully explode
    {0x90192081, 0.8, 0.0}, // Bowser puzzle piece move
    {0x501A5081, 1.8, 0.0}, // pounding cannon
    {0x501B3081, 0.8, 0.0}, // Bully walk
    {0x901C0081, 1.1, 0.0}, // Bully attacked
    {0x501D8081, 0.9, 0.0},
    {0x501EA081, 1.0, 0.0},
    {0x501F4081, 0.9, 0.0}, // baby penguin dive
    {0x50200081, 0.8, 0.0}, // Goomba walk
    {0x50210081, 1.2, 0.0}, // Ukiki chatter long
    {0x50220081, 1.6, 0.0}, // Monty Mole attack
    {0x50230001, 5.4, 0.0},
    {0x50244081, 1.0, 0.0}, // dying enemy 1
    {0x50254081, 1.7, 0.0}, // cannon 4
    {0x50260001, 5.7, 0.0}, // dying enemy 2
    {0x50270081, 0.7, 0.0}, // Bob-omb walk
    {0x50288081, 1.6, 0.0}, // something landing
    {0x5029A081, 1.5, 0.0}, // diving in water
    {0x502A0081, 1.0, 0.0}, // snow sand 1
    {0x502B0081, 1.0, 0.0}, // snow sand 2
    {0x502C8081, 1.4, 0.0}, // default death
    {0x502D0081, 1.2, 0.0}, // big penguin yell
    {0x502E8081, 1.8, 0.0}, // water bomb bouncing
    {0x502F0081, 1.6, 0.0}, // Goomba alert
    {0x50308081, 0.9, 0.0}, // stomped
    {0x50310081, 0.8, 0.0},
    {0x50324081, 1.5, 0.0}, // diving into water
    {0x50334081, 2.0, 0.0}, // Piranha plant shrink
    {0x50342081, 0.8, 0.0}, // Koopa the quick walk
    {0x50350081, 0.8, 0.0}, // Koopa walk
    {0x50366081, 0.8, 0.0}, // Bully walking
    {0x50376081, 1.6, 0.0}, // Dorrie
    {0x50388081, 4.1, 0.0}, // Bowser laugh
    {0x50390081, 0.8, 0.0}, // Ukiki chatter short
    {0x503A0081, 1.0, 0.0}, // Ukiki chatter idle
    {0x503B0081, 0.7, 0.0}, // Ukiki step default
    {0x503C0081, 0.8, 0.0}, // Ukiki step leaves
    {0x503DA081, 1.1, 0.0}, // Koopa talk
    {0x503EA081, 0.9, 0.0}, // Koopa damage
    {0x503F4081, 0.9, 0.0}, // Klepto 1
    {0x50406081, 0.8, 0.0}, // Klepto 2
    {0x50410081, 2.1, 0.0}, // King Bob-omb talk
    {0x91424081, 1.8, 0.0}, // King Bob-omb damage
    {0x90434081, 0.7, 0.0}, // Scuttlebug walk
    {0x90444081, 0.8, 0.0}, // Scuttlebug alert
    {0x90450081, 1.2, 0.0}, // baby penguin yell
    {0x50468081, 1.0, 0.0}, // King Bob-omb jump
    {0x5147C081, 2.2, 0.0}, // King Whomp death
    {0x50480081, 1.7, 0.0}, // Boo laugh long
    {0x90490081, 0.7, 0.0}, // Swoop
    {0x504A0081, 2.3, 0.0}, // eel
    {0x524B0081, 1.4, 0.0}, // Eyerok show eye
    {0x504C0081, 1.5, 0.0}, // Mr. Blizzard alert
    {0x504D0081, 0.8, 0.0}, // Snufit shoot
    {0x504E0081, 0.8, 0.0}, // Skeeter walk
    {0x504F0081, 1.2, 0.0}, // walking water
    {0x50542081, 1.6, 0.0}, // Piranha plant appear
    {0x50558081, 1.1, 0.0}, // flame blown
    {0x52564081, 1.4, 0.0}, // mad piano chomping
    {0x90570081, 1.3, 0.0}, // large Bully attacked
    {0x50584081, 1.5, 0.0}, // Bob-omb buddy talk
    {0x50591081, 0.8, 0.0}, // Spiny (other)
    {0x935A0081, 1.0, 0.0}, // Eyerok sound short
    {0x925B0081, 1.9, 0.0}, // Eyerok sound long
    {0x505C4081, 1.1, 0.0}, // Wiggler high pitch
    {0x505D4081, 1.1, 0.0}, // Heave-Ho tossed
    {0x505E0001, 6.3, 0.0}, // Wiggler death
    {0x505F8091, 5.5, 0.0}, // Bowser intro laugh
    {0x5060B081, 1.6, 0.0}, // enemy death high
    {0x5061B081, 1.7, 0.0}, // enemy death low
    {0x5062B081, 1.4, 0.0}, // Swoop death
    {0x5063B081, 1.5, 0.0}, // Koopa Fly Guy death
    {0x5064C081, 1.8, 0.0}, // snowman bounce
    {0x5065D081, 1.7, 0.0}, // snowman explode
    {0x90668081, 1.4, 0.0}, // Bowser teleport
    {0x90678081, 1.6, 0.0}, // Monty Mole appear
    {0x50684081, 1.8, 0.0}, // pounding loud
    {0x90694081, 1.4, 0.0}, // boss dialog grunt
    {0x506A0081, 0.8, 0.0}, // MIPS rabbit
    {0x906B0081, 1.0, 0.0}, // Mr. I spinning
    {0x506C0081, 1.0, 0.0}, // MIPS rabbit water
    {0x506D0081, 2.9, 0.0}, // Eyerok explode
    {0x516E0081, 2.2, 0.0}, // Chuckya death
    {0x506F0081, 1.6, 0.0}, // Wiggler talk
    {0x50706081, 0.9, 0.0}, // Wiggler attacked
    {0x50712081, 1.2, 0.0}, // Wiggler low pitch
    {0x5072C081, 1.6, 0.0}, // Snufit Skeeter death
    {0x50734081, 1.1, 0.0}, // Bubba chomp
    {0x50744081, 1.4, 0.0}, // enemy defeat shrink
    {0x60000001, 5.8, 0.0}, // Bowser spit fire
    {0x60010001, 5.7, 0.0},
    {0x60028001, 5.6, 0.0}, // Lakitu fly
    {0x60034001, 5.6, 0.0}, // Amp buzz
    {0x60048001, 5.8, 0.0}, // blow fire
    {0x60064001, 5.7, 0.0}, // Heave-Ho move
    {0x60070001, 5.6, 0.0},
    {0x60086001, 5.6, 0.0}, // Bob-omb lit fuse
    {0x60098001, 7.3, 0.0}, // howling wind
    {0x600A4001, 5.6, 0.0}, // Chuckya move
    {0x7000F881, 0.9, 0.0}, // change select
    {0x70010001, 5.7, 0.0}, // reverse pause
    {0x7002FF81, 1.3, 0.0}, // pause
    {0x70040081, 1.0, 0.0}, // message appear
    {0x70050081, 1.0, 0.0}, // message disappear
    {0x70060081, 1.1, 0.0}, // camera zoom in
    {0x70070081, 1.1, 0.0}, // camera zoom out
    {0x70080081, 0.8, 0.0}, // pinch Mario face
    {0x70090081, 1.7, 0.0}, // let go Mario face
    {0x700A0081, 0.9, 0.0}, // hand appear
    {0x700B0081, 0.9, 0.0}, // hand disappear
    {0x700C0081, 1.3, 0.0},
    {0x700D0081, 1.6, 0.0}, // power meter
    {0x700E0081, 0.8, 0.0}, // camera buzz
    {0x700F0081, 1.5, 0.0}, // camera turn
    {0x70110081, 0.8, 0.0}, // click file select
    {0x70128081, 1.5, 0.0},
    {0x70130081, 0.7, 0.0}, // message next page
    {0x70140081, 3.9, 0.0}, // coin it's a me Mario
    {0x70150081, 1.4, 0.0}, // Yoshi gain lives
    {0x7016A081, 2.1, 0.0}, // enter pipe
    {0x7017A081, 2.2, 0.0}, // exit pipe
    {0x70188081, 3.2, 0.0}, // Bowser laugh
    {0x701A8081, 0.8, 0.0}, // click change view
    {0x701B0001, 5.7, 0.0}, // camera unused 1
    {0x701C0001, 5.7, 0.0}, // camera unused 2
    {0x701DB081, 2.8, 0.0}, // Mario castle warp
    {0x701FFF81, 3.1, 0.0}, // thank you playing my game
    {0x70200001, 5.6, 0.0}, // read a sign
    {0x70210001, 5.6, 0.0}, // exit a sign
    {0x70222081, 2.3, 0.0}, // Mario castle warp 2
};
// clang-format on

// The title of the sound with `code`, such as "Bank 5, sound 0x61".
std::string SoundTitle(uint32_t code)
{
    char title[32];
    std::snprintf(title, sizeof(title), "Bank %u, sound 0x%02X", static_cast<unsigned>(code >> kBankShift),
                  static_cast<unsigned>((code >> kSoundShift) & kSoundMask));

    return title;
}

} // namespace

GameProfile NusNsmeUsa()
{
    GameProfile profile;
    profile.game = "Super Mario 64";
    profile.serial = "NUS-NSME-USA";
    profile.sha1 = "9bef1128717f958171a4afac3ed78ee2bb4e86ce";
    profile.set = "sound effects";
    profile.sound_effects = true;
    profile.cic = ultralib::machine::Cic::k6102;
    profile.sound_driver = kSuperMario64SoundEngine;

    // The driver takes over the game thread at the first return from the sound reset. It requests the track's sound
    // once. A sound that plays only while requested is requested again in each of the first 150 frames. Each frame ends
    // with the game-loop tick. A frame is two retraces, as in the game. The driver drops the message that waits at the
    // snapshot, then waits for two messages in each frame, as the game thread does. The idle thread runs meanwhile.
    profile.snapshot_address = kAfterSoundReset;
    profile.snapshot_hit = 1;
    profile.quiet_tries = 30;
    profile.boot_seconds = 5.0;

    const std::vector<CallArgument> kArguments = {{.track = true}, {.value = kGlobalSource}};
    CallDriverOptions driver;
    driver.address = kDriverAddress;
    driver.start_calls = {
        {.function = kPlaySound, .arguments = kArguments, .track_mask = kDiscrete, .track_value = kDiscrete},
        MessageDrop(kRecvMesg, kRetraceQueue)};
    driver.frame_calls = {{.function = kPlaySound,
                           .arguments = kArguments,
                           .track_mask = kDiscrete,
                           .track_value = 0,
                           .frames = kHeldFrames},
                          {.function = kGameLoopTick}};
    driver.wait_call = MessageWait(kRecvMesg, kRetraceQueue);
    driver.fields = 2;
    profile.driver = BuildCallDriver(driver);

    for (const Sound& sound : kSounds)
    {
        Track track;
        track.number = sound.code;
        track.name = SoundTitle(sound.code);
        track.length_seconds = sound.seconds;
        if (sound.fade > 0.0)
        {
            track.fade_seconds = sound.fade;
        }
        profile.tracks.push_back(track);
    }

    return profile;
}

} // namespace usf::rip::games
