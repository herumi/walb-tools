#!/usr/bin/env python
import unittest, yaml
import sys
from walblib import *
from walb_worker import *

VOL = 'vol0'

def toDatetime(ts):
    return str_to_datetime(ts, DatetimeFormatPretty)

class ControllerMock:
    def __init__(self, path, layout, isDebug=False):
        '''
            mock of walb.Controller
        '''
        self.path = path
        self.layout = layout
        self.isDebug = isDebug
    def get_restorable(self, ax, vol, opt=''):
        '''
        Get restorable gid list.
        ax :: ServerParams - archive server.
        vol :: str       - volume name.
        opt :: str       - you can specify 'all'.
        return :: [GidInfo] - gid info list.
        '''
        return []
    def get_total_diff_size(self, ax, vol, gid0=0, gid1=UINT64_MAX):
        '''
        Get total wdiff size.
        ax :: ServerParams - archive server.
        vol :: str       - volume name.
        gid0 :: int      - gid range begin.
        gid1 :: int      - gid range end.
        return :: int    - total size in the gid range [byte].
        '''
        return 0
    def get_applicable_diff_list(self, ax, vol, gid=UINT64_MAX):
        '''
        Get wdiff to list to apply.
        ax :: ServerParams - archive server.
        vol :: str       - volume name.
        gid :: u64       - target gid.
        return :: [Diff] - wdiff information list managed by the archive server.
        '''
        return []
    def get_state(self, s, vol):
        '''
        Get state of a volume.
        s :: ServerParams
        vol :: str    - volume name.
        return :: str - state.
        '''
        return ""
    def get_vol_list(self, s):
        '''
        Get volume list.
        s :: ServerParams
        return :: [str] - volume name list.
        '''
        return [VOL]
    def get_base(self, ax, vol):
        '''
        Get base meta state of a volume in an archive server.
        ax :: ServerParams    - archive server.
        vol :: str          - volume name.
        return :: MetaState - meta state.
        '''
        return ""
    def get_num_diff(self, ax, vol, gid0=0, gid1=UINT64_MAX):
        '''
        Get number of wdiff files for a volume.
        ax :: ServerParams - archive server.
        vol :: str       - volume name.
        gid0 :: int      - gid range begin.
        gid1 :: int      - gid range end.
        return :: int    - number of wdiffs in the gid range.
        '''
        return 0

class TestParsePERIOD(unittest.TestCase):
    def test(self):
        d = {
            '123':123,
            '10m':10 * 60,
            '100d':100 * 86400,
        }
        for (s, expect) in d.items():
            v = parsePERIOD(s)
            self.assertEqual(v, datetime.timedelta(seconds=expect))

class TestParseSIZE_UNIT(unittest.TestCase):
    def test(self):
        d = {
            '123':123,
            '10K':10 * 1024,
            '7M':7 * 1024 * 1024,
            '8G':8 * 1024 * 1024 * 1024,
        }
        for (s, expect) in d.items():
            v = parseSIZE_UNIT(s)
            self.assertEqual(v, expect)

class TestParseCOMPRESS_OPT(unittest.TestCase):
    def test(self):
        d = {
            "none":('none', 0, 0),
            "snappy:3":('snappy', 3, 0),
            "gzip:9:4":('gzip', 9, 4),
            "lzma:0:123":('lzma', 0, 123),
        }
        for (s, expect) in d.items():
            v = parseCOMPRESS_OPT(s)
            self.assertEqual(v, expect)

configStr = """
general:
  addr: 192.168.0.1
  port: 10000
  walbc_path: binsrc/walbc
  max_concurrent_tasks: 10
apply:
  keep_period: 14d
merge:
  interval: 10
  max_nr: 10
  max_size: 1M
  threshold_nr: 5
repl_servers:
  repl0:
    addr: 192.168.0.2
    port: 10001
    interval: 3d
    compress: snappy:3:4
    max_merge_size: 5K
    bulk_size: 40
  repl1:
    addr: 192.168.0.3
    port: 10002
    interval: 2h
    compress: gzip
    max_merge_size: 2M
    bulk_size: 400
"""

