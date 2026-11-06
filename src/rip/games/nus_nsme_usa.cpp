// SPDX-License-Identifier: MIT

// Super Mario 64 (USA): the game's sound effects, played one at a time through the game's sound-effect interface.
//
// Addresses come from the game's code in the image and from runs in usf's core.
//
// - The play-sound function at 0x8031EB00 takes a sound's code in a0 and a pointer to its source's position in a1.
//   It queues a request. The global source at 0x803331F0 plays a sound at the listener, with no pan or distance.
// - The game-loop tick at 0x8031FD84 takes no arguments. It sets the word at 0x80332E54. The sound thread then
//   updates the effects at its next audio frame. The game calls the tick once a frame, every second video field.
// - A code holds the bank in bits 28 to 31, flags in bits 24 to 27, the sound's number in bits 16 to 23 and its
//   priority in bits 8 to 15. Bit 7 marks a sound that plays once. Bit 0 marks a new request. A sound without bit 7
//   plays only while the game keeps requesting it.
// - The sound reset at 0x80322230 returns to 0x802491D8 in the game thread. The first return comes before any music and
//   before the first effect.
// - The sound-player sequence holds a channel table for each bank. Banks 3 and 8 share one, and so do banks 5 and 9.
//   The tables hold 542 entries. An entry whose script is the same as an earlier entry's plays the same sound. The
//   profile leaves those out. A sound with variants by ground (default, grass, water, stone, creaky floor, snow, ice
//   and sand) adds the variant to its number. A variant is a track of its own only where its script differs. That
//   leaves 431 sounds. One of them, menu sound 0x10, makes no sound when played alone. The profile leaves it out too.
//   That leaves 430 tracks, 89 of them sounds that play only while requested.
// - The lengths come from runs of each track in usf's core: the last sound and 0.5 s more. A sound that plays only
//   while requested is requested for 5 s. Sound 0x60 of bank 8 plays once but holds its level until the game stops
//   it. Its track plays for 5 s and fades out over 2 s.
// - A track's number is the sound's code. Its title gives the bank and the sound's number, and a description where
//   one is known.

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

// The last lines of the third frame buffer (0x803DA800 to 0x80400000). The game's set-up stores the three frame
// buffers' addresses at 0x802489C4 to 0x80248A20: 0x8038F800, 0x803B5000 and 0x803DA800. Only the RDP writes there,
// and it draws nothing after the snapshot.
constexpr uint32_t kDriverAddress = 0x803FF000;

constexpr uint32_t kDiscrete = 0x80;

// A sound that plays only while requested is requested for 150 frames, 5 s.
constexpr uint32_t kHeldFrames = 150;

struct Sound
{
    uint32_t code;
    const char* title;
    double seconds; // The track's length.
    double fade;    // The track's fade.
};

