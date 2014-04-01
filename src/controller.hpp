#pragma once
#include "protocol.hpp"
#include "constant.hpp"
#include "host_info.hpp"

namespace walb {

/**
 * params.size() == 0 or 1.
 * params[0]: volId
 */
inline void c2xGetStrVecClient(protocol::ClientParams &p)
{
    const char *const FUNC = __func__;
    packet::Packet packet(p.sock);
    packet.write(p.params);

    std::string st;
    packet.read(st);
    if (st != msgOk) {
        throw cybozu::Exception(FUNC) << "not ok" << st;
    }

    std::vector<std::string> v;
    packet.read(v);
    for (const std::string &s : v) {
        std::cout << s << std::endl;
    }
}

/**
 * No parameter.
 */
inline void c2xListVolClient(protocol::ClientParams &p)
{
    const char *const FUNC = __func__;
    const StrVec volIdV = protocol::recvStrVec(p.sock, 0, FUNC, false);
    packet::Ack(p.sock).recv();
    for (const std::string &volId : volIdV) {
        std::cout << volId << std::endl;
    }
}

/**
 * For storage:
 *   params[0]: volId
 *   params[2]: wdevPath
 * For archive:
 *   params[0]: volId
 */
inline void c2xInitVolClient(protocol::ClientParams &p)
{
    protocol::sendStrVec(p.sock, p.params, 0, "c2xInitVolClient", msgOk);
}

/**
 * Server is storage or archive.
 * params[0]: volId
 */
inline void c2xClearVolClient(protocol::ClientParams &p)
{
    protocol::sendStrVec(p.sock, p.params, 1, "c2xClearVolClient", msgOk);
}

/**
 * For storage:
 *   params[0]: volId
 *   params[1]: master
 * For archive:
 *   params[0]: volId
 */
inline void c2xStartClient(protocol::ClientParams &p)
{
    protocol::sendStrVec(p.sock, p.params, 0, "c2xStartClient", msgOk);
}

/**
 * params[0]: volId
 * params[1]: isForce: "0" or "1".
 */
inline void c2xStopClient(protocol::ClientParams &p)
{
    protocol::sendStrVec(p.sock, p.params, 2, "c2xStopClient", msgOk);
}

inline void c2sFullBkpClient(protocol::ClientParams &p)
{
    const char *const FUNC = __func__;
    if (p.params.size() != 1 && p.params.size() != 2) {
        throw cybozu::Exception(FUNC) << "bad size param" << p.params.size();
    }
    std::vector<std::string> v;
    if (p.params[0].empty()) {
        throw cybozu::Exception(FUNC) << "empty volId";
    }
    v.push_back(p.params[0]);
    uint64_t bulkLb = walb::DEFAULT_BULK_LB;
    if (p.params.size() == 2) {
        bulkLb = cybozu::atoi(p.params[1]);
        if (bulkLb == 0) {
            throw cybozu::Exception(FUNC) << "zero bulkLb";
        }
    }
    v.push_back(cybozu::itoa(bulkLb));
    LOGd("sendStrVec");
    protocol::sendStrVec(p.sock, v, 2, FUNC);

    std::string st;
    packet::Packet packet(p.sock);
    packet.read(st);
    if (st != msgAccept) {
        throw cybozu::Exception(FUNC) << "not accept" << st;
    }
}

inline void c2sHashBkpClient(protocol::ClientParams &/*p*/)
{
    // QQQ
}

/**
 * Restore command.
 * parameters: volId, gid
 */
inline void c2aRestoreClient(protocol::ClientParams &p)
{
    const char *const FUNC = __func__;
    protocol::sendStrVec(p.sock, p.params, 2, FUNC);
    packet::Packet pkt(p.sock);

    std::string s;
    pkt.read(s);
    if (s != msgAccept) {
        throw cybozu::Exception(FUNC) << "failed" << s;
    }
}

/**
 * Drop command.
 * parameters: volId, gid
 */
inline void c2aDropClient(protocol::ClientParams &p)
{
    const char *const FUNC = __func__;
    protocol::sendStrVec(p.sock, p.params, 2, FUNC, msgOk);
}

namespace ctrl_local {

inline void verifyEnoughParameters(const StrVec& params, size_t num, const char *msg)
{
    if (params.size() < num) {
        throw cybozu::Exception(msg) << "not enough parameters";
    }
}

} // ctrl_local

/**
 * pattern (1)
 *   list <volId>
 * pattern (2)
 *   get/delete <volId> <archiveId>
 * pattern (3)
 *   add/update <volId> <archiveId> <addr>:<port> <cmprType>:<cmprLevel>:<cmprNumCPU> <wdiffSendDelaySec>
 *
 * <cmprType>: compression type. none, snappy, gzip, or lzma.
 * <cmprLevel>: compression level. integer from 0 to 9.
 * <cmprType>:<cmprLevel>:<cmprNumCPU> and <wdiffSendDelay> can be omitted.
 */
inline void c2pArchiveInfoClient(protocol::ClientParams &p)
{
    const char * const FUNC = __func__;
    ctrl_local::verifyEnoughParameters(p.params, 2, FUNC);
    const std::string &cmd = p.params[0];
    const std::string &volId = p.params[1];
    protocol::sendStrVec(p.sock, {cmd, volId}, 2, FUNC);
    packet::Packet pkt(p.sock);
    if (cmd != "list") {
        ctrl_local::verifyEnoughParameters(p.params, 3, FUNC);
        const std::string &archiveId = p.params[2];
        pkt.write(archiveId);
    }
    if (cmd == "add" || cmd == "update") {
        ctrl_local::verifyEnoughParameters(p.params, 4, FUNC);
        const std::string &addrPort = p.params[3];
        std::string compressOpt = "snappy:0:1";
        if (p.params.size() > 4) compressOpt = p.params[4];
        std::string delay = "0";
        if (p.params.size() > 5) delay = p.params[5];
        HostInfo hi = parseHostInfo(addrPort, compressOpt, delay);
        LOGs.debug() << hi;
        pkt.write(hi);
    }

    std::string res;
    pkt.read(res);
    if (res != msgOk) {
        throw cybozu::Exception(FUNC) << "command failed" << res;
    }

    if (cmd == "list") {
        StrVec v;
        pkt.read(v);
        for (const std::string& s : v) {
            std::cout << s << std::endl;
        }
        return;
    }
    if (cmd == "get") {
        HostInfo hi;
        pkt.read(hi);
        std::cout << hi << std::endl;
        return;
    }
}

/**
 * Take a snapshot which will be restorable at the archive site.
 *
 * params[0]: volId.
 *
 * Print the gid of the snapshot.
 */
inline void c2sSnapshotClient(protocol::ClientParams &p)
{
    const char *const FUNC = __func__;
    protocol::sendStrVec(p.sock, p.params, 1, FUNC);
    packet::Packet pkt(p.sock);

    std::string res;
    pkt.read(res);
    if (res != msgOk) {
        throw cybozu::Exception(FUNC) << "failed" << res;
    }

    uint64_t gid;
    pkt.read(gid);
    std::cout << gid << std::endl;
}

/**
 * params[0]: volId.
 *
 * !!!CAUSION!!!
 * This is for test and debug.
 */
inline void c2aReloadMetadataClient(protocol::ClientParams &p)
{
    protocol::sendStrVec(p.sock, p.params, 1, __func__, msgOk);
}

/**
 * params[0] volId
 * params[1] archiveAddrPortStr like "192.168.1.1:10000".
 * params[2] archiveCompressionOptStr like "snappy:0:1".
 */
inline void c2aReplicateClient(protocol::ClientParams &/*p*/)
{
    // QQQ
}

/**
 * params[0]: volId
 * params[1]: gid: All snapshots where snap.gidB < gid will be deleted.
 */
inline void c2aApplyClient(protocol::ClientParams &p)
{
    protocol::sendStrVec(p.sock, p.params, 2, __func__, msgAccept);
}

/**
 * params[0]: volId
 * params[1]: gidB: begin of the range.
 * params[2]: gidE: end of the range.
 * params[3]: maxSizeMb: (optional) max size of total input wdiff files [MiB].
 */
inline void c2aMergeClient(protocol::ClientParams &/*p*/)
{
    // QQQ
}

/**
 * params[0]: volId
 * params[1]: size [byte] suffix k/m/g can be used.
 */
inline void c2xResizeClient(protocol::ClientParams &p)
{
    protocol::sendStrVec(p.sock, p.params, 2, __func__, msgOk);
}

inline void c2xHostTypeClient(protocol::ClientParams &p)
{
    const std::string hostType = protocol::runHostTypeClient(p.sock);
    std::cout << hostType << std::endl;
}

/**
 * params[0]: volId
 * params[1]: gid as string (optional)
 */
inline void c2xResetVolClient(protocol::ClientParams &p)
{
    const char *const FUNC = __func__;
    protocol::sendStrVec(p.sock, p.params, 0, FUNC, msgOk);
}

} // namespace walb
