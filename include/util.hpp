#pragma once
/**
 * @file
 * @brief Utilities.
 * @author HOSHINO Takashi
 *
 * (C) 2012 Cybozu Labs, Inc.
 */
#include <algorithm>
#include <iostream>
#include <memory>
#include <vector>
#include <stdexcept>
#include <string>
#include <cstdarg>
#include <cstring>
#include <cstdint>
#include <cinttypes>
#include <cerrno>
#include <cassert>
#include <cstdio>
#include <cctype>

#include <sys/time.h>

#ifdef _MSC_VER
#define UNUSED
#define DEPRECATED
#else
#define UNUSED __attribute__((unused))
#define DEPRECATED __attribute__((deprecated))
#endif

#define RT_ERR(...)                                             \
    std::runtime_error(cybozu::util::formatString(__VA_ARGS__))

#define CHECKx(cond) cybozu::util::checkCond(cond, __func__, __LINE__)

#define DISABLE_COPY_AND_ASSIGN(ClassName)              \
    ClassName(const ClassName &rhs) = delete;           \
    ClassName &operator=(const ClassName &rhs) = delete

#define DISABLE_MOVE(ClassName)                     \
    ClassName(ClassName &&rhs) = delete;            \
    ClassName &operator=(ClassName &&rhs) = delete

