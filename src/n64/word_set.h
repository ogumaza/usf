// SPDX-License-Identifier: MIT

// Sets of the 4-byte words of an image, for coverage and for trimming a rip.

#pragma once

#include <cstdint>
#include <vector>

namespace usf::n64
{

// A range of bytes [offset, offset + length).
struct ByteRange
{
    uint32_t offset;
    uint32_t length;
};

// A set of the 4-byte words of an image.
class WordSet
{
public:
    WordSet() = default;
    explicit WordSet(uint32_t bytes);

    // Makes room for `words` words. A set never shrinks.
    void Grow(uint32_t words);

    // Adds one word. The word must be within the set's size.
    void AddWord(uint32_t word)
    {
        bits_[word / 64] |= uint64_t{1} << (word % 64);
    }

    bool Has(uint32_t word) const
    {
        return word < words_ && (bits_[word / 64] & (uint64_t{1} << (word % 64))) != 0;
    }

    uint64_t Count() const;

    // Adds the words of `other`, up to the set's size.
    void Merge(const WordSet& other);

    // The words as byte ranges in ascending order. Ranges at most `max_gap` bytes apart are joined, with the words
    // between them.
    std::vector<ByteRange> Ranges(uint32_t max_gap = 0) const;

private:
    std::vector<uint64_t> bits_;
    uint32_t words_ = 0;
};

} // namespace usf::n64
