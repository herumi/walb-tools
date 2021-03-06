#pragma once
/**
 * @file
 * @brief Checksum utilities.
 * @author HOSHINO Takashi
 *
 * (C) 2012 Cybozu Labs, Inc.
 */
#include <cstring>
#include <cinttypes>
#include <cassert>

namespace cybozu {
namespace util {

/**
 * Calculate checksum partially.
 * You must call this several time and finally call checksumFinish() to get csum.
 *
 * @data pointer to data.
 * @size data size.
 * @csum result of previous call, or salt.
 */
inline uint32_t checksumPartial(const void *data, size_t size, uint32_t csum)
{
    const char *p = (const char *)data;
    uint32_t v;
    while (sizeof(v) <= size) {
        ::memcpy(&v, p, sizeof(v));
        csum += v;
        size -= sizeof(v);
        p += sizeof(v);
    }
    assert(size < sizeof(v));
    if (0 < size) {
        uint32_t padding = 0;
        ::memcpy(&padding, p, size);
        csum += padding;
    }
    return csum;
}

/**
 * Finish checksum calculation.
 */
inline uint32_t checksumFinish(uint32_t csum)
{
    return ~csum + 1;
}

/**
 * Get checksum of a byte array.
 */
inline uint32_t calcChecksum(const void *data, size_t size, uint32_t salt)
{
    return checksumFinish(checksumPartial(data, size, salt));
}

}} //namespace cybozu::util
