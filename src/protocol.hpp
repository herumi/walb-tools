#pragma once
/**
 * @file
 * @brief Protocol set.
 * @author HOSHINO Takashi
 *
 * (C) 2013 Cybozu Labs, Inc.
 */
#include <map>
#include <string>
#include <memory>
#include "cybozu/socket.hpp"
#include "packet.hpp"
#include "util.hpp"
#include "walb_logger.hpp"
#include "walb_util.hpp"
#include "process.hpp"
#include "command_param_parser.hpp"

namespace walb {

/**
 * Message string.
 */
const char *const msgOk = "ok"; // used for synchronous protocol.
const char *const msgAccept = "accept"; // used for asynchronous protocol.

const char *const msgTooNewDiff = "too-new-diff";
const char *const msgTooOldDiff = "too-old-diff";
const char *const msgDifferentUuid = "different-uuid";
const char *const msgStopped = "stopped";
const char *const msgWdiffRecv = "wdiff-recv";
const char *const msgSyncing = "syncing";
const char *const msgArchiveNotFound = "archive-not-found";
const char *const msgSmallerLvSize = "smaller-lv-size";

/**
 * Host type.
 */
const char *const controllerHT = "controller";
const char *const storageHT = "storage";
const char *const proxyHT = "proxy";
const char *const archiveHT = "archive";

/**
 * Command name.
 */
const char *const statusCN = "status";
const char *const initVolCN = "init-vol";
const char *const clearVolCN = "clear-vol";
const char *const resetVolCN = "reset-vol";
const char *const startCN = "start";
const char *const stopCN = "stop";
const char *const fullBkpCN = "full-bkp";
const char *const hashBkpCN = "hash-bkp";
const char *const snapshotCN = "snapshot";
const char *const archiveInfoCN = "archive-info";
const char *const restoreCN = "restore";
const char *const delRestoredCN = "del-restored";
const char *const replicateCN = "replicate";
const char *const applyCN = "apply";
const char *const mergeCN = "merge";
const char *const resizeCN = "resize";
const char *const shutdownCN = "shutdown";
const char *const kickCN = "kick";
const char *const blockHashCN = "bhash";
const char *const dbgReloadMetadataCN = "dbg-reload-metadata";
const char *const dbgSetUuidCN = "dbg-set-uuid";
const char *const dbgSetStateCN = "dbg-set-state";
const char *const dbgSetBaseCN = "dbg-set-base";
const char *const getCN = "get";
const char *const execCN = "exec";
const char *const disableSnapshotCN = "disable-snapshot";
const char *const enableSnapshotCN = "enable-snapshot";
const char *const dbgDumpLogpackHeaderCN = "dbg-dump-logpack-header";

/**
 * Target name of 'get' command.
 */
const char *const isOverflowTN = "is-overflow";
const char *const isWdiffSendErrorTN = "is-wdiff-send-error";
const char *const numActionTN = "num-action";
const char *const stateTN = "state";
const char *const hostTypeTN = "host-type";
const char *const volTN = "vol";
const char *const pidTN = "pid";
const char *const diffTN = "diff";
const char *const applicableDiffTN = "applicable-diff";
const char *const totalDiffSizeTN = "total-diff-size";
const char *const existsDiffTN = "exists-diff";
const char *const existsBaseImageTN = "exists-base-image";
const char *const restoredTN = "restored";
const char *const restorableTN = "restorable";
const char *const uuidTN = "uuid";
const char *const archiveUuidTN = "archive-uuid";
const char *const baseTN = "base";
const char *const volSizeTN = "vol-size";
const char *const progressTN = "progress";

/**
 * Internal protocol name.
 */
const char *const dirtyFullSyncPN = "dirty-full-sync";
const char *const dirtyHashSyncPN = "dirty-hash-sync";
const char *const wlogTransferPN = "wlog-transfer";
const char *const wdiffTransferPN = "wdiff-transfer";
const char *const replSyncPN = "repl-sync";


inline cybozu::SocketAddr parseSocketAddr(const std::string &addrPort)
{
    const StrVec v = cybozu::Split(addrPort, ':', 2);
    if (v.size() != 2) {
        throw cybozu::Exception("parseSocketAddr:parse error") << addrPort;
    }
    return cybozu::SocketAddr(v[0], static_cast<uint16_t>(cybozu::atoi(v[1])));
}

inline std::vector<cybozu::SocketAddr> parseMultiSocketAddr(const std::string &multiAddrPort)
{
    std::vector<cybozu::SocketAddr> ret;
    const StrVec v = cybozu::Split(multiAddrPort, ',');
    for (const std::string &addrPort : v) {
        ret.emplace_back(parseSocketAddr(addrPort));
    }
    return ret;
}

namespace protocol {

/**
 * RETURN:
 *   Server ID.
 */
inline std::string run1stNegotiateAsClient(
    cybozu::Socket &sock,
    const std::string &clientId, const std::string &protocolName)
{
    packet::Packet pkt(sock);
    pkt.write(clientId);
    pkt.write(protocolName);
    packet::Version ver(sock);
    ver.send();
    pkt.flush();
    std::string serverId;
    pkt.read(serverId);

    ProtocolLogger logger(clientId, serverId);
    std::string msg;
    pkt.read(msg);
    if (msg != msgOk) throw cybozu::Exception(__func__) << msg;
    return serverId;
}

/**
 * Parameters for commands as a client.
 */
struct ClientParams
{
    cybozu::Socket &sock;
    ProtocolLogger &logger;
    const StrVec &params;

