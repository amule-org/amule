#!/usr/bin/env python3
"""Regression checks for capture integrity, failure isolation and derived metrics."""
import csv
import io
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from summarize import summarize

ROOT = Path(__file__).resolve().parents[2]
COLUMNS = 'kind,elapsed_ns,duration_ns,bytes,success,part,mmap\n'
HEADER = 'amule-upload-io,2\n' + COLUMNS
VALID = 'read,10,2,42,1,0,0\nloop,10,3,0,0,0,0\nend,12,0,0,0,0,0\n'


class SummaryTest(unittest.TestCase):
    def test_percentiles_rates_and_failed_time(self):
        rows = ''.join(f'read,10000000000,{i * 1000000},1024,1,0,0\n' for i in range(1, 101))
        rows += 'read,10000000000,50000000,0,0,1,0\n'
        rows += 'loop,10000000000,6000000000,0,0,0,0\nend,20000000000,0,0,0,0,0\n'
        result = summarize(io.StringIO(HEADER + rows))
        self.assertEqual([result[k] for k in ('p50_ms', 'p90_ms', 'p95_ms', 'p99_ms')],
                         [50, 90, 95, 99])
        self.assertEqual(result['reads_per_second'], 5)
        self.assertEqual(result['bytes_per_second'], 5120)
        self.assertEqual(result['failed_or_deferred_reads'], 1)
        self.assertEqual(result['read_share_active_percent'], 85)

    def test_mmap_and_empty_capture(self):
        result = summarize(io.StringIO(HEADER + VALID.replace('42,1,0,0', '42,1,0,1')))
        self.assertTrue(result['mmap_enabled'])
        mixed = VALID.replace('loop,', 'read,10,0,0,0,0,1\nloop,')
        with self.assertRaisesRegex(ValueError, 'keep mmap mode fixed'):
            summarize(io.StringIO(HEADER + mixed))
        with self.assertRaisesRegex(ValueError, 'no successful reads'):
            summarize(io.StringIO(HEADER + 'loop,10,1,0,0,0,0\nend,12,0,0,0,0,0\n'))

    def test_reject_incomplete_or_corrupt_capture(self):
        corrupt = [
            '', HEADER, HEADER + VALID.split('end,')[0], HEADER + VALID[:-1],
            HEADER + VALID + VALID, HEADER + VALID + '\n',
            HEADER.replace('mmap', 'other') + VALID,
            HEADER + VALID.replace('42,1,0,0', '42,2,0,0'),
            HEADER + VALID.replace('42,1,0,0', '42,1,2,0'),
            HEADER + VALID.replace('42,1,0,0', '42,1,0,2'),
            HEADER + VALID.replace('42,1,0,0', '-1,1,0,0'),
            HEADER + VALID.replace('10,2,42', '10,-2,42'),
            HEADER + VALID.replace('10,2,42', '9,2,42'),
            HEADER + VALID.replace('10,2,42', '10,4,42'),
            HEADER + VALID.replace('10,2,42', '10,11,42'),
            HEADER + VALID.replace('loop,10,3', 'loop,10,11'),
            HEADER + VALID.replace('loop,10,3', 'loop,0,0'),
            HEADER + VALID.replace('end,12', 'end,9'),
            HEADER + VALID.replace('42,1,0,0', '42,1,0,0,extra'),
            HEADER + VALID.replace('42,1,0,0', '42,1,0'),
            HEADER + VALID.replace('10,2,42', '10,text,42'),
            HEADER + VALID.replace('10,2,42', '10,2,' + str(2**64)),
            HEADER + VALID.replace('10,2,42', '10,' + str(2**63) + ',42'),
            HEADER + VALID.replace('loop,10,3,0', 'loop,10,3,1'),
            HEADER + VALID.replace('end,12,0', 'end,12,1'),
            HEADER + 'read,10,1,1,1,0,0\nend,12,0,0,0,0,0\n',
            HEADER + 'loop,10,1,0,0,0,0\n' + VALID,
        ]
        for data in corrupt:
            with self.subTest(data=data), self.assertRaises(ValueError):
                summarize(io.StringIO(data))

    def test_reject_old_timing_semantics(self):
        with self.assertRaisesRegex(ValueError, 'format version'):
            summarize(io.StringIO(COLUMNS + VALID))
        with self.assertRaisesRegex(ValueError, 'format version'):
            summarize(io.StringIO(HEADER.replace('amule-upload-io,2', 'amule-upload-io,1') + VALID))

    def test_reject_non_collector_numbers_and_sample_counts(self):
        for field in ('+10', '1_0', ' 10', '10 ', '١٠'):
            with self.subTest(field=field), self.assertRaises(ValueError):
                summarize(io.StringIO(HEADER + VALID.replace('read,10,', f'read,{field},')))
        rows = 'read,10,0,42,1,0,0\n' * 4097
        with self.assertRaisesRegex(ValueError, 'sample limit'):
            summarize(io.StringIO(HEADER + rows + 'loop,10,3,0,0,0,0\nend,12,0,0,0,0,0\n'))
        # The limit applies to each loop, not to the whole capture.
        rows = 'read,10,0,42,1,0,0\n' * 4096 + 'loop,10,3,0,0,0,0\n'
        rows += 'read,20,0,42,1,0,0\nloop,20,3,0,0,0,0\nend,22,0,0,0,0,0\n'
        self.assertEqual(summarize(io.StringIO(HEADER + rows))['successful_reads'], 4097)

    def test_cli_rejects_corruption_without_traceback(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'bad.csv'
            path.write_text(HEADER + VALID.replace('10,2,42', '10,text,42'))
            result = subprocess.run([os.sys.executable, str(ROOT / 'experiments/upload-io/summarize.py'),
                                     str(path)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertNotIn('Traceback', result.stderr)


HARNESS = r'''
#include "UploadIOProfile.h"
#include <cstring>
#include <new>
#ifndef _WIN32
#include <csignal>
#include <sys/resource.h>
#include <sys/stat.h>
#endif
struct Commas : std::numpunct<char> {
    char do_thousands_sep() const override { return ','; }
    std::string do_grouping() const override { return "\3"; }
};
bool denyAlloc = false;
void *operator new(std::size_t size) {
    if (denyAlloc) throw std::bad_alloc();
    if (void *p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "normal";
    if (std::strcmp(mode, "platform") == 0) {
#ifdef _WIN32
        return 77;
#else
        return 0;
#endif
    }
    if (std::strcmp(mode, "locale") == 0) {
        std::locale::global(std::locale(std::locale::classic(), new Commas));
    }
    denyAlloc = std::strcmp(mode, "allocation") == 0;
    UploadIOProfile profile;
    denyAlloc = false;
    if (profile.Failed()) return 2;
    profile.BeginLoop();
    // No read-lifetime operation may allocate, including exception unwinding.
    denyAlloc = true;
    { UploadIOProfile::Read read(profile, 42, false, false);
      read.Start(); read.Finish(); }
    try {
        UploadIOProfile::Read read(profile, 99, true, false);
        read.Start();
        throw 7;
    } catch (int) {}
    { UploadIOProfile::Read read(profile, 1, false, false); }
    if (std::strcmp(mode, "overflow") == 0) {
        for (size_t i = 0; i < UploadIOProfile::MaxSamplesPerLoop; ++i) {
            UploadIOProfile::Read read(profile, 1, false, false);
            read.Start(); read.Finish();
        }
    }
    denyAlloc = false;
    if (!profile.EndLoop()) {
        if (!profile.Failed() || profile.Enabled() || !profile.EndLoop() || !profile.Close()) return 5;
        return 3;
    }
    if (std::strcmp(mode, "abandon") == 0) return 0;
#ifndef _WIN32
    if (std::strcmp(mode, "write-failure") == 0 || std::strcmp(mode, "close-failure") == 0) {
        struct stat st;
        if (stat(std::getenv("AMULE_UPLOAD_IO_PROFILE"), &st)) return 6;
        struct rlimit limit;
        if (getrlimit(RLIMIT_FSIZE, &limit)) return 6;
        limit.rlim_cur = st.st_size;
        if (setrlimit(RLIMIT_FSIZE, &limit)) return 6;
        std::signal(SIGXFSZ, SIG_IGN);
        if (std::strcmp(mode, "write-failure") == 0) {
            profile.BeginLoop();
            { UploadIOProfile::Read read(profile, 1, false, false); read.Start(); read.Finish(); }
            if (profile.EndLoop() || !profile.Failed() || profile.Enabled() || !profile.Close()) return 5;
            return 3;
        }
    }
#endif
    return profile.Close() ? 0 : 4;
}
'''


class CollectorTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.directory = Path(cls.temp.name)
        source = cls.directory / 'profile.cpp'
        cls.binary = cls.directory / 'profile.exe'
        source.write_text(HARNESS)
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-I', str(ROOT / 'src'), str(source), '-o', str(cls.binary)], check=True)
        cls.has_rlimit = subprocess.run([cls.binary, 'platform']).returncode == 0

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_collector(self, mode='normal', output=True):
        path = self.directory / f'{mode}.csv'
        env = dict(os.environ)
        env.pop('AMULE_UPLOAD_IO_PROFILE', None)
        if output:
            env['AMULE_UPLOAD_IO_PROFILE'] = str(path)
        result = subprocess.run([self.binary, mode], env=env, timeout=10)
        return result.returncode, path

    def test_lifecycle_and_no_allocation_during_reads(self):
        code, path = self.run_collector()
        self.assertEqual(code, 0)
        with path.open() as stream:
            self.assertEqual(stream.readline(), 'amule-upload-io,2\n')
            rows = list(csv.DictReader(stream))
        self.assertEqual([r['kind'] for r in rows], ['read', 'read', 'loop', 'end'])
        self.assertEqual([r['success'] for r in rows[:2]], ['1', '0'])
        with path.open() as stream:
            metrics = summarize(stream)
        self.assertEqual(metrics['successful_reads'], 1)
        self.assertEqual(metrics['failed_or_deferred_reads'], 1)

    def test_numeric_locale_does_not_corrupt_csv(self):
        code, path = self.run_collector('locale')
        self.assertEqual(code, 0)
        with path.open() as stream:
            self.assertEqual(summarize(stream)['successful_reads'], 1)

    def test_disabled_and_startup_allocation_failure(self):
        code, path = self.run_collector('disabled', output=False)
        self.assertEqual(code, 0)
        self.assertFalse(path.exists())
        env = dict(os.environ, AMULE_UPLOAD_IO_PROFILE='')
        self.assertEqual(subprocess.run([self.binary], env=env).returncode, 0)
        code, path = self.run_collector('allocation')
        self.assertEqual(code, 2)
        self.assertFalse(path.exists())

    def test_overflow_and_unfinished_capture_are_rejected(self):
        for mode, expected in [('overflow', 3), ('abandon', 0)]:
            with self.subTest(mode=mode):
                code, path = self.run_collector(mode)
                self.assertEqual(code, expected)
                with path.open() as stream, self.assertRaisesRegex(ValueError, 'completion marker'):
                    summarize(stream)

    def test_open_failure(self):
        env = dict(os.environ, AMULE_UPLOAD_IO_PROFILE=str(self.directory / 'missing' / 'data.csv'))
        self.assertEqual(subprocess.run([self.binary], env=env).returncode, 2)
        if Path('/dev/full').exists():
            env['AMULE_UPLOAD_IO_PROFILE'] = '/dev/full'
            self.assertEqual(subprocess.run([self.binary], env=env).returncode, 2)

    def test_late_output_failures_cannot_produce_valid_baselines(self):
        if not self.has_rlimit:
            self.skipTest('requires a POSIX collector with RLIMIT_FSIZE')
        for mode, expected in [('write-failure', 3), ('close-failure', 4)]:
            with self.subTest(mode=mode):
                code, path = self.run_collector(mode)
                self.assertEqual(code, expected)
                with path.open() as stream, self.assertRaisesRegex(ValueError, 'completion marker'):
                    summarize(stream)


if __name__ == '__main__':
    unittest.main()
