// SPDX-License-Identifier: MIT

// USF files: the writer's layout of the header and the reserved area, a round trip of a set through the writer and the
// loader, the order in which the loader applies a chain, and the files that it rejects.

#include "usf/usf_file.h"

#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"
#include "xsflib/testing/sets.h"

#include "n64/save_state.h"
#include "n64/system.h"
#include "test_programs.h"
#include "usf/track.h"

namespace
{

using usf::Chunk;
using usf::LoadedUsf;
using usf::Sections;
using usf::n64::kRdramSize4;
using usf::n64::kRdramSize8;
using usf::n64::kStateRdramOffset;
using usf::testing::kStateA0;
using xsflib::psf::Tags;
using xsflib::testing::ExactReader;

// Files in memory, by path.
using Files = std::map<std::string, std::vector<uint8_t>>;

std::vector<uint8_t> Bytes(std::initializer_list<int> values)
{
    std::vector<uint8_t> bytes;
    for (int value : values)
    {
        bytes.push_back(static_cast<uint8_t>(value));
    }

    return bytes;
}

// A state image of 4 MiB with the identifier and the RDRAM size, and a recognisable byte.
std::vector<uint8_t> MinimalState()
{
    std::vector<uint8_t> state(usf::kStateOverhead + kRdramSize4, 0);
    xsflib::StoreLe32(state.data(), usf::n64::kStateId);
    xsflib::StoreLe32(state.data() + 4, kRdramSize4);
    state[kStateRdramOffset + 0x100] = 0x42;

    return state;
}

// A file whose reserved area is `reserved` as it stands, with `program_size` bytes of program.
std::vector<uint8_t> RawFile(const std::vector<uint8_t>& reserved, uint8_t version = 0x21, uint32_t program_size = 0)
{
    std::vector<uint8_t> file = {'P', 'S', 'F', version, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    xsflib::StoreLe32(file.data() + 4, static_cast<uint32_t>(reserved.size()));
    xsflib::StoreLe32(file.data() + 8, program_size);
    file.insert(file.end(), reserved.begin(), reserved.end());
    file.resize(file.size() + program_size, 0);

    return file;
}

void Layout()
{
    Sections sections;
    sections.rom = std::vector<Chunk>{{0x10, Bytes({1, 2, 3, 4})}};
    sections.state = std::vector<Chunk>{{0x4, Bytes({0xAA})}};
    const Tags tags = {{"_lib", "set.usflib"}, {"title", "A"}};

    // The header: the signature, version 0x21, the reserved area's size, no program and a CRC of 0.
    const std::vector<uint8_t> reserved =
        Bytes({'S', 'R', '6', '4', 4,   0, 0, 0, 0x10, 0, 0, 0, 1, 2,    3, 4, 0, 0, 0,
               0,   'S', 'R', '6', '4', 1, 0, 0, 0,    4, 0, 0, 0, 0xAA, 0, 0, 0, 0});
    std::vector<uint8_t> expected =
        Bytes({'P', 'S', 'F', 0x21, static_cast<int>(reserved.size()), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    expected.insert(expected.end(), reserved.begin(), reserved.end());
    const std::string tag_text = "[TAG]_lib=set.usflib\ntitle=A\n";
    expected.insert(expected.end(), tag_text.begin(), tag_text.end());

    const std::vector<uint8_t> file = usf::WriteUsf(sections, tags);

    XSFLIB_CHECK(file == expected);
}

// An absent section is 4 zero bytes. A file with only a state starts with them and then SR64.
void AbsentSection()
{
    Sections state_only;
    state_only.state = std::vector<Chunk>{};

    const std::vector<uint8_t> reserved = usf::WriteReservedArea(state_only);

    XSFLIB_CHECK(reserved == Bytes({0, 0, 0, 0, 'S', 'R', '6', '4', 0, 0, 0, 0}));
}

void ParseReserved()
{
    Sections sections;
    sections.rom = std::vector<Chunk>{{0x0, Bytes({9})}, {0x100, Bytes({7, 8})}};

    const std::vector<uint8_t> reserved = usf::WriteReservedArea(sections);
    Sections parsed;
    const auto error = usf::ParseReservedArea(reserved, parsed);

    XSFLIB_REQUIRE(!error && parsed.rom && parsed.rom->size() == 2);
    XSFLIB_CHECK((*parsed.rom)[1].offset == 0x100);
    XSFLIB_CHECK((*parsed.rom)[1].data == Bytes({7, 8}));
    XSFLIB_CHECK(!parsed.state);
}

// A set written from a ROM and a state loads back as the same images.
void RoundTrip()
{
    usf::n64::System system;
    std::vector<uint8_t> rom = usf::testing::TestRom(0x1000);
    for (size_t i = 4; i < rom.size(); i++)
    {
        rom[i] = static_cast<uint8_t>(i * 7 + 3);
    }

    system.LoadRom(rom);
    system.GetCpu().SetGpr(4, 0x1234);
    system.Rdram()[0x2000] = 0x99;
    const std::vector<uint8_t> state = system.SaveState();

    // The library holds the ROM in LE word order and the whole state. The mini changes a0 and one ROM byte.
    Sections library;
    std::vector<uint8_t> le(rom.size());
    usf::n64::CopyToLeWords(rom.data(), le.data(), rom.size());
    library.rom = std::vector<Chunk>{{0, le}};
    library.state = std::vector<Chunk>{{0, state}};
    Sections mini;
    mini.rom = std::vector<Chunk>{{0x800 ^ 3, Bytes({0x5A})}};
    std::vector<uint8_t> a0(8, 0);
    xsflib::StoreLe64(a0.data(), 7);
    mini.state = std::vector<Chunk>{{kStateA0, a0}};

    Files files;
    files["dir/set.usflib"] = usf::WriteUsf(library, {{"_enablecompare", "1"}});
    files["dir/a.miniusf"] =
        usf::WriteUsf(mini, {{"_lib", "set.usflib"}, {"_enableFIFOfull", "1"}, {"title", "Track A"}});
    std::vector<uint8_t> expected_rom = rom;
    expected_rom[0x800] = 0x5A;
    std::vector<uint8_t> expected_state = state;
    xsflib::StoreLe64(expected_state.data() + kStateA0, 7);

    LoadedUsf loaded;
    const auto error = usf::LoadUsf("dir/a.miniusf", ExactReader(files), loaded);

    XSFLIB_CHECK(!error);
    XSFLIB_CHECK(loaded.rom == expected_rom);
    XSFLIB_CHECK(loaded.state == expected_state);
    XSFLIB_CHECK(loaded.enable_compare && loaded.enable_fifo_full);
    XSFLIB_CHECK(loaded.tags.count("title") == 1 && loaded.tags.count("_enablecompare") == 0);
    XSFLIB_CHECK(loaded.sources == (std::vector<std::string>{"dir/set.usflib", "dir/a.miniusf"}));
}

// _lib loads before the file and _lib2 after it. A flag with an empty value stays off.
void ChainOrder()
{
    const std::vector<uint8_t> state = MinimalState();
    Sections base;
    base.state = std::vector<Chunk>{{0, state}};
    Sections mine;
    mine.state = std::vector<Chunk>{{kStateRdramOffset + 0x200, Bytes({1})}, {kStateRdramOffset + 0x204, Bytes({1})}};
    Sections second;
    second.state = std::vector<Chunk>{{kStateRdramOffset + 0x204, Bytes({2})}};

    Files files;
    files["base.usflib"] = usf::WriteUsf(base, {});
    files["second.usflib"] = usf::WriteUsf(second, {});
    files["track.miniusf"] =
        usf::WriteUsf(mine, {{"_lib", "base.usflib"}, {"_lib2", "second.usflib"}, {"_enablecompare", ""}});

    LoadedUsf loaded;
    const auto error = usf::LoadUsf("track.miniusf", ExactReader(files), loaded);

    XSFLIB_REQUIRE(!error && loaded.state.size() == usf::kStateOverhead + kRdramSize4);
    XSFLIB_CHECK(loaded.state[kStateRdramOffset + 0x200] == 1);
    XSFLIB_CHECK(loaded.state[kStateRdramOffset + 0x204] == 2);
    XSFLIB_CHECK(loaded.state[kStateRdramOffset + 0x100] == 0x42);
    XSFLIB_CHECK(!loaded.enable_compare);
}

// An empty _lib2 names no library. Neither does an empty _lib. The loader then reads _lib3.
void EmptyLibTags()
{
    Sections base;
    base.state = std::vector<Chunk>{{0, MinimalState()}};
    Sections mine;
    mine.state = std::vector<Chunk>{{kStateRdramOffset + 0x200, Bytes({1})}};
    Sections third;
    third.state = std::vector<Chunk>{{kStateRdramOffset + 0x204, Bytes({3})}};

    Files files;
    files["base.usflib"] = usf::WriteUsf(base, {{"_lib", ""}});
    files["third.usflib"] = usf::WriteUsf(third, {});
    files["track.miniusf"] = usf::WriteUsf(mine, {{"_lib", "base.usflib"}, {"_lib2", ""}, {"_lib3", "third.usflib"}});

    LoadedUsf loaded;
    const auto error = usf::LoadUsf("track.miniusf", ExactReader(files), loaded);

    XSFLIB_REQUIRE(!error && loaded.state.size() == usf::kStateOverhead + kRdramSize4);
    XSFLIB_CHECK(loaded.state[kStateRdramOffset + 0x200] == 1);
    XSFLIB_CHECK(loaded.state[kStateRdramOffset + 0x204] == 3);
    XSFLIB_CHECK(loaded.sources == (std::vector<std::string>{"base.usflib", "track.miniusf", "third.usflib"}));
}

// Loads `file` alone and returns whether the loader rejects it.
bool Rejects(const std::vector<uint8_t>& file)
{
    Files files;
    files["x.usf"] = file;
    LoadedUsf loaded;

    return usf::LoadUsf("x.usf", ExactReader(files), loaded).has_value();
}

std::vector<uint8_t> StateFile(const std::vector<Chunk>& extra)
{
    Sections sections;
    sections.state = std::vector<Chunk>{{0, MinimalState()}};
    sections.state->insert(sections.state->end(), extra.begin(), extra.end());

    return usf::WriteUsf(sections, {});
}

// A track's ROM is in console byte order whatever its first bytes are. A trimmed ROM can start with bytes that look
// like another byte order.
void TrackRomKeepsItsOrder()
{
    usf::n64::System system;
    const std::vector<uint8_t> state = MinimalState();

    for (const std::vector<uint8_t>& rom : {Bytes({0x37, 0x80, 0x40, 0x12}), Bytes({0x40, 0x12, 0x37, 0x80})})
    {
        usf::StartTrack(system, rom, state);
        XSFLIB_CHECK(system.Rom() == rom);
    }
}

// The final RDRAM size word counts. A library says 8 MiB with chunks up to the end of that state, and a later file
// rewrites the word to 4 MiB: the state ends at 0x40275C, and the chunks past that have no effect. The opposite way,
// the state grows. DMEM follows the final value.
void FinalSizeWord()
{
    constexpr uint32_t kDmem4 = kStateRdramOffset + kRdramSize4;
    constexpr uint32_t kDmem8 = kStateRdramOffset + kRdramSize8;
    const auto load = [](uint32_t library_size, uint32_t track_size)
    {
        std::vector<uint8_t> size_word(4);
        xsflib::StoreLe32(size_word.data(), library_size);
        Sections library;
        library.state = std::vector<Chunk>{
            {0, Bytes({0xC8, 0xA6, 0xD8, 0x23})}, {4, size_word}, {kDmem4, Bytes({0x44})}, {kDmem8, Bytes({0x88})}};
        xsflib::StoreLe32(size_word.data(), track_size);
        Sections track;
        track.state = std::vector<Chunk>{{4, size_word}};

        Files files;
        files["set.usflib"] = usf::WriteUsf(library, {});
        files["x.usf"] = usf::WriteUsf(track, {{"_lib", "set.usflib"}});
        LoadedUsf loaded;
        XSFLIB_CHECK(!usf::LoadUsf("x.usf", ExactReader(files), loaded).has_value());

        return loaded;
    };

    const LoadedUsf smaller = load(kRdramSize8, kRdramSize4);
    const LoadedUsf larger = load(kRdramSize4, kRdramSize8);

    XSFLIB_REQUIRE(smaller.state.size() == usf::kStateOverhead + kRdramSize4);
    XSFLIB_REQUIRE(larger.state.size() == usf::kStateOverhead + kRdramSize8);
    XSFLIB_CHECK(smaller.state[kDmem4] == 0x44);
    XSFLIB_CHECK(larger.state[kDmem4] == 0x44);
    XSFLIB_CHECK(larger.state[kDmem8] == 0x88);
}

// A reserved area with a state section that holds MinimalState(), and no ROM section.
std::vector<uint8_t> StateArea()
{
    return usf::WriteReservedArea({std::nullopt, std::vector<Chunk>{{0, MinimalState()}}});
}

// A good file passes, so that each rejection below comes from its one fault.
void GoodFilePasses()
{
    XSFLIB_CHECK(!Rejects(StateFile({})));
}

// Not USF: another signature, another version, a program.
void RejectsOtherFormats()
{
    std::vector<uint8_t> not_psf = StateFile({});
    not_psf[0] = 'X';
    XSFLIB_CHECK(Rejects(not_psf));
    const std::vector<uint8_t> state_area = StateArea();
    XSFLIB_CHECK(Rejects(RawFile(state_area, 0x22)));
    XSFLIB_CHECK(Rejects(RawFile(state_area, 0x21, 4)));
}

// A reserved area that ends inside a marker, a length or an offset, or before a chunk's data ends.
void RejectsShortAreas()
{
    std::vector<uint8_t> area = StateArea();
    area.resize(area.size() - 4); // Drops the state section's terminator: the area ends after a chunk, which is fine.
    XSFLIB_CHECK(!Rejects(RawFile(area)));
    XSFLIB_CHECK(Rejects(RawFile(Bytes({0, 0}))));
    std::vector<uint8_t> inside_length = area;
    inside_length.insert(inside_length.end(), {1, 0});
    XSFLIB_CHECK(Rejects(RawFile(inside_length)));
    std::vector<uint8_t> inside_offset = area;
    inside_offset.insert(inside_offset.end(), {1, 0, 0, 0, 0, 0});
    XSFLIB_CHECK(Rejects(RawFile(inside_offset)));
    std::vector<uint8_t> short_data = area;
    short_data.insert(short_data.end(), {8, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3});
    XSFLIB_CHECK(Rejects(RawFile(short_data)));
}

// A ROM chunk past cartridge space, and a state chunk past the largest state image. A chunk past the image that the
// size word gives is fine. A later file may rewrite the word.
void RejectsChunksOutside()
{
    Sections past_rom;
    past_rom.rom = std::vector<Chunk>{{usf::kMaxRomOffset - 2, Bytes({1, 2, 3, 4})}};
    past_rom.state = std::vector<Chunk>{{0, MinimalState()}};
    XSFLIB_CHECK(Rejects(usf::WriteUsf(past_rom, {})));
    XSFLIB_CHECK(Rejects(StateFile({{usf::kStateOverhead + kRdramSize8 - 1, Bytes({1, 2})}})));
    XSFLIB_CHECK(!Rejects(StateFile({{usf::kStateOverhead + kRdramSize8 - 1, Bytes({1})}})));
    XSFLIB_CHECK(!Rejects(StateFile({{usf::kStateOverhead + kRdramSize4, Bytes({1})}})));
}

// A set without a state, or with a state whose identifier or RDRAM size is wrong.
void RejectsBadStates()
{
    Sections rom_only;
    rom_only.rom = std::vector<Chunk>{{0, Bytes({1, 2, 3, 4})}};
    XSFLIB_CHECK(Rejects(usf::WriteUsf(rom_only, {})));
    XSFLIB_CHECK(Rejects(StateFile({{0, Bytes({0, 0, 0, 0})}})));
    XSFLIB_CHECK(Rejects(StateFile({{4, Bytes({0, 0, 0x20, 0})}})));
}

// A library that's missing, and one that names itself. Each file holds a state. The library alone is then at fault.
void RejectsBadLibraries()
{
    Sections with_state;
    with_state.state = std::vector<Chunk>{{0, MinimalState()}};
    Files files;
    files["missing.usf"] = usf::WriteUsf(with_state, {{"_lib", "missing.usflib"}});
    files["self.usf"] = usf::WriteUsf(with_state, {{"_lib", "self.usf"}});
    LoadedUsf loaded;
    const std::optional<std::string> missing = usf::LoadUsf("missing.usf", ExactReader(files), loaded);
    XSFLIB_CHECK(missing && missing->find("can't read missing.usflib") != std::string::npos);
    const std::optional<std::string> self = usf::LoadUsf("self.usf", ExactReader(files), loaded);
    XSFLIB_CHECK(self && self->find("nest deeper") != std::string::npos);
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("usf_file",
                                    []
                                    {
                                        Layout();
                                        AbsentSection();
                                        ParseReserved();
                                        RoundTrip();
                                        ChainOrder();
                                        EmptyLibTags();
                                        TrackRomKeepsItsOrder();
                                        FinalSizeWord();
                                        GoodFilePasses();
                                        RejectsOtherFormats();
                                        RejectsShortAreas();
                                        RejectsChunksOutside();
                                        RejectsBadStates();
                                        RejectsBadLibraries();
                                    });
}
