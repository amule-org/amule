#!/usr/bin/env python3
"""Hostile inputs: every result is a clean verdict (0, 10 or 11) with valid framing, fast."""
import json
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile
import unittest

HELPER = str(Path(sys.argv.pop(1)).resolve())

MAGICS = (b'RIFF\xff\xff\xff\x7fWAVE', b'RIFF\xff\xff\xff\x7fAVI ', b'FORM\xff\xff\xff\x7fAIFF',
          b'\x1aE\xdf\xa3', b'\x00\x00\x00\x18ftypisom', b'OggS\x00\x02', b'fLaC', b'ID3\x04\x00\x00',
          b'0&\xb2u\x8ef\xcf\x11\xa6\xd9\x00\xaa\x00b\xcel', b'FLV\x01\x05\x00\x00\x00\x09',
          b'#!AMR\n', b'TTA1', b'wvpk', b'\x00\x00\x01\xba', b'\x47\x40\x00\x10', b'\xff\xfb\x90\x64',
          b'\xff\xf1\x50\x80', b'\x00\x00\x00\x01\x67', b'.RMF\x00\x00\x00\x12\x00\x00')


class HostileTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = Path(self.tmp.name) / 'hostile.bin'

    def tearDown(self):
        self.tmp.cleanup()

    def verdict(self, data, timeout=10):
        self.path.write_bytes(data)
        with self.path.open('rb') as f:
            p = subprocess.run([HELPER, '--json'], stdin=f, capture_output=True, timeout=timeout)
        self.assertIn(p.returncode, (0, 10, 11), (data[:64], p.returncode, p.stderr))
        self.result = json.loads(p.stdout) if p.returncode == 0 else None
        return p.returncode

    def test_random_tails_after_every_magic(self):
        rng = random.Random(1904)
        for magic in MAGICS:
            for _ in range(60):
                tail = bytes(rng.getrandbits(8) for _ in range(rng.choice((0, 7, 64, 700, 5000))))
                with self.subTest(magic=magic):
                    self.verdict(magic + tail)

    def test_impossible_sizes(self):
        cases = (
            b'\x00\x00\x00\x18ftypisom\x00\x00\x00\x00isom' + b'\xff\xff\xff\xffmoov',
            b'\x00\x00\x00\x01moov' + struct.pack('>Q', 1 << 62),
            b'\x1aE\xdf\xa3\x01\xff\xff\xff\xff\xff\xff\xff',
            b'ID3\x04\x00\x00\x7f\x7f\x7f\x7f' + b'\0' * 64,
            b'RIFF\x24\x00\x00\x00WAVEfmt \xff\xff\xff\xff',
            b'0&\xb2u\x8ef\xcf\x11\xa6\xd9\x00\xaa\x00b\xcel' + struct.pack('<Q', 1 << 60) + b'\0' * 64,
        )
        for data in cases:
            with self.subTest(data=data[:16]):
                self.assertIn(self.verdict(data), (10, 11))

    def test_deeply_nested_boxes_do_not_exhaust_the_stack(self):
        inner = b''
        for _ in range(20000):
            inner = struct.pack('>I', len(inner) + 8) + b'wave' + inner
        entry = struct.pack('>I', 36 + len(inner)) + b'mp4a' + b'\0' * 28 + inner
        self.verdict(b'\x00\x00\x00\x08ftyp' + struct.pack('>I', len(entry) + 8) + b'moov' + entry)

    def test_matroska_seekhead_pointing_at_itself(self):
        def element(eid, payload):  # One-byte EBML size; payloads here are under 127 bytes.
            return eid + bytes((0x80 | len(payload),)) + payload
        seek = element(b'\x4d\xbb', element(b'\x53\xab', b'\x11\x4d\x9b\x74') + element(b'\x53\xac', b'\x00'))
        seekhead = element(b'\x11\x4d\x9b\x74', seek)
        ebml = element(b'\x1a\x45\xdf\xa3', element(b'\x42\x82', b'matroska'))
        segment = b'\x18\x53\x80\x67' + bytes((0x80 | len(seekhead),)) + seekhead
        self.verdict(ebml + segment)

    def test_truncations_of_a_valid_wav(self):
        samples = bytes(8000 * 2)
        fmt = struct.pack('<HHIIHH', 1, 1, 8000, 16000, 2, 16)
        payload = b'WAVEfmt ' + struct.pack('<I', len(fmt)) + fmt + b'data' + struct.pack('<I', len(samples)) + samples
        data = b'RIFF' + struct.pack('<I', len(payload)) + payload
        for cut in (5, 12, 20, 36, 44, 45, 1000):
            with self.subTest(cut=cut):
                self.verdict(data[:cut])

    def test_flv_codec_id_out_of_integer_range(self):
        # Found by libFuzzer: a huge AMF number used to reach an int conversion.
        def amf_string(s):
            return struct.pack('>H', len(s)) + s
        meta = (b'\x02' + amf_string(b'onMetaData') + b'\x08' + struct.pack('>I', 2) +
                amf_string(b'videocodecid') + b'\x00' + struct.pack('>d', 1e300) +
                amf_string(b'audiocodecid') + b'\x00' + struct.pack('>d', -1e300) + b'\x00\x00\x09')
        tag = b'\x12' + struct.pack('>I', len(meta))[1:] + b'\0' * 7 + meta
        self.verdict(b'FLV\x01\x05\x00\x00\x00\x09\x00\x00\x00\x00' + tag + struct.pack('>I', len(tag)))

    def test_endless_mpeg_frames_are_bounded(self):
        frame = b'\xff\xfb\x90\x64' + b'\0' * 413  # 128 kbps, 44.1 kHz layer 3
        self.verdict(frame * 20000, timeout=10)


