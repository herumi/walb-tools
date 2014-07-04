#!/usr/bin/env python

import collections
import os
import threading
import subprocess
import sys
import time
import socket
import errno
import shutil

from util import *

TIMEOUT_SEC = 100

Mebi = (1 << 20)  # mebi.
Lbs = (1 << 9)  # logical block size

K_STORAGE = 0
K_PROXY = 1
K_ARCHIVE = 2

serverKinds = [K_STORAGE, K_PROXY, K_ARCHIVE]


# storage steady states
sClear = "Clear"
sSyncReady = "SyncReady"
sStopped = "Stopped"
sMaster = "Master"
sSlave = "Slave"

# storage temporary states
stInitVol = "InitVol"
stClearVol = "ClearVol"
stStartSlave = "StartSlave"
stStopSlave = "StopSlave"
stFullSync = "FullSync"
stHashSync = "HashSync"
stStartMaster = "StartMaster"
stStopMaster = "StopMaster"
stReset = "Reset"

# storage actions
saWlogSend = "WlogSend"
saWlogRemove = "WlogRemove"

# proxy steady states
pClear = "Clear"
pStopped = "Stopped"
pStarted = "Started"

# proxy temporary states
ptStart = "Start"
ptStop = "Stop"
ptClearVol = "ClearVol"
ptAddArchiveInfo = "AddArchiveInfo"
ptDeleteArchiveInfo = "DeleteArchiveInfo"
ptWlogRecv = "WlogRecv"
ptWaitForEmpty = "WaitForEmpty"

# archive steady states
aClear = "Clear"
aSyncReady = "SyncReady"
aArchived = "Archived"
aStopped = "Stopped"

# archive temporary states
atInitVol = "InitVol"
atClearVol = "ClearVol"
atResetVol = "ResetVol"
atFullSync = "FullSync"
atHashSync = "HashSync"
atWdiffRecv = "WdiffRecv"
atReplSync = "ReplSyncAsServer"
atStop = "Stop"
atStart = "Start"

# archive actions
aaMerge = "Merge"
aaApply = "Apply"
aaRestore = "Restore"
aaReplSync = "ReplSyncAsClient"
aaResize = "Resize"

sDuringFullSync = [stFullSync, sStopped, stStartMaster]
sDuringHashSync = [stHashSync, sStopped, stStartMaster]
sDuringStopForMaster = [sMaster, stStopMaster]
sDuringStopForSlave = [sSlave, stStopSlave]
pActive = [pStarted, ptWlogRecv]
pDuringStop = [pStarted, ptWlogRecv, ptStop, ptWaitForEmpty]
aActive = [aArchived, atWdiffRecv, atHashSync, atReplSync]
aAcceptForResize = aActive + [aStopped]
aAcceptForClearVol = [aStopped, aSyncReady]
aDuringReplicate = [atReplSync, atFullSync]
aDuringStop = aActive + [atStop]


class Device:
    '''
    Walb device.

    '''
    def __init__(self, iD, ldev, ddev, walbctlPath, runCommand=run_command):
        '''
        iD :: int          - walb device id.
        ldev :: str        - underlying log block device path.
        ddev :: str        - underlying data block device path.
        walbctlPath :: str - walbctl path.
        runCommand:: RunCommand - function that run commands.
        '''
        verify_type(iD, int)
        verify_type(ldev, str)
        verify_type(ddev, str)
        verify_type(walbctlPath, str)
        verify_function(runCommand)
        self.iD = iD
        self.ldev = ldev
        self.ddev = ddev
        self.walbctlPath = walbctlPath
        self.runCommand = runCommand

    @property
    def path(self):
        return '/dev/walb/' + str(self.iD)

    def run_walbctl(self, cmdArgs):
        '''
        Run walbctl command.
        cmdArgs :: [str] - command line arguments.
        '''
        verify_list_type(cmdArgs, str)
        self.runCommand([self.walbctlPath] + cmdArgs)

    def exists(self):
        '''
        Check the walb device exists or not.
        return :: bool
        '''
        if 0:
            # This is local only.
            return os.path.exists(self.path)
        else:
            cmd = 'if [ -b "%s" ]; ' \
                  'then echo 1;' \
                  'else echo 0; fi' % self.path
            res = self.runCommand(['/bin/sh', '-c', cmd])
            return int(res) != 0

    def format_ldev(self):
        '''
        Format devices for a walb device.
        TODO: support format_ldev options.
        '''
        self.run_walbctl(['format_ldev',
                          '--ldev', self.ldev,
                          '--ddev', self.ddev])

    def create(self):
        '''
        Create a walb device.
        TODO: support create_wdev options.
        '''
        self.run_walbctl(['create_wdev',
                          '--ldev', self.ldev,
                          '--ddev', self.ddev,
                          '--name', str(self.iD)])

    def delete(self):
        '''
        Delete a walb device.
        '''
        self.run_walbctl(['delete_wdev', '--wdev', self.path])

    def resize(self, newSizeMb):
        '''
        Resize a walb device.
        Underlying data device must be resized before calling this function.
        newSizeMb :: int - new size [MiB].
        '''
        newSizeLb = newSizeMb * Mebi / Lbs
        self.run_walbctl(['resize', '--wdev', self.path,
                          '--size', str(newSizeLb)])

    def reset(self):
        '''
        Reset a walb device.
        This will remove all logs in the log device.
        You should call this to recover from log overflow.

        '''
        self.run_walbctl(['reset_wal', '--wdev', self.path])

    def get_size_lb(self):
        '''
        Get walb device size.
        This will read sysfs to get size.
        return :: int -- device size [logical block].
        '''
        path = self._get_sys_path() + 'size'
        return int(self.runCommand(['/bin/cat', path]).strip())

    def get_size_mb(self):
        '''
        Get walb device size.
        This will read sysfs to get size.
        return :: int -- device size [MiB].
        '''
        sizeB = self.get_size_lb() * Lbs
        if sizeB % Mebi != 0:
            raise Exception('get_size_mb: not multiple of MiB.', sizeB)
        return sizeB / Mebi

    def wait_for_log_empty(self, timeoutS=TIMEOUT_SEC):
        '''
        Wait for log device becomes empty.
        wdev :: Wdev    - walb device.
        timeoutS :: int - timeout [sec].
        '''
        verify_type(timeoutS, int)

        def create_key_value(ls):
            ret = []
            for s in ls:
                (k, v) = s.strip().split()
                ret.append((k, int(v)))
            return dict(ret)

        path = self._get_sys_path() + 'walb/lsids'
        t0 = time.time()
        while time.time() < t0 + timeoutS:
            out = self.runCommand(['/bin/cat', path]).strip()
            kv = create_key_value(out.split('\n'))
            completed = kv['completed']
            oldest = kv['oldest']
            if completed == oldest:
                return
            print "wait_for_log_empty", oldest, completed
            time.sleep(0.3)
        raise Exception("wait_for_log_empty", self.path)

    '''
    private member functions.
    '''
    def _get_sys_path(self):
        return '/sys/block/walb!%d/' % self.iD


