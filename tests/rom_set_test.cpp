// SPDX-License-Identifier: MIT

// The loader of arcade ROM sets, on the profiles' chips. xsflib's tests cover the reader of ZIP archives and the
// loader itself.

#include "rip/rom_set.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "xsflib/sha1.h"
#include "xsflib/testing/check.h"
#include "xsflib/testing/temp_folder.h"

namespace
{

using usf::rip::GameProfile;
using usf::rip::RomChip;
using usf::rip::SetFile;

std::string Sha1(const std::vector<uint8_t>& data)
{
    return xsflib::Sha1::Hex(xsflib::Sha1::Of(data));
}

// A game of two chips: a byte-swapped one at offset 0 and a plain one at offset 4.
GameProfile TwoChips(const std::vector<uint8_t>& low, const std::vector<uint8_t>& high)
{
    GameProfile profile;
    profile.game = "Two Chips";
    profile.chips = {
        RomChip{Sha1(low), static_cast<uint32_t>(low.size()), 0, true},
        RomChip{Sha1(high), static_cast<uint32_t>(high.size()), 4, false},
    };

    return profile;
}

void BuildsImage()
{
    const std::vector<uint8_t> low = {0x37, 0x80, 0x40, 0x12};
    const std::vector<uint8_t> high = {5, 6, 7, 8};
    // A console game has no chips. It comes first and never matches.
    const std::vector<GameProfile> profiles = {GameProfile{}, TwoChips(low, high)};
    const std::vector<SetFile> files = {{"other", {9}}, {"high", high}, {"low", low}};

    const usf::rip::RomSetImage image = usf::rip::BuildRomSetImage(profiles, files);
    const usf::rip::RomSetImage missing = usf::rip::BuildRomSetImage(profiles, {{"low", low}});

    XSFLIB_CHECK(image.profile == &profiles[1]);
    XSFLIB_CHECK(image.rom == std::vector<uint8_t>({0x80, 0x37, 0x12, 0x40, 5, 6, 7, 8}));
    XSFLIB_CHECK(missing.profile == nullptr);
    XSFLIB_CHECK(missing.problem.find("the set of Two Chips is missing 1 of its 2 chips") != std::string::npos);
    XSFLIB_CHECK(missing.problem.find(Sha1(high)) != std::string::npos);
}

// A folder's files are read only at the sizes of the profiles' chips.
void ReadsFolders()
{
    const std::vector<uint8_t> low = {0x37, 0x80, 0x40, 0x12};
    const std::vector<uint8_t> high = {5, 6, 7, 8};
    const std::vector<GameProfile> profiles = {TwoChips(low, high)};
    const xsflib::testing::TempFolder folder("usf_rom_set_test");
    {
        std::ofstream(folder.Path() / "x", std::ios::binary)
            .write(reinterpret_cast<const char*>(low.data()), static_cast<std::streamsize>(low.size()));
        std::ofstream(folder.Path() / "y", std::ios::binary)
            .write(reinterpret_cast<const char*>(high.data()), static_cast<std::streamsize>(high.size()));
        std::ofstream(folder.Path() / "other", std::ios::binary).write("12345", 5);
    }

    std::vector<SetFile> files;
    const auto problem = usf::rip::ReadRomSet(folder.Path(), profiles, files);

    XSFLIB_CHECK(usf::rip::IsRomSet(folder.Path()));
    XSFLIB_CHECK(!problem && files.size() == 2);
    XSFLIB_CHECK(usf::rip::BuildRomSetImage(profiles, files).profile == &profiles[0]);
}

} // namespace

int main()
{
    return xsflib::testing::RunTest("rom_set",
                                    []
                                    {
                                        BuildsImage();
                                        ReadsFolders();
                                    });
}
