#!/usr/bin/env python
import os, socket
from util import *
from contextlib import closing

def send_cmd_to_repeater(ip, cmd):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    with closing(s):
        s.connect(('localhost', ip))
        s.send(cmd)
        s.close()

class Repeater:
    '''
    packet repeater
    '''
    def __init__(self, server, serverIp, recvIp, cmdIp, rateMbps=0, isDebug=False):
        args = [
            os.getcwd() + '/binsrc/packet-repeater',
            server, str(serverIp), str(recvIp), str(cmdIp)
        ]
        if rateMbps:
            args += ['-r', str(rateMbps)]
        if isDebug:
            args += ['-v']
        run_daemon(args)
        self.cmdIp = cmdIp

    def stop(self):
        send_cmd_to_repeater(self.cmdIp, 'stop')

    def start(self):
        send_cmd_to_repeater(self.cmdIp, 'start')

    def quit(self):
        send_cmd_to_repeater(self.cmdIp, 'quit')