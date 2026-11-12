// SPDX-License-Identifier: MIT

#include "n64/word_set.h"

#include <bit>

namespace usf::n64
{

WordSet::WordSet(uint32_t bytes) : bits_((bytes / 4 + 63) / 64), words_(bytes / 4)
{
}

void WordSet::Grow(uint32_t words)
{
    if (words > words_)
    {
        words_ = words;
        bits_.resize((uint64_t{words} + 63) / 64);
    }
}

uint64_t WordSet::Count() const
{
    uint64_t count = 0;
    for (uint64_t bits : bits_)
    {
        count += static_cast<uint64_t>(std::popcount(bits));
    }

    return count;
}

void WordSet::Merge(const WordSet& other)
{
    for (size_t i = 0; i < bits_.size() && i < other.bits_.size(); i++)
    {
        bits_[i] |= other.bits_[i];
    }

    // The words of the last block past the set's size stay clear.
    if (words_ % 64 != 0)
    {
        bits_.back() &= (uint64_t{1} << (words_ % 64)) - 1;
    }
}

void WordSet::Intersect(const WordSet& other)
{
    for (size_t i = 0; i < bits_.size(); i++)
    {
        bits_[i] &= i < other.bits_.size() ? other.bits_[i] : 0;
    }
}

void WordSet::Subtract(const WordSet& other)
{
    for (size_t i = 0; i < bits_.size() && i < other.bits_.size(); i++)
    {
        bits_[i] &= ~other.bits_[i];
    }
}

std::vector<ByteRange> WordSet::Ranges(uint32_t max_gap) const
{
    std::vector<ByteRange> ranges;
    uint32_t word = 0;
    while (word < words_)
    {
        if (!Has(word))
        {
            word++;
            continue;
        }

        const uint32_t start = word;
        while (word < words_ && Has(word))
        {
            word++;
        }

        const uint32_t offset = start * 4;
        const uint32_t end = word * 4;
        if (!ranges.empty() && offset - (ranges.back().offset + ranges.back().length) <= max_gap)
        {
            ranges.back().length = end - ranges.back().offset;
        }
        else
        {
            ranges.push_back({offset, end - offset});
        }
    }

    return ranges;
}

} // namespace usf::n64
