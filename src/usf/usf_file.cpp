// SPDX-License-Identifier: MIT

#include "usf/usf_file.h"

#include <algorithm>
#include <cstring>

#include "xsflib/bytes.h"

namespace usf
{

namespace
{

constexpr uint8_t kMarker[4] = {'S', 'R', '6', '4'};
constexpr uint32_t kStateId = 0x23D8A6C8;
constexpr uint32_t kRdramSize4 = 0x400000;
constexpr uint32_t kRdramSize8 = 0x800000;
constexpr size_t kMaxStateSize = kStateOverhead + kRdramSize8;

using xsflib::psf::Put32;

void WriteSection(std::vector<uint8_t>& out, const std::optional<std::vector<Chunk>>& section)
{
    if (!section)
    {
        Put32(out, 0);
        return;
    }

    out.insert(out.end(), kMarker, kMarker + 4);
    for (const Chunk& chunk : *section)
    {
        if (chunk.data.empty())
        {
            continue;
        }

        Put32(out, static_cast<uint32_t>(chunk.data.size()));
        Put32(out, chunk.offset);
        out.insert(out.end(), chunk.data.begin(), chunk.data.end());
    }

    Put32(out, 0);
}

// Loads a set's files into the images: a file's `_lib` first, then the file, then its `_lib2`, `_lib3` and on.
class Loader
{
public:
    Loader(const xsflib::psf::FileReader& reader, LoadedUsf& out, std::string_view separators)
        : reader_(reader), out_(out), separators_(separators)
    {
    }

    std::optional<std::string> Load(const std::string& path, int depth, bool top)
    {
        if (depth > xsflib::psf::kMaxLibDepth)
        {
            return "the libraries nest deeper than " + std::to_string(xsflib::psf::kMaxLibDepth) + " levels";
        }
        if (++files_ > xsflib::psf::kMaxSetFiles)
        {
            return "the set has more than " + std::to_string(xsflib::psf::kMaxSetFiles) + " files";
        }

        std::vector<uint8_t> data;
        if (!reader_(path, data))
        {
            return "can't read " + path;
        }

        xsflib::psf::PsfFile file;
        xsflib::psf::ParseOptions options;
        options.with_program = false;
        options.version = kUsfVersion;
        options.wrong_version = "not a USF file";
        if (auto error = xsflib::psf::ParsePsf(data, file, options))
        {
            return path + ": " + *error;
        }
        if (file.compressed_size != 0)
        {
            return path + ": a USF file holds no program";
        }

        Sections sections;
        if (auto error = ParseReservedArea(file.reserved, sections))
        {
            return path + ": " + *error;
        }

        if (top)
        {
            out_.tags = file.tags;
        }

        out_.enable_compare = out_.enable_compare || Flag(file.tags, "_enablecompare");
        out_.enable_fifo_full = out_.enable_fifo_full || Flag(file.tags, "_enablefifofull");

        // The library first, then this file, then the libraries that follow it.
        const std::string dir = xsflib::psf::DirectoryOf(path, separators_);
        if (auto lib = file.tags.find("_lib"); lib != file.tags.end() && !lib->second.empty())
        {
            if (auto error = Load(dir + lib->second, depth + 1, false))
            {
                return error;
            }
        }

        if (auto error = Apply(sections))
        {
            return path + ": " + *error;
        }

        out_.sources.push_back(path);

        for (int n = 2;; n++)
        {
            const auto lib = file.tags.find("_lib" + std::to_string(n));
            if (lib == file.tags.end())
            {
                break;
            }

            if (auto error = Load(dir + lib->second, depth + 1, false))
            {
                return error;
            }
        }

        return std::nullopt;
    }

    std::optional<std::string> Finish()
    {
        if (!has_state_)
        {
            return "the set holds no save state";
        }
        if (xsflib::LoadLe32(state_.data()) != kStateId)
        {
            return "the save state's identifier is wrong";
        }

        const uint32_t rdram_size = xsflib::LoadLe32(state_.data() + 4);
        if (rdram_size != kRdramSize4 && rdram_size != kRdramSize8)
        {
            return "the save state's RDRAM size is neither 4 nor 8 MiB";
        }

        // The final size word counts. Bytes past that size are dropped.
        state_.resize(kStateOverhead + rdram_size);
        out_.state = std::move(state_);

        // The ROM section is in LE word order: console byte A is at A XOR 3.
        out_.rom.assign(rom_.size(), 0);
        for (size_t i = 0; i < rom_.size(); i++)
        {
            out_.rom[i] = rom_[i ^ 3];
        }

        return std::nullopt;
    }

private:
    static bool Flag(const xsflib::psf::Tags& tags, const std::string& name)
    {
        const auto it = tags.find(name);
        return it != tags.end() && !it->second.empty();
    }