class Server:
    '''
    Server configuration.
    '''
    def __init__(self, name, address, port, kind, binDir, dataDir,
                 logPath=None, vg=None):
        '''
        name :: str            - daemon identifier in the system.
        address :: str         - host name
        port :: int            - port number.
        kind :: int            - K_STORAGE, K_PROXY, or K_ARCHIVE.
        binDir :: str          - directory path containing server executable
                                 at the host.
        dataDir :: str         - persistent data directory path.
        logPath :: str or None - log path. None means default.
        vg :: str              - volume group name.
                                 This is required by archive server only.
        '''
        verify_type(name, str)
        verify_type(address, str)
        verify_type(port, int)
        verify_type(kind, int)
        if kind not in serverKinds:
            raise Exception('Server: wrong server kind', kind)
        verify_type(binDir, str)
        verify_type(dataDir, str)
        if logPath is not None:
            verify_type(logPath, str)
        if kind == K_ARCHIVE or vg is not None:
            verify_type(vg, str)

        self.name = name
        self.address = address
        self.port = port
        self.kind = kind
        self.binDir = binDir
        self.dataDir = dataDir
        self.logPath = logPath
        self.vg = vg

    def get_host_port(self):
        '''
        Get 'address:port' string.
        return :: str
        '''
        return self.address + ":" + str(self.port)


def verify_server_kind(s, kind):
    '''
    s :: Server
    kind :: int
    '''
    if s.kind != kind:
        raise Exception('invalid server type', s.kind, kind)


class ServerLayout:
    '''
    Server layout of a backup group.
    '''
    def __init__(self, storageL, proxyL, archiveL):
        '''
        storageL :: [Server] - storage server list.
        proxyL :: [Server]   - proxy server list.
                               Before items have high priority.
        archiveL :: [Server] - archive server list. The head is primary server.
        '''
        verify_list_type(storageL, Server)
        verify_list_type(proxyL, Server)
        verify_list_type(archiveL, Server)

        if len(storageL) == 0:
            raise Exception('server_layout: no storage server')
        if len(proxyL) == 0:
            raise Exception('server_layout: no proxy server')
        if len(archiveL) == 0:
            raise Exception('server_layout: no archive server')

        self.storageL = storageL
        self.proxyL = proxyL
        self.archiveL = archiveL

    def get_primary_archive(self):
        '''
        return :: Server
        '''
        return self.archiveL[0]

    def get_all(self):
        '''
        return :: [Server]
        '''
        return self.storageL + self.proxyL + self.archiveL

    def replace(self, storageL=None, proxyL=None, archiveL=None):
        '''
        Make a copy replacing arguments which are not None.
        storageL :: [Server] or None - if None, storageL will not unchange.
        proxyL :: [Server] or None - if None, archiveL will not unchange.
        archiveL :: [Server] or None - if None, archiveL will not unchange.
        return :: ServerLayout
        '''
        sL = self.storageL
        if storageL:
            sL = storageL
        pL = self.proxyL
        if proxyL:
            pL = proxyL
        aL = self.archiveL
        if archiveL:
            aL = archiveL
        return ServerLayout(sL, pL, aL)


def get_server_args(s, sLayout, isDebug=False):
    '''
    Get walb-tools server arguments.
    s :: Server     - server.
    return :: [str] - argument list.
    '''
    verify_type(s, Server)
    verify_type(sLayout, ServerLayout)
    verify_type(isDebug, bool)

    if s.kind == K_STORAGE:
        proxies = ",".join(map(lambda p: p.get_host_port(), sLayout.proxyL))
        ret = [s.binDir + "storage-server",
               "-archive", sLayout.get_primary_archive().get_host_port(),
               "-proxy", proxies]
    elif s.kind == K_PROXY:
        ret = [s.binDir + "proxy-server"]
    else:
        assert s.kind == K_ARCHIVE
        ret = [s.binDir + "archive-server", "-vg", s.vg]
    if s.logPath:
        logPath = s.logPath
    else:
        logPath = s.dataDir + '/' + s.name + '.log'
    ret += ["-p", str(s.port),
            "-b", s.dataDir,
            "-l", logPath,
            "-id", s.name]
    if isDebug:
        ret += ['-debug']
    return ret


