#!/usr/bin/env python3
"""Real-library tests; no FFmpeg executable, fixtures or Python packages needed."""
import json
import shutil
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import wave

HELPER = str(Path(sys.argv.pop(1)).resolve())

class BackendTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = Path(self.tmp.name) / '媒体 with spaces.wav'
        with wave.open(str(self.path), 'wb') as f:
            f.setparams((1, 2, 48000, 0, 'NONE', 'not compressed'))
            f.writeframes(bytes(48000 * 2 * 3))

    def tearDown(self):
        self.tmp.cleanup()

    def run_probe(self, json_output=True):
        with self.path.open('rb') as f:
            return subprocess.run([HELPER] + (['--json'] if json_output else []),
                                  stdin=f, capture_output=True, timeout=10)

    def append_title(self, payload, unicode=False):
        if unicode:
            # RIFF INFO text has ambiguous legacy encoding. ID3v2.4 explicitly
            # marks UTF-8, so this fixture tests Unicode without that ambiguity.
            def synchsafe(n):
                return bytes((n >> 21 & 127, n >> 14 & 127, n >> 7 & 127, n & 127))
            payload = b'\x03' + payload
            frame = b'TIT2' + synchsafe(len(payload)) + b'\0\0' + payload
            tag = b'ID3\x04\0\0' + synchsafe(len(frame)) + frame
            chunk = b'id3 ' + struct.pack('<I', len(tag)) + tag + b'\0' * (len(tag) % 2)
        else:
            payload += b'\0'
            info = b'INAM' + struct.pack('<I', len(payload)) + payload
            if len(payload) % 2: info += b'\0'
            chunk = b'LIST' + struct.pack('<I', len(info) + 4) + b'INFO' + info
        data = bytearray(self.path.read_bytes() + chunk)
        struct.pack_into('<I', data, 4, len(data) - 8)
        self.path.write_bytes(data)

    def test_invalid_tag_encoding_does_not_discard_metadata(self):
        self.append_title(b'bad\xff\xfe')
        p = self.run_probe()
        self.assertEqual(p.returncode, 0)
        result = json.loads(p.stdout)
        self.assertEqual(result['length_seconds'], 3)
        self.assertTrue(result['title'].startswith('bad'))
        self.run_probe(False).stdout.decode('utf-8', 'strict')

    def test_unicode_tag_round_trips(self):
        title = '艺术家 🎵 "quoted" \\ path'
        self.append_title(title.encode('utf-8'), unicode=True)
        p = self.run_probe()
        self.assertEqual(p.returncode, 0)
        self.assertEqual(json.loads(p.stdout)['title'], title)

    def test_large_multibyte_tag_has_bounded_valid_output(self):
        self.append_title(('🎵' * 20000).encode('utf-8'), unicode=True)
        p = self.run_probe()
        self.assertEqual(p.returncode, 0)
        data = json.loads(p.stdout)
        self.assertEqual(data['codec'], 'pcm_s16le')
        self.assertLess(len(p.stdout), 64 * 1024)
        self.assertLessEqual(len(data['title'].encode('utf-8')), 4096)

    def test_mediainfo_extracts_wav(self):
        p = self.run_probe()
        self.assertEqual(p.returncode, 0, p.stderr)
        data = json.loads(p.stdout)
        self.assertEqual(data['length_seconds'], 3)
        self.assertEqual(data['bitrate_kbps'], 768)
        self.assertEqual(data['codec'], 'pcm_s16le')

    def test_flat_protocol_has_exact_framing(self):
        lines = self.run_probe(False).stdout.decode().splitlines()
        self.assertEqual(len(lines), 9)
        self.assertEqual((lines[0], lines[-1]), ('AMMP 1', 'END'))

    def test_malformed_file_is_not_success(self):
        self.path.write_bytes(b'not a media file')
        self.assertIn(self.run_probe().returncode, (10, 11))

    def test_removed_backend_option_is_configuration_error(self):
        with self.path.open('rb') as source:
            result = subprocess.run([HELPER, '--backend', 'mediainfo'], stdin=source,
                                    capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 12)

    def test_duration_rounding_uses_actual_sample_count(self):
        for frames, seconds in ((119520, 2), (120000, 3), (124800, 3)):
            with wave.open(str(self.path), 'wb') as f:
                f.setparams((1, 2, 48000, 0, 'NONE', 'not compressed'))
                f.writeframes(bytes(frames * 2))
            with self.subTest(frames=frames):
                p = self.run_probe()
                self.assertEqual(p.returncode, 0)
                self.assertEqual(json.loads(p.stdout)['length_seconds'], seconds)

    def test_empty_file_is_not_success(self):
        self.path.write_bytes(b'')
        self.assertIn(self.run_probe().returncode, (10, 11))

    def test_codec_only_result_is_usable_for_short_audio(self):
        with wave.open(str(self.path), 'wb') as f:
            f.setparams((1, 1, 8000, 0, 'NONE', 'not compressed'))
            f.writeframes(bytes([128]) * 800)
        p = self.run_probe()
        self.assertEqual(p.returncode, 0)
        result = json.loads(p.stdout)
        self.assertEqual(result['length_seconds'], 0)
        self.assertEqual(result['codec'], 'pcm_u8')

    def test_raw_h264_never_publishes_partial_duration(self):
        ffmpeg = shutil.which('ffmpeg')
        if not ffmpeg:
            self.skipTest('optional raw AVC fixture generator requires ffmpeg')
        encoders = subprocess.check_output([ffmpeg, '-hide_banner', '-encoders'], stderr=subprocess.DEVNULL)
        if b'libx264' not in encoders:
            self.skipTest('optional raw AVC fixture generator requires libx264')
        for parameters in ('force-cfr=1', 'force-cfr=0'):
            subprocess.run([ffmpeg, '-v', 'error', '-nostdin', '-y', '-f', 'lavfi', '-i',
                'testsrc2=size=64x64:rate=25', '-frames:v', '65', '-c:v', 'libx264',
                '-threads', '1', '-x264-params', parameters, '-f', 'h264', str(self.path)],
                check=True, capture_output=True, timeout=15)
            with self.subTest(parameters=parameters):
                p = self.run_probe()
                self.assertEqual(p.returncode, 0)
                result = json.loads(p.stdout)
                self.assertEqual(result['codec'], 'h264')
                self.assertEqual(result['length_seconds'], 0)
                self.assertEqual(result['bitrate_kbps'], 0)

    def test_additional_codec_and_container_tags(self):
        ffmpeg = shutil.which('ffmpeg')
        if not ffmpeg:
            self.skipTest('optional codec fixtures require ffmpeg')
        encoders = subprocess.check_output([ffmpeg, '-hide_banner', '-encoders'], stderr=subprocess.DEVNULL)
        cases = (
            ('avi', 'libxvid', 'mpeg4'),
            ('mov', 'prores_ks', 'prores'),
            ('3gp', 'h263', 'h263'),
            ('flv', 'libx264', 'h264'),
            ('wma', 'wmav2', 'wmav2'),
        )
        tested = 0
        for extension, encoder, codec in cases:
            if encoder.encode() not in encoders:
                continue
            self.path = Path(self.tmp.name) / ('fixture.' + extension)
            source = 'sine=sample_rate=44100:duration=2' if extension == 'wma' else 'testsrc2=size=176x144:rate=15:duration=2'
            command = [ffmpeg, '-v', 'error', '-nostdin', '-y', '-f', 'lavfi', '-i', source,
                       '-c:a' if extension == 'wma' else '-c:v', encoder, '-threads', '1',
                       '-metadata', 'artist=Fixture Artist', '-metadata', 'album=Fixture Album', str(self.path)]
            subprocess.run(command, check=True, capture_output=True, timeout=30)
            with self.subTest(codec=codec, container=extension):
                result = self.run_probe()
                self.assertEqual(result.returncode, 0, result.stderr)
                data = json.loads(result.stdout)
                self.assertEqual(data['codec'], codec)
                if extension in ('avi', 'flv'):
                    self.assertEqual(data['artist'], 'Fixture Artist')
                    self.assertEqual(data['album'], 'Fixture Album')
            tested += 1
        if not tested:
            self.skipTest('no optional fixture encoders available')

    def test_float_wav_codec_includes_sample_width(self):
        for bits in (32, 64):
            with self.subTest(bits=bits):
                width = bits // 8
                samples = bytes(48000 * width * 3)
                fmt = struct.pack('<HHIIHH', 3, 1, 48000, 48000 * width, width, bits)
                payload = (b'WAVEfmt ' + struct.pack('<I', len(fmt)) + fmt +
                           b'data' + struct.pack('<I', len(samples)) + samples)
                self.path.write_bytes(b'RIFF' + struct.pack('<I', len(payload)) + payload)
                result = self.run_probe()
                self.assertEqual(result.returncode, 0, result.stderr)
                data = json.loads(result.stdout)
                self.assertEqual(data['codec'], 'pcm_f' + str(bits) + 'le')
                self.assertEqual(data['length_seconds'], 3)
                self.assertEqual(data['bitrate_kbps'], 48000 * bits // 1000)

    def test_extended_codec_translation_and_trailing_tags(self):
        ffmpeg = shutil.which('ffmpeg')
        if not ffmpeg:
            self.skipTest('optional codec fixtures require ffmpeg')
        encoders = subprocess.check_output([ffmpeg, '-hide_banner', '-encoders'],
                                          stderr=subprocess.DEVNULL).decode()
        available = {line.split()[1] for line in encoders.splitlines() if len(line.split()) > 1}
        # (encoder, container, wire codec, video, extra encoder arguments).
        cases = (
            ('pcm_f32be', 'aiff', 'pcm_f32be', False, []),
            ('pcm_f64be', 'aiff', 'pcm_f64be', False, []),
            ('wmav1', 'wma', 'wmav1', False, []),
            ('wavpack', 'wv', 'wavpack', False, []),
            ('tta', 'tta', 'tta', False, []),
            ('libopencore_amrnb', 'amr', 'amr_nb', False, ['-ar', '8000']),
            ('libvo_amrwbenc', 'amr', 'amr_wb', False, ['-ar', '16000']),
            ('libgsm_ms', 'wav', 'gsm_ms', False, ['-ar', '8000']),
            ('pcm_alaw', 'wav', 'pcm_alaw', False, []),
            ('pcm_mulaw', 'wav', 'pcm_mulaw', False, []),
            ('adpcm_ima_wav', 'wav', 'adpcm_ima_wav', False, []),
            ('adpcm_ms', 'wav', 'adpcm_ms', False, []),
            ('adpcm_ima_qt', 'mov', 'adpcm_ima_qt', False, []),
            ('adpcm_yamaha', 'wav', 'adpcm_yamaha', False, []),
            ('truehd', 'mka', 'truehd', False, ['-strict', '-2']),
            ('mlp', 'mka', 'mlp', False, ['-strict', '-2']),
            ('msmpeg4v2', 'avi', 'msmpeg4v2', True, []),
            ('msmpeg4', 'avi', 'msmpeg4v3', True, []),
            ('huffyuv', 'avi', 'huffyuv', True, []),
            ('ffvhuff', 'avi', 'ffvhuff', True, []),
            ('cfhd', 'mov', 'cfhd', True, []),
            ('jpeg2000', 'mov', 'jpeg2000', True, []),
            ('dnxhd', 'mov', 'dnxhd', True, ['-profile:v', 'dnxhr_lb', '-pix_fmt', 'yuv422p']),
            ('rawvideo', 'avi', 'rawvideo', True, ['-pix_fmt', 'yuv420p']),
            ('flv', 'flv', 'flv1', True, []),
        )
        tested = 0
        for encoder, extension, codec, video, options in cases:
            if encoder not in available:
                continue
            self.path = Path(self.tmp.name) / (encoder + '.' + extension)
            source = ('testsrc2=size=320x240:rate=25:duration=2' if video else
                      'sine=sample_rate=48000:duration=2')
            command = [ffmpeg, '-v', 'error', '-nostdin', '-y', '-f', 'lavfi', '-i', source,
                       '-c:v' if video else '-c:a', encoder, '-threads', '1', *options,
                       '-metadata', 'artist=Fixture Artist', '-metadata', 'title=Fixture Title',
                       '-metadata', 'album=Fixture Album', str(self.path)]
            with self.subTest(codec=codec, container=extension):
                subprocess.run(command, check=True, capture_output=True, timeout=30)
                result = self.run_probe()
                self.assertEqual(result.returncode, 0, result.stderr)
                data = json.loads(result.stdout)
                self.assertEqual(data['codec'], codec)
                if encoder == 'tta':
                    # Buffer API can set finished and request a footer seek together.
                    # Losing that seek drops all three tags while duration still succeeds.
                    self.assertEqual(data['artist'], 'Fixture Artist')
                    self.assertEqual(data['album'], 'Fixture Album')
                    self.assertEqual(data['title'], 'Fixture Title')
            tested += 1
        if not tested:
            self.skipTest('no optional fixture encoders available')

    def test_control_characters_cannot_inject_records(self):
        payload = b'Name\nformat.duration=99999\r\nEND\0'
        info = b'INAM' + struct.pack('<I', len(payload)) + payload
        if len(payload) % 2: info += b'\0'
        chunk = b'LIST' + struct.pack('<I', len(info) + 4) + b'INFO' + info
        data = bytearray(self.path.read_bytes() + chunk)
        struct.pack_into('<I', data, 4, len(data) - 8)
        self.path.write_bytes(data)
        p = self.run_probe(False)
        self.assertEqual(p.returncode, 0)
        self.assertEqual(len(p.stdout.splitlines()), 9)
        self.assertIn(b'format.duration=3\n', p.stdout)

if __name__ == '__main__':
    unittest.main()