    ClientParams(
        cybozu::Socket &sock0,
        ProtocolLogger &logger0,
        const StrVec &params0)
        : sock(sock0)
        , logger(logger0)
        , params(params0) {
    }
};

/**
 * Client handler type.
 */
using ClientHandler = void (*)(ClientParams &);

/**
 * @sock socket for the connection.
 * @protocolName will be set.
 * @clientId will be set.
 *
 * This function will do only the common negotiation.
 */
inline void run1stNegotiateAsServer(
    cybozu::Socket &sock, const std::string &serverId,
    std::string &protocolName, std::string &clientId)
{
    const char *const FUNC = __func__;
    packet::Packet pkt(sock);

    pkt.read(clientId);
    pkt.read(protocolName);
    packet::Version ver(sock);
    const bool isVersionSame = ver.recv();
    pkt.write(serverId);
    LOGs.debug() << FUNC << clientId << protocolName << ver.get();

    if (!isVersionSame) {
        throw cybozu::Exception(FUNC) << "version differ c/s" << ver.get() << packet::VERSION;
    }
    ProtocolLogger logger(serverId, clientId);
    logger.debug() << "initial negotiation succeeded" << protocolName;
}

/**
 * Parameters for commands as a server.
 */
struct ServerParams
{
    cybozu::Socket &sock;
    const std::string& clientId;
    walb::ProcessStatus &ps;

