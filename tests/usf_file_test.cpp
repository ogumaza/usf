// SPDX-License-Identifier: MIT

// USF files: the writer's layout of the header and the reserved area, a round trip of a set through the writer and the
// loader, the order in which the loader applies a chain, and the files that it rejects.

#include "usf/usf_file.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "n64/system.h"
#include "usf/track.h"

namespace
{

using usf::Chunk;
using usf::LoadedUsf;
using usf::Sections;
using xsflib::psf::Tags;

// Files in memory, by path.
struct Files
{
    std::map<std::string, std::vector<uint8_t>> files;

    xsflib::psf::FileReader Reader() const
    {
        return [this](const std::string& path, std::vector<uint8_t>& data)
        {
            const auto it = files.find(path);
            if (it == files.end())
            {
                return false;
            }

            data = it->second;

            return true;
        };
    }
};

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
    std::vector<uint8_t> state(usf::kStateOverhead + 0x400000, 0);
    xsflib::StoreLe32(state.data(), 0x23D8A6C8);
    xsflib::StoreLe32(state.data() + 4, 0x400000);
    state[0x75C + 0x100] = 0x42;

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

    const std::vector<uint8_t> file = usf::WriteUsf(sections, tags);

    // The header: the signature, version 0x21, the reserved area's size, no program and a CRC of 0.
    const std::vector<uint8_t> reserved =
        Bytes({'S', 'R', '6', '4', 4,   0, 0, 0, 0x10, 0, 0, 0, 1, 2,    3, 4, 0, 0, 0,
               0,   'S', 'R', '6', '4', 1, 0, 0, 0,    4, 0, 0, 0, 0xAA, 0, 0, 0, 0});
    std::vector<uint8_t> expected =
        Bytes({'P', 'S', 'F', 0x21, static_cast<int>(reserved.size()), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    expected.insert(expected.end(), reserved.begin(), reserved.end());
    const std::string tag_text = "[TAG]_lib=set.usflib\ntitle=A\n";
    expected.insert(expected.end(), tag_text.begin(), tag_text.end());
    XSFLIB_CHECK(file == expected);

    // An absent section is 4 zero bytes. A file with only a state starts with them and then SR64.
    Sections state_only;
    state_only.state = std::vector<Chunk>{};
    XSFLIB_CHECK(usf::WriteReservedArea(state_only) == Bytes({0, 0, 0, 0, 'S', 'R', '6', '4', 0, 0, 0, 0}));
}

void ParseReserved()
{
    Sections sections;
    sections.rom = std::vector<Chunk>{{0x0, Bytes({9})}, {0x100, Bytes({7, 8})}};

    const std::vector<uint8_t> reserved = usf::WriteReservedArea(sections);
    Sections parsed;
    const auto error = usf::ParseReservedArea(reserved, parsed);

    XSFLIB_CHECK(!error);
    XSFLIB_CHECK(parsed.rom && parsed.rom->size() == 2 && (*parsed.rom)[1].offset == 0x100);
    XSFLIB_CHECK(parsed.rom && (*parsed.rom)[1].data == Bytes({7, 8}));
    XSFLIB_CHECK(!parsed.state);
}

// A set written from a ROM and a state loads back as the same images.
void RoundTrip()
{
    usf::n64::System system;
    std::vector<uint8_t> rom(0x1000);
    for (size_t i = 0; i < rom.size(); i++)
    {
        rom[i] = static_cast<uint8_t>(i * 7 + 3);
    }
    xsflib::StoreBe32(rom.data(), 0x80371240);

    system.LoadRom(rom);
    system.GetCpu().SetGpr(4, 0x1234);
    system.Rdram()[0x2000] = 0x99;
    const std::vector<uint8_t> state = system.SaveState();

    // The library holds the ROM in LE word order and the whole state. The mini changes a0 and one ROM byte.
    Sections library;
    std::vector<uint8_t> le(rom.size());
    for (size_t i = 0; i < rom.size(); i++)
    {
        le[i] = rom[i ^ 3];
    }
    library.rom = std::vector<Chunk>{{0, le}};
    library.state = std::vector<Chunk>{{0, state}};
    Sections mini;
    mini.rom = std::vector<Chunk>{{0x800 ^ 3, Bytes({0x5A})}};
    std::vector<uint8_t> a0(8, 0);
    xsflib::StoreLe64(a0.data(), 7);
    mini.state = std::vector<Chunk>{{0x070, a0}};

    Files files;
    files.files["dir/set.usflib"] = usf::WriteUsf(library, {{"_enablecompare", "1"}});
    files.files["dir/a.miniusf"] =
        usf::WriteUsf(mini, {{"_lib", "set.usflib"}, {"_enableFIFOfull", "1"}, {"title", "Track A"}});

    LoadedUsf loaded;
    const auto error = usf::LoadUsf("dir/a.miniusf", files.Reader(), loaded);

    XSFLIB_CHECK(!error);
    std::vector<uint8_t> expected_rom = rom;
    expected_rom[0x800] = 0x5A;
    XSFLIB_CHECK(loaded.rom == expected_rom);
    std::vector<uint8_t> expected_state = state;
    xsflib::StoreLe64(expected_state.data() + 0x070, 7);
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
    mine.state = std::vector<Chunk>{{0x75C + 0x200, Bytes({1})}, {0x75C + 0x204, Bytes({1})}};
    Sections second;
    second.state = std::vector<Chunk>{{0x75C + 0x204, Bytes({2})}};

    Files files;
    files.files["base.usflib"] = usf::WriteUsf(base, {});
    files.files["second.usflib"] = usf::WriteUsf(second, {});
    files.files["track.miniusf"] =
        usf::WriteUsf(mine, {{"_lib", "base.usflib"}, {"_lib2", "second.usflib"}, {"_enablecompare", ""}});

    LoadedUsf loaded;
    const auto error = usf::LoadUsf("track.miniusf", files.Reader(), loaded);

    XSFLIB_CHECK(!error);
    XSFLIB_CHECK(loaded.state[0x75C + 0x200] == 1);
    XSFLIB_CHECK(loaded.state[0x75C + 0x204] == 2);
    XSFLIB_CHECK(loaded.state[0x75C + 0x100] == 0x42);
    XSFLIB_CHECK(!loaded.enable_compare);
    XSFLIB_CHECK(loaded.state.size() == usf::kStateOverhead + 0x400000);
}

// Loads `file` alone and returns whether the loader rejects it.
bool Rejects(const std::vector<uint8_t>& file)
{
    Files files;
    files.files["x.usf"] = file;
    LoadedUsf loaded;

    return usf::LoadUsf("x.usf", files.Reader(), loaded).has_value();
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
    constexpr uint32_t kDmem4 = 0x75C + 0x400000;
    constexpr uint32_t kDmem8 = 0x75C + 0x800000;
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
        files.files["set.usflib"] = usf::WriteUsf(library, {});
        files.files["x.usf"] = usf::WriteUsf(track, {{"_lib", "set.usflib"}});
        LoadedUsf loaded;
        XSFLIB_CHECK(!usf::LoadUsf("x.usf", files.Reader(), loaded).has_value());

        return loaded;
    };

    const LoadedUsf smaller = load(0x800000, 0x400000);
    XSFLIB_CHECK(smaller.state.size() == usf::kStateOverhead + 0x400000);
    XSFLIB_CHECK(smaller.state[kDmem4] == 0x44);

    const LoadedUsf larger = load(0x400000, 0x800000);
    XSFLIB_CHECK(larger.state.size() == usf::kStateOverhead + 0x800000);
    XSFLIB_CHECK(larger.state[kDmem4] == 0x44);
    XSFLIB_CHECK(larger.state[kDmem8] == 0x88);
}

void Rejections()
{
    // A good file passes, so that each rejection below comes from its one fault.
    XSFLIB_CHECK(!Rejects(StateFile({})));

    // Not USF: another signature, another version, a program.
    std::vector<uint8_t> not_psf = StateFile({});
    not_psf[0] = 'X';
    XSFLIB_CHECK(Rejects(not_psf));
    const std::vector<uint8_t> state_area =
        usf::WriteReservedArea({std::nullopt, std::vector<Chunk>{{0, MinimalState()}}});
    XSFLIB_CHECK(Rejects(RawFile(state_area, 0x22)));
    XSFLIB_CHECK(Rejects(RawFile(state_area, 0x21, 4)));

    // A reserved area that ends inside a marker, a length or an offset, or before a chunk's data ends.
    std::vector<uint8_t> area = state_area;
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

    // A ROM chunk past cartridge space, and a state chunk past the largest state image. A chunk past the image that the
    // size word gives is fine. A later file may rewrite the word.
    Sections past_rom;
    past_rom.rom = std::vector<Chunk>{{usf::kMaxRomOffset - 2, Bytes({1, 2, 3, 4})}};
    past_rom.state = std::vector<Chunk>{{0, MinimalState()}};
    XSFLIB_CHECK(Rejects(usf::WriteUsf(past_rom, {})));
    XSFLIB_CHECK(Rejects(StateFile({{usf::kStateOverhead + 0x800000 - 1, Bytes({1, 2})}})));
    XSFLIB_CHECK(!Rejects(StateFile({{usf::kStateOverhead + 0x800000 - 1, Bytes({1})}})));
    XSFLIB_CHECK(!Rejects(StateFile({{usf::kStateOverhead + 0x400000, Bytes({1})}})));

    // A set without a state, or with a state whose identifier or RDRAM size is wrong.
    Sections rom_only;
    rom_only.rom = std::vector<Chunk>{{0, Bytes({1, 2, 3, 4})}};
    XSFLIB_CHECK(Rejects(usf::WriteUsf(rom_only, {})));
    XSFLIB_CHECK(Rejects(StateFile({{0, Bytes({0, 0, 0, 0})}})));
    XSFLIB_CHECK(Rejects(StateFile({{4, Bytes({0, 0, 0x20, 0})}})));

    // A library that's missing, and one that names itself. Each file holds a state, so that the library alone is at
    // fault.
    Sections with_state;
    with_state.state = std::vector<Chunk>{{0, MinimalState()}};
    Files files;
    files.files["missing.usf"] = usf::WriteUsf(with_state, {{"_lib", "missing.usflib"}});
    files.files["self.usf"] = usf::WriteUsf(with_state, {{"_lib", "self.usf"}});
    LoadedUsf loaded;
    const std::optional<std::string> missing = usf::LoadUsf("missing.usf", files.Reader(), loaded);
    XSFLIB_CHECK(missing && missing->find("can't read missing.usflib") != std::string::npos);
    const std::optional<std::string> self = usf::LoadUsf("self.usf", files.Reader(), loaded);
    XSFLIB_CHECK(self && self->find("nest deeper") != std::string::npos);
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("usf_file",
                                    []
                                    {
                                        Layout();
                                        ParseReserved();
                                        RoundTrip();
                                        ChainOrder();
                                        TrackRomKeepsItsOrder();
                                        FinalSizeWord();
                                        Rejections();
                                    });
}