class Controller:
    '''
    To handle all walb servers in a backup group.

    '''
    def __init__(self, binDir, sLayout, isDebug=False):
        '''
        binDir :: str           - directory where walb controller exists.
        sLayout :: ServerLayout - server layout.
        isDebug :: bool
        '''
        verify_type(binDir, str)
        verify_type(isDebug, bool)
        self.controllerPath = binDir + '/controller'
        self.isDebug = isDebug
        self.set_server_layout(sLayout)

    def set_server_layout(self, sLayout):
        '''
        sLayout :: ServerLayout - server layout.
        '''
        verify_type(sLayout, ServerLayout)
        self.sLayout = sLayout

    def run_ctl(self, s, cmdArgs, putMsg=False):
        '''
        Run walb-tools controller.
        s :: Server      - a server.
        cmdArgs :: [str] - command arguments.
        putMsg :: bool   - put debug message if True.
        return :: str    - stdout of the control command.
        '''
        verify_type(s, Server)
        verify_list_type(cmdArgs, str)
        verify_type(putMsg, bool)
        ctlArgs = [self.controllerPath,
                   "-id", "ctl",
                   "-a", s.address,
                   "-p", str(s.port)]
        if putMsg:
            ctlArgs += ['-debug']
        return run_command(ctlArgs + cmdArgs, putMsg)

    def run_remote_command(self, s, args, putMsg=False):
        '''
        Run arbitrary executable at a remote server running walb daemon.
        This will use walb-tools daemons to run commands.

        s :: Server    - server where you want to run command.
        args :: [str]  - command line arguments.
                         The head item must be a full-path executable.
        putMsg :: bool - put debug message if True.
        return :: str  - stdout of the command if the command returned 0.
        '''
        verify_type(s, Server)
        verify_list_type(args, str)
        return self.run_ctl(s, ['exec', '---'] + args, True)

    def get_remote_run_command(self, s):
        '''
        Get run_command function for RPC.
        walbc :: Controller
        s :: Server
        return :: ([str],bool -> str)

        '''
        verify_type(s, Server)

        def func(args, putMsg=False):
            return self.run_remote_command(s, args, putMsg)

        return func

    def get_host_type(self, s, putMsg=True):
        '''
        Get host type.
        s :: Server
        return :: str - 'storage', 'proxy', or 'archive'.
        '''
        verify_type(s, Server)
        return self.run_ctl(s, ['get', 'host-type'], putMsg)

    def get_state(self, s, vol, putMsg=True):
        '''
        Get state of a volume.
        s :: Server
        vol :: str    - volume name.
        return :: str - state.
        '''
        verify_type(s, Server)
        verify_type(vol, str)
        return self.run_ctl(s, ['get', 'state', vol], putMsg)

    def verify_state(self, s, vol, state, putMsg=True):
        '''
        s :: Server
        vol :: str
        state :: str
        putMsg :: bool
        '''
        verify_type(state, str)
        st = self.get_state(s, vol, putMsg)
        if st != state:
            raise Exception('verify_state: differ', st, state)

    def reset_vol(self, s, vol):
        '''
        Reset a volume.
        s :: Server - storage or archive.
        vol :: str  - volume name.
        '''
        verify_type(s, Server)
        verify_type(vol, str)
        self.run_ctl(s, ["reset-vol", vol])  # this is synchronous command.
        if s.kind == K_STORAGE:
            self.verify_state(s, vol, sSyncReady)
        elif s.kind == K_ARCHIVE:
            self.verify_state(s, vol, aSyncReady)
        else:
            raise Exception('reset_vol:bad server', s)

    def set_slave_storage(self, sx, vol):
        '''
        Set a volume slave storage in sx.

        sx :: Server - storage server.
        vol :: str   - volume name.
        '''
        verify_type(sx, Server)
        verify_type(vol, str)
        state = self.get_state(sx, vol)
        if state == sSlave:
            return
        if state == sSyncReady:
            self.start(sx, vol)
            return
        if state == sMaster:
            self.stop(sx, vol)
        else:
            raise Exception('set_slave_storage:bad state', state)
        self.stop_sync(self.sLayout.get_primary_archive(), vol)
        self.reset_vol(sx, vol)
        self.start(sx, vol)

    def kick_all(self, sL):
        '''
        Kick all servers.
        sL :: [Server] - list of servers each of which
                         must be storage or proxy.
        '''
        verify_list_type(sL, Server)
        for s in sL:
            self.run_ctl(s, ["kick"])

    def kick_all_storage(self):
        ''' Kick all storage servers. '''
        self.kick_all(self.sLayout.storageL)

    def is_overflow(self, sx, vol):
        '''
        Check a storage is overflow or not.
        sx :: Server   - storage server.
        vol :: str     - volume name.
        return :: bool - True if the storage overflows.
        '''
        verify_type(sx, Server)
        verify_type(vol, str)
        args = ['get', 'is-overflow', vol]
        ret = self.run_ctl(sx, args)
        return int(ret) != 0

    def verify_not_overflow(self, sx, vol):
        ''' Verify a volume does not overflow. '''
        if self.is_overflow(sx, vol):
            raise Exception('verify_not_overflow', sx, vol)

    def is_wdiff_send_error(self, px, vol, ax):
        '''
        Get wdiff-send-error value.
        px :: Server   - proxy.
        vol :: str     - volume name.
        ax :: Server   - archive.
        return :: bool
        '''
        verify_type(px, Server)
        verify_type(vol, str)
        verify_type(ax, Server)
        args = ['get', 'is-wdiff-send-error', vol, ax.name]
        ret = self.run_ctl(px, args)
        return int(ret) != 0

    def status(self, sL=[], vol=None):
        '''
        print server status.
        sL :: [Server] - server list.
        vol :: str or None - volume name. None means all.
        '''
        verify_list_type(sL, Server)
        if not sL:
            sL = self.sLayout.get_all()
        for s in sL:
            args = ['status']
            if vol:
                args.append(vol)
            print '++++++++++++++++++++', s.name, '++++++++++++++++++++'
            print self.run_ctl(s, args, False)

    def shutdown(self, s, mode="graceful"):
        '''
        Shutdown a server.
        s :: Server
        mode :: str - 'graceful' or 'force'.
        '''
        verify_type(s, Server)
        verify_type(mode, str)
        self._verify_shutdown_mode(mode, 'shutdown')
        self.run_ctl(s, ["shutdown", mode])
        time.sleep(1)  # shutdown is asynchronous command.

    def shutdown_all(self, mode='graceful'):
        '''
        Shutdown all servers.
        '''
        self._verify_shutdown_mode(mode, 'shutdown_all')
        for s in self.sLayout.get_all():
            self.run_ctl(s, ["shutdown", mode])
        time.sleep(1)  # shutdown is asynchronous command.

    def get_alive_server(self):
        '''
        Get alive servers.
        return :: [str] - list of server name.
        '''
        ret = []
        for s in self.sLayout.get_all():
            try:
                self.get_host_type(s, False)
                ret.append(s.name)
            except:
                pass
        return ret

    def init_storage(self, sx, vol, wdevPath):
        '''
        Initialize a volume at storage.
        sx :: Server
        vol :: str
        wdevPath :: str
        '''
        verify_type(sx, Server)
        verify_server_kind(sx, K_STORAGE)
        verify_type(vol, str)
        verify_type(wdevPath, str)
        self.run_ctl(sx, ["init-vol", vol, wdevPath])
        self.start(sx, vol)  # start as slave.

    def clear_vol(self, s, vol):
        '''
        Clear a volume.
        s :: Server
        vol :: str
        '''
        verify_type(s, Server)
        verify_type(vol, str)
        st = self.get_state(s, vol)
        if s.kind == K_STORAGE:
            if st == sClear:
                return
            if st in [sMaster, sSlave]:
                self.stop(s, vol)
                st = self.get_state(s, vol)
            if st == sStopped:
                self.reset_vol(s, vol)
                st = self.get_state(s, vol)
            if st != sSyncReady:
                raise Exception('clear_vol', s, vol, st)
        elif s.kind == K_PROXY:
            if st == pClear:
                return
            if st in pActive:
                self.stop(s, vol)
                st = self.get_state(s, vol)
            if st != pStopped:
                raise Exception('clear_vol', s, vol, st)
        else:
            assert s.kind == K_ARCHIVE
            if st == aClear:
                return
            if st == aArchived:
                self.stop(s, vol)
                st = self.get_state(s, vol)
            if st not in aAcceptForClearVol:
                raise Exception('clear_vol', s, vol, st)
        self.run_ctl(s, ['clear-vol', vol])

    def get_archive_info_list(self, px, vol):
        '''
        Get archive list registered to a proxy.
        return :: [str] - list of archive name.
        '''
        verify_type(px, Server)
        verify_type(vol, str)
        st = self.run_ctl(px, ["archive-info", "list", vol])
        return st.split()

    def is_synchronizing(self, ax, vol):
        '''
        Check whether a volume is synchronizing with an archive server or not.
        ax :: Server - archive server.
        vol :: str
        return :: bool
        '''
        verify_type(ax, Server)
        verify_type(vol, str)
        ret = []
        for px in self.sLayout.proxyL:
            ret.append(ax.name in self.get_archive_info_list(px, vol))
        v = sum(ret)
        if v == len(ret):  # all True
            return True
        elif v == 0:  # all False
            return False
        raise Exception('is_synchronizing: '
                        'some proxies are synchronizing, some are not.')

    def wait_for_stopped(self, s, vol, prevSt=None):
        '''
        Wait for a volue of a server stopped.
        s :: Server
        vol :: str
        prevSt :: None or str - specify if s is storage server.
        '''
        verify_type(s, Server)
        verify_type(vol, str)
        if prevSt:
            verify_type(prevSt, str)
        if s.kind == K_STORAGE:
            if not prevSt:
                raise Exception('wait_for_stopped: '
                                'prevSt not specified', s, vol)
            if prevSt == sSlave:
                tmpStL = sDuringStopForSlave
                goalSt = sSyncReady
            else:
                tmpStL = sDuringStopForMaster
                goalSt = sStopped
        elif s.kind == K_PROXY:
            tmpStL = pDuringStop
            goalSt = pStopped
        else:
            assert s.kind == K_ARCHIVE
            tmpStL = aDuringStop
            goalSt = aStopped
        self._wait_for_state_change(s, vol, tmpStL, [goalSt], TIMEOUT_SEC)

    def stop_async(self, s, vol, mode='graceful'):
        '''
        Stop a volume at a server.
        This is asynchrnous command. See stop().
        s :: Server
        vol :: str
        mode :: str - 'graceful' or 'force' or 'empty'.
            'empty' is valid only if s is proxy.
        return :: str - state before running stop.
        '''
        verify_type(s, Server)
        verify_type(vol, str)
        if mode not in ['graceful', 'empty', 'force']:
            raise Exception('stop:bad mode', mode)
        prevSt = self.get_state(s, vol)
        self.run_ctl(s, ["stop", vol, mode])
        return prevSt

    def stop(self, s, vol, mode='graceful'):
        '''
        Stop a volume at a server and wait for it stopped.
        '''
        prevSt = self.stop_async(s, vol, mode)
        self.wait_for_stopped(s, vol, prevSt)

    def start(self, s, vol):
        '''
        Start a volume at a server and wait for it started.
        s :: Server
        vol :: str
        '''
        verify_type(s, Server)
        verify_type(vol, str)
        if s.kind == K_STORAGE:
            st = self.get_state(s, vol)
            if st == sSyncReady:
                self.run_ctl(s, ['start', vol, 'slave'])
                self._wait_for_state_change(s, vol, [stStartSlave], [sSlave])
            else:
                assert st == sStopped
                self.run_ctl(s, ['start', vol, 'master'])
                self._wait_for_state_change(s, vol, [stStartMaster], [sMaster])
        elif s.kind == K_PROXY:
            self.run_ctl(s, ['start', vol])
            self._wait_for_state_change(s, vol, [ptStart], pActive)
        else:
            assert s.kind == K_ARCHIVE
            self.run_ctl(s, ['start', vol])
            self._wait_for_state_change(s, vol, [atStart], aActive)

    def del_archive_from_proxy(self, px, vol, ax):
        '''
        Delete an archive from a proxy.
        px :: Server - proxy server.
        vol :: str   - voume name.
        ax :: Server - archive server.
        '''
        verify_type(px, Server)
        verify_type(vol, str)
        verify_type(ax, Server)
        st = self.get_state(px, vol)
        if st in pActive:
            self.stop(px, vol)
        aL = self.get_archive_info_list(px, vol)
        if ax.name in aL:
            self.run_ctl(px, ['archive-info', 'delete', vol, ax.name])
        st = self.get_state(px, vol)
        if st == pStopped:
            self.start(px, vol)

    def add_archive_to_proxy(self, px, vol, ax, doStart=True):
        '''
        Add an archive to a proxy.
        px :: Server    - proxy server.
        vol :: str      - volume name.
        ax :: server    - archive server.
        doStart :: bool - False if not to start proxy after adding.
        '''
        verify_type(px, Server)
        verify_type(vol, str)
        verify_type(ax, Server)
        verify_type(doStart, bool)
        st = self.get_state(px, vol)
        if st in pActive:
            self.stop(px, vol)
        aL = self.get_archive_info_list(px, vol)
        if ax.name not in aL:
            self.run_ctl(px, ['archive-info', 'add', vol, ax.name,
                              ax.get_host_port()])
        st = self.get_state(px, vol)
        if st == pStopped and doStart:
            self.start(px, vol)

    def get_server(self, name, L):
        '''
        Get only one element from L having name
        name :: str   - server name.
        L :: [Server] - server list.
        return :: Server
        '''
        verify_type(name, str)
        verify_list_type(L, Server)
        ret = []
        for x in L:
            if x.name == name:
                ret.append(x)
        if len(ret) != 1:
            raise Exception('get_server:not one', ret, name, L)
        return ret[0]

    def copy_archive_info(self, pSrc, vol, pDst):
        '''
        Copy archive info from a proxy to another proxy.
        pSrc :: Server - srouce proxy.
        vol :: str
        pDst :: Server - destination proxy. It must be stopped.
        '''
        verify_type(pSrc, Server)
        verify_type(vol, str)
        verify_type(pDst, Server)
        for axName in self.get_archive_info_list(pSrc, vol):
            ax = self.get_server(axName, self.sLayout.archiveL)
            self.add_archive_to_proxy(pDst, vol, ax, doStart=False)
        self.start(pDst, vol)

    def stop_sync(self, ax, vol):
        '''
        Stop synchronization of a volume with an archive.
        ax :: Server - archive server.
        vol :: str   - volume name.
        '''
        verify_type(ax, Server)
        verify_type(vol, str)
        for px in self.sLayout.proxyL:
            self.del_archive_from_proxy(px, vol, ax)
        self.kick_all_storage()

    def start_sync(self, ax, vol):
        '''
        Start synchronization of a volume with an archive.
        ax :: Server - archive server.
        vol :: str   - volume name.
        '''
        verify_type(ax, Server)
        verify_type(vol, str)
        for px in self.sLayout.proxyL:
            self.add_archive_to_proxy(px, vol, ax)
        self.kick_all_storage()

    def list_restorable(self, ax, vol, opt=''):
        '''
        Get restorable gid list.
        ax :: Server - archive server.
        vol :: str   - volume name.
        opt :: str   - you can specify 'all'.
        '''
        verify_type(ax, Server)
        verify_type(vol, str)
        verify_type(opt, str)
        optL = []
        if opt:
            if opt == 'all':
                optL.append(opt)
            else:
                raise Exception('list_restorable:bad opt', opt)
        return self._get_gid_list(ax, vol, 'restorable', optL)

    def list_restored(self, ax, vol):
        '''
        Get restored gid list.
        ax :: Server - archive server.
        vol :: str   - volume name.
        '''
        return self._get_gid_list(ax, vol, 'restored')

    def wait_for_restorable(self, ax, vol, gid, timeoutS=TIMEOUT_SEC):
        '''
        Wait for a snapshot specified of a gid to be restorable.
        ax :: Server    - archive server.
        vol :: str      - volume name.
        gid :: int      - generation id.
        timeoutS :: int - timeout [sec].
        '''
        self._wait_for_gid(ax, vol, gid, 'restorable', timeoutS)

    def wait_for_restored(self, ax, vol, gid, timeoutS=TIMEOUT_SEC):
        '''
        Wait for a snapshot specified of a gid to be restored.
        ax :: Server    - archive server.
        vol :: str      - volume name.
        gid :: int      - generation id.
        timeoutS :: int - timeout [sec].
        '''
        verify_type(gid, int)
        self._wait_for_no_action(ax, vol, 'Restore', timeoutS)
        gids = self.list_restored(ax, vol)
        if gid in gids:
            return
        raise Exception('wait_for_restored:failed', ax.name, vol, gid, gids)

    def verify_not_restorable(self, ax, vol, gid, waitS, msg):
        '''
        Verify a snapshot does not become restorable
        at an archive server in a period.
        ax :: Server    - archive server.
        vol :: str      - volume name.
        gid :: int      - generation id.
        waitS :: int    - wait period [sec].
        msg :: str      - message for error.
        '''
        verify_type(msg, str)
        e = False
        try:
            self.wait_for_restorable(ax, vol, gid, waitS)
            e = True
        except:
            # expect to fail due to timeout.
            pass
        if e:
            raise Exception(msg, 'gid must not be restorable', gid)

    def replicate_sync(self, aSrc, vol, aDst):
        '''
        Copy current (aSrc, vol) to aDst.
        aSrc :: Server - source archive (as a client).
        vol :: str     - volume name.
        aDst :: Server - destination archive (as a server).
        '''
        gidL = self.list_restorable(aSrc, vol)
        gid = gidL[-1]
        self.run_ctl(aSrc, ['replicate', vol, "gid", str(gid),
                            aDst.get_host_port()])
        self._wait_for_replicated(aDst, vol, gid)

    def synchronize(self, aSrc, vol, aDst):
        '''
        Synchronize aDst with (aSrc, vol).
        To reduce proxies stopped period, replicate nosync before calling this.
        aSrc :: Server - source archive (as a client).
        vol :: str     - volume name.
        aDst :: Server - destination archive (as a server).
        '''
        verify_type(aSrc, Server)
        verify_type(vol, str)
        verify_type(aDst, Server)

        for px in self.sLayout.proxyL:
            st = self.get_state(px, vol)
            if st in pActive:
                self.run_ctl(px, ["stop", vol, 'empty'])

        for px in self.sLayout.proxyL:
            self.wait_for_stopped(px, vol)
            aL = self.get_archive_info_list(px, vol)
            if aDst.name not in aL:
                self.run_ctl(px, ["archive-info", "add", vol,
                                  aDst.name, aDst.get_host_port()])

        self.replicate_sync(aSrc, vol, aDst)

        for px in self.sLayout.proxyL:
            self.start(px, vol)
        self.kick_all_storage()

    def full_backup(self, sx, vol, timeoutS=TIMEOUT_SEC):
        '''
        Run full backup a volume of a storage server.
        Log transfer to the primary archive server will start automatically.
        This function will return when a clean snapshot is
        sx :: Server    - storage server.
        vol :: str      - volume name.
        timeoutS :: int - timeout [sec].
                          Counter will start after dirty full backup done.
        return :: int   - generation id of a clean snapshot.
        '''
        verify_type(sx, Server)
        verify_type(vol, str)
        verify_type(timeoutS, int)

        a0 = self.sLayout.get_primary_archive()
        self._prepare_backup(sx, vol)
        self.run_ctl(sx, ["full-bkp", vol])
        self._wait_for_state_change(sx, vol, sDuringFullSync,
                                    [sMaster], timeoutS)
        st = self.get_state(a0, vol)
        if st not in aActive:
            raise Exception('full_backup: sync failed', sx, a0, vol, st)

        t0 = time.time()
        while time.time() < t0 + timeoutS:
            gids = self.list_restorable(a0, vol)
            if gids:
                return gids[-1]
            time.sleep(0.3)
        raise Exception('full_backup:timeout', sx, vol, gids)

    def hash_backup(self, sx, vol, timeoutS=TIMEOUT_SEC):
        '''
        Run hash backup a volume of a storage server.
        Log transfer to the primary archive server will start automatically.
        This function will return a gid of a clean snapshot that is
        restorable at the primary archive server.

        sx :: Server    - storage server.
        vol :: str      - volume name.
        timeoutS :: int - timeout [sec].
                          Counter will start after dirty hash backup done.
        return :: int   - generation id of a clean snapshot.
        '''
        verify_type(sx, Server)
        verify_type(vol, str)
        verify_type(timeoutS, int)

        a0 = self.sLayout.get_primary_archive()
        self._prepare_backup(sx, vol)
        prev_gids = self.list_restorable(a0, vol)
        if prev_gids:
            max_gid = prev_gids[-1]
        else:
            max_gid = -1
        self.run_ctl(sx, ["hash-bkp", vol])
        self._wait_for_state_change(sx, vol, sDuringHashSync,
                                    [sMaster], timeoutS)
        st = self.get_state(a0, vol)
        if st not in aActive:
            raise Exception('hash_backup: sync failed', sx, a0, vol, st)

        t0 = time.time()
        while time.time() < t0 + timeoutS:
            gids = self.list_restorable(a0, vol)
            if gids and gids[-1] > max_gid:
                return gids[-1]
            time.sleep(0.3)
        raise Exception('hash_backup:timeout', sx, vol, max_gid, gids)

    def restore(self, ax, vol, gid):
        '''
        Restore a volume.
        ax :: Server  - archive server.
        vol :: str    - volume name.
        gid :: int    - generation id.
        '''
        verify_type(ax, Server)
        verify_type(vol, str)
        verify_type(gid, int)

        self.run_ctl(ax, ['restore', vol, str(gid)])
        self.wait_for_restored(ax, vol, gid)
        runCommand = self.get_remote_run_command(ax)
        path = self.get_restored_path(ax, vol, gid)
        wait_for_lv_ready(path, runCommand)

    def get_restored_path(self, ax, vol, gid):
        '''
        ax :: Server  - archive server.
        vol :: str    - volume name.
        gid :: int    - generation id.
        return :: str - restored path.
        '''
        verify_type(ax, Server)
        verify_type(vol, str)
        verify_type(gid, int)
        return '/dev/' + ax.vg + '/r_' + vol + '_' + str(gid)

    def del_restored(self, ax, vol, gid):
        '''
        Delete a restored volume.
        ax :: Server  - archive server.
        vol :: str    - volume name.
        gid :: int    - generation id.
        '''
        verify_type(ax, Server)
        verify_type(vol, str)
        verify_type(gid, int)

        runCommand = self.get_remote_run_command(ax)
        path = self._get_lv_path(ax, vol)
        wait_for_lv_ready(path, runCommand)
        retryTimes = 3
        for i in xrange(retryTimes):
            try:
                self.run_ctl(ax, ['del-restored', vol, str(gid)])
                break
            except Exception, e:
                print 'del-restored retry', i, e
                time.sleep(1)
        else:
            raise Exception('del-restored: exceeds max retry times')
        self._wait_for_not_restored(ax, vol, gid)

    def snapshot_async(self, sx, vol):
        '''
        Take a snaphsot.
        sx :: Server  - storage server.
        vol :: str    - volume name.
        return :: int - gid of the taken snapshot.
        '''
        verify_type(sx, Server)
        verify_type(vol, str)
        gid = self.run_ctl(sx, ['snapshot', vol])
        return int(gid)

    def snapshot_sync(self, sx, vol, axL):
        '''
        Take a snaphsot and wait for it to be restorable in archive servers.
        sx :: Server    - storage server.
        vol :: str      - volume name.
        axL :: [Server] - archive server list.
        return :: int   - gid of the taken snapshot.
        '''
        verify_list_type(axL, Server)
        gid = self.snapshot_async(sx, vol)
        for ax in axL:
            self.wait_for_restorable(ax, vol, gid)
        return gid

    def apply_diff(self, ax, vol, gid):
        '''
        Apply diffs older than a gid the base lv.
        ax :: Server - archive server
        vol :: str   - volume name.
        gid :: int   - generation id.
        '''
        verify_type(ax, Server)
        verify_type(vol, str)
        verify_type(gid, int)
        self.run_ctl(ax, ["apply", vol, str(gid)])
        self._wait_for_applied(ax, vol, gid)

    def merge_diff(self, ax, vol, gidB, gidE):
        '''
        Merge diffs in gid ranges.
        ax :: Server - archive server.
        vol :: str   - volume name.
        gidB :: int  - begin gid.
        gidE :: int  - end gid.
        '''
        verify_type(ax, Server)
        verify_type(vol, str)
        verify_gid_range(gidB, gidE, 'merge_diff')
        self.run_ctl(ax, ["merge", vol, str(gidB), "gid", str(gidE)])
        self._wait_for_merged(ax, vol, gidB, gidE)

    def replicate(self, aSrc, vol, aDst, synchronizing):
        '''
        Replicate archive data by copying a volume from one archive to another.
        aSrc :: Server        - source archive server (as client).
        vol :: str            - volume name.
        aDst :: Server        - destination archive server (as server).
        synchronizing :: bool - True if you want to make aDst synchronizing.
        '''
        verify_type(aSrc, Server)
        verify_type(vol, str)
        verify_type(aDst, Server)
        verify_type(synchronizing, bool)

        st = self.get_state(aDst, vol)
        if st == aClear:
            self.run_ctl(aDst, ["init-vol", vol])

        self.replicate_sync(aSrc, vol, aDst)
        if synchronizing:
            self.synchronize(aSrc, vol, aDst)

    def get_latest_clean_snapshot(self, ax, vol):
        '''
        ax :: Server - archive server.
        vol :: str   - volume name.
        '''
        verify_type(ax, Server)
        verify_type(vol, str)
        xL = self.list_restorable(ax, vol)
        if xL:
            return xL[-1]
        else:
            raise Exception('get_latest_clean_snapshot:not found')

    def resize_archive(self, ax, vol, sizeMb, doZeroClear):
        '''
        Resize archive volume.
        ax :: Server        - archive server.
        vol :: str          - volume name.
        sizeMb :: int       - new size [MiB].
        doZeroClear :: bool - True if zero-clear extended area.
        '''
        verify_type(ax, Server)
        verify_type(vol, str)
        verify_type(sizeMb, int)
        verify_type(doZeroClear, bool)
        st = self.get_state(ax, vol)
        if st == aClear:
            return
        elif st in aAcceptForResize:
            args = ['resize', vol, str(sizeMb) + 'm']
            if doZeroClear:
                args += ['zeroclear']
            self.run_ctl(ax, args)
            self._wait_for_resize(ax, vol, sizeMb)
        else:
            raise Exception('resize_archive:bad state', ax, vol, sizeMb, st)

    def resize_storage(self, sx, vol, sizeMb):
        '''
        Resize storage volume.
        You must resize ddev before calling this.
        sx :: Server  - storage server.
        vol :: str    - voume name.
        sizeMb :: int - new size [MiB].
        '''
        verify_type(sx, Server)
        verify_type(vol, str)
        verify_type(sizeMb, int)
        st = self.get_state(sx, vol)
        if st == sClear:
            return
        else:
            self.run_ctl(sx, ['resize', vol, str(sizeMb) + 'm'])

    def resize(self, vol, sizeMb, doZeroClear):
        '''
        Resize a volume.
        This will affect all storage servers and archive servers.
        vol :: str          - volume name.
        sizeMb :: int       - new size [MiB].
        doZeroClear :: bool - True if you want to zero-clear the extended area.
        '''
        verify_type(vol, str)
        verify_type(sizeMb, int)
        verify_type(doZeroClear, bool)
        for ax in self.sLayout.archiveL:
            self.resize_archive(ax, vol, sizeMb, doZeroClear)
        for sx in self.sLayout.storageL:
            self.resize_storage(sx, vol, sizeMb)

    '''
    Private member functions.

    '''
    def _wait_for_state_cond(self, s, vol, pred, msg, timeoutS=10):
        t0 = time.time()
        while time.time() < t0 + timeoutS:
            st = self.get_state(s, vol)
            if pred(st):
                return
            time.sleep(0.3)
        raise Exception("wait_for_state_cond", s, vol, msg)

    def _wait_for_state(self, s, vol, stateL, timeoutS=10):
        def pred(st):
            return st in stateL
        self._wait_for_state_cond(
            s, vol, pred, 'stateL:' + str(stateL), timeoutS)

    def _wait_for_not_state(self, s, vol, stateL, timeoutS=10):
        def pred(st):
            return st not in stateL
        self._wait_for_state_cond(
            s, vol, pred, 'not stateL:' + str(stateL), timeoutS)

    def _wait_for_state_change(self, s, vol, tmpStateL,
                               goalStateL, timeoutS=10):
        self._wait_for_not_state(s, vol, tmpStateL, timeoutS)
        st = self.get_state(s, vol)
        if st not in goalStateL:
            raise Exception('wait_for_state_change:bad goal',
                            s, vol, tmpStateL, goalStateL, st)

    def _get_gid_list(self, ax, vol, cmd, optL=[]):
        '''
        Get gid list
        ax :: Server    - archive server.
        vol :: str      - volume name.
        cmd :: str      - 'restorable' or 'restored'.
        optL :: [str]   - options.
        return :: [int] - gid list.
        '''
        verify_type(ax, Server)
        verify_type(vol, str)
        if not cmd in ['restorable', 'restored']:
            raise Exception('get_list_gid : bad cmd', cmd)
        verify_list_type(optL, str)
        ret = self.run_ctl(ax, ['get', cmd, vol] + optL)
        return map(int, ret.split())

    def _wait_for_gid(self, ax, vol, gid, cmd, timeoutS=TIMEOUT_SEC):
        '''
        Wait for an gid is available.
        ax :: Server    - archive server.
        vol :: str      - volume name.
        cmd :: str      - 'restorable' or 'restored'.
        timeoutS :: int - timeout [sec].
        '''
        verify_type(ax, Server)
        verify_type(vol, str)
        verify_type(gid, int)
        verify_type(timeoutS, int)
        t0 = time.time()
        while time.time() < t0 + timeoutS:
            gids = self._get_gid_list(ax, vol, cmd)
            if gid in gids:
                return
            time.sleep(0.3)
        raise Exception('wait_for_gid: timeout', ax.name, vol, gid, cmd, gids)

    def _wait_for_not_gid(self, ax, vol, gid, cmd, timeoutS=TIMEOUT_SEC):
        '''
        Wait for a gid is not available.
        ax :: Server    - archive server.
        vol :: str      - volume name.
        cmd :: str      - 'restorable' or 'restored'.
        timeoutS :: int - timeout [sec].
        '''
        verify_type(ax, Server)
        verify_type(vol, str)
        verify_type(gid, int)
        verify_type(timeoutS, int)
        t0 = time.time()
        while time.time() < t0 + timeoutS:
            gids = self._get_gid_list(ax, vol, cmd)
            if gid not in gids:
                return
            time.sleep(0.3)
        raise Exception('wait_for_gid: timeout', ax.name, vol, gid, cmd, gids)

    def _wait_for_not_restored(self, ax, vol, gid, timeoutS=TIMEOUT_SEC):
        '''
        Wait for a restored snapshot specified of a gid to be removed
        ax :: Server    - archive server.
        vol :: str      - volume name.
        gid :: int      - generation id.
        timeoutS :: int - timeout [sec].
        '''
        self._wait_for_not_gid(ax, vol, gid, 'restored', timeoutS)

    def _wait_for_applied(self, ax, vol, gid, timeoutS=TIMEOUT_SEC):
        '''
        Wait for diffs older than a gid to be applied.
        ax :: Server    - archive server.
        vol :: str      - volume name.
        gid :: int      - generation id.
        timeoutS :: int - timeout [sec].
        '''
        verify_type(gid, int)
        self._wait_for_no_action(ax, vol, aaApply, timeoutS)
        gidL = self.list_restorable(ax, vol)
        if gidL and gid <= gidL[0]:
            return
        raise Exception('wait_for_applied:failed', ax.name, vol, gid, gidL)

    def _wait_for_merged(self, ax, vol, gidB, gidE, timeoutS=TIMEOUT_SEC):
        '''
        Wait for diffs in a gid range to be merged.
        ax :: Server    - archive server.
        vol :: str      - volume name.
        gidB :: int     - begin generation id.
        gidE :: int     - end generation id.
        timeoutS :: int - timeout [sec].
        '''
        verify_gid_range(gidB, gidE, 'wait_for_merged')
        self._wait_for_no_action(ax, vol, aaMerge, timeoutS)
        gidL = self.list_restorable(ax, vol, 'all')
        pos = gidL.index(gidB)
        if gidL[pos + 1] == gidE:
            return
        raise Exception("wait_for_merged:failed",
                        ax.name, vol, gidB, gidE, pos, gidL)

    def _wait_for_replicated(self, ax, vol, gid, timeoutS=TIMEOUT_SEC):
        '''
        Wait for a snapshot is restorable at an archive server.
        ax :: Server    - archive server as a replication server (not client).
        vol :: str      - volume name.
        gid :: int      - generation id.
        timeoutS :: int - timeout [sec].
        '''
        verify_type(gid, int)
        self._wait_for_not_state(ax, vol, aDuringReplicate, timeoutS)
        gidL = self.list_restorable(ax, vol, 'all')
        if gidL and gid <= gidL[-1]:
            return
        raise Exception("wait_for_replicated:replicate failed",
                        ax.name, vol, gid, gidL)

    def _prepare_backup(self, sx, vol):
        '''
        Prepare backup.
        sx :: Server - storage server.
        vol :: str   - volume name.
        '''
        verify_type(sx, Server)
        verify_type(vol, str)

        st = self.get_state(sx, vol)
        if st == sSlave:
            self.stop(sx, vol)
        elif st == sMaster:
            self.stop(sx, vol)
            self.reset_vol(sx, vol)

        for s in self.sLayout.storageL:
            if s == sx:
                continue
            st = self.get_state(s, vol)
            if st not in [sSlave, sClear]:
                raise Exception("prepare_backup:bad state", s.name, vol, st)

        for ax in self.sLayout.archiveL:
            if self.is_synchronizing(ax, vol):
                self.stop_sync(ax, vol)

        # Initialize the volume at the primary archive if necessary.
        a0 = self.sLayout.get_primary_archive()
        st = self.get_state(a0, vol)
        if st == aClear:
            self.run_ctl(a0, ["init-vol", vol])

        self.start_sync(a0, vol)

    def _get_lv_path(self, ax, vol):
        '''
        ax :: Server  - archive server.
        vol :: str    - volume name.
        return :: str - lv path.
        '''
        verify_type(ax, Server)
        verify_type(vol, str)
        return '/dev/' + ax.vg + '/i_' + vol

    def _wait_for_no_action(self, s, vol, action, timeoutS=TIMEOUT_SEC):
        '''
        s :: Server     - server.
        vol :: str      - volume name.
        action :: str   - action name.
        timeoutS :: int - timeout [sec].
        '''
        verify_type(s, Server)
        verify_type(vol, str)
        verify_type(action, str)
        verify_type(timeoutS, int)

        t0 = time.time()
        while time.time() < t0 + timeoutS:
            num = int(self.run_ctl(s, ['get', 'num-action', vol, action]))
            if num == 0:
                return
            time.sleep(0.3)
        raise Exception("wait_for_no_action", s, vol, action)

    def _wait_for_resize(self, ax, vol, sizeMb):
        '''
        Wait for resize done.
        ax :: Server  - archive server.
        vol :: str    - volume name.
        sizeMb :: int - new size [MiB].
        '''
        verify_type(ax, Server)
        verify_type(vol, str)
        verify_type(sizeMb, int)
        self._wait_for_no_action(ax, vol, aaResize)
        runCommand = self.get_remote_run_command(ax)
        curSizeMb = get_lv_size_mb(self._get_lv_path(ax, vol), runCommand)
        if curSizeMb != sizeMb:
            raise Exception('wait_for_resize:failed',
                            ax, vol, sizeMb, curSizeMb)

    def _verify_shutdown_mode(self, mode, msg):
        if mode not in ['graceful', 'force']:
            raise Exception(msg, 'bad mode', mode)