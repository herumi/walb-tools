#pragma once
/**
 * @file
 * @brief walb utilities.
 * @author HOSHINO Takashi
 *
 * (C) 2014 Cybozu Labs, Inc.
 */
#include <atomic>
#include <chrono>
#include <thread>
#include <fstream>
#include "util.hpp"
#include "file_path.hpp"
#include "tmp_file.hpp"
#include "tmp_file_serializer.hpp"
#include "fileio.hpp"
#include "fileio_serializer.hpp"
#include "constant.hpp"
#include "task_queue.hpp"
#include "action_counter.hpp"
#include "thread_util.hpp"
#include "time.hpp"
#include "process.hpp"
#include "linux/walb/walb.h"
#include "linux/walb/block_size.h"
#include "cybozu/exception.hpp"
#include "cybozu/string_operation.hpp"
#include "cybozu/socket.hpp"
#include "cybozu/option.hpp"
#include "cybozu/atoi.hpp"
#include "cybozu/itoa.hpp"
#include "cybozu/log.hpp"
#include "cybozu/file.hpp"
#include "cybozu/serializer.hpp"
#include "cybozu/array.hpp"
#include "walb_logger.hpp"
#include "walb_types.hpp"
#include "version.hpp"

namespace walb {

class ProcessStatus
{
    std::atomic<int> status_;
    enum {
        RUNNING, GRACEFUL_SHUTDOWN, FORCE_SHUTDOWN
    };
public:
    ProcessStatus() : status_(RUNNING) {}
    bool isRunning() const noexcept { return status_ == RUNNING; }
    bool isGracefulShutdown() const noexcept { return status_ == GRACEFUL_SHUTDOWN; }
    bool isForceShutdown() const noexcept { return status_ == FORCE_SHUTDOWN; }
    void setGracefulShutdown() noexcept { status_ = GRACEFUL_SHUTDOWN; }
    void setForceShutdown() noexcept { status_ = FORCE_SHUTDOWN; }
};

struct KeepAliveParams
{
    bool enabled;
    int idle;
    int intvl;
    int cnt;