class TestLoadConfigParam(unittest.TestCase):
    def test(self):
        d = yaml.load(configStr)
        cfg = Config()
        cfg.set(d)
        general = cfg.general
        self.assertEqual(general.addr, '192.168.0.1')
        self.assertEqual(general.port, 10000)
        self.assertEqual(general.max_concurrent_tasks, 10)
        apply_ = cfg.apply_
        self.assertEqual(apply_.keep_period, datetime.timedelta(days=14))
        merge = cfg.merge
        self.assertEqual(merge.interval, datetime.timedelta(seconds=10))
        self.assertEqual(merge.max_nr, 10)
        self.assertEqual(merge.max_size, 1024 * 1024)
        self.assertEqual(merge.threshold_nr, 5)
        repl_servers = cfg.repl_servers
        r = repl_servers['repl0']
        self.assertEqual(r.addr, '192.168.0.2')
        self.assertEqual(r.port, 10001)
        self.assertEqual(r.interval, datetime.timedelta(days=3))
        self.assertEqual(r.compress, ('snappy', 3, 4))
        self.assertEqual(r.max_merge_size, 5 * 1024)
        self.assertEqual(r.bulk_size, 40)

        r = repl_servers['repl1']
        self.assertEqual(r.addr, '192.168.0.3')
        self.assertEqual(r.port, 10002)
        self.assertEqual(r.interval, datetime.timedelta(hours=2))
        self.assertEqual(r.compress, ('gzip', 0, 0))
        self.assertEqual(r.max_merge_size, 2 * 1024 * 1024)
        self.assertEqual(r.bulk_size, 400)

class TestSnapshot(unittest.TestCase):
    def test(self):
        self.assertEqual(Snapshot(2, 3), Snapshot(2, 3))
        self.assertTrue(Snapshot(2, 3) != Snapshot(3, 3))
        self.assertTrue(Snapshot(2, 3) != Snapshot(2, 4))

        tbl = [
            ("|2,3|", Snapshot(2, 3)),
            ("|5|", Snapshot(5, 5)),
            ("|7,%d|" % UINT64_MAX, Snapshot(7)),
        ]
        for t in tbl:
            s = create_snapshot_from_str(t[0])
            self.assertEqual(s, t[1])
            self.assertEqual(str(s), t[0])

class TestMetaState(unittest.TestCase):
    def test(self):
        self.assertEqual(MetaState(Snapshot(2, 3)), MetaState(Snapshot(2, 3)))
        self.assertEqual(MetaState(Snapshot(2, 3), Snapshot(3, 4)), MetaState(Snapshot(2, 3), Snapshot(3, 4)))
        self.assertTrue(MetaState(Snapshot(2, 3), Snapshot(4, 5)) != MetaState(Snapshot(2, 3)))
        self.assertTrue(MetaState(Snapshot(2, 3)) != MetaState(Snapshot(2, 3), Snapshot(4, 5)))
        self.assertTrue(MetaState(Snapshot(2, 3)) != MetaState(Snapshot(2, 4)))

class TestDiff(unittest.TestCase):
    def test(self):
        tbl = [
            "|24|-->|25,26| -- 2015-11-16T07:32:08 1",
            "|24,28|-->|30,35| M- 2015-11-16T07:32:09 123",
            "|1,5|-->|25| -C 2015-11-16T07:32:10 4567",
            "|24|-->|25| MC 2015-11-16T07:32:11 89101",
        ]
        for s in tbl:
            d = create_diff_from_str(s)
            ss = str(d)
            self.assertEqual(s, ss)

class TestGetLatestGidInfoBefore(unittest.TestCase):
    def test(self):
        tbl = [
            '1 2015-11-16T07:32:04',
            '2 2015-11-16T07:32:08',
            '3 2015-11-16T07:32:11',
        ]
        expectedTbl = [
            ('2015-11-16T07:32:00', None),
            ('2015-11-16T07:32:04', None),
            ('2015-11-16T07:32:05', None),
            ('2015-11-16T07:32:08', GidInfo('2 2015-11-16T07:32:08')),
            ('2015-11-16T07:32:11', GidInfo('3 2015-11-16T07:32:11')),
            ('2015-11-16T07:32:12', GidInfo('3 2015-11-16T07:32:11')),
        ]
        infoL = map(GidInfo, tbl)
        for (ts, expected) in expectedTbl:
            t = toDatetime(ts)
            r = getLatestGidInfoBefore(t, infoL)
            self.assertEqual(r, expected)