namespace cybozu {
namespace util {

/**
 * Formst string with va_list.
 */
inline std::string formatStringV(const char *format, va_list ap)
{
    char *p = nullptr;
    int ret = ::vasprintf(&p, format, ap);
    if (ret < 0) throw std::runtime_error("vasprintf failed.");
    try {
        std::string s(p, ret);
        ::free(p);
        return s;
    } catch (...) {
        ::free(p);
        throw;
    }
}

/**
 * Create a std::string using printf() like formatting.
 */
inline std::string formatString(const char * format, ...)
{
    std::string s;
    std::exception_ptr ep;
    va_list args;
    va_start(args, format);
    try {
        s = formatStringV(format, args);
    } catch (...) {
        ep = std::current_exception();
    }
    va_end(args);
    if (ep) std::rethrow_exception(ep);
    return s;
}

inline void checkCond(bool cond, const char *name, int line)
{
    if (!cond) {
        throw RT_ERR("check error: %s:%d", name, line);
    }
}

/**
 * Get unix time in double.
 */
inline double getTime()
{
    struct timeval tv;
    ::gettimeofday(&tv, NULL);
    return static_cast<double>(tv.tv_sec) +
        static_cast<double>(tv.tv_usec) / 1000000.0;
}

/**
 * Libc error wrapper.
 */
class LibcError : public std::exception {
public:
    LibcError() : LibcError(errno) {}
    LibcError(int errnum, const char* msg = "libc_error: ")
        : errnum_(errnum)
        , str_(generateMessage(errnum, msg)) {}
    virtual const char *what() const noexcept {
        return str_.c_str();
    }
    int errnum() const { return errnum_; }
private:
    int errnum_;
    std::string str_;
    static std::string generateMessage(int errnum, const char* msg) {
        std::string s(msg);
        const size_t BUF_SIZE = 1024;
        char buf[BUF_SIZE];
#ifndef _GNU_SOURCE
#error "We use GNU strerror_r()."
#endif
        char *c = ::strerror_r(errnum, buf, BUF_SIZE);
        s += c;
        return s;
    }
};

/**
 * Convert size string with unit suffix to unsigned integer.
 */
inline uint64_t fromUnitIntString(const std::string &valStr)
{
    if (valStr.empty()) throw RT_ERR("Invalid argument.");
    char *endp;
    uint64_t val = ::strtoll(valStr.c_str(), &endp, 10);
    int shift = 0;
    switch (*endp) {
    case 'e': case 'E': shift = 60; break;
    case 'p': case 'P': shift = 50; break;
    case 't': case 'T': shift = 40; break;
    case 'g': case 'G': shift = 30; break;
    case 'm': case 'M': shift = 20; break;
    case 'k': case 'K': shift = 10; break;
    case '\0': break;
    default:
        throw RT_ERR("Invalid suffix charactor.");
    }
    if (((val << shift) >> shift) != val) throw RT_ERR("fromUnitIntString: overflow.");
    return val << shift;
}

/**
 * Unit suffixes:
 *   k: 2^10
 *   m: 2^20
 *   g: 2^30
 *   t: 2^40
 *   p: 2^50
 *   e: 2^60
 */
inline std::string toUnitIntString(uint64_t val)
{
    uint64_t mask = (1ULL << 10) - 1;
    const char units[] = " kmgtpezy";

    size_t i = 0;
    while (i < sizeof(units)) {
        if ((val & ~mask) != val) { break; }
        i++;
        val >>= 10;
    }

    if (0 < i && i < sizeof(units)) {
        return formatString("%" PRIu64 "%c", val, units[i]);
    } else {
        return formatString("%" PRIu64 "", val);
    }
}

inline std::string byteArrayToStr(const void *data, size_t size)
{
    std::string s;
    s.resize(size * 2 + 1);
    for (size_t i = 0; i < size; i++) {
        ::snprintf(&s[i * 2], 3, "%02x", ((const uint8_t *)data)[i]);
    }
    s.resize(size * 2);
    return s;
}

/**
 * Print byte array as hex list.
 */
inline void printByteArray(::FILE *fp, const void *data, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        ::fprintf(fp, "%02x", ((const uint8_t *)data)[i]);
        if (i % 64 == 63) { ::fprintf(fp, "\n"); }
    }
    if (size % 64 != 0) { ::fprintf(fp, "\n"); }
}

inline void printByteArray(const void *data, size_t size)
{
    printByteArray(::stdout, data, size);
}

/**
 * "0x" prefix will not be put.
 */
template <typename IntType>
std::string intToHexStr(IntType i)
{
    if (i == 0) return "0";
    std::string s;
    while (0 < i) {
        int m = i % 16;
        if (m < 10) {
            s.push_back(m + '0');
        } else {
            s.push_back(m - 10 + 'a');
        }
        i /= 16;
    }
    std::reverse(s.begin(), s.end());
    return s;
}

/**
 * The function does not assume "0x" prefix.
 */
template <typename IntType>
bool hexStrToInt(const std::string &hexStr, IntType &i)
{
    std::string s1;
    i = 0;
    for (char c : hexStr) {
        c = std::tolower(c);
        s1.push_back(c);
        if ('0' <= c && c <= '9') {
            i = i * 16 + (c - '0');
        } else if ('a' <= c && c <= 'f') {
            i = i * 16 + (c - 'a' + 10);
        } else {
            return false;
        }
    }
    return intToHexStr(i) == s1;
}

/**
 * Trim first and last spaces from a string.
 */
inline std::string trimSpace(const std::string &str, const std::string &spaces = " \t\r\n")
{
    auto isSpace = [&](char c) -> bool {
        for (char space : spaces) {
            if (c == space) return true;
        }
        return false;
    };

    size_t i0, i1;
    for (i0 = 0; i0 < str.size(); i0++) {
        if (!isSpace(str[i0])) break;
    }
    for (i1 = str.size(); 0 < i1; i1--) {
        if (!isSpace(str[i1 - 1])) break;
    }
    if (i0 < i1) {
        return str.substr(i0, i1 - i0);
    } else {
        return "";
    }
}

/**
 * Split a string with separators.
 */
inline std::vector<std::string> splitString(
    const std::string &str, const std::string &separators, bool isTrimSpace = true)
{
    auto isSep = [&](int c) -> bool {
        for (char sepChar : separators) {
            if (sepChar == c) return true;
        }
        return false;
    };
    auto findSep = [&](size_t pos) -> size_t {
        for (size_t i = pos; i < str.size(); i++) {
            if (isSep(str[i])) return i;
        }
        return std::string::npos;
    };

    std::vector<std::string> v;
    size_t cur = 0;
    while (true) {
        size_t pos = findSep(cur);
        if (pos == std::string::npos) {
            v.push_back(str.substr(cur));
            break;
        }
        v.push_back(str.substr(cur, pos - cur));
        cur = pos + 1;
    }
    if (isTrimSpace) {
        for (std::string &s : v) s = trimSpace(s);
    }
    return v;
}

inline bool hasPrefix(const std::string &name, const std::string &prefix)
{
    return name.substr(0, prefix.size()) == prefix;
}

inline std::string removePrefix(const std::string &name, const std::string &prefix)
{
    assert(hasPrefix(name, prefix));
    return name.substr(prefix.size());
}

inline bool isAllDigit(const std::string &s)
{
    return std::all_of(s.cbegin(), s.cend(), [](const char &c) {
            return '0' <= c && c <= '9';
        });
}

/**
 * @name like "XXX_YYYYY"
 * @base like "YYYYY"
 * RETURN:
 *   prefix like "XXX_".
 */
inline std::string getPrefix(const std::string &name, const std::string &base)
{
    size_t s0 = name.size();
    size_t s1 = base.size();
    if (s0 <= s1) {
        throw std::runtime_error("There is no prefix.");
    }
    if (name.substr(s0 - s1) != base) {
        throw std::runtime_error("Base name differs.");
    }
    return name.substr(0, s0 - s1);
}

template <typename C>
void printList(const C &container)
{
    std::cout << "[";
    if (!container.empty()) {
        typename C::const_iterator it = container.cbegin();
        std::cout << *it;
        ++it;
        while (it != container.cend()) {
            std::cout << ", " << *it;
            ++it;
        }
    }
    std::cout << "]" << std::endl;
}

inline bool calcIsAllZero(const void *data, size_t size)
{
    assert(data);
    const char *p = (const char *)data;
    if (uintptr_t(data) % sizeof(uintptr_t) == 0) {
        /* aligned. */
        const uintptr_t *q = (const uintptr_t *)p;
        while (sizeof(uintptr_t) <= size) {
            if (*q) return false;
            ++q;
            size -= sizeof(uintptr_t);
        }
        p = (const char *)q;
    }
    while (0 < size) {
        if (*p) return false;
        ++p;
        --size;
    }
    return true;
}

inline void parseStrVec(
    const std::vector<std::string>& v, size_t pos, size_t numMust,
    std::initializer_list<std::string *> list)
{
    size_t i = 0;
    for (std::string *p : list) {
        if (pos + i >= v.size()) break;
        *p = v[pos + i];
        i++;
    }
    if (i < numMust) {
        throw RT_ERR("missing required params %zu %zu", i, numMust);
    }
}

} //namespace util
} //namespace cybozu