    std::string toStr() const {
        auto fmt = cybozu::util::formatString;
        if (enabled) {
            return fmt("ON (idle %d intvl %d cnt %d)", idle, intvl, cnt);
        } else {
            return fmt("OFF");
        }
    }
    void verify() const {
        if (!enabled) return;
        if (idle < 0 || idle > MAX_TCP_KEEPIDLE) {
            throw cybozu::Exception("bad TCP keep-alive idle") << idle;
        }
        if (intvl < 0 || intvl > MAX_TCP_KEEPINTVL) {
            throw cybozu::Exception("bad TCP keep-alive interval") << intvl;
        }
        if (cnt < 0 || cnt > MAX_TCP_KEEPCNT) {
            throw cybozu::Exception("bad TCP keep-alive count") << cnt;
        }
    }
};

namespace util {

inline void saveMap(const std::string& file)
{
    const int pid = getpid();
    char name[256];
    snprintf(name, sizeof(name), "/proc/%d/maps", pid);
    std::ifstream ifs(name, std::ios::binary);
    std::ofstream ofs(file.c_str(), std::ios::binary);
    std::string line;
    while (std::getline(ifs, line)) {
        ofs << line << std::endl;
    }
}

/**
 * Make a directory.
 *
 * If not exists, make a specified directory.
   If exists,
 *   ensureNotExistance == false
 *     check the directory existance.
 *   ensureNotExistance == true
 *     throw an error.
 */
void makeDir(const std::string &dirStr, const char *msg,
             bool ensureNotExistance = false)
{
    cybozu::FilePath dir(dirStr);
    if (dir.stat().exists()) {
        if (ensureNotExistance) {
            throw cybozu::Exception(msg) << "already exists" << dirStr;
        } else {
            if (dir.stat().isDirectory()) {
                return;
            } else {
                throw cybozu::Exception(msg) << "not directory" << dirStr;
            }
        }
    }
    if (!dir.mkdir()) {
        throw cybozu::Exception(msg) << "mkdir failed" << dirStr;
    }
}

namespace walb_util_local {

/**
 * isDir: if true, get directories only.
 *        othwerwise, get files only.
 */
inline StrVec getDirEntNameList(const std::string &dirStr, bool isDir, const char *ext = "")
{
    StrVec ret;
    std::vector<cybozu::FileInfo> list = cybozu::GetFileList(dirStr, ext);
    for (const cybozu::FileInfo &info : list) {
        bool isDir2, isFile2;
        if (info.isUnknown()) {
            cybozu::FilePath fpath(dirStr);
            fpath += info.name;
            cybozu::FileStat stat = fpath.stat();
            if (!stat.exists()) continue;
            isDir2 = stat.isDirectory();
            isFile2 = stat.isFile();
        } else {
            isDir2 = info.isDirectory();
            isFile2 = info.isFile();
        }
        if ((isDir && isDir2) || (!isDir && isFile2)) {
            ret.push_back(info.name);
        }
    }
    return ret;
}

} // walb_util_local

inline StrVec getDirNameList(const std::string &dirStr)
{
    return walb_util_local::getDirEntNameList(dirStr, true);
}

inline StrVec getFileNameList(const std::string &dirStr, const char *ext)
{
    return walb_util_local::getDirEntNameList(dirStr, false, ext);
}

template <typename T>
void saveFile(const cybozu::FilePath &dir, const std::string &fname, const T &t)
{
    cybozu::TmpFile tmp(dir.str());
    cybozu::save(tmp, t);
    tmp.save((dir + fname).str());
}

template <typename T>
void loadFile(const cybozu::FilePath &dir, const std::string &fname, T &t)
{
    cybozu::util::File r((dir + fname).str(), O_RDONLY);
    cybozu::load(t, r);
}

inline void setLogSetting(const std::string &pathStr, bool isDebug)
{
    cybozu::SetLogUseMsec(true);
    if (pathStr == "-") {
        cybozu::SetLogFILE(::stderr);
    } else {
        cybozu::OpenLogFile(pathStr);
    }
    if (isDebug) {
        cybozu::SetLogPriority(cybozu::LogDebug);
    } else {
        cybozu::SetLogPriority(cybozu::LogInfo);
    }
}

inline void sleepMs(size_t ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

inline std::string getNowStr()
{
    struct timespec ts;
    if (::clock_gettime(CLOCK_REALTIME, &ts) < 0) {
        throw cybozu::Exception("getNowStr: clock_gettime failed.");
    }
    return cybozu::getHighResolutionTimeStr(ts);
}

/**
 * Convert binary data to hex string.
 */
inline std::string binaryToStr(const void *data, size_t size)
{
    std::string s;
    const uint8_t *p = static_cast<const uint8_t *>(data);
    s.resize(size * 2);
    for (size_t i = 0; i < size; i++) {
        cybozu::itohex(&s[i * 2], 2, p[i], false);
    }
    return s;
}

/**
 * Convert a hex string to binary data.
 */
template <typename CharT>
inline void strToBinary(const std::string &s, CharT *p, size_t size)
{
    if (size * 2 != s.size()) {
        throw cybozu::Exception(__func__) << "bad size" << s << size * 2;
    }
    for (size_t i = 0; i < size; i++) {
        p[i] = cybozu::hextoi(&s[i * 2], 2);
    }
}

inline std::string timeToPrintable(uint64_t ts)
{
    if (ts == 0) {
        return "---";
    } else {
        return cybozu::unixTimeToPrettyStr(ts);
    }
}

/**
 * @sock socket to connect.
 * @sockAddr socket address.
 * @timeout connection/read/write timeout [sec].
 */
inline void connectWithTimeout(cybozu::Socket &sock, const cybozu::SocketAddr &sockAddr, size_t timeout)
{
    const size_t timeoutMs = timeout * 1000;
    sock.connect(sockAddr, timeoutMs);
    sock.setSendTimeout(timeoutMs);
    sock.setReceiveTimeout(timeoutMs);
}

/**
 * @sock socket to use.
 * @idle TCP keep-alive idle time [sec].
 * @intvl TCP keep-alive interval period [sec].
 * @cnt TCP keep-alive the number of probes.
 */
inline void enableKeepAlive(cybozu::Socket &sock, int idle, int intvl, int cnt)
{
    sock.setSocketOption(SO_KEEPALIVE, 1, SOL_SOCKET);
    sock.setSocketOption(TCP_KEEPIDLE, idle, IPPROTO_TCP);
    sock.setSocketOption(TCP_KEEPINTVL, intvl, IPPROTO_TCP);
    sock.setSocketOption(TCP_KEEPCNT, cnt, IPPROTO_TCP);
}

inline void setSocketParams(cybozu::Socket& sock, const KeepAliveParams& params, size_t timeoutS)
{
    if (params.enabled) {
        sock.setSendTimeout(0);
        sock.setReceiveTimeout(0);
        util::enableKeepAlive(sock, params.idle, params.intvl, params.cnt);
    } else {
        sock.setSendTimeout(timeoutS * 1000);
        sock.setReceiveTimeout(timeoutS * 1000);
    }
}

inline void setKeepAliveOptions(cybozu::Option& opt, KeepAliveParams& params)
{
    opt.appendBoolOpt(&params.enabled, "ka", "enable TCP keep-alive.");
    opt.appendOpt(&params.idle, DEFAULT_TCP_KEEPIDLE, "kaidle", "TCP keep-alive idle time [sec].");
    opt.appendOpt(&params.intvl, DEFAULT_TCP_KEEPINTVL, "kaintvl", "TCP keep-alive interval time [sec].");
    opt.appendOpt(&params.cnt, DEFAULT_TCP_KEEPCNT, "kacnt", "TCP keep-alive count.");
}

/**
 * Parse integer string with suffix character k/m/g/t/p which means kibi/mebi/gibi/tebi/pebi.
 * and convert from [byte] to [logical block size].
 */
inline uint64_t parseSizeLb(const std::string &str, const char *msg, uint64_t minB = 0, uint64_t maxB = -1)
{
    const uint64_t sizeLb = cybozu::util::fromUnitIntString(str) / LOGICAL_BLOCK_SIZE;
    const uint64_t minLb = minB / LOGICAL_BLOCK_SIZE;
    const uint64_t maxLb = maxB / LOGICAL_BLOCK_SIZE;
    if (sizeLb < minLb) throw cybozu::Exception(msg) << "too small size" << minB << sizeLb;
    if (maxLb < sizeLb) throw cybozu::Exception(msg) << "too large size" << maxB << sizeLb;
    return sizeLb;
}

inline uint64_t parseBulkLb(const std::string &str, const char *msg)
{
    return parseSizeLb(str, msg, LOGICAL_BLOCK_SIZE, MAX_BULK_SIZE);
}

class TemporaryExistingFile
{
    const cybozu::FilePath  path_;
    static constexpr const char *NAME() { return "TemporaryExistingFile"; }
public:
    explicit TemporaryExistingFile(const cybozu::FilePath &path)
        : path_(path) {
        if (path.stat().exists()) {
            throw cybozu::Exception(NAME()) << "file exists" << path.str();
        }
        ::FILE *fp = ::fopen(path.str().c_str(), "w");
        if (fp) {
            ::fclose(fp);
        } else {
            throw cybozu::Exception(NAME()) << "fopen failed" << path.str();
        }
    }
    ~TemporaryExistingFile() noexcept {
        if (!path_.unlink()) {
            LOGs.error() << NAME() << "unlink error" << path_.str();
        }
    }
};

template <typename IntType>
inline void verifyNotZero(const IntType &t, const char *msg)
{
    if (t == 0) {
        throw cybozu::Exception(msg) << "must not be 0.";
    }
}

inline std::string getElapsedTimeStr(double elapsedSec)
{
    return cybozu::util::formatString("elapsed_time %.3f sec", elapsedSec);
}

inline void assignAlignedArray(AlignedArray& array, const void *data, size_t size)
{
    assert(data);
    array.resize(size, false);
    ::memcpy(array.data(), data, size);
}

inline void flushBdevBufs(const std::string& path)
{
    cybozu::process::call("/sbin/blockdev", {"--flushbufs", path});
}

/*
 * Parse integer from a string.
 * Empty strings mean 0.
 * Prefix "0x" means hex value.
 */
template <typename Int>
inline void parseDecOrHexInt(const std::string& s, Int& v)
{
    if (s.empty()) {
        v = 0;
        return;
    }
    if (s.substr(0, 2) != "0x") {
        v = cybozu::atoi(s);
        return;
    }
    if (!cybozu::util::hexStrToInt(s.substr(2), v)) {
        throw cybozu::Exception("hex string parse error") << s;
    }
}

inline std::string getDescription(const char *prefix)
{
    return cybozu::util::formatString(
        "%s version %s build at %s (wlog version %d)\n"
#ifndef DISABLE_COMMIT_ID
        "commit %s\n"
#endif
        , prefix
        , getWalbToolsVersion()
        , getWalbToolsBuildDate()
        , WALB_LOG_VERSION
#ifndef DISABLE_COMMIT_ID
        , getWalbToolsCommitId()
#endif
        );
}

}} // walb::util

inline int errorSafeMain(int (*doMain)(int, char *[]), int argc, char *argv[], const char *msg)
{
    try {
        walb::util::setLogSetting("-", false);
        return doMain(argc, argv);
    } catch (std::exception &e) {
        LOGs.error() << msg << e.what();
    } catch (...) {
        LOGs.error() << msg << "unknown error";
    }
    return 1;
}

#define DEFINE_ERROR_SAFE_MAIN(msg)                    \
    int main(int argc, char *argv[]) {                 \
        return errorSafeMain(doMain, argc, argv, msg); \
    }
