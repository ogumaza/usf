// SPDX-License-Identifier: MIT

#include <cstdint>
#include <vector>

#include "xsflib/psf/psf.h"

#include "usf/usf_file.h"

namespace usf
{

std::vector<uint8_t> WriteUsf(const Sections& sections, const xsflib::psf::Tags& tags)
{
    // With no program, the PSF writer sets the program size and CRC to 0 and compresses nothing.
    return xsflib::psf::WritePsf(kUsfVersion, WriteReservedArea(sections), {}, tags);
}

} // namespace usf