// clang-format off
const Sound kSounds[] = {
    {0x04008081, "Action 0x00: jump (default)", 0.8, 0.0},
    {0x04018081, "Action 0x01: jump (grass)", 0.8, 0.0},
    {0x04028081, "Action 0x02: jump (water)", 1.0, 0.0},
    {0x04038081, "Action 0x03: jump (stone)", 0.8, 0.0},
    {0x04048081, "Action 0x04: jump (creaky floor)", 0.9, 0.0},
    {0x04058081, "Action 0x05: jump (snow)", 0.9, 0.0},
    {0x04068081, "Action 0x06: jump (ice)", 0.8, 0.0},
    {0x04078081, "Action 0x07: jump (sand)", 1.0, 0.0},
    {0x04088081, "Action 0x08: landing (default)", 0.8, 0.0},
    {0x04098081, "Action 0x09: landing (grass)", 0.9, 0.0},
    {0x040A8081, "Action 0x0A: landing (water)", 1.0, 0.0},
    {0x040B8081, "Action 0x0B: landing (stone)", 0.9, 0.0},
    {0x040C8081, "Action 0x0C: landing (creaky floor)", 1.0, 0.0},
    {0x040D8081, "Action 0x0D: landing (snow)", 1.0, 0.0},
    {0x040E8081, "Action 0x0E: landing (ice)", 0.9, 0.0},
    {0x040F8081, "Action 0x0F: landing (sand)", 1.0, 0.0},
    {0x06108081, "Action 0x10: step (default)", 0.8, 0.0},
    {0x06118081, "Action 0x11: step (grass)", 1.0, 0.0},
    {0x06128081, "Action 0x12: step (water)", 1.1, 0.0},
    {0x06138081, "Action 0x13: step (stone)", 0.9, 0.0},
    {0x06148081, "Action 0x14: step (creaky floor)", 1.1, 0.0},
    {0x06158081, "Action 0x15: step (snow)", 1.1, 0.0},
    {0x06168081, "Action 0x16: step (ice)", 0.9, 0.0},
    {0x06178081, "Action 0x17: step (sand)", 1.1, 0.0},
    {0x04188081, "Action 0x18: body hit ground (default)", 0.8, 0.0},
    {0x04198081, "Action 0x19: body hit ground (grass)", 1.0, 0.0},
    {0x041A8081, "Action 0x1A: body hit ground (water)", 1.0, 0.0},
    {0x041B8081, "Action 0x1B: body hit ground (stone)", 0.9, 0.0},
    {0x041C8081, "Action 0x1C: body hit ground (creaky floor)", 1.0, 0.0},
    {0x041D8081, "Action 0x1D: body hit ground (snow)", 1.0, 0.0},
    {0x041E8081, "Action 0x1E: body hit ground (ice)", 0.9, 0.0},
    {0x041F8081, "Action 0x1F: body hit ground (sand)", 1.0, 0.0},
    {0x06208081, "Action 0x20: step tiptoe (default)", 0.8, 0.0},
    {0x06218081, "Action 0x21: step tiptoe (grass)", 0.9, 0.0},
    {0x06228081, "Action 0x22: step tiptoe (water)", 1.0, 0.0},
    {0x06238081, "Action 0x23: step tiptoe (stone)", 0.8, 0.0},
    {0x06248081, "Action 0x24: step tiptoe (creaky floor)", 0.9, 0.0},
    {0x06258081, "Action 0x25: step tiptoe (snow)", 0.9, 0.0},
    {0x06268081, "Action 0x26: step tiptoe (ice)", 0.9, 0.0},
    {0x06278081, "Action 0x27: step tiptoe (sand)", 1.1, 0.0},
    {0x04289081, "Action 0x28: metal jump", 1.0, 0.0},
    {0x04299081, "Action 0x29: metal landing", 1.1, 0.0},
    {0x042A9081, "Action 0x2A: metal step", 0.9, 0.0},
    {0x042B9081, "Action 0x2B: metal heavy landing", 1.1, 0.0},
    {0x062C0081, "Action 0x2C: clap hands cold", 1.0, 0.0},
    {0x042DA081, "Action 0x2D: hanging step", 0.7, 0.0},
    {0x042E0081, "Action 0x2E: quicksand step", 0.8, 0.0},
    {0x042F9081, "Action 0x2F: metal step tiptoe", 0.9, 0.0},
    {0x0430C081, "Action 0x30", 2.0, 0.0},
    {0x04316081, "Action 0x31", 1.3, 0.0},
    {0x04328081, "Action 0x32", 1.3, 0.0},
    {0x04338081, "Action 0x33: swim", 1.3, 0.0},
    {0x04348081, "Action 0x34", 2.0, 0.0},
    {0x04358081, "Action 0x35: throw", 0.8, 0.0},
    {0x04368081, "Action 0x36: key swish", 0.8, 0.0},
    {0x04378081, "Action 0x37: spin", 0.8, 0.0},
    {0x043A8081, "Action 0x3A: climb up tree", 1.1, 0.0},
    {0x003B0001, "Action 0x3B: climb down tree", 5.6, 0.0},
    {0x003C0001, "Action 0x3C", 5.6, 0.0},
    {0x043D8081, "Action 0x3D", 0.8, 0.0},
    {0x043E8081, "Action 0x3E", 0.8, 0.0},
    {0x043F8081, "Action 0x3F: pat back", 0.8, 0.0},
    {0x04408081, "Action 0x40: brush hair", 0.8, 0.0},
    {0x04418081, "Action 0x41: climb up pole", 1.0, 0.0},
    {0x04428081, "Action 0x42: metal bonk", 1.1, 0.0},
    {0x04438081, "Action 0x43: unstuck from ground", 1.2, 0.0},
    {0x0444C081, "Action 0x44: hit", 1.7, 0.0},
    {0x0445A081, "Action 0x45: bonk", 0.8, 0.0},
    {0x0446A081, "Action 0x46: shrink into the haunted house", 2.8, 0.0},
    {0x0447A081, "Action 0x47: swim fast", 1.7, 0.0},
    {0x04488081, "Action 0x48: stuck in ground (default)", 0.7, 0.0},
    {0x044D8081, "Action 0x4D: stuck in ground (snow)", 0.8, 0.0},
    {0x044E8081, "Action 0x4E: stuck in ground (ice)", 0.8, 0.0},
    {0x04509081, "Action 0x50: metal jump water", 1.1, 0.0},
    {0x04519081, "Action 0x51: metal land water", 1.2, 0.0},
    {0x04529081, "Action 0x52: metal step water", 1.0, 0.0},
    {0x00530001, "Action 0x53", 5.7, 0.0},
    {0x00540001, "Action 0x54", 5.7, 0.0},
    {0x00550001, "Action 0x55", 5.7, 0.0},
    {0x04568081, "Action 0x56: flying fast", 1.8, 0.0},
    {0x0457C081, "Action 0x57: teleport", 1.1, 0.0},
    {0x0458A081, "Action 0x58", 0.8, 0.0},
    {0x0459B081, "Action 0x59: bounce off object", 1.0, 0.0},
    {0x045A8081, "Action 0x5A: side flip (other)", 0.9, 0.0},
    {0x045BFF81, "Action 0x5B: read sign", 1.5, 0.0},
    {0x045C8081, "Action 0x5C", 1.0, 0.0},
    {0x005D0001, "Action 0x5D", 5.7, 0.0},
    {0x04608081, "Action 0x60: heavy landing (default)", 1.1, 0.0},
    {0x04618081, "Action 0x61: heavy landing (grass)", 1.1, 0.0},
    {0x04628081, "Action 0x62: heavy landing (water)", 1.1, 0.0},
    {0x04638081, "Action 0x63: heavy landing (stone)", 1.1, 0.0},
    {0x04648081, "Action 0x64: heavy landing (creaky floor)", 1.1, 0.0},
    {0x04658081, "Action 0x65: heavy landing (snow)", 1.1, 0.0},
    {0x04668081, "Action 0x66: heavy landing (ice)", 1.1, 0.0},
    {0x04678081, "Action 0x67: heavy landing (sand)", 1.1, 0.0},
    {0x14000001, "Movement 0x00: slide (default)", 5.6, 0.0},
    {0x14010001, "Movement 0x01: slide (grass)", 5.6, 0.0},
    {0x14020001, "Movement 0x02: slide (water)", 5.6, 0.0},
    {0x14030001, "Movement 0x03: slide (stone)", 5.6, 0.0},
    {0x14040001, "Movement 0x04: slide (creaky floor)", 5.6, 0.0},
    {0x14050001, "Movement 0x05: slide (snow)", 5.6, 0.0},
    {0x14060001, "Movement 0x06: slide (ice)", 5.6, 0.0},
    {0x14070001, "Movement 0x07: slide (sand)", 5.6, 0.0},
    {0x14100001, "Movement 0x10: lava burn", 5.7, 0.0},
    {0x14110001, "Movement 0x11: slide down pole", 5.7, 0.0},
    {0x14128001, "Movement 0x12: slide down tree", 5.6, 0.0},
    {0x38118081, "General 0x11: coin", 1.6, 0.0},
    {0x14140001, "Movement 0x14: quicksand death", 5.7, 0.0},
    {0x14160001, "Movement 0x16: shocked", 5.7, 0.0},
    {0x14170001, "Movement 0x17: flying", 5.7, 0.0},
    {0x1C180001, "Movement 0x18: almost drowning", 6.2, 0.0},
    {0x1D192001, "Movement 0x19: aim cannon", 5.7, 0.0},
    {0x101A0001, "Movement 0x1A", 5.6, 0.0},
    {0x14200001, "Movement 0x20: riding shell (default)", 5.6, 0.0},
    {0x14210001, "Movement 0x21: riding shell (grass)", 5.7, 0.0},
    {0x14220001, "Movement 0x22: riding shell (water)", 5.7, 0.0},
    {0x14230001, "Movement 0x23: riding shell (stone)", 5.6, 0.0},
    {0x14240001, "Movement 0x24: riding shell (creaky floor)", 5.6, 0.0},
    {0x14250001, "Movement 0x25: riding shell (snow)", 5.6, 0.0},
    {0x14260001, "Movement 0x26: riding shell (ice)", 5.6, 0.0},
    {0x14270001, "Movement 0x27: riding shell (sand)", 5.7, 0.0},
    {0x14280001, "Movement 0x28: riding shell lava", 5.7, 0.0},
    {0x24008081, "Voice 0x00: Mario: yah wah hoo", 0.9, 0.0},
    {0x20018081, "Voice 0x01: Mario: yah wah hoo (variant)", 0.9, 0.0},
    {0x20028081, "Voice 0x02: Mario: yah wah hoo (variant)", 0.8, 0.0},
    {0x24038081, "Voice 0x03: Mario: hoo-hoo", 1.1, 0.0},
    {0x24048081, "Voice 0x04: Mario: yahoo", 1.6, 0.0},
    {0x24058081, "Voice 0x05: Mario: uh", 1.0, 0.0},
    {0x24068081, "Voice 0x06: Mario: hrmm", 1.0, 0.0},
    {0x24078081, "Voice 0x07: Mario: wah 2", 0.9, 0.0},
    {0x2408C081, "Voice 0x08: Mario: whoa", 1.1, 0.0},
    {0x24098081, "Voice 0x09: Mario: eeuh", 1.4, 0.0},
    {0x240AFF81, "Voice 0x0A: Mario: attacked", 1.1, 0.0},
    {0x240B8081, "Voice 0x0B: Mario: ooof", 1.1, 0.0},
    {0x240C8081, "Voice 0x0C: Mario: here we go", 2.0, 0.0},
    {0x240D8081, "Voice 0x0D: Mario: yawning", 2.0, 0.0},
    {0x240E8081, "Voice 0x0E: Mario: snoring 1", 1.6, 0.0},
    {0x240F8081, "Voice 0x0F: Mario: snoring 2", 1.0, 0.0},
    {0x2410C081, "Voice 0x10: Mario: waaaooow", 2.4, 0.0},
    {0x24118081, "Voice 0x11: Mario: ha-ha", 1.4, 0.0},
    {0x24188081, "Voice 0x18: Mario: panting", 1.3, 0.0},
    {0x2413D081, "Voice 0x13: Mario: uh 2", 0.8, 0.0},
    {0x2414A081, "Voice 0x14: Mario: on fire", 2.7, 0.0},
    {0x2415FF81, "Voice 0x15: Mario: dying", 2.1, 0.0},
    {0x24168081, "Voice 0x16: Mario: panting cold", 1.1, 0.0},
    {0x241D8081, "Voice 0x1D: Mario: coughing 3", 0.9, 0.0},
    {0x20198081, "Voice 0x19: Mario: panting (variant)", 1.3, 0.0},
    {0x201A8081, "Voice 0x1A: Mario: panting (variant)", 1.3, 0.0},
    {0x241B8081, "Voice 0x1B: Mario: coughing 1", 0.8, 0.0},
    {0x241C8081, "Voice 0x1C: Mario: coughing 2", 0.9, 0.0},
    {0x241E8081, "Voice 0x1E: Mario: punch yah", 0.9, 0.0},
    {0x241F8081, "Voice 0x1F: Mario: punch hoo", 1.1, 0.0},
    {0x24208081, "Voice 0x20: Mario: mamma mia", 1.6, 0.0},
    {0x20210001, "Voice 0x21: Mario: okey dokey", 5.7, 0.0},
    {0x2423F081, "Voice 0x23: Mario: drowning", 2.0, 0.0},
    {0x24248081, "Voice 0x24: Mario: punch wah", 0.9, 0.0},
    {0x30008081, "General 0x00: activate cap switch", 2.0, 0.0},
    {0x71198081, "Menu 0x19: enter hole", 1.2, 0.0},
    {0x30038081, "General 0x03: flame out", 2.0, 0.0},
    {0x3004C081, "General 0x04: open wood door", 1.7, 0.0},
    {0x3005C081, "General 0x05: close wood door", 1.0, 0.0},
    {0x3006C081, "General 0x06: open iron door", 1.4, 0.0},
    {0x3007C081, "General 0x07: close iron door", 1.1, 0.0},
    {0x30080001, "General 0x08: bubbles", 5.5, 0.0},
    {0x30090081, "General 0x09: moving water", 2.2, 0.0},
    {0x300A0081, "General 0x0A: swish water", 1.2, 0.0},
    {0x300B0081, "General 0x0B: quiet bubble", 0.8, 0.0},
    {0x300C8081, "General 0x0C: volcano explosion", 2.5, 0.0},
    {0x300D0081, "General 0x0D: quiet bubble 2", 0.9, 0.0},
    {0x300E8081, "General 0x0E: castle trap open", 1.3, 0.0},
    {0x300F0081, "General 0x0F: wall explosion", 2.0, 0.0},
    {0x38128081, "General 0x12: coin water", 1.7, 0.0},
    {0x30160091, "General 0x16: short star", 1.1, 0.0},
    {0x30170081, "General 0x17: big clock", 0.9, 0.0},
    {0x30180001, "General 0x18: loud pound", 5.6, 0.0},
    {0x301A0001, "General 0x1A: short pound 1", 5.7, 0.0},
    {0x31208081, "General 0x20: open chest", 2.4, 0.0},
    {0x31228081, "General 0x22: clam shell 1", 2.0, 0.0},
    {0x30240081, "General 0x24: box landing", 0.8, 0.0},
    {0x30250081, "General 0x25", 1.6, 0.0},
    {0x30264081, "General 0x26: clam shell 2", 2.0, 0.0},
    {0x30274081, "General 0x27: clam shell 3", 1.7, 0.0},
    {0x39280081, "General 0x28: painting eject", 1.8, 0.0},
    {0x701EFF81, "Menu 0x1E: star sound", 2.0, 0.0},
    {0x302D8081, "General 0x2D: platform", 2.2, 0.0},
    {0x302E2081, "General 0x2E: donut platform explosion", 2.1, 0.0},
    {0x312F0081, "General 0x2F: Bowser bomb explosion", 2.4, 0.0},
    {0x38300081, "General 0x30: coin spurt", 1.2, 0.0},
    {0x30310001, "General 0x31: explosion 6", 5.6, 0.0},
    {0x30320001, "General 0x32", 5.7, 0.0},
    {0x30344081, "General 0x34: boat tilt 1", 1.8, 0.0},
    {0x30354081, "General 0x35: boat tilt 2", 1.8, 0.0},
    {0x30364081, "General 0x36: coin drop", 1.4, 0.0},
    {0x38378081, "General 0x37", 1.5, 0.0},
    {0x30380081, "General 0x38: pendulum swing", 2.5, 0.0},
    {0x30390081, "General 0x39: chain chomp 1", 1.4, 0.0},
    {0x303A0081, "General 0x3A: chain chomp 2", 1.7, 0.0},
    {0x303B0081, "General 0x3B: door turn key", 0.9, 0.0},
    {0x303C0081, "General 0x3C: moving in sand", 1.3, 0.0},
    {0x303D0081, "General 0x3D", 1.6, 0.0},
    {0x303E0081, "General 0x3E: moving platform switch", 1.1, 0.0},
    {0x303FA081, "General 0x3F: cage open", 2.9, 0.0},
    {0x30400081, "General 0x40: quiet pound 1 low priority", 1.6, 0.0},
    {0x3041C081, "General 0x41: break box", 2.1, 0.0},
    {0x30420081, "General 0x42: door insert key", 0.8, 0.0},
    {0x30430081, "General 0x43: quiet pound 2", 3.2, 0.0},
    {0x30440081, "General 0x44: big pound", 1.9, 0.0},
    {0x30450081, "General 0x45", 2.7, 0.0},
    {0x30468081, "General 0x46", 1.5, 0.0},
    {0x30478081, "General 0x47: cannon up", 3.3, 0.0},
    {0x30480081, "General 0x48: Grindel roll", 1.8, 0.0},
    {0x30490001, "General 0x49: explosion 7", 5.6, 0.0},
    {0x304A0001, "General 0x4A: shake coffin", 5.7, 0.0},
    {0x814BE081, "General 2 0x4B: pyramid top spin", 6.8, 0.0},
    {0x814CF081, "General 2 0x4C: pyramid top explosion", 4.0, 0.0},
    {0x314D4081, "General 0x4D: race gun shot", 1.5, 0.0},
    {0x304EC081, "General 0x4E: star door open", 1.4, 0.0},
    {0x304FC081, "General 0x4F: star door close", 1.4, 0.0},
    {0x80504001, "General 2 0x50: bird chirp 2", 4.8, 0.0},
    {0x50514001, "Object 0x51: bird chirp 3", 5.7, 0.0},
    {0x90524001, "Object 2 0x52: bird chirp 1", 4.3, 0.0},
    {0x60104001, "Air 0x10: castle outdoors ambient", 5.9, 0.0},
    {0x8054F011, "General 2 0x54: switch tick fast", 5.9, 0.0},
    {0x8055F011, "General 2 0x55: switch tick slow", 5.9, 0.0},
    {0x30560081, "General 0x56: pound rock", 1.6, 0.0},
    {0x3057FF91, "General 0x57: star appears", 2.0, 0.0},
    {0x3058FF81, "General 0x58: collect 1-up", 1.6, 0.0},
    {0x80590081, "General 2 0x59: rotating block alert", 1.0, 0.0},
    {0x305A0081, "General 0x5A: button press low priority", 0.9, 0.0},
    {0x305B0081, "General 0x5B: elevator move", 0.7, 0.0},
    {0x305C0081, "General 0x5C: swish air", 0.9, 0.0},
    {0x305D0081, "General 0x5D: haunted chair", 1.9, 0.0},
    {0x305E0081, "General 0x5E: soft landing", 0.8, 0.0},
    {0x305F0081, "General 0x5F: haunted chair move", 0.9, 0.0},
    {0x80600081, "General 2 0x60: Bowser explode", 5.0, 2.0},
    {0x80610081, "General 2 0x61: Bowser key", 4.3, 0.0},
    {0x30628081, "General 0x62: Bowser platform", 2.3, 0.0},
    {0x8063D081, "General 2 0x63: 1-up appear", 3.4, 0.0},
    {0x3064C081, "General 0x64: heart spin", 1.9, 0.0},
    {0x3065C081, "General 0x65: pound wood post", 1.2, 0.0},
    {0x30668081, "General 0x66: water level trigger", 1.4, 0.0},
    {0x3067A081, "General 0x67: switch door open", 1.0, 0.0},
    {0x30689081, "General 0x68: red coin", 1.6, 0.0},
    {0x30690081, "General 0x69: birds fly away", 5.9, 0.0},
    {0x806AA081, "General 2 0x6A: right answer", 1.6, 0.0},
    {0x306B8081, "General 0x6B: metal pound", 1.4, 0.0},
    {0x306C4081, "General 0x6C: boing 1", 1.1, 0.0},
    {0x306D2081, "General 0x6D: boing 2 low priority", 1.2, 0.0},
    {0x30703081, "General 0x70: Yoshi talk", 1.1, 0.0},
    {0x30713081, "General 0x71: splattering", 1.0, 0.0},
    {0x30720001, "General 0x72: boing 3", 6.1, 0.0},
    {0x30730081, "General 0x73: grand star", 3.8, 0.0},
    {0x30740081, "General 0x74: grand star jump", 1.4, 0.0},
    {0x40000001, "Environment 0x00: waterfall 1", 6.2, 0.0},
    {0x40020001, "Environment 0x02: elevator 1", 5.6, 0.0},
    {0x41030001, "Environment 0x03: droning 1", 5.6, 0.0},
    {0x40050001, "Environment 0x05: wind 1", 6.0, 0.0},
    {0x40060001, "Environment 0x06: moving sand snow", 5.7, 0.0},
    {0x40070001, "Environment 0x07", 5.7, 0.0},
    {0x40090001, "Environment 0x09: water", 5.8, 0.0},
    {0x400A0001, "Environment 0x0A", 5.7, 0.0},
    {0x400B0001, "Environment 0x0B: boat rocking 1", 5.8, 0.0},
    {0x400C0001, "Environment 0x0C: elevator 3", 6.2, 0.0},
    {0x400D0001, "Environment 0x0D: elevator 4", 5.7, 0.0},
    {0x400E0001, "Environment 0x0E: moving sand", 6.6, 0.0},
    {0x400F4001, "Environment 0x0F: merry go round creaking", 5.8, 0.0},
    {0x40108001, "Environment 0x10: wind 2", 6.0, 0.0},
    {0x60050001, "Air 0x05: rough slide", 5.6, 0.0},
    {0x40120001, "Environment 0x12", 5.6, 0.0},
    {0x40130001, "Environment 0x13: sliding", 6.0, 0.0},
    {0x40140011, "Environment 0x14: star", 5.7, 0.0},
    {0x41150001, "Environment 0x15: moving big platform", 6.6, 0.0},
    {0x41160001, "Environment 0x16: water drain", 5.9, 0.0},
    {0x40178001, "Environment 0x17: metal box push", 5.7, 0.0},
    {0x40188001, "Environment 0x18: sink quicksand", 5.7, 0.0},
    {0x600B4001, "Air 0x0B: Peach twinkle", 6.2, 0.0},
    {0x50008081, "Object 0x00: Sushi shark water sound", 1.7, 0.0},
    {0x50010081, "Object 0x01: Mr. I shoot", 1.0, 0.0},
    {0x50020081, "Object 0x02: baby penguin walk", 0.8, 0.0},
    {0x50030081, "Object 0x03: Bowser walk", 1.9, 0.0},
    {0x90040081, "Object 2 0x04: Bowser roar", 2.4, 0.0},
    {0x50050081, "Object 0x05: Bowser tail pickup", 1.7, 0.0},
    {0x50060081, "Object 0x06: Bowser defeated", 3.8, 0.0},
    {0x50070081, "Object 0x07: Bowser spinning", 1.2, 0.0},
    {0x50080081, "Object 0x08: Bowser inhaling", 1.8, 0.0},
    {0x50098081, "Object 0x09: big penguin walk", 0.8, 0.0},
    {0x500A0081, "Object 0x0A: Boo bounce top", 0.9, 0.0},
    {0x500B0081, "Object 0x0B: Boo laugh short", 0.9, 0.0},
    {0x500CA081, "Object 0x0C: Thwomp", 1.8, 0.0},
    {0x500DF081, "Object 0x0D: cannon 1", 2.9, 0.0},
    {0x500EF081, "Object 0x0E: cannon 2", 2.9, 0.0},
    {0x500FF081, "Object 0x0F: cannon 3", 1.1, 0.0},
    {0x90105081, "Object 2 0x10: Piranha plant bite", 0.8, 0.0},
    {0x90116081, "Object 2 0x11: Piranha plant dying", 1.5, 0.0},
    {0x50120001, "Object 0x12: jump walk water", 5.7, 0.0},
    {0x50130081, "Object 0x13", 1.5, 0.0},
    {0x50140081, "Object 0x14: Mr. I death", 0.9, 0.0},
    {0x50155081, "Object 0x15: pounding 1", 1.0, 0.0},
    {0x50166081, "Object 0x16: Whomp", 1.1, 0.0},
    {0x50178081, "Object 0x17: Bully metal", 1.0, 0.0},
    {0x5018A081, "Object 0x18: Bully explode", 1.6, 0.0},
    {0x90192081, "Object 2 0x19: Bowser puzzle piece move", 0.8, 0.0},
    {0x501A5081, "Object 0x1A: pounding cannon", 1.8, 0.0},
    {0x501B3081, "Object 0x1B: Bully walk", 0.8, 0.0},
    {0x901C0081, "Object 2 0x1C: Bully attacked", 1.1, 0.0},
    {0x501D8081, "Object 0x1D", 0.9, 0.0},
    {0x501EA081, "Object 0x1E", 1.0, 0.0},
    {0x501F4081, "Object 0x1F: baby penguin dive", 0.9, 0.0},
    {0x50200081, "Object 0x20: Goomba walk", 0.8, 0.0},
    {0x50210081, "Object 0x21: Ukiki chatter long", 1.2, 0.0},
    {0x50220081, "Object 0x22: Monty Mole attack", 1.6, 0.0},
    {0x50230001, "Object 0x23", 5.4, 0.0},
    {0x50244081, "Object 0x24: dying enemy 1", 1.0, 0.0},
    {0x50254081, "Object 0x25: cannon 4", 1.7, 0.0},
    {0x50260001, "Object 0x26: dying enemy 2", 5.7, 0.0},
    {0x50270081, "Object 0x27: Bob-omb walk", 0.7, 0.0},
    {0x50288081, "Object 0x28: something landing", 1.6, 0.0},
    {0x5029A081, "Object 0x29: diving in water", 1.5, 0.0},
    {0x502A0081, "Object 0x2A: snow sand 1", 1.0, 0.0},
    {0x502B0081, "Object 0x2B: snow sand 2", 1.0, 0.0},
    {0x502C8081, "Object 0x2C: default death", 1.4, 0.0},
    {0x502D0081, "Object 0x2D: big penguin yell", 1.2, 0.0},
    {0x502E8081, "Object 0x2E: water bomb bouncing", 1.8, 0.0},
    {0x502F0081, "Object 0x2F: Goomba alert", 1.6, 0.0},
    {0x50308081, "Object 0x30: stomped", 0.9, 0.0},
    {0x50310081, "Object 0x31", 0.8, 0.0},
    {0x50324081, "Object 0x32: diving into water", 1.5, 0.0},
    {0x50334081, "Object 0x33: Piranha plant shrink", 2.0, 0.0},
    {0x50342081, "Object 0x34: Koopa the quick walk", 0.8, 0.0},
    {0x50350081, "Object 0x35: Koopa walk", 0.8, 0.0},
    {0x50366081, "Object 0x36: Bully walking", 0.8, 0.0},
    {0x50376081, "Object 0x37: Dorrie", 1.6, 0.0},
    {0x50388081, "Object 0x38: Bowser laugh", 4.1, 0.0},
    {0x50390081, "Object 0x39: Ukiki chatter short", 0.8, 0.0},
    {0x503A0081, "Object 0x3A: Ukiki chatter idle", 1.0, 0.0},
    {0x503B0081, "Object 0x3B: Ukiki step default", 0.7, 0.0},
    {0x503C0081, "Object 0x3C: Ukiki step leaves", 0.8, 0.0},
    {0x503DA081, "Object 0x3D: Koopa talk", 1.1, 0.0},
    {0x503EA081, "Object 0x3E: Koopa damage", 0.9, 0.0},
    {0x503F4081, "Object 0x3F: Klepto 1", 0.9, 0.0},
    {0x50406081, "Object 0x40: Klepto 2", 0.8, 0.0},
    {0x50410081, "Object 0x41: King Bob-omb talk", 2.1, 0.0},
    {0x91424081, "Object 2 0x42: King Bob-omb damage", 1.8, 0.0},
    {0x90434081, "Object 2 0x43: Scuttlebug walk", 0.7, 0.0},
    {0x90444081, "Object 2 0x44: Scuttlebug alert", 0.8, 0.0},
    {0x90450081, "Object 2 0x45: baby penguin yell", 1.2, 0.0},
    {0x50468081, "Object 0x46: King Bob-omb jump", 1.0, 0.0},
    {0x5147C081, "Object 0x47: King Whomp death", 2.2, 0.0},
    {0x50480081, "Object 0x48: Boo laugh long", 1.7, 0.0},
    {0x90490081, "Object 2 0x49: Swoop", 0.7, 0.0},
    {0x504A0081, "Object 0x4A: eel", 2.3, 0.0},
    {0x524B0081, "Object 0x4B: Eyerok show eye", 1.4, 0.0},
    {0x504C0081, "Object 0x4C: Mr. Blizzard alert", 1.5, 0.0},
    {0x504D0081, "Object 0x4D: Snufit shoot", 0.8, 0.0},
    {0x504E0081, "Object 0x4E: Skeeter walk", 0.8, 0.0},
    {0x504F0081, "Object 0x4F: walking water", 1.2, 0.0},
    {0x50542081, "Object 0x54: Piranha plant appear", 1.6, 0.0},
    {0x50558081, "Object 0x55: flame blown", 1.1, 0.0},
    {0x52564081, "Object 0x56: mad piano chomping", 1.4, 0.0},
    {0x90570081, "Object 2 0x57: large Bully attacked", 1.3, 0.0},
    {0x50584081, "Object 0x58: Bob-omb buddy talk", 1.5, 0.0},
    {0x50591081, "Object 0x59: Spiny (other)", 0.8, 0.0},
    {0x935A0081, "Object 2 0x5A: Eyerok sound short", 1.0, 0.0},
    {0x925B0081, "Object 2 0x5B: Eyerok sound long", 1.9, 0.0},
    {0x505C4081, "Object 0x5C: Wiggler high pitch", 1.1, 0.0},
    {0x505D4081, "Object 0x5D: Heave-Ho tossed", 1.1, 0.0},
    {0x505E0001, "Object 0x5E: Wiggler death", 6.3, 0.0},
    {0x505F8091, "Object 0x5F: Bowser intro laugh", 5.5, 0.0},
    {0x5060B081, "Object 0x60: enemy death high", 1.6, 0.0},
    {0x5061B081, "Object 0x61: enemy death low", 1.7, 0.0},
    {0x5062B081, "Object 0x62: Swoop death", 1.4, 0.0},
    {0x5063B081, "Object 0x63: Koopa Fly Guy death", 1.5, 0.0},
    {0x5064C081, "Object 0x64: snowman bounce", 1.8, 0.0},
    {0x5065D081, "Object 0x65: snowman explode", 1.7, 0.0},
    {0x90668081, "Object 2 0x66: Bowser teleport", 1.4, 0.0},
    {0x90678081, "Object 2 0x67: Monty Mole appear", 1.6, 0.0},
    {0x50684081, "Object 0x68: pounding loud", 1.8, 0.0},
    {0x90694081, "Object 2 0x69: boss dialog grunt", 1.4, 0.0},
    {0x506A0081, "Object 0x6A: MIPS rabbit", 0.8, 0.0},
    {0x906B0081, "Object 2 0x6B: Mr. I spinning", 1.0, 0.0},
    {0x506C0081, "Object 0x6C: MIPS rabbit water", 1.0, 0.0},
    {0x506D0081, "Object 0x6D: Eyerok explode", 2.9, 0.0},
    {0x516E0081, "Object 0x6E: Chuckya death", 2.2, 0.0},
    {0x506F0081, "Object 0x6F: Wiggler talk", 1.6, 0.0},
    {0x50706081, "Object 0x70: Wiggler attacked", 0.9, 0.0},
    {0x50712081, "Object 0x71: Wiggler low pitch", 1.2, 0.0},
    {0x5072C081, "Object 0x72: Snufit Skeeter death", 1.6, 0.0},
    {0x50734081, "Object 0x73: Bubba chomp", 1.1, 0.0},
    {0x50744081, "Object 0x74: enemy defeat shrink", 1.4, 0.0},
    {0x60000001, "Air 0x00: Bowser spit fire", 5.8, 0.0},
    {0x60010001, "Air 0x01", 5.7, 0.0},
    {0x60028001, "Air 0x02: Lakitu fly", 5.6, 0.0},
    {0x60034001, "Air 0x03: Amp buzz", 5.6, 0.0},
    {0x60048001, "Air 0x04: blow fire", 5.8, 0.0},
    {0x60064001, "Air 0x06: Heave-Ho move", 5.7, 0.0},
    {0x60070001, "Air 0x07", 5.6, 0.0},
    {0x60086001, "Air 0x08: Bob-omb lit fuse", 5.6, 0.0},
    {0x60098001, "Air 0x09: howling wind", 7.3, 0.0},
    {0x600A4001, "Air 0x0A: Chuckya move", 5.6, 0.0},
    {0x7000F881, "Menu 0x00: change select", 0.9, 0.0},
    {0x70010001, "Menu 0x01: reverse pause", 5.7, 0.0},
    {0x7002FF81, "Menu 0x02: pause", 1.3, 0.0},
    {0x70040081, "Menu 0x04: message appear", 1.0, 0.0},
    {0x70050081, "Menu 0x05: message disappear", 1.0, 0.0},
    {0x70060081, "Menu 0x06: camera zoom in", 1.1, 0.0},
    {0x70070081, "Menu 0x07: camera zoom out", 1.1, 0.0},
    {0x70080081, "Menu 0x08: pinch Mario face", 0.8, 0.0},
    {0x70090081, "Menu 0x09: let go Mario face", 1.7, 0.0},
    {0x700A0081, "Menu 0x0A: hand appear", 0.9, 0.0},
    {0x700B0081, "Menu 0x0B: hand disappear", 0.9, 0.0},
    {0x700C0081, "Menu 0x0C", 1.3, 0.0},
    {0x700D0081, "Menu 0x0D: power meter", 1.6, 0.0},
    {0x700E0081, "Menu 0x0E: camera buzz", 0.8, 0.0},
    {0x700F0081, "Menu 0x0F: camera turn", 1.5, 0.0},
    {0x70110081, "Menu 0x11: click file select", 0.8, 0.0},
    {0x70128081, "Menu 0x12", 1.5, 0.0},
    {0x70130081, "Menu 0x13: message next page", 0.7, 0.0},
    {0x70140081, "Menu 0x14: coin it's a me Mario", 3.9, 0.0},
    {0x70150081, "Menu 0x15: Yoshi gain lives", 1.4, 0.0},
    {0x7016A081, "Menu 0x16: enter pipe", 2.1, 0.0},
    {0x7017A081, "Menu 0x17: exit pipe", 2.2, 0.0},
    {0x70188081, "Menu 0x18: Bowser laugh", 3.2, 0.0},
    {0x701A8081, "Menu 0x1A: click change view", 0.8, 0.0},
    {0x701B0001, "Menu 0x1B: camera unused 1", 5.7, 0.0},
    {0x701C0001, "Menu 0x1C: camera unused 2", 5.7, 0.0},
    {0x701DB081, "Menu 0x1D: Mario castle warp", 2.8, 0.0},
    {0x701FFF81, "Menu 0x1F: thank you playing my game", 3.1, 0.0},
    {0x70200001, "Menu 0x20: read a sign", 5.6, 0.0},
    {0x70210001, "Menu 0x21: exit a sign", 5.6, 0.0},
    {0x70222081, "Menu 0x22: Mario castle warp 2", 2.3, 0.0},
};
// clang-format on

} // namespace

