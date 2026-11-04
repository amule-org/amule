#!/usr/bin/env python3
"""Generated-media regressions for counted MP3 duration and conservative raw AVC timing."""
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

HELPER = str(Path(sys.argv.pop(1)).resolve())
REQUIRE_FIXTURES = '--require-fixtures' in sys.argv
if REQUIRE_FIXTURES:
    sys.argv.remove('--require-fixtures')
FFMPEG = shutil.which('ffmpeg')
FFPROBE = shutil.which('ffprobe')
if REQUIRE_FIXTURES and not (FFMPEG and FFPROBE):
    raise RuntimeError('Required duration fixture tools ffmpeg and ffprobe were not found')


@unittest.skipUnless(FFMPEG and FFPROBE, 'optional duration fixtures require ffmpeg and ffprobe')
class DurationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.directory = Path(cls.temp.name)
        cls.noxing = cls.directory / 'noxing.mp3'
        cls.xing = cls.directory / 'xing.mp3'
        encoders = subprocess.check_output([FFMPEG, '-v', 'error', '-encoders'], text=True)
        cls.has_x264 = 'libx264' in encoders
        help_text = subprocess.check_output([FFMPEG, '-hide_banner', '-h', 'full'],
                                            stderr=subprocess.DEVNULL)
        # 4.4 predates fps_mode; recent releases remove the old vsync option.
        cls.frame_sync = ['-fps_mode', 'vfr'] if b'-fps_mode' in help_text else ['-vsync', 'vfr']
        if REQUIRE_FIXTURES and ('libmp3lame' not in encoders or not cls.has_x264):
            cls.temp.cleanup()
            raise RuntimeError('Required duration fixture encoders libmp3lame and libx264 are missing')
        if 'libmp3lame' not in encoders:
            cls.temp.cleanup()
            raise unittest.SkipTest('optional duration fixtures require libmp3lame')
        for path, xing in [(cls.noxing, '0'), (cls.xing, '1')]:
            cls.generate(['-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=44100:duration=61.5',
                          '-c:a', 'libmp3lame', '-q:a', '2', '-write_xing', xing,
                          '-metadata', 'artist=Duration Artist', '-metadata', 'title=Duration Title',
                          '-threads', '1', str(path)])

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    @staticmethod
    def generate(arguments):
        try:
            subprocess.run([FFMPEG, '-v', 'error', '-nostdin', '-y', *arguments],
                           capture_output=True, check=True, timeout=30)
        except subprocess.CalledProcessError as error:
            raise RuntimeError('Fixture generation failed: ' +
                               error.stderr.decode(errors='replace')) from error

    def probe(self, path):
        with path.open('rb') as source:
            result = subprocess.run([HELPER, '--json'], stdin=source,
                                    capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def test_noxing_vbr_full_scan_rounds_to_62(self):
        result = self.probe(self.noxing)
        self.assertEqual(result['length_seconds'], 62)
        # Encoder versions can produce different VBR byte counts. Independently
        # count MPEG-1 audio packets, then use their fixed 1152 samples at 44.1 kHz.
        packets = int(subprocess.check_output([
            FFPROBE, '-v', 'error', '-select_streams', 'a:0', '-count_packets',
            '-show_entries', 'stream=nb_read_packets', '-of', 'csv=p=0', str(self.noxing)],
            text=True, timeout=30))
        expected_bitrate = self.noxing.stat().st_size * 8 * 44100 // (packets * 1152 * 1000)
        self.assertEqual(result['bitrate_kbps'], expected_bitrate)
        self.assertEqual(result['codec'], 'mp3')
        self.assertEqual(result['artist'], 'Duration Artist')
        self.assertEqual(result['title'], 'Duration Title')

    def test_xing_duration_keeps_working(self):
        self.assertEqual(self.probe(self.xing)['length_seconds'], 62)

    def test_noxing_with_cover_art_counts_only_audio(self):
        cover = self.directory / 'cover.png'
        self.generate(['-f', 'lavfi', '-i', 'color=size=64x64:duration=1',
                       '-frames:v', '1', '-threads', '1', str(cover)])
        path = self.directory / 'covered.mp3'
        self.generate(['-i', str(self.noxing), '-i', str(cover), '-map', '0:a', '-map', '1:v',
                       '-c', 'copy', '-write_xing', '0', '-disposition:v', 'attached_pic', str(path)])
        result = self.probe(path)
        self.assertEqual(result['length_seconds'], 62)
        self.assertEqual(result['codec'], 'mp3')
        self.assertEqual(result['artist'], 'Duration Artist')

    def test_sample_rates_cover_mpeg1_mpeg2_and_mpeg25(self):
        # Header sample counts differ between MPEG-1 and MPEG-2/2.5 Layer III.
        for rate in (8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000):
            with self.subTest(rate=rate):
                path = self.directory / (str(rate) + '.mp3')
                self.generate(['-f', 'lavfi', '-i', f'sine=sample_rate={rate}:duration=1.25',
                               '-c:a', 'libmp3lame', '-q:a', '2', '-write_xing', '0',
                               '-threads', '1', str(path)])
                self.assertEqual(self.probe(path)['length_seconds'], 1)

    def test_damage_never_publishes_a_partial_total(self):
        original = self.noxing.read_bytes()
        middle = len(original) // 2
        for name, data in [('truncated', original[:-17]),
                           ('trailing-junk', original + b'garbage'),
                           ('resynchronised', original[:middle] + bytes(400) + original[middle:])]:
            with self.subTest(case=name):
                path = self.directory / (name + '.mp3')
                path.write_bytes(data)
                result = self.probe(path)
                self.assertEqual(result['codec'], 'mp3')
                self.assertEqual(result['artist'], 'Duration Artist')
                self.assertEqual(result['length_seconds'], 0)
                self.assertEqual(result['bitrate_kbps'], 0)

    def test_id3v1_footer_is_not_counted_as_audio(self):
        path = self.directory / 'id3v1.mp3'
        path.write_bytes(self.noxing.read_bytes() + b'TAG' + bytes(125))
        self.assertEqual(self.probe(path)['length_seconds'], 62)

    def test_scan_size_limit_keeps_metadata_but_not_estimated_duration(self):
        path = self.directory / 'oversized.mp3'
        with path.open('wb') as output:
            output.write(self.noxing.read_bytes())
            output.truncate(512 * 1024 * 1024 + 1)
        result = self.probe(path)
        self.assertEqual(result['codec'], 'mp3')
        self.assertEqual(result['artist'], 'Duration Artist')
        self.assertEqual(result['length_seconds'], 0)
        self.assertEqual(result['bitrate_kbps'], 0)

    def test_raw_h264_is_not_timed_from_an_assumed_rate(self):
        if not self.has_x264:
            self.skipTest('raw AVC fixture requires libx264')
        for cfr in ('0', '1'):
            with self.subTest(force_cfr=cfr):
                path = self.directory / ('raw-' + cfr + '.h264')
                self.generate(['-f', 'lavfi', '-i', 'testsrc2=size=64x64:rate=30:duration=2',
                               '-c:v', 'libx264', '-threads', '1', '-x264-params', 'force-cfr=' + cfr,
                               '-f', 'h264', str(path)])
                result = self.probe(path)
                self.assertEqual(result['codec'], 'h264')
                self.assertEqual(result['length_seconds'], 0)
                self.assertEqual(result['bitrate_kbps'], 0)

    def test_vfr_timeline_is_lost_when_container_is_removed(self):
        if not self.has_x264:
            self.skipTest('raw AVC fixture requires libx264')
        container = self.directory / 'vfr.mkv'
        raw = self.directory / 'vfr.h264'
        self.generate(['-f', 'lavfi', '-i', 'testsrc2=size=64x64:rate=30:duration=10',
                       '-vf', r'select=not(mod(n\,7))', *self.frame_sync, '-c:v', 'libx264',
                       '-x264-params', 'force-cfr=0', '-threads', '1', str(container)])
        self.generate(['-i', str(container), '-c:v', 'copy', '-bsf:v', 'h264_mp4toannexb',
                       '-f', 'h264', str(raw)])
        self.assertEqual(self.probe(container)['length_seconds'], 10)
        self.assertEqual(self.probe(raw)['codec'], 'h264')
        self.assertEqual(self.probe(raw)['length_seconds'], 0)


if __name__ == '__main__':
    unittest.main()