class RealWorldQuirkTests(unittest.TestCase):
    """Layouts real encoders write: each must give the values FFmpeg reports."""
    setUp, tearDown, verdict = HostileTests.setUp, HostileTests.tearDown, HostileTests.verdict

    def probe(self, data, fields=('length_seconds', 'codec')):
        self.assertEqual(self.verdict(data), 0)
        return tuple(self.result[k] for k in fields)

    def test_matroska_truncated_segment_and_nul_padded_codec_id(self):
        def element(eid, payload):
            return eid + bytes((0x80 | len(payload),)) + payload
        ebml = element(b'\x1a\x45\xdf\xa3', element(b'\x42\x82', b'matroska'))
        info = element(b'\x15\x49\xa9\x66', element(b'\x2a\xd7\xb1', b'\x0f\x42\x40') +
                       element(b'\x44\x89', struct.pack('>f', 2000.0)))
        entry = element(b'\xae', element(b'\x83', b'\x01') + element(b'\x86', b'V_MPEG4/ISO/AVC\0'))
        tracks = element(b'\x16\x54\xae\x6b', entry)
        # The Segment claims 1 TiB: the file was cut short.
        segment = b'\x18\x53\x80\x67\x01' + (1 << 40).to_bytes(7, 'big') + info + tracks
        self.assertEqual(self.probe(ebml + segment), (2, 'h264'))
        # Cut inside the first Cluster: the headers before it are complete.
        cluster = b'\x1f\x43\xb6\x75\x01' + (1 << 30).to_bytes(7, 'big') + b'\xe7\x81\x00'
        self.assertEqual(self.probe(ebml + segment + cluster), (2, 'h264'))
        # VfW-wrapped video: CodecPrivate is a binary BITMAPINFOHEADER full of NULs.
        bih = struct.pack('<IiiHH', 40, 64, 48, 1, 24) + b'XVID' + b'\0' * 20
        entry = element(b'\xae', element(b'\x83', b'\x01') + element(b'\x86', b'V_MS/VFW/FOURCC') +
                        element(b'\x63\xa2', bih))
        segment = b'\x18\x53\x80\x67\x01' + (1 << 40).to_bytes(7, 'big') + info + element(b'\x16\x54\xae\x6b', entry)
        self.assertEqual(self.probe(ebml + segment), (2, 'mpeg4'))

    def test_fragmented_mp4_without_total_duration(self):
        def box(kind, *payload):
            body = b''.join(payload)
            return struct.pack('>I', 8 + len(body)) + kind + body
        def full(kind, version_flags, *payload):
            return box(kind, struct.pack('>I', version_flags), *payload)
        stsd = full(b'stsd', 0, struct.pack('>I', 1), box(b'avc1', b'\0' * 78))
        mdia = box(b'mdia', full(b'mdhd', 0, struct.pack('>5I', 0, 0, 1000, 0, 0)),
                   full(b'hdlr', 0, b'\0' * 4 + b'vide' + b'\0' * 13),
                   box(b'minf', box(b'stbl', stsd)))
        trak = box(b'trak', full(b'tkhd', 1, struct.pack('>5I', 0, 0, 1, 0, 0) + b'\0' * 64), mdia)
        mvex = box(b'mvex', full(b'trex', 0, struct.pack('>5I', 1, 1, 0, 0, 0)))
        # Version 1 mvhd with an all-ones duration, which means unknown (Smooth Streaming).
        mvhd = full(b'mvhd', 1 << 24, struct.pack('>QQIQ', 0, 0, 1000, (1 << 64) - 1) + b'\0' * 80)
        moov = box(b'moov', mvhd, trak, mvex)
        # Five samples of the default 1,000 units, then two at an explicit decode time.
        first = box(b'moof', box(b'traf', full(b'tfhd', 8, struct.pack('>II', 1, 1000)),
                                 full(b'trun', 0, struct.pack('>I', 5))))
        second = box(b'moof', box(b'traf', full(b'tfhd', 0, struct.pack('>I', 1)),
                                  full(b'tfdt', 0, struct.pack('>I', 5000)),
                                  full(b'trun', 0x100, struct.pack('>3I', 2, 1000, 1000))))
        data = box(b'ftyp', b'isom\0\0\0\0') + moov + first + box(b'mdat') + second + box(b'mdat')
        self.assertEqual(self.probe(data), (7, 'h264'))

    def test_avi_ignores_dmlh_counting_every_chunk(self):
        def chunk(cid, payload):
            return cid + struct.pack('<I', len(payload)) + payload + b'\0' * (len(payload) & 1)
        avih = struct.pack('<14I', 40000, 0, 0, 0, 50, 0, 2, 0, 0, 0, 0, 0, 0, 0)
        strh = b'vidsXVID' + struct.pack('<IHHIIIIIIII4H', 0, 0, 0, 0, 1, 25, 0, 50, 0, 0, 0, 0, 0, 0, 0)
        strf = struct.pack('<IiiHH', 40, 64, 48, 1, 24) + b'XVID' + b'\0' * 20
        strl = chunk(b'LIST', b'strl' + chunk(b'strh', strh) + chunk(b'strf', strf))
        odml = chunk(b'LIST', b'odml' + chunk(b'dmlh', struct.pack('<I', 1000) + b'\0' * 244))
        hdrl = chunk(b'LIST', b'hdrl' + chunk(b'avih', avih) + strl + odml)
        body = b'AVI ' + hdrl + chunk(b'LIST', b'movi')
        self.assertEqual(self.probe(b'RIFF' + struct.pack('<I', len(body)) + body), (2, 'mpeg4'))

    def test_ogm_video_in_a_file_named_avi(self):
        def page(kind, granule, packet):
            return (b'OggS\0' + bytes((kind,)) + struct.pack('<QIII', granule, 7, 0, 0) +
                    bytes((1, len(packet))) + packet)
        # OGM stream header: XviD at 25 fps (400000 x 100 ns per frame).
        header = (b'\x01video\0\0\0XVID' + struct.pack('<IqqIIHH', 0x38, 400000, 1, 0, 0, 24, 0) +
                  struct.pack('<II', 64, 48))
        data = page(2, 0, header) + page(0, 0, b'\x03') + page(4, 50, b'\x08')
        self.assertEqual(self.probe(data), (2, 'mpeg4'))

    def test_realmedia_headers(self):
        def chunk(cid, payload):
            return cid + struct.pack('>IH', 10 + len(payload), 0) + payload
        def mdpr(mime, specific):
            return chunk(b'MDPR', struct.pack('>H7I', 0, 0, 0, 0, 0, 0, 0, 0) + b'\x00' +
                         bytes((len(mime),)) + mime + struct.pack('>I', len(specific)) + specific)
        def str16(s):
            return struct.pack('>H', len(s)) + s
        header = chunk(b'.RMF', struct.pack('>II', 0, 5))
        prop = chunk(b'PROP', struct.pack('>9I2H', 0, 0, 0, 0, 0, 5000, 0, 0, 0, 2, 0))
        cook = b'.ra\xfd\x00\x05' + b'\0' * 60 + b'cook'  # RealAudio v5: FourCC at 66.
        audio = mdpr(b'audio/x-pn-realaudio', cook)
        video = mdpr(b'video/x-pn-realvideo', struct.pack('>I', 34) + b'VIDORV40' + b'\0' * 22)
        cont = chunk(b'CONT', str16(b'Real Title') + str16(b'Real Author') + str16(b'') + str16(b''))
        data = header + prop + audio + video + cont + chunk(b'DATA', b'\0' * 8)
        # Video wins over the earlier audio stream; CONT author is the artist.
        self.assertEqual(self.probe(data, ('length_seconds', 'codec', 'title', 'artist')),
                         (5, 'rv40', 'Real Title', 'Real Author'))
        self.assertEqual(self.probe(header + prop + audio + chunk(b'DATA', b'')), (5, 'cook'))
        # FFmpeg writes a DATA size that can run past the end of the file.
        data = b'DATA' + struct.pack('>IH', 1 << 30, 0)
        self.assertEqual(self.probe(header + prop + audio + data), (5, 'cook'))


if __name__ == '__main__':
    unittest.main()