GameProfile NusNsmeUsa()
{
    GameProfile profile;
    profile.game = "Super Mario 64";
    profile.serial = "NUS-NSME-USA";
    profile.sha1 = "9bef1128717f958171a4afac3ed78ee2bb4e86ce";
    profile.year = "1996";
    profile.cic = n64::Cic::k6102;
    profile.sound_driver = "Super Mario 64 sound engine";

    // The driver takes over the game thread at the first return from the sound reset. It requests the track's sound
    // once. A sound that plays only while requested is requested again in each of the first 150 frames. Each frame
    // ends with the game-loop tick. A frame is two fields, as in the game.
    profile.snapshot_address = kAfterSoundReset;
    profile.snapshot_hit = 1;
    profile.quiet_tries = 30;
    profile.boot_seconds = 5.0;

    const std::vector<CallArgument> arguments = {{.track = true}, {.value = kGlobalSource}};
    CallDriverOptions driver;
    driver.address = kDriverAddress;
    driver.start_calls = {
        {.function = kPlaySound, .arguments = arguments, .track_mask = kDiscrete, .track_value = kDiscrete}};
    driver.frame_calls = {{.function = kPlaySound,
                           .arguments = arguments,
                           .track_mask = kDiscrete,
                           .track_value = 0,
                           .frames = kHeldFrames},
                          {.function = kGameLoopTick}};
    driver.fields = 2;
    profile.driver = BuildCallDriver(driver);

    for (const Sound& sound : kSounds)
    {
        Track track;
        track.number = sound.code;
        track.name = sound.title;
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