    ServerParams(
        cybozu::Socket &sock,
        const std::string &clientId,
        walb::ProcessStatus &ps)
        : sock(sock)
        , clientId(clientId)
        , ps(ps) {
    }
};

inline void shutdownClient(ClientParams &p)
{
    const bool isForce = parseShutdownParam(p.params);
    packet::Packet pkt(p.sock);
    pkt.write(isForce);
    std::string res;
    pkt.read(res);
    if (res != msgAccept) {
        throw cybozu::Exception(__func__) << res;
    }
}

inline void shutdownServer(ServerParams &p)
{
    bool isForce;
    packet::Packet pkt(p.sock);
    pkt.read(isForce);
    if (isForce) {
        p.ps.setForceShutdown();
    } else {
        p.ps.setGracefulShutdown();
    }
    LOGs.info() << "shutdown" << (isForce ? "force" : "graceful") << p.clientId;
    pkt.writeFin(msgAccept);
}

/**
 * Server handler type.
 */
using ServerHandler = void (*)(ServerParams &);
typedef std::map<std::string, ServerHandler> Str2ServerHandler;

inline ServerHandler findServerHandler(
    const Str2ServerHandler &handlers, const std::string &protocolName)
{
    if (protocolName == shutdownCN) {
        return shutdownServer;
    }
    Str2ServerHandler::const_iterator it = handlers.find(protocolName);
    if (it == handlers.cend()) {
        throw cybozu::Exception(__func__) << "bad protocol" << protocolName;
    }
    return it->second;
}

/**
 * Server dispatcher.
 */
class RequestWorker {
    cybozu::Socket sock;
    std::string nodeId;
    ProcessStatus &ps;
public:
    const protocol::Str2ServerHandler& handlers;
    RequestWorker(cybozu::Socket &&sock, const std::string &nodeId,
                  ProcessStatus &ps, const protocol::Str2ServerHandler& handlers)
        : sock(std::move(sock))
        , nodeId(nodeId)
        , ps(ps)
        , handlers(handlers) {}
    void operator()() noexcept {
// #define DEBUG_HANDLER
#ifdef DEBUG_HANDLER
        static std::atomic<int> ccc;
        LOGs.info() << "SERVER_START" << nodeId << int(ccc++);
#endif
        try {
            std::string clientId, protocolName;
            packet::Packet pkt(sock);
            bool sendErr = true;
            try {
                run1stNegotiateAsServer(sock, nodeId, protocolName, clientId);
                ServerHandler handler = findServerHandler(handlers, protocolName);
                ServerParams serverParams(sock, clientId, ps);
                pkt.write(msgOk);
                pkt.flush();
                sendErr = false;
#ifdef DEBUG_HANDLER
                LOGs.info() << "SERVER_HANDLE" << nodeId << protocolName;
#endif
                handler(serverParams);
            } catch (std::exception &e) {
                LOGs.error() << e.what();
                if (sendErr) pkt.write(e.what());
            } catch (...) {
                cybozu::Exception e(__func__);
                e << "other error";
                LOGs.error() << e.what();
                if (sendErr) pkt.write(e.what());
            }
        } catch (std::exception &e) {
            LOGs.error() << e.what();
        } catch (...) {
            LOGs.error() << "other error";
        }
        const bool dontThrow = true;
        sock.close(dontThrow);
#ifdef DEBUG_HANDLER
        LOGs.info() << "SERVER_END  " << nodeId << int(ccc--);
#endif
    }
};

/**
 * If numToSend == 0, it will not check the vector size.
 */
inline void sendStrVec(
    cybozu::Socket &sock,
    const StrVec &v, size_t numToSend, const char *msg, const char *confirmMsg = nullptr)
{
    if (numToSend != 0 && v.size() != numToSend) {
        throw cybozu::Exception(msg) << "bad size" << numToSend << v.size();
    }
    packet::Packet packet(sock);
    for (size_t i = 0; i < v.size(); i++) {
        if (v[i].empty()) {
            throw cybozu::Exception(msg) << "empty string" << i;
        }
    }
    packet.write(v);
    packet.flush();

    if (confirmMsg) {
        packet::Packet pkt(sock);
        std::string res;
        pkt.read(res);
        if (res != confirmMsg) {
            throw cybozu::Exception(msg) << res;
        }
    }
}

/**
 * If numToRecv == 0, it will not check the vector size.
 */
inline StrVec recvStrVec(
    cybozu::Socket &sock, size_t numToRecv, const char *msg)
{
    packet::Packet packet(sock);
    StrVec v;
    packet.read(v);
    if (numToRecv != 0 && v.size() != numToRecv) {
        throw cybozu::Exception(msg) << "bad size" << numToRecv << v.size();
    }
    for (size_t i = 0; i < v.size(); i++) {
        if (v[i].empty()) {
            throw cybozu::Exception(msg) << "empty string" << i;
        }
    }
    return v;
}

enum ValueType {
    SizeType,
    StringType,
    StringVecType,
};

struct GetCommandInfo
{
    ValueType valueType;
    void (*verify)(const StrVec &);
    std::string helpMsg;
};

using GetCommandInfoMap = std::map<std::string, GetCommandInfo>;

inline const GetCommandInfo &getGetCommandInfo(const std::string &name, const GetCommandInfoMap &infoM, const char *msg)
{
    GetCommandInfoMap::const_iterator it = infoM.find(name);
    if (it == infoM.cend()) {
        throw cybozu::Exception(msg) << "name not found" << name;
    }
    return it->second;
}

namespace local {

template <typename T>
inline T recvValue(cybozu::Socket &sock)
{
    packet::Packet pkt(sock);
    T t;
    pkt.read(t);
    packet::Ack(sock).recv();
    return t;
}

} // namespace local

inline void recvValueAndPut(cybozu::Socket &sock, ValueType valType, const char *msg)
{
    packet::Packet pkt(sock);
    switch (valType) {
    case protocol::SizeType:
        std::cout << local::recvValue<size_t>(sock) << std::endl;
        return;
    case protocol::StringType:
        std::cout << local::recvValue<std::string>(sock) << std::endl;
        return;
    case protocol::StringVecType:
        for (const std::string &s : local::recvValue<StrVec>(sock)) {
            std::cout << s << std::endl;
        }
        return;
    default:
        throw cybozu::Exception(msg) << "bad ValueType" << int(valType);
    }
}

struct GetCommandParams
{
    const StrVec &params;
    packet::Packet &pkt;
    Logger &logger;
    bool &sendErr;
};

using GetCommandHandler = void (*)(GetCommandParams&);
using GetCommandHandlerMap = std::map<std::string, GetCommandHandler>;

inline void runGetCommandServer(ServerParams &p, const std::string &nodeId, const GetCommandHandlerMap &hMap)
{
    const char *const FUNC = __func__;
    ProtocolLogger logger(nodeId, p.clientId);
    packet::Packet pkt(p.sock);

    bool sendErr = true;
    try {
        const StrVec params = recvStrVec(p.sock, 0, FUNC);
        if (params.empty()) throw cybozu::Exception(FUNC) << "no target specified";
        const std::string &targetName = params[0];
        protocol::GetCommandHandlerMap::const_iterator it = hMap.find(targetName);
        if (it == hMap.cend()) throw cybozu::Exception(FUNC) << "no such target" << targetName;
        protocol::GetCommandHandler handler = it->second;
        GetCommandParams cParams{params, pkt, logger, sendErr};
        handler(cParams);
    } catch (std::exception &e) {
        logger.error() << e.what();
        if (sendErr) pkt.write(e.what());
    }
}

template <typename T>
inline void sendValueAndFin(packet::Packet &pkt, bool &sendErr, const T &t)
{
    pkt.write(msgOk);
    sendErr = false;
    pkt.write(t);
    packet::Ack(pkt.sock()).sendFin();
}

template <typename T>
inline void sendValueAndFin(GetCommandParams &p, const T &t)
{
    sendValueAndFin(p.pkt, p.sendErr, t);
}

template <typename VolStateGetter>
inline void runGetStateServer(GetCommandParams &p, VolStateGetter getter)
{
    const std::string volId = parseVolIdParam(p.params, 1);
    const std::string state = getter(volId).sm.get();
    sendValueAndFin(p, state);
}

inline std::string runGetHostTypeClient(cybozu::Socket &sock, const std::string &nodeId)
{
    run1stNegotiateAsClient(sock, nodeId, getCN);
    sendStrVec(sock, {hostTypeTN}, 1, __func__, msgOk);
    return local::recvValue<std::string>(sock);
}

inline void runExecServer(ServerParams &p, const std::string &nodeId)
{
    const char *const FUNC = __func__;
    ProtocolLogger logger(nodeId, p.clientId);
    packet::Packet pkt(p.sock);

    bool sendErr = true;
    try {
        const StrVec v = recvStrVec(p.sock, 0, FUNC);
        const std::string res = cybozu::process::call(v);
        StrVec ret = cybozu::util::splitString(res, "\r\n");
        cybozu::util::removeEmptyItemFromVec(ret);
        sendValueAndFin(pkt, sendErr, ret);
        logger.info() << "exec done" << ret.size() << cybozu::util::concat(v, " ");
    } catch (std::exception &e) {
        logger.error() << e.what();
        if (sendErr) pkt.write(e.what());
    }
}

}} // namespace walb::protocol
