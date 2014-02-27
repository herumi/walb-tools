#pragma once
#include "protocol.hpp"
#include "storage_vol_info.hpp"
#include "state_map.hpp"
#include "state_machine.hpp"
#include "constant.hpp"
#include <snappy.h>

namespace walb {

struct StorageVolState {
    std::recursive_mutex mu;
    std::atomic<int> stopState;
    StateMachine sm;

    explicit StorageVolState(const std::string& volId) : stopState(NotStopping), sm(mu) {
        const struct StateMachine::Pair tbl[] = {
            { sClear, stInitVol },
            { stInitVol, sSyncReady },
            { sSyncReady, stClearVol },
            { stClearVol, sClear },

            { sSyncReady, stStartSlave },
            { stStartSlave, sSlave },
            { sSlave, stStopSlave },
            { stStopSlave, sSyncReady },

            { sSlave, stWlogRemove },
            { stWlogRemove, sSlave },

            { sSyncReady, stFullSync },
            { stFullSync, sStopped },
            { sSyncReady, stHashSync },
            { stHashSync, sStopped },
            { sStopped, stReset },
            { stReset, sSyncReady },

            { sStopped, stStartMaster },
            { stStartMaster, sMaster },
            { sMaster, stStopMaster },
            { stStopMaster, sStopped },

            { sMaster, stWlogSend },
            { stWlogSend, sMaster },
        };
        sm.init(tbl);

        initInner(volId);
    }
private:
    void initInner(const std::string& volId);
};

struct StorageSingleton
{
    static StorageSingleton& getInstance() {
        static StorageSingleton instance;
        return instance;
    }
    cybozu::SocketAddr archive;
    std::vector<cybozu::SocketAddr> proxyV;
    std::string nodeId;
    std::string baseDirStr;
    walb::StateMap<StorageVolState> stMap;
};

inline StorageSingleton& getStorageGlobal()
{
    return StorageSingleton::getInstance();
}

const StorageSingleton& gs = getStorageGlobal();

inline void StorageVolState::initInner(const std::string& volId)
{
    StorageVolInfo volInfo(gs.baseDirStr, volId);
    if (volInfo.existsVolDir()) {
        sm.set(volInfo.getState());
    } else {
        sm.set(sClear);
    }
}

inline StorageVolState &getStorageVolState(const std::string &volId)
{
    return getStorageGlobal().stMap.get(volId);
}

inline void c2sStatusServer(protocol::ServerParams &p)
{
    packet::Packet packet(p.sock);
    std::vector<std::string> params;
    packet.read(params);

    if (params.empty()) {
        // for all volumes
        packet.write("not implemented yet");
        // TODO
    } else {
        // for a volume
        const std::string &volId = params[0];
        StorageVolInfo volInfo(gs.baseDirStr, volId);
        if (!volInfo.existsVolDir()) {
            cybozu::Exception e("c2sStatusServer:no such volume");
            packet.write(e.what());
            throw e;
        }
        packet.write("ok");

        StrVec statusStrV;
        // TODO: show the memory state of the volume.
        for (std::string &s : volInfo.getStatusAsStrVec()) {
            statusStrV.push_back(std::move(s));
        }
        packet.write(statusStrV);
    }
}

inline void c2sInitVolServer(protocol::ServerParams &p)
{
    const StrVec v = protocol::recvStrVec(p.sock, 2, "c2sInitVolServer", false);
    const std::string &volId = v[0];
    const std::string &wdevPathName = v[1];

    StateMachine &sm = getStorageVolState(volId).sm;
    {
        StateMachineTransaction tran(sm, sClear, stInitVol, "c2sInitVolServer");
        StorageVolInfo volInfo(gs.baseDirStr, volId, wdevPathName);
        volInfo.init();
        tran.commit(sSyncReady);
    }
    packet::Ack(p.sock).send();

    ProtocolLogger logger(gs.nodeId, p.clientId);
    logger.info("c2sInitVolServer: initialize volId %s wdev %s", volId.c_str(), wdevPathName.c_str());
}

inline void c2sClearVolServer(protocol::ServerParams &p)
{
    StrVec v = protocol::recvStrVec(p.sock, 1, "c2sClearVolServer", false);
    const std::string &volId = v[0];

    StateMachine &sm = getStorageVolState(volId).sm;
    {
        StateMachineTransaction tran(sm, sSyncReady, stClearVol, "c2sClearVolServer");
        StorageVolInfo volInfo(gs.baseDirStr, volId);
        volInfo.clear();
        tran.commit(sClear);
    }

    packet::Ack(p.sock).send();

    ProtocolLogger logger(gs.nodeId, p.clientId);
    logger.info("c2sClearVolServer: cleared volId %s", volId.c_str());
}

/**
 * params[0]: volId
 * params[1]: "master" or "slave".
 */
inline void c2sStartServer(protocol::ServerParams &p)
{
    const StrVec v = protocol::recvStrVec(p.sock, 2, "c2sStartServer", false);
    const std::string &volId = v[0];
    const bool isMaster = (v[1] == "master");

    StateMachine &sm = getStorageVolState(volId).sm;
    if (isMaster) {
        StateMachineTransaction tran(sm, sStopped, stStartMaster, "c2sStartServer");
        StorageVolInfo volInfo(gs.baseDirStr, volId);
        volInfo.setState(sMaster);
        // TODO: start monitoring of the target walb device.
        tran.commit(sMaster);
    } else {
        StateMachineTransaction tran(sm, sSyncReady, stStartSlave, "c2sStartServer");
        StorageVolInfo volInfo(gs.baseDirStr, volId);
        volInfo.setState(sSlave);
        // TODO: start monitoring of the target walb device.
        tran.commit(sSlave);
    }
    packet::Ack(p.sock).send();
}

/**
 * params[0]: volId
 * params[1]: isForce: "0" or "1".
 */
inline void c2sStopServer(protocol::ServerParams &p)
{
    const StrVec v = protocol::recvStrVec(p.sock, 2, "c2sStopServer", false);
    const std::string &volId = v[0];
    const int isForceInt = cybozu::atoi(v[1]);
    UNUSED const bool isForce = (isForceInt != 0);

    StorageVolState &volSt = getStorageVolState(volId);
    packet::Ack(p.sock).send();

    util::Stopper stopper(volSt.stopState, isForce);
    if (!stopper.isSuccess()) {
        return;
    }

    UniqueLock ul(volSt.mu);
    StateMachine &sm = volSt.sm;

    util::waitUntil(ul, [&]() {
            const std::string &st = sm.get();
            return st == stFullSync || st == stHashSync || st == stWlogSend || st == stWlogRemove;
        }, "c2sStopServer");

    const std::string st = sm.get();
    if (st != sMaster || st != sSlave) {
        /* For SyncReady state (after FullSync and HashSync canceled),
           there is nothing to do. */
        return;
    }

    StorageVolInfo volInfo(gs.baseDirStr, volId);
    if (st == sMaster) {
        StateMachineTransaction tran(sm, sMaster, stStopMaster, "c2sStopServer");
        ul.unlock();

        // TODO: stop monitoring.

        volInfo.setState(sStopped);
        tran.commit(sStopped);
    } else {
        assert(st == sSlave);
        StateMachineTransaction tran(sm, sSlave, stStopSlave, "c2sStopServer");
        ul.unlock();

        // TODO: stop monitoring.

        volInfo.setState(sSyncReady);
        tran.commit(sSyncReady);
    }
}

inline void c2sFullSyncServer(protocol::ServerParams &p)
{
    ProtocolLogger logger(gs.nodeId, p.clientId);

    const StrVec v = protocol::recvStrVec(p.sock, 2, "c2sFullSyncServer", false);
    const std::string& volId = v[0];
    const uint64_t bulkLb = cybozu::atoi(v[1]);
    const uint64_t curTime = ::time(0);
    LOGd("volId %s bulkLb %" PRIu64 " curTime %" PRIu64 ""
         , volId.c_str(), bulkLb, curTime);
    const std::string& nodeId = gs.nodeId;
    std::string archiveId;

    StorageVolInfo volInfo(gs.baseDirStr, volId);
    packet::Packet cPack(p.sock);

    StorageVolState &volSt = getStorageVolState(volId);

    if (volSt.stopState != NotStopping) {
        cybozu::Exception e("c2sFullSyncServer:Stopping");
        e << volId << volSt.stopState;
        cPack.write(e.what());
        throw e;
    }

    StateMachine &sm = volSt.sm;
    {
        StateMachineTransaction tran0(sm, sSyncReady, stFullSync, "c2sFullSyncServer");

        volInfo.resetWlog(0);

        const uint64_t sizeLb = getSizeLb(volInfo.getWdevPath());
        const cybozu::Uuid uuid = volInfo.getUuid();
        LOGd("sizeLb %" PRIu64 " uuid %s", sizeLb, uuid.str().c_str());

        // ToDo : start master((3) at full-sync as client in storage-daemon.txt)

        const cybozu::SocketAddr& archive = gs.archive;
        {
            cybozu::Socket aSock;
            aSock.connect(archive);
            archiveId = walb::protocol::run1stNegotiateAsClient(aSock, gs.nodeId, "dirty-full-sync");
            walb::packet::Packet aPack(aSock);
            aPack.write("storageD");
            aPack.write(volId);
            aPack.write(uuid);
            aPack.write(sizeLb);
            aPack.write(curTime);
            aPack.write(bulkLb);
            {
                std::string res;
                aPack.read(res);
                if (res == "ok") {
                    cPack.write("ok");
                    p.sock.close();
                } else {
                    cybozu::Exception e("c2sFullSyncServer:bad response");
                    e << archiveId << res;
                    cPack.write(e.what());
                    throw e;
                }
            }
            // (7) in storage-daemon.txt
            {
                std::vector<char> buf(bulkLb * LOGICAL_BLOCK_SIZE);
                cybozu::util::BlockDevice bd(volInfo.getWdevPath(), O_RDONLY);
                std::string encBuf;

                uint64_t remainingLb = sizeLb;
                while (0 < remainingLb) {
                    if (volSt.stopState == ForceStopping || p.forceQuit) {
                        logger.warn("c2sFullSyncServer:force stopped");
                        // TODO: stop monitoring.
                        return;
                    }
                    uint16_t lb = std::min<uint64_t>(bulkLb, remainingLb);
                    size_t size = lb * LOGICAL_BLOCK_SIZE;
                    bd.read(&buf[0], size);
                    const size_t encSize = snappy::Compress(&buf[0], size, &encBuf);
                    aPack.write(encSize);
                    aPack.write(&encBuf[0], encSize);
                    remainingLb -= lb;
                }
            }
            // (8), (9) in storage-daemon.txt
            {
                // TODO take a snapshot
                uint64_t gidB = 0, gidE = 1;
                aPack.write(gidB);
                aPack.write(gidE);
            }
            walb::packet::Ack(aSock).recv();
        }
        tran0.commit(sStopped);

        StateMachineTransaction tran1(sm, sStopped, stStartMaster, "c2sFullSyncServer");
        volInfo.setState(sMaster);
        tran1.commit(sMaster);
    }

    // TODO: If thrown an error, someone must stop monitoring task.

    LOGi("c2sFullSyncServer done, ctrl:%s storage:%s archive:%s", p.clientId.c_str(), nodeId.c_str(), archiveId.c_str());
}

/**
 * Take a snapshot to restore in archive hosts.
 */
inline void c2sSnapshotServer(protocol::ServerParams &/*p*/)
{
    // QQQ
}

} // walb