    std::optional<std::string> Apply(const Sections& sections)
    {
        if (sections.rom)
        {
            for (const Chunk& chunk : *sections.rom)
            {
                const uint64_t end = uint64_t{chunk.offset} + chunk.data.size();
                if (end > kMaxRomOffset)
                {
                    return "a ROM chunk runs past the end of cartridge space";
                }

                // Whole words, so that the reordering at the end stays inside the image.
                const size_t size = static_cast<size_t>((end + 3) & ~uint64_t{3});
                if (rom_.size() < size)
                {
                    rom_.resize(size, 0);
                }

                std::copy(chunk.data.begin(), chunk.data.end(), rom_.begin() + chunk.offset);
            }
        }

        if (sections.state)
        {
            if (state_.empty())
            {
                state_.assign(kMaxStateSize, 0);
            }
            has_state_ = true;

            for (const Chunk& chunk : *sections.state)
            {
                const uint64_t end = uint64_t{chunk.offset} + chunk.data.size();
                if (end > kMaxStateSize)
                {
                    return "a state chunk runs past the end of the state image";
                }

                std::copy(chunk.data.begin(), chunk.data.end(), state_.begin() + chunk.offset);
            }
        }

        return std::nullopt;
    }

    const xsflib::psf::FileReader& reader_;
    LoadedUsf& out_;
    std::string_view separators_;
    size_t files_ = 0;
    std::vector<uint8_t> rom_; // LE word order
    std::vector<uint8_t> state_;
    bool has_state_ = false;
};

} // namespace

std::vector<uint8_t> WriteReservedArea(const Sections& sections)
{
    std::vector<uint8_t> out;
    WriteSection(out, sections.rom);
    WriteSection(out, sections.state);

    return out;
}

std::optional<std::string> ParseReservedArea(std::span<const uint8_t> reserved, Sections& out)
{
    out = {};
    size_t pos = 0;
    const size_t size = reserved.size();
    for (std::optional<std::vector<Chunk>>* section : {&out.rom, &out.state})
    {
        // A reserved area may end before a section's marker. The section is then absent.
        if (pos == size)
        {
            break;
        }
        if (size - pos < 4)
        {
            return "the reserved area ends inside a section's marker";
        }

        const bool present = std::memcmp(reserved.data() + pos, kMarker, 4) == 0;
        pos += 4;
        if (!present)
        {
            continue;
        }

        // Chunks up to a zero length, or up to the end of the reserved area.
        section->emplace();
        while (pos < size)
        {
            if (size - pos < 4)
            {
                return "the reserved area ends inside a chunk's length";
            }

            const uint32_t length = xsflib::LoadLe32(reserved.data() + pos);
            pos += 4;
            if (length == 0)
            {
                break;
            }

            if (size - pos < 4)
            {
                return "the reserved area ends inside a chunk's offset";
            }

            const uint32_t offset = xsflib::LoadLe32(reserved.data() + pos);
            pos += 4;
            if (length > size - pos)
            {
                return "a chunk's data runs past the end of the reserved area";
            }

            Chunk chunk;
            chunk.offset = offset;
            chunk.data.assign(reserved.begin() + static_cast<std::ptrdiff_t>(pos),
                              reserved.begin() + static_cast<std::ptrdiff_t>(pos + length));
            (*section)->push_back(std::move(chunk));
            pos += length;
        }
    }

    return std::nullopt;
}

std::vector<uint8_t> WriteUsf(const Sections& sections, const xsflib::psf::Tags& tags)
{
    const std::vector<uint8_t> reserved = WriteReservedArea(sections);
    std::vector<uint8_t> out = {'P', 'S', 'F', kUsfVersion};
    Put32(out, static_cast<uint32_t>(reserved.size()));
    Put32(out, 0); // No program.
    Put32(out, 0); // Its CRC-32.
    out.insert(out.end(), reserved.begin(), reserved.end());

    const std::string tag_area = xsflib::psf::FormatTagArea(tags);
    out.insert(out.end(), tag_area.begin(), tag_area.end());

    return out;
}

std::optional<std::string> LoadUsf(const std::string& path, const xsflib::psf::FileReader& reader, LoadedUsf& out,
                                   std::string_view separators)
{
    out = {};
    Loader loader(reader, out, separators);
    if (auto error = loader.Load(path, 0, true))
    {
        return error;
    }

    return loader.Finish();
}

} // namespace usf