class TestGetMergeGidRange(unittest.TestCase):
    def test(self):
        tbl = [
            ([
            '|0|-->|1| -- 2015-12-08T07:10:15 4120',
            '|1|-->|2| -- 2015-12-08T07:10:18 8728',
            '|2|-->|5| -- 2015-12-08T07:10:25 8728',
            '|5|-->|6| -- 2015-12-08T07:10:26 8728',
            '|6|-->|7| M- 2015-12-08T07:10:28 8728',
            ], (5, 7)),
        ]
        for t in tbl:
            diffL = map(create_diff_from_str, t[0])
            r = getMergeGidRange(diffL)
            self.assertEqual(r, t[1])


class TestWoker(unittest.TestCase):
    def test(self):
        d = yaml.load(configStr)
        cfg = Config()
        cfg.set(d)
        w = Worker(cfg, ControllerMock)
#        task = w.selectTask()
        volL = w.walbc.get_vol_list(w.a0)

        def test_selectApplyTask1():
            keep = w.walbc.get_base
            i = 0
            tbl = [
                (MetaState(Snapshot()), None),
                (MetaState(Snapshot(2, 2)), None),
                (MetaState(Snapshot(2, 4)), None),
                (MetaState(Snapshot(2, 3), Snapshot(4, 5)), Task("apply", VOL, (w.a0, 2))),
            ]
            def get_base(a0, vol):
                return tbl[i][0]
            w.walbc.get_base = get_base
            for t in tbl:
                self.assertEqual(w._selectApplyTask1(volL), tbl[i][1])
                i = i + 1
            w.walbc.get_base = keep

        test_selectApplyTask1()

        def test_selectApplyTask2():
            keep_get_restorable = w.walbc.get_restorable
            keep_get_total_diff_size = w.walbc.get_total_diff_size
            keep_keep_period = w.cfg.apply_.keep_period

            def get_restorable(a0, vol, opt):
                tbl = {
                    'vol0': map(GidInfo, [
                        '24 2015-11-16T07:32:00',
                        '25 2015-11-16T07:32:02',
                        '26 2015-11-16T07:32:04',
                        '27 2015-11-16T07:32:06'
                    ]),
                    'vol1': map(GidInfo, [
                        '28 2015-11-16T07:32:01',
                        '29 2015-11-16T07:32:02',
                        '30 2015-11-16T07:32:05',
                        '31 2015-11-16T07:32:07',
                    ])
                }
                return tbl[vol]
            def get_total_diff_size(a0, vol, gid1):
                tbl = {
                    'vol0': {
                        24:105248,
                        25:96520,
                        26:87792,
                        27:79064,
                    },
                    'vol1': {
                        28:70336,
                        29:91608,
                        30:52880,
                        31:134152,
                    }
                }
                return tbl[vol][gid1]

            w.walbc.get_restorable = get_restorable
            w.walbc.get_total_diff_size = get_total_diff_size

            tbl = [
                ('2015-11-16T07:32:00', '0', None),
                ('2015-11-16T07:32:02', '0', Task("apply", 'vol0', (w.a0, 25))),
                ('2015-11-16T07:32:03', '0', Task("apply", 'vol0', (w.a0, 25))),
                ('2015-11-16T07:32:04', '0', Task("apply", 'vol1', (w.a0, 29))),
                ('2015-11-16T07:32:10', '0', Task("apply", 'vol1', (w.a0, 31))),
                ('2015-11-16T07:32:10', '8', Task("apply", 'vol0', (w.a0, 25))),
            ]
            for t in tbl:
                curTime = toDatetime(t[0])
                period = parsePERIOD(t[1])
                w.cfg.apply_.keep_period = period
                r = w._selectApplyTask2(['vol0', 'vol1'], curTime)
                self.assertEqual(r, t[2])

            w.walbc.get_restorable = keep_get_restorable
            w.walbc.get_total_diff_size = keep_get_total_diff_size
            w.cfg.apply_.keep_period = keep_keep_period

        test_selectApplyTask2()

        def test_getNumDiffList():
            keep_get_num_diff = w.walbc.get_num_diff
            def get_num_diff(ax, vol):
                tbl = {
                    'vol0': 3,
                    'vol1': 5,
                    'vol2': 8,
                }
                return tbl[vol]
            tbl = [
                (['vol0'], [3]),
                (['vol0', 'vol1'], [3, 5]),
                (['vol0', 'vol2'], [3, 8]),
            ]
            w.walbc.get_num_diff = get_num_diff
            for t in tbl:
                self.assertEqual(w.getNumDiffList(t[0]), t[1])

            w.walbc.get_num_diff = keep_get_num_diff

        test_getNumDiffList()


if __name__ == '__main__':
    unittest.main()