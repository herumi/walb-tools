/**
 * @file
 * @brief WalB proxy daemon.
 * @author HOSHINO Takashi
 *
 * (C) 2014 Cybozu Labs, Inc.
 */
#include <cstdio>
#include <chrono>
#include <thread>
#include <string>
#include <atomic>
#include "thread_util.hpp"
#include "cybozu/socket.hpp"
#include "cybozu/serializer.hpp"
#include "cybozu/option.hpp"
#include "file_path.hpp"
#include "net_util.hpp"
#include "server_util.hpp"
#include "walb_util.hpp"
#include "proxy.hpp"

/* These should be defined in the parameter header. */
const uint16_t DEFAULT_LISTEN_PORT = 5000;
const std::string DEFAULT_BASE_DIR = "/var/forest/walb/proxy";
const std::string DEFAULT_LOG_FILE = "-";

namespace walb {

/**
 * Request worker.
 */
class ProxyRequestWorker : public server::RequestWorker
{
public:
    using RequestWorker :: RequestWorker;
    void run() override {
        const std::map<std::string, protocol::ServerHandler> h = {
            { "status", c2pStatusServer },
        };
        protocol::serverDispatch(
            sock_, nodeId_, forceQuit_, procStat_, h);
    }
};

} // namespace walb

struct Option : cybozu::Option
{
    uint16_t port;
    std::string baseDirStr;
    std::string logFileStr;
    std::string nodeId;
    bool isDebug;
    Option() {
        //setUsage();
        appendOpt(&port, DEFAULT_LISTEN_PORT, "p", "listen port");
        appendOpt(&baseDirStr, DEFAULT_BASE_DIR, "b", "base directory (full path)");
        appendOpt(&logFileStr, DEFAULT_LOG_FILE, "l", "log file name.");
        appendBoolOpt(&isDebug, "debug", "put debug message.");

        std::string hostName = cybozu::net::getHostName();
        appendOpt(&nodeId, hostName, "id", "node identifier");

        appendHelp("h");
    }
    std::string logFilePath() const {
        if (logFileStr == "-") return logFileStr;
        return (cybozu::FilePath(baseDirStr) + logFileStr).str();
    }
};

void initSingleton(Option &opt)
{
    walb::ProxySingleton &s = walb::ProxySingleton::getInstance();

    s.nodeId = opt.nodeId;
    s.baseDirStr = opt.baseDirStr;

    // QQQ
}

int main(int argc, char *argv[]) try
{
    Option opt;
    if (!opt.parse(argc, argv)) {
        opt.usage();
        return 1;
    }
    walb::util::makeDir(opt.baseDirStr, "proxyServer", false);
    walb::util::setLogSetting(opt.logFilePath(), opt.isDebug);
    initSingleton(opt);
    auto createRequestWorker = [&](
        cybozu::Socket &&sock, const std::atomic<bool> &forceQuit,
        std::atomic<walb::server::ProcessStatus> &procStat) {
        return std::make_shared<walb::ProxyRequestWorker>(
            std::move(sock), opt.nodeId, forceQuit, procStat);
    };
    walb::server::MultiThreadedServer server;
    server.run(opt.port, createRequestWorker);

} catch (std::exception &e) {
    LOGe("ProxyServer: error: %s", e.what());
    return 1;
} catch (...) {
    LOGe("ProxyServer: caught other error.");
    return 1;
}

/* end of file */