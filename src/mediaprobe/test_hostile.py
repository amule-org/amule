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

from corpus import save

HELPER = str(Path(sys.argv.pop(1)).resolve())

MAGICS = (b'RIFF\xff\xff\xff\x7fWAVE', b'RIFF\xff\xff\xff\x7fAVI ', b'FORM\xff\xff\xff\x7fAIFF',
          b'\x1aE\xdf\xa3', b'\x00\x00\x00\x18ftypisom', b'OggS\x00\x02', b'fLaC', b'ID3\x04\x00\x00',
          b'0&\xb2u\x8ef\xcf\x11\xa6\xd9\x00\xaa\x00b\xcel', b'FLV\x01\x05\x00\x00\x00\x09',
          b'#!AMR\n', b'TTA1', b'wvpk', b'\x00\x00\x01\xba', b'\x47\x40\x00\x10', b'\xff\xfb\x90\x64',
          b'\xff\xf1\x50\x80', b'\x00\x00\x00\x01\x67', b'.RMF\x00\x00\x00\x12\x00\x00')


def box(kind, *payload):
    body = b''.join(payload)
    return struct.pack('>I', 8 + len(body)) + kind + body


def full(kind, version_flags, *payload):
    return box(kind, struct.pack('>I', version_flags), *payload)


def chunk(cid, payload):
    return cid + struct.pack('<I', len(payload)) + payload + b'\0' * (len(payload) & 1)


def pts_bytes(pts):
    return bytes((0x21 | (pts >> 29 & 0x0e), pts >> 22 & 0xff, (pts >> 14 & 0xfe) | 1,
                  pts >> 7 & 0xff, (pts << 1 & 0xfe) | 1))


def ts_packet(pid, payload, prefix=b''):
    body = payload + b'\xff' * (184 - len(payload))
    return prefix + bytes((0x47, 0x40 | pid >> 8, pid & 0xff, 0x10)) + body


def pes(stream, pts, payload=b''):
    return b'\0\0\1' + bytes((stream,)) + struct.pack('>H', 8 + len(payload)) + b'\x80\x80\x05' + \
        pts_bytes(pts) + payload


def mp4(mvhd_duration, mdhd_duration, extra_mvex=b''):
    stsd = full(b'stsd', 0, struct.pack('>I', 1), box(b'avc1', b'\0' * 78))
    mdia = box(b'mdia', full(b'mdhd', 0, struct.pack('>5I', 0, 0, 1000, mdhd_duration, 0)),
               full(b'hdlr', 0, b'\0' * 4 + b'vide' + b'\0' * 13),
               box(b'minf', box(b'stbl', stsd)))
    trak = box(b'trak', full(b'tkhd', 1, struct.pack('>5I', 0, 0, 1, 0, 0) + b'\0' * 64), mdia)
    mvex = box(b'mvex', extra_mvex, full(b'trex', 0, struct.pack('>5I', 1, 1, 0, 0, 0)))
    if mvhd_duration is None:  # Version 1 with an all-ones duration: unknown (Smooth Streaming).
        mvhd = full(b'mvhd', 1 << 24, struct.pack('>QQIQ', 0, 0, 1000, (1 << 64) - 1) + b'\0' * 80)
    else:
        mvhd = full(b'mvhd', 0, struct.pack('>4I', 0, 0, 1000, mvhd_duration) + b'\0' * 80)
    return box(b'ftyp', b'isom\0\0\0\0') + box(b'moov', mvhd, trak, mvex)


def moof(duration, count, decode_time=None):
    tfdt = b'' if decode_time is None else full(b'tfdt', 0, struct.pack('>I', decode_time))
    return box(b'moof', box(b'traf', full(b'tfhd', 8, struct.pack('>II', 1, duration)), tfdt,
                            full(b'trun', 0, struct.pack('>I', count)))) + box(b'mdat')


class HostileTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = Path(self.tmp.name) / 'hostile.bin'

    def tearDown(self):
        self.tmp.cleanup()

    def verdict(self, data, timeout=10):
        save(data)
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
        moov = mp4(None, 0)[len(box(b'ftyp', b'isom\0\0\0\0')):]
        # Five samples of the default 1,000 units, then two at an explicit decode time.
        first = box(b'moof', box(b'traf', full(b'tfhd', 8, struct.pack('>II', 1, 1000)),
                                 full(b'trun', 0, struct.pack('>I', 5))))
        second = box(b'moof', box(b'traf', full(b'tfhd', 0, struct.pack('>I', 1)),
                                  full(b'tfdt', 0, struct.pack('>I', 5000)),
                                  full(b'trun', 0x100, struct.pack('>3I', 2, 1000, 1000))))
        head = box(b'ftyp', b'isom\0\0\0\0') + moov + first + box(b'mdat')
        self.assertEqual(self.probe(head + second + box(b'mdat')), (7, 'h264'))
        # A file cut in its last mdat keeps every fragment.
        self.assertEqual(self.probe(head + second + box(b'mdat', b'\0' * 100)[:50]), (7, 'h264'))
        # A broken fragment, a zeroed hole or the box limit would make the sum short.
        self.assertEqual(self.probe(head + second[:20]), (0, 'h264'))
        self.assertEqual(self.probe(head + b'\0' * 4096 + second), (0, 'h264'))
        self.assertEqual(self.probe(head + box(b'free') * 200000 + second), (0, 'h264'))

    def test_fragmented_mp4_with_samples_in_moov(self):
        # FFmpeg's frag_keyframe without empty_moov: mvhd and mdhd cover only the first second.
        fragments = moof(1000, 5) + moof(1000, 2, 6000)
        self.assertEqual(self.probe(mp4(1000, 1000) + fragments), (8, 'h264'))
        # With mehd, its total wins over mvhd.
        mehd = full(b'mehd', 0, struct.pack('>I', 8000))
        self.assertEqual(self.probe(mp4(1000, 1000, mehd) + moof(1000, 7)), (8, 'h264'))

    def test_damage_in_mp3_and_adts_frame_counts(self):
        mp3 = b'\xff\xfb\x90\x64' + b'\0' * 413  # 128 kbps, 44.1 kHz: 1,152 samples.
        length = 200
        adts = (b'\xff\xf1\x50\x80' + bytes((length >> 3, (length & 7) << 5 | 0x1f, 0xfc)) +
                b'\0' * (length - 7))  # 44.1 kHz AAC: 1,024 samples.
        for frame, codec, seconds in ((mp3, 'mp3', 5), (adts, 'aac', 5)):
            half = frame * 100
            with self.subTest(codec=codec):
                # A gap within the 64 KiB resync window is crossed, and trailing junk is ignored.
                self.assertEqual(self.probe(half + b'\0' * 8192 + half), (seconds, codec))
                self.assertEqual(self.probe(half + half + b'\0' * 100000), (seconds, codec))
                # Even when the junk ends in something that looks like a frame header.
                self.assertEqual(self.probe(half + half + b'\0' * 70000 + frame[:4]), (seconds, codec))
                # A wider gap would leave the count short, so there is no duration.
                self.assertEqual(self.probe(half + b'\0' * 100000 + half), (0, codec))

    def test_mpeg1_layer1_frame_length(self):
        # 128 kbps at 44.1 kHz: 34 slots of 4 bytes, and a padding slot in some frames.
        data, slots = b'', 0
        for _ in range(1000):
            slots += 12 * 128000 % 44100
            pad = slots >= 44100
            slots -= 44100 * pad
            data += struct.pack('>I', 0xFFFF40C0 | pad << 9) + b'\0' * (132 + 4 * pad)
        self.assertEqual(self.probe(data), (9, 'mp1'))

    def test_amr_with_a_reserved_frame_type(self):
        frames = b'\x3c' + b'\0' * 31  # 12.2 kbit/s, 20 ms.
        self.assertEqual(self.probe(b'#!AMR\n' + frames * 500), (10, 'amr_nb'))
        self.assertEqual(self.probe(b'#!AMR\n' + frames * 250 + b'\x74' + frames * 250), (0, 'amr_nb'))

    def test_m2ts_sync_byte_at_offset_188_and_last_packet(self):
        def packet(pid, payload):
            return ts_packet(pid, payload, b'\0' * 4)
        pat = packet(0, b'\0\x00\xb0\x0d\0\x01\xc1\0\0' + b'\0\x01\xf0\x00' + b'\0' * 4)
        pmt = packet(0x1000, b'\0\x02\xb0\x12\0\x01\xc1\0\0' + b'\xe1\x00\xf0\x00' +
                     b'\x1b\xe1\x00\xf0\x00' + b'\0' * 4)
        data = bytearray(pat + pmt + packet(0x100, pes(0xe0, 0)) + packet(0x100, pes(0xe0, 900000)))
        data[188] = 0x47  # Inside the first packet's stuffing. The last PTS is in the last packet.
        self.assertEqual(self.probe(bytes(data)), (10, 'h264'))

    def test_bluray_stream_types_need_an_hdmv_registration(self):
        def pmt(registration):
            info = b'\x05\x04HDMV' if registration else b''
            streams = b'\x80\xe1\x00\xf0\x00' + b'\x81\xe1\x01\xf0\x00'
            section = (b'\0\x01\xc1\0\0' + b'\xe1\x00' + struct.pack('>H', 0xf000 | len(info)) +
                       info + streams)
            return ts_packet(0x1000, b'\0\x02' + struct.pack('>H', 0xb000 | len(section) + 4) +
                             section + b'\0' * 4)
        pat = ts_packet(0, b'\0\x00\xb0\x0d\0\x01\xc1\0\0' + b'\0\x01\xf0\x00' + b'\0' * 4)
        timing = b''.join(ts_packet(pid, pes(0xbd, pts)) for pts in (0, 900000) for pid in (0x100, 0x101))
        # Outside Blu-ray, 0x80 is MPEG-2 video in DigiCipher II, so only the AC-3 stream counts.
        self.assertEqual(self.probe(pat + pmt(False) + timing), (10, 'ac3'))
        self.assertEqual(self.probe(pat + pmt(True) + timing), (10, 'pcm_bluray'))

    def test_h264_in_mpeg_ps_is_not_mpeg_video(self):
        pack = b'\0\0\1\xba\x44\x00\x04\x00\x04\x01\x01\x89\xc3\xf8'
        h264 = b'\0\0\0\1\x09\xf0\0\0\0\1\x67\x64\x00\x1e'
        sequence = b'\0\0\1\xb3\x04\x00\x30\x13\xff\xff\xe0\x18'
        for payload, codec in ((h264, ''), (sequence, 'mpeg1video')):
            with self.subTest(codec=codec):
                data = pack + pes(0xe0, 0, payload) + pack + pes(0xe0, 900000, payload) + b'\0\0\1\xb9'
                self.assertEqual(self.probe(data), (10, codec))

    def test_avi_written_to_a_pipe(self):
        # FFmpeg cannot seek back: no RIFF size, no avih frame count, a placeholder strh length.
        avih = struct.pack('<14I', 40000, 0, 0, 0, 0, 0, 1, 0, 64, 48, 0, 0, 0, 0)
        strh = b'vidsXVID' + struct.pack('<IHHIIIIIIII4H', 0, 0, 0, 0, 1, 25, 0, 0x40000000, 0, 0, 0,
                                         0, 0, 0, 0)
        strf = struct.pack('<IiiHH', 40, 64, 48, 1, 24) + b'XVID' + b'\0' * 20
        strl = chunk(b'LIST', b'strl' + chunk(b'strh', strh) + chunk(b'strf', strf))
        body = b'AVI ' + chunk(b'LIST', b'hdrl' + chunk(b'avih', avih) + strl) + chunk(b'LIST', b'movi')
        self.assertEqual(self.probe(b'RIFF\xff\xff\xff\xff' + body), (0, 'mpeg4'))

    def test_chained_ogg_has_no_duration(self):
        def link(serial, seconds):
            def page(kind, granule, packet):
                return (b'OggS\0' + bytes((kind,)) + struct.pack('<QIII', granule, serial, 0, 0) +
                        bytes((1, len(packet))) + packet)
            head = b'OpusHead\x01\x01' + struct.pack('<HIhB', 0, 48000, 0, 0)
            return (page(2, 0, head) + page(0, 0, b'OpusTags' + struct.pack('<II', 0, 0)) +
                    page(4, 48000 * seconds, b'\xf8'))
        self.assertEqual(self.probe(link(7, 5)), (5, 'opus'))
        self.assertEqual(self.probe(link(7, 5) + link(8, 7)), (0, 'opus'))

    def test_joined_mp3_files_with_an_info_header(self):
        def part(frames):
            frame = b'\xff\xfb\x90\x64' + b'\0' * 413  # 417 bytes, 1,152 samples at 44.1 kHz.
            info = (b'\xff\xfb\x90\x64' + b'\0' * 32 + b'Info' +
                    struct.pack('>3I', 3, frames, 417 * (frames + 1)))
            return info + b'\0' * (417 - len(info)) + frame * frames
        self.assertEqual(self.probe(part(250)), (7, 'mp3'))
        # The second part's frames are counted, as FFmpeg does past 1/16 more than the header says.
        self.assertEqual(self.probe(part(250) + part(250)), (13, 'mp3'))

    def test_flv_keyframe_index_longer_than_the_value_cap(self):
        def number(key, value):
            return struct.pack('>H', len(key)) + key + b'\0' + struct.pack('>d', value)
        def array(key, count):
            return (struct.pack('>H', len(key)) + key + b'\x0a' + struct.pack('>I', count) +
                    (b'\0' + struct.pack('>d', 1.0)) * count)
        keyframes = b'\x03' + array(b'times', 5000) + array(b'filepositions', 5000) + b'\0\0\x09'
        meta = (b'\x02\0\x0aonMetaData\x08' + struct.pack('>I', 3) + number(b'duration', 200.0) +
                number(b'videocodecid', 2.0) + struct.pack('>H', 9) + b'keyframes' + keyframes +
                b'\0\0\x09')
        tag = b'\x12' + len(meta).to_bytes(3, 'big') + b'\0' * 7 + meta
        video = b'\x09' + (1).to_bytes(3, 'big') + b'\0' * 7 + b'\x12'
        data = (b'FLV\x01\x01\0\0\0\x09\0\0\0\0' + tag + struct.pack('>I', len(tag)) + video +
                struct.pack('>I', len(video)))
        self.assertEqual(self.probe(data), (200, 'flv1'))

    def test_avi_ignores_dmlh_counting_every_chunk(self):
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
