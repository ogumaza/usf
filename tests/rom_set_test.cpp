// SPDX-License-Identifier: MIT

// The loader of arcade ROM sets, on the profiles' chips. xsflib's tests cover the reader of ZIP archives and the loader
// itself.

#include "rip/rom_set.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "xsflib/files.h"
#include "xsflib/sha1.h"
#include "xsflib/testing/check.h"
#include "xsflib/testing/temp_folder.h"

namespace
{

using usf::rip::GameProfile;
using usf::rip::RomChip;
using usf::rip::SetFile;

// The chips of the game that the tests find: a byte-swapped chip with the start of a cartridge's header, and a plain
// one.
const std::vector<uint8_t> kLow = {0x37, 0x80, 0x40, 0x12};
const std::vector<uint8_t> kHigh = {5, 6, 7, 8};

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
    // A console game has no chips. It comes first and never matches.
    const std::vector<GameProfile> profiles = {GameProfile{}, TwoChips(kLow, kHigh)};
    const std::vector<SetFile> files = {{"other", {9}}, {"high", kHigh}, {"low", kLow}};

    const usf::rip::RomSetImage image = usf::rip::BuildRomSetImage(profiles, files);
    const usf::rip::RomSetImage missing = usf::rip::BuildRomSetImage(profiles, {{"low", kLow}});

    XSFLIB_CHECK(image.profile == &profiles[1]);
    XSFLIB_CHECK(image.rom == std::vector<uint8_t>({0x80, 0x37, 0x12, 0x40, 5, 6, 7, 8}));
    XSFLIB_CHECK(missing.profile == nullptr);
    XSFLIB_CHECK(missing.problem.find("the set of Two Chips is missing 1 of its 2 chips") != std::string::npos);
    XSFLIB_CHECK(missing.problem.find(Sha1(kHigh)) != std::string::npos);
}

// Given the chips of two games, BuildRomSetImage builds the image of the first game and lists the second in `others`. A
// second profile with the SHA-1 of that image is a second set of the same game, not another game. A third game with a
// chip missing isn't listed.
void NamesOtherGames()
{
    const std::vector<uint8_t> other_low = {1, 2, 3, 4};
    const std::vector<uint8_t> other_high = {9, 10, 11, 12};
    std::vector<GameProfile> profiles = {TwoChips(kLow, kHigh), TwoChips(kLow, kHigh), TwoChips(other_low, other_high),
                                         TwoChips(other_low, {13, 14, 15, 16})};
    profiles[0].sha1 = "first";
    profiles[1].sha1 = "first";
    profiles[2].sha1 = "second";
    profiles[3].sha1 = "third";
    const std::vector<SetFile> files = {
        {"low", kLow}, {"high", kHigh}, {"other low", other_low}, {"other high", other_high}};

    const usf::rip::RomSetImage image = usf::rip::BuildRomSetImage(profiles, files);
    const usf::rip::RomSetImage alone = usf::rip::BuildRomSetImage(profiles, {{"low", kLow}, {"high", kHigh}});

    XSFLIB_CHECK(image.profile == &profiles[0]);
    XSFLIB_CHECK(image.others == std::vector<const GameProfile*>({&profiles[2]}));
    XSFLIB_CHECK(alone.profile == &profiles[0] && alone.others.empty());
}

// A folder's files are read only at the sizes of the profiles' chips.
void ReadsFolders()
{
    const std::vector<GameProfile> profiles = {TwoChips(kLow, kHigh)};
    const xsflib::testing::TempFolder folder("usf_rom_set_test");
    XSFLIB_CHECK(!xsflib::WriteFile(folder.Path() / "x", kLow));
    XSFLIB_CHECK(!xsflib::WriteFile(folder.Path() / "y", kHigh));
    XSFLIB_CHECK(!xsflib::WriteFile(folder.Path() / "other", std::vector<uint8_t>{'1', '2', '3', '4', '5'}));

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
                                        NamesOtherGames();
                                        ReadsFolders();
                                    });
}
