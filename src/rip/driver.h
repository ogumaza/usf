// SPDX-License-Identifier: MIT

// The generic driver: a small program in RDRAM that calls game functions with a track's number. A profile uses it where
// the game has no song-select call to stop at, such as a set of sound effects. BuildCallDriver writes the program's
// instruction words with a small MIPS encoder (mips.h).
//
// The driver runs in the thread that reached the snapshot point (profile.h). It reads the number from its track word
// once: all 32 bits of it. It makes the start calls in order, once. Then, for ever, it makes the frame calls in order
// and waits for the next frame. Without frame calls or a wait, it ends in an idle loop. Each call may be limited to
// some tracks, and a frame call to a range of frames.
//
// Calls follow the O32 convention of libultra's games: up to four arguments in a0 to a3, the return address in ra, the
// function's address in t9, and 16 bytes of argument space at the bottom of the caller's stack frame. The driver keeps
// the number in s0 and counts the frames in s1. The callee preserves both.

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "rip/profile.h"

namespace usf::rip
{

// One argument of a call: the constant `value`, or with `track` set, the track's number shifted left by `shift` and
// ORed with `value`. A track's number may hold two fields: `right_shift` and `mask` pick one out before the left shift.
// With `result` set, the argument is the value that the previous call returned.
struct CallArgument
{
    uint32_t value = 0;
    bool track = false;
    int shift = 0;
    int right_shift = 0;
    uint16_t mask = 0;
    bool result = false;
};

// A call of the function at `function`, with at most four arguments.
struct GameCall
{
    uint32_t function = 0;
    std::vector<CallArgument> arguments;

    // The driver makes the call only for a track whose number gives (number & track_mask) == track_value. The defaults
    // make it for every track.
    uint32_t track_mask = 0;
    uint32_t track_value = 0;

    // A frame call is made only from frame `first_frame` on, and only in the first `frames` frames. Frame 0 is the
    // first after the start calls. `frames` 0 makes no end.
    uint32_t first_frame = 0;
    uint32_t frames = 0;
};

struct CallDriverOptions
{
    // Where the driver goes, in KSEG0. It takes about 40 bytes for each call, and 4 bytes at its end for its track
    // word. Choose RDRAM that the game never uses.
    uint32_t address = 0;

    // The first address past the room for the driver, such as the address of data that the profile puts after it. The
    // code and its track word must end at or before it. 0 sets no limit.
    uint32_t end = 0;

    std::vector<GameCall> start_calls;
    std::vector<GameCall> frame_calls;

    // How the driver waits for the next frame. With `wait_call`, it calls a function of the game that waits, such as
    // osRecvMesg on the queue that the game's scheduler sends each retrace to. It makes that call `fields` times. A
    // frame then lasts `fields` messages, however many video fields come between them. Without a wait call, the driver
    // polls VI_CURRENT until a new field starts, and a frame is `fields` video fields. A poll keeps the thread busy. A
    // legacy player runs every instruction of it, while a thread that waits lets the idle thread run.
    std::optional<GameCall> wait_call;
    int fields = 1;
};

// A wait call: the game's osRecvMesg at `recv_mesg`, called with the queue at `queue`, no message pointer and
// OS_MESG_BLOCK. It returns once the queue holds a message, and drops the message.
GameCall MessageWait(uint32_t recv_mesg, uint32_t queue);

// A start call that drops a message that waits in the queue at `queue`, through the game's osRecvMesg at `recv_mesg`
// with OS_MESG_NOBLOCK. It returns at once, with or without one. The last start call can drop a message that came
// before the first frame, so that the first wait doesn't return at once.
GameCall MessageDrop(uint32_t recv_mesg, uint32_t queue);

// A message queue that the driver adds to the clients of a game's retrace manager, for a game whose retrace queues
// all have threads of their own that wait on them. The manager sends a message to each client at every retrace. The
// queue holds one message. It takes 24 bytes at `address`, its message buffer the 4 bytes after it, and the client's
// record of two words the 8 bytes after that: 36 bytes in all, in RDRAM that the game never uses.
struct RetraceClient
{
    uint32_t address = 0;

    // The game's osCreateMesgQueue, and its function that adds a client to the manager at `manager`. That function
    // takes the manager in a0, the client's record in a1 and the queue in a2. It writes the queue and the next client
    // into the record, and puts the record at the head of the manager's list.
    uint32_t create_queue = 0;
    uint32_t add_client = 0;
    uint32_t manager = 0;
};

// The start calls that make the client's queue and add the client. Throws std::invalid_argument for an address outside
// KSEG0 or one that isn't word-aligned.
std::vector<GameCall> AddRetraceClient(const RetraceClient& client);

// The driver for `options`. Throws std::invalid_argument for a call with more than four arguments, a shift or a right
// shift outside 0 to 31, a frame of fewer than one field, a driver's address outside KSEG0, an address that isn't
// word-aligned, or a driver that runs past its end.
Driver BuildCallDriver(const CallDriverOptions& options);

// The patch that replaces the one word of the driver's code that holds `word`. Throws std::logic_error if the code
// doesn't contain `word` exactly once.
RdramPatch DriverWordPatch(const Driver& driver, uint32_t word, uint32_t replacement);

} // namespace usf::rip
