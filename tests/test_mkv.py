# SPDX-License-Identifier: BSD-3-Clause
"""mkv.c on the host, sanitizers on: what it writes, by our own EBML reader and by ffmpeg."""
import random
import re
import shutil
import struct
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
WIDTH, HEIGHT, FPS, SECONDS = 320, 180, 30, 6

EBML, SEGMENT, SEEK_HEAD, SEEK, SEEK_ID, SEEK_POSITION = 0x1A45DFA3, 0x18538067, 0x114D9B74, 0x4DBB, 0x53AB, 0x53AC
INFO, DURATION, TRACKS, TRACK_ENTRY, CODEC_ID, CODEC_PRIVATE = 0x1549A966, 0x4489, 0x1654AE6B, 0xAE, 0x86, 0x63A2
DEFAULT_DURATION, VIDEO, PIXEL_WIDTH, PIXEL_HEIGHT = 0x23E383, 0xE0, 0xB0, 0xBA
CLUSTER, TIMESTAMP, SIMPLE_BLOCK, CUES, CUE_POINT, CUE_TIME = 0x1F43B675, 0xE7, 0xA3, 0x1C53BB6B, 0xBB, 0xB3
CUE_TRACK_POSITIONS, CUE_TRACK, CUE_CLUSTER_POSITION, VOID = 0xB7, 0xF7, 0xF1, 0xEC
TOP_LEVEL = {SEEK_HEAD, INFO, TRACKS, CLUSTER, CUES, VOID}
ERR_WRITE, ERR_STREAM, ERR_ROOM = 1, 2, 3
MEMORY = 4 << 20


def ebml_number(data, pos, keep_marker):
    """A variable-length number at @pos: (value, its width); value None for all ones, the unknown size."""
    first = data[pos]
    assert first != 0, f"no EBML number at {pos}"
    width = 9 - first.bit_length()
    value = int.from_bytes(data[pos:pos + width], "big")
    if keep_marker:
        return value, width
    value &= (1 << (7 * width)) - 1
    return (None if value == (1 << (7 * width)) - 1 else value), width


def element(data, pos):
    """The element at @pos: (id, size or None, where its body starts)."""
    eid, w1 = ebml_number(data, pos, True)
    size, w2 = ebml_number(data, pos + w1, False)
    return eid, size, pos + w1 + w2


def children(data, start, end):
    """The elements in data[start:end]: a list of (id, body start, size)."""
    out, pos = [], start
    while pos < end:
        eid, size, body = element(data, pos)
        assert size is not None, f"unknown size inside a known one at {pos}"
        assert body + size <= end, f"element {eid:x} at {pos} runs past its parent"
        out.append((eid, body, size))
        pos = body + size
    return out


def one(items, eid):
    """The only element of an id among @items."""
    found = [i for i in items if i[0] == eid]
    assert len(found) == 1, f"{len(found)} of element {eid:x}"
    return found[0]


def number(data, item):
    """An unsigned element's value."""
    return int.from_bytes(data[item[1]:item[1] + item[2]], "big")


class File:
    """A file read by the rules: everything at the Segment's level, every cluster's blocks."""

    def __init__(self, data):
        self.data = data
        eid, size, body = element(data, 0)
        assert eid == EBML
        self.doctype = data[body:body + size]
        eid, size, self.segment = element(data, body + size)
        assert eid == SEGMENT and size is None, "the Segment stays of unknown size"
        self.top, self.clusters = [], []
        pos = self.segment
        while pos < len(data):
            try:
                eid, size, body = element(data, pos)
            except (IndexError, AssertionError):
                break  # the cut
            if eid == CLUSTER:
                assert size is None, "a Cluster stays of unknown size"
                pos = self.cluster(pos, body)
                continue
            assert eid in TOP_LEVEL, f"unexpected element {eid:x} at {pos}"
            self.top.append((eid, body, size, pos))
            pos = body + size

    def cluster(self, start, pos):
        """A cluster's timestamp and blocks: its elements up to the next top-level one, or the end."""
        c = {"pos": start, "blocks": [], "time": None}
        while pos < len(self.data):
            try:
                eid, size, body = element(self.data, pos)
            except (IndexError, AssertionError):
                pos = len(self.data)
                break
            if eid in TOP_LEVEL:
                break
            if eid == TIMESTAMP:
                c["time"] = int.from_bytes(self.data[body:body + size], "big")
            else:
                assert eid == SIMPLE_BLOCK, f"unexpected element {eid:x} in a cluster"
                if body + size > len(self.data):
                    pos = len(self.data)  # a block cut short
                    break
                assert self.data[body] == 0x81, "track 1"
                rel, flags = struct.unpack(">hB", self.data[body + 1:body + 4])
                c["blocks"].append({"ms": c["time"] + rel, "key": bool(flags & 0x80), "body": self.data[body + 4:body + size]})
            pos = body + size
        self.clusters.append(c)
        return pos

    def top_one(self, eid):
        """The only top-level element of an id: (id, body, size, position)."""
        found = [t for t in self.top if t[0] == eid]
        assert len(found) == 1, f"{len(found)} of top-level {eid:x}"
        return found[0]

    def seek_targets(self):
        """What the SeekHead points at: {id: file position}."""
        head = self.top_one(SEEK_HEAD)
        out = {}
        for item in children(self.data, head[1], head[1] + head[2]):
            assert item[0] == SEEK
            parts = children(self.data, item[1], item[1] + item[2])
            sid = one(parts, SEEK_ID)
            target = int.from_bytes(self.data[sid[1]:sid[1] + sid[2]], "big")
            out[target] = self.segment + number(self.data, one(parts, SEEK_POSITION))
        return out

    def cues(self):
        """The Cues: a list of (time, file position of the cluster)."""
        c = self.top_one(CUES)
        out = []
        for point in children(self.data, c[1], c[1] + c[2]):
            assert point[0] == CUE_POINT
            parts = children(self.data, point[1], point[1] + point[2])
            track = one(parts, CUE_TRACK_POSITIONS)
            inner = children(self.data, track[1], track[1] + track[2])
            assert number(self.data, one(inner, CUE_TRACK)) == 1
            out.append((number(self.data, one(parts, CUE_TIME)), self.segment + number(self.data, one(inner, CUE_CLUSTER_POSITION))))
        return out

    def duration(self):
        """The Duration in ms, None while it is still a Void."""
        info = self.top_one(INFO)
        found = [i for i in children(self.data, info[1], info[1] + info[2]) if i[0] == DURATION]
        return struct.unpack(">d", self.data[found[0][1]:found[0][1] + 8])[0] if found else None

    def blocks(self):
        """Every block, in the file's order."""
        return [b for c in self.clusters for b in c["blocks"]]


def nal_units(block):
    """The NAL units of a block: lengths, as the file has them."""
    out, pos = [], 0
    while pos < len(block):
        size = int.from_bytes(block[pos:pos + 4], "big")
        assert size != 0 and pos + 4 + size <= len(block), f"a bad length at {pos}"
        out.append(block[pos + 4:pos + 4 + size])
        pos += 4 + size
    return out


def run(cmd, **kw):
    """A tool's completed process."""
    return subprocess.run([str(c) for c in cmd], capture_output=True, **kw)


@pytest.fixture(scope="module")
def host(tmp_path_factory):
    """mkv_host, built with the sanitizers: a stray byte fails the test."""
    cc = shutil.which("cc")
    if cc is None:
        pytest.skip("no host C compiler")
    exe = tmp_path_factory.mktemp("mkv") / "mkv_host"
    r = run([cc, "-std=c99", "-g", "-O1", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
             f"-I{ROOT}", Path(__file__).parent / "mkv_host.c", ROOT / "mkv.c", "-o", exe])
    assert r.returncode == 0, r.stderr.decode()
    return exe


@pytest.fixture(scope="module")
def stream(tmp_path_factory):
    """Six seconds of H.264 like the encoder's: no B frames, one slice, a key frame a second; (raw file, pictures).

    Each picture: its time in ms, whether it is a key frame, its Annex-B with four-byte start codes.
    """
    if shutil.which("ffmpeg") is None:
        pytest.skip("no ffmpeg")
    raw = tmp_path_factory.mktemp("h264") / "clip.h264"
    r = run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", f"testsrc2=s={WIDTH}x{HEIGHT}:r={FPS}:d={SECONDS}", "-c:v", "libx264", "-pix_fmt",
             "yuv420p", "-profile:v", "high", "-bf", "0", "-g", str(FPS), "-x264-params", "slices=1:scenecut=0", "-f", "h264", raw])
    if r.returncode != 0:
        pytest.skip(f"no libx264: {r.stderr.decode()[-200:]}")
    data = raw.read_bytes()
    starts = [m.start() for m in re.finditer(b"\x00\x00\x01", data)]
    pictures, current, key = [], b"", False
    for i, at in enumerate(starts):
        end = starts[i + 1] if i + 1 < len(starts) else len(data)
        nal = data[at + 3:end].rstrip(b"\x00")
        current += b"\x00\x00\x00\x01" + nal
        kind = nal[0] & 0x1F
        key = key or kind == 5
        if kind in (1, 5):
            pictures.append((len(pictures) * 1000 // FPS, key, current))
            current, key = b"", False
    assert len(pictures) == FPS * SECONDS
    return raw, pictures


def script(path, pictures, pieces=None):
    """The recorder's calls for @pictures; @pieces(bytes) cuts a picture up, whole without it."""
    with open(path, "wb") as f:
        for ms, key, data in pictures:
            parts = pieces(data) if pieces else [data]
            f.write(struct.pack("<III", ms, 1 if key else 0, len(parts)))
            for p in parts:
                f.write(struct.pack("<I", len(p)) + p)
    return path


def mux(host, tmp_path, pictures, *options, pieces=None, name="out.mkv", expect=0, width=WIDTH, height=HEIGHT, fps=FPS):
    """The file mkv_host writes for @pictures."""
    out = tmp_path / name
    r = run([host, script(tmp_path / f"{name}.script", pictures, pieces), out, width, height, fps, *options])
    assert r.returncode == expect, f"exit {r.returncode}: {r.stdout.decode()}{r.stderr.decode()[-1500:]}"
    return out


def probe(path):
    """Each packet as ffprobe has it: (ms, key)."""
    out = run(["ffprobe", "-v", "error", "-select_streams", "v", "-show_entries", "packet=pts_time,flags", "-of", "csv=p=0", path]).stdout
    packets = []
    for line in out.decode().split():
        pts, flags = line.split(",")[:2]
        packets.append((round(float(pts) * 1000), flags.startswith("K")))
    return packets


def frame_md5(*args):
    """ffmpeg's hash of each decoded picture, and what it complained of."""
    r = run(["ffmpeg", "-v", "error", *args, "-f", "framemd5", "-fps_mode", "passthrough", "-"])
    return [line.rsplit(",", 1)[1].strip() for line in r.stdout.decode().splitlines() if not line.startswith("#")], r.stderr.decode()


def tiny_pictures(count, size=32):
    """@count key frames of nonsense, parameter sets in the first: for the file's own structure."""
    sps = b"\x00\x00\x00\x01" + bytes([0x67, 100, 0, 40]) + b"\x11" * 16
    pps = b"\x00\x00\x00\x01" + bytes([0x68, 0xEE, 0x38, 0x80])
    picture = b"\x00\x00\x00\x01\x65" + b"\x55" * size
    return [(i * 2000, True, (sps + pps if i == 0 else b"") + picture) for i in range(count)]


def test_ffmpeg_reads_every_picture_the_same(host, tmp_path, stream):
    """Decoded from the file, each picture hashes as it does from the raw stream, and ffmpeg has nothing to complain of."""
    raw, pictures = stream
    out = mux(host, tmp_path, pictures)
    want, _ = frame_md5("-i", raw)
    got, errors = frame_md5("-i", out)
    assert errors == "", errors
    assert got == want and len(got) == len(pictures)


def test_ffprobe_sees_the_track(host, tmp_path, stream):
    """H.264, the size, the rate, the duration; every packet's time and key flag as given."""
    _, pictures = stream
    out = mux(host, tmp_path, pictures)
    r = run(["ffprobe", "-v", "error", "-select_streams", "v", "-show_entries", "stream=codec_name,width,height,avg_frame_rate:format=duration",
             "-of", "default=nw=1", out])
    fields = dict(line.split("=") for line in r.stdout.decode().split())
    assert fields["codec_name"] == "h264" and (int(fields["width"]), int(fields["height"])) == (WIDTH, HEIGHT)
    assert fields["avg_frame_rate"] == "30/1"
    assert abs(float(fields["duration"]) - SECONDS) < 0.05
    assert probe(out) == [(ms, key) for ms, key, _ in pictures]


def test_structure_by_the_rules(host, tmp_path, stream):
    """By our own reader: SeekHead and Cues point at what they say, the avcC is right, no parameter sets in the stream."""
    _, pictures = stream
    f = File(mux(host, tmp_path, pictures).read_bytes())
    assert b"matroska" in f.doctype
    targets = f.seek_targets()
    for eid in (INFO, TRACKS, CUES):
        assert targets[eid] == f.top_one(eid)[3], f"the SeekHead misses {eid:x}"
    assert [c["pos"] for c in f.clusters] == [pos for _, pos in f.cues()]
    assert [c["time"] for c in f.clusters] == [ms for ms, _ in f.cues()] == [ms for ms, key, _ in pictures if key]
    for _, pos in f.cues():
        assert f.data[pos:pos + 4] == struct.pack(">I", CLUSTER)
    assert f.duration() == pictures[-1][0] + 1000 // FPS

    tracks = f.top_one(TRACKS)
    entry = one(children(f.data, tracks[1], tracks[1] + tracks[2]), TRACK_ENTRY)
    parts = children(f.data, entry[1], entry[1] + entry[2])
    codec = one(parts, CODEC_ID)
    assert f.data[codec[1]:codec[1] + codec[2]] == b"V_MPEG4/ISO/AVC"
    assert number(f.data, one(parts, DEFAULT_DURATION)) == 1000000000 // FPS
    video = one(parts, VIDEO)
    inner = children(f.data, video[1], video[1] + video[2])
    assert (number(f.data, one(inner, PIXEL_WIDTH)), number(f.data, one(inner, PIXEL_HEIGHT))) == (WIDTH, HEIGHT)

    avcc = f.data[one(parts, CODEC_PRIVATE)[1]:]
    first = pictures[0][2]
    sps = next(n for n in first.split(b"\x00\x00\x00\x01") if n and n[0] & 0x1F == 7)
    pps = next(n for n in first.split(b"\x00\x00\x00\x01") if n and n[0] & 0x1F == 8)
    assert avcc[:6] == bytes([1, sps[1], sps[2], sps[3], 0xFF, 0xE1])
    assert avcc[6:8 + len(sps)] == struct.pack(">H", len(sps)) + sps
    at = 8 + len(sps)
    assert avcc[at:at + 3 + len(pps)] == b"\x01" + struct.pack(">H", len(pps)) + pps
    assert avcc[at + 3 + len(pps):at + 7 + len(pps)] == b"\xFD\xF8\xF8\x00"  # High: 4:2:0, 8 bits

    blocks = f.blocks()
    assert len(blocks) == len(pictures)
    for block, (ms, key, data) in zip(blocks, pictures):
        assert (block["ms"], block["key"]) == (ms, key)
        kinds = [n[0] & 0x1F for n in nal_units(block["body"])]
        assert 7 not in kinds and 8 not in kinds, "parameter sets stay in the avcC"
        assert kinds[-1] in (1, 5)


def test_pieces_change_nothing(host, tmp_path, stream):
    """A picture in pieces of any size writes the same file as a picture whole."""
    _, pictures = stream
    rng = random.Random(1)

    def pieces(data):
        out = []
        while data:
            n = rng.choice((1, 2, 3, 5, 64, 4096, 65536))
            out.append(data[:n])
            data = data[n:]
        return out

    whole = mux(host, tmp_path, pictures, name="whole.mkv").read_bytes()
    assert mux(host, tmp_path, pictures, pieces=pieces, name="cut.mkv").read_bytes() == whole


@pytest.mark.parametrize("seconds", [1.0, 2.5, 4.0, 5.5])
def test_seeking(host, tmp_path, stream, seconds):
    """Seeking lands on the right picture: a decode from there matches the decode from the start."""
    raw, pictures = stream
    out = mux(host, tmp_path, pictures)
    want, _ = frame_md5("-i", raw)
    got, errors = frame_md5("-ss", str(seconds), "-i", out, "-frames:v", "3")
    assert errors == "", errors
    at = want.index(got[0])
    assert abs(at - seconds * FPS) <= 1 and got == want[at:at + 3], f"{seconds} s landed on picture {at}"


@pytest.mark.parametrize("cut", [0.25, 0.5, 0.75, 0.9, 0.999])
def test_a_cut_file_plays_to_the_cut(host, tmp_path, stream, cut):
    """A crash leaves no Cues and no Duration, and a file that plays up to where it stops."""
    raw, pictures = stream
    data = mux(host, tmp_path, pictures).read_bytes()
    cues_at = File(data).top_one(CUES)[3]
    short = tmp_path / "short.mkv"
    short.write_bytes(data[:int(cues_at * cut)])
    want, _ = frame_md5("-i", raw)
    got, _ = frame_md5("-i", short)
    assert len(got) >= int(len(want) * cut) - FPS, f"{len(got)} of {len(want)} pictures"
    assert got == want[:len(got)], "the pictures it does play are the right ones"
    assert File(short.read_bytes()).blocks(), "our reader finds the blocks too"


def test_a_long_pause_opens_a_cluster(host, tmp_path, stream):
    """Over a block's 16 bits of time: a new cluster with no key frame and no cue, the times right."""
    _, pictures = stream
    late = [(ms if i < 100 else ms + 40000, key, data) for i, (ms, key, data) in enumerate(pictures)]
    out = mux(host, tmp_path, late)
    f = File(out.read_bytes())
    assert [b["ms"] for b in f.blocks()] == [ms for ms, _, _ in late]
    opened = [c["time"] for c in f.clusters if not c["blocks"][0]["key"]]
    assert opened == [late[100][0]], "the pause opens one without a key frame, and nothing else does"
    assert [ms for ms, _ in f.cues()] == [ms for ms, key, _ in late if key]
    assert probe(out) == [(ms, key) for ms, key, _ in late]


def test_a_full_disk(host, tmp_path, stream):
    """Mid-picture: the error reported, no Cues sought, the Duration still in, and it plays to the cut."""
    raw, pictures = stream
    total = len(mux(host, tmp_path, pictures).read_bytes())
    out = mux(host, tmp_path, pictures, f"full={total // 2}", name="full.mkv", expect=ERR_WRITE)
    data = out.read_bytes()
    assert len(data) == total // 2
    f = File(data)
    assert f.duration() is not None
    assert not [t for t in f.top if t[0] in (SEEK_HEAD, CUES)]
    want, _ = frame_md5("-i", raw)
    got, _ = frame_md5("-i", out)
    assert len(got) > len(want) // 3 and got == want[:len(got)]


def test_nothing_recorded_writes_nothing(host, tmp_path):
    """No picture: an empty file, which the recorder deletes."""
    assert mux(host, tmp_path, []).read_bytes() == b""


def test_parameter_sets_on_their_own(host, tmp_path, stream):
    """If the firmware ever sends SPS and PPS as a packet of their own: the same file, byte for byte."""
    _, pictures = stream
    ms, key, first = pictures[0]
    nals = [b"\x00\x00\x00\x01" + n for n in first.split(b"\x00\x00\x00\x01") if n]
    sets = b"".join(n for n in nals if n[4] & 0x1F in (7, 8))
    rest = b"".join(n for n in nals if n[4] & 0x1F not in (7, 8))
    together = mux(host, tmp_path, pictures, name="together.mkv").read_bytes()
    apart = mux(host, tmp_path, [(0, True, sets), (ms, key, rest)] + pictures[1:], name="apart.mkv").read_bytes()
    assert apart == together


@pytest.mark.parametrize("picture", [
    b"\x65\x88\x84",  # no start code
    b"\x00\x01\x65\x88\x84",  # half of one
    b"\x00\x00\x00\x01",  # a start code and nothing after it
    b"\x00\x00\x01\x65\x88\x00\x00\x00\x01",  # the same at the end
], ids=["none", "half", "empty", "empty-last"])
def test_not_annex_b(host, tmp_path, picture):
    """What is not Annex-B is an error, not a broken file."""
    mux(host, tmp_path, tiny_pictures(1) + [(33, False, picture)], expect=ERR_STREAM)


def recoded(pictures, start_code):
    """@pictures with every start code rewritten by @start_code(index of the NAL unit in its picture)."""
    out = []
    for ms, key, data in pictures:
        nals = [n for n in data.split(b"\x00\x00\x00\x01") if n]
        out.append((ms, key, b"".join(start_code(i) + n for i, n in enumerate(nals))))
    return out


@pytest.mark.parametrize("start_code", [
    lambda i: b"\x00\x00\x01",
    lambda i: b"\x00\x00\x01" if i % 2 else b"\x00\x00\x00\x01",
    lambda i: b"\x00\x00\x00\x01" if i % 2 else b"\x00\x00\x01",
    lambda i: b"\x00\x00\x00\x00\x00\x01",
], ids=["three", "three-after-four", "four-after-three", "padded"])
def test_start_codes_of_any_length(host, tmp_path, stream, start_code):
    """Three-byte start codes, mixed ones and zero padding write the same file as four-byte ones, byte for byte."""
    _, pictures = stream
    want = mux(host, tmp_path, pictures, name="four.mkv").read_bytes()
    assert mux(host, tmp_path, recoded(pictures, start_code), name="other.mkv").read_bytes() == want


def test_three_byte_start_codes_in_pieces(host, tmp_path, stream):
    """A start code cut in two by the packets changes nothing either."""
    _, pictures = stream
    want = mux(host, tmp_path, pictures, name="four.mkv").read_bytes()
    three = recoded(pictures, lambda i: b"\x00\x00\x01")
    pieces = lambda d: [d[i:i + 2] for i in range(0, len(d), 2)] if len(d) < 2000 else [d]
    assert mux(host, tmp_path, three, pieces=pieces, name="three.mkv").read_bytes() == want


def test_padding_after_a_picture(host, tmp_path, stream):
    """Zero bytes after the last NAL unit are not part of it."""
    _, pictures = stream
    want = mux(host, tmp_path, pictures, name="clean.mkv").read_bytes()
    padded = [(ms, key, data + bytes(7)) for ms, key, data in pictures]
    assert mux(host, tmp_path, padded, name="padded.mkv").read_bytes() == want


def many_nals(count, size, code):
    """A key frame after parameter sets, of @count NAL units of @size bytes, with @code as each start code."""
    return code + bytes([0x67, 100, 0, 40]) + b"\x11" * 16 + code + bytes([0x68, 0xEE, 0x38, 0x80]) \
        + b"".join(code + b"\x65" + bytes([i % 200 + 2]) * (size - 1) for i in range(count))


def test_three_byte_start_codes_at_the_end_of_the_buffer(host, tmp_path):
    """Growing into the last of the memory closes the buffer up, and all of it lands in the right place."""
    code = b"\x00\x00\x01"
    pictures = [(i * 40, True, many_nals(3000, 1024, code)) for i in range(3)]
    f = File(mux(host, tmp_path, pictures).read_bytes())
    assert len(f.blocks()) == 3
    for block in f.blocks():
        nals = nal_units(block["body"])
        assert len(nals) == 3000 and all(n == b"\x65" + bytes([i % 200 + 2]) * 1023 for i, n in enumerate(nals))


def test_a_picture_that_cannot_grow_is_refused(host, tmp_path):
    """It fits as Annex-B, but not once each start code takes a byte more: the error, and the file before it stays."""
    target, count = MEMORY - MEMORY // 8 - 1000, 40000  # it fits with 1000 bytes to spare, and each unit wants one more
    size = target // count - 3
    last = b"\x55" * (target - count * (size + 3))
    pictures = tiny_pictures(1) + [(2000, True, b"".join(b"\x00\x00\x01\x65" + b"\x55" * (size - 1) for _ in range(count)) + last)]
    cut = lambda d: [d[i:i + 65536] for i in range(0, len(d), 65536)]
    f = File(mux(host, tmp_path, pictures, pieces=cut, expect=ERR_ROOM).read_bytes())
    assert len(f.blocks()) == 1


@pytest.fixture(scope="module")
def jpegs(tmp_path_factory):
    """Six seconds of MJPEG: (raw file, pictures), each picture its time in ms, False for no key flag given, its JPEG."""
    if shutil.which("ffmpeg") is None:
        pytest.skip("no ffmpeg")
    raw = tmp_path_factory.mktemp("mjpeg") / "clip.mjpeg"
    r = run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", f"testsrc2=s={WIDTH}x{HEIGHT}:r={FPS}:d={SECONDS}", "-c:v", "mjpeg", "-q:v", "5",
             "-pix_fmt", "yuvj422p", "-f", "mjpeg", raw])
    assert r.returncode == 0, r.stderr.decode()
    parts = raw.read_bytes().split(b"\xFF\xD9\xFF\xD8")
    frames = [(b"" if i == 0 else b"\xFF\xD8") + p + (b"" if i == len(parts) - 1 else b"\xFF\xD9") for i, p in enumerate(parts)]
    assert len(frames) == FPS * SECONDS
    return raw, [(i * 1000 // FPS, False, f) for i, f in enumerate(frames)]


def test_mjpeg_decodes_the_same(host, tmp_path, jpegs):
    """Each picture decodes from the file as it does from the raw stream, and ffmpeg has nothing to complain of."""
    raw, pictures = jpegs
    out = mux(host, tmp_path, pictures, "mjpeg")
    want, _ = frame_md5("-f", "mjpeg", "-framerate", str(FPS), "-i", raw)
    got, errors = frame_md5("-i", out)
    assert errors == "", errors
    assert got == want and len(got) == len(pictures)


def test_mjpeg_track(host, tmp_path, jpegs):
    """MJPEG, the size, the rate; every packet's time, and every one a key frame though the caller did not say."""
    _, pictures = jpegs
    out = mux(host, tmp_path, pictures, "mjpeg")
    r = run(["ffprobe", "-v", "error", "-select_streams", "v", "-show_entries", "stream=codec_name,width,height,avg_frame_rate", "-of",
             "default=nw=1", out])
    fields = dict(line.split("=") for line in r.stdout.decode().split())
    assert fields["codec_name"] == "mjpeg" and (int(fields["width"]), int(fields["height"])) == (WIDTH, HEIGHT)
    assert fields["avg_frame_rate"] == "30/1"
    assert probe(out) == [(ms, True) for ms, _, _ in pictures]


def test_mjpeg_structure(host, tmp_path, jpegs):
    """V_MJPEG with no avcC, the JPEGs as they came, a cluster and a cue a second, the Cues pointing at the clusters."""
    _, pictures = jpegs
    f = File(mux(host, tmp_path, pictures, "mjpeg").read_bytes())
    tracks = f.top_one(TRACKS)
    entry = one(children(f.data, tracks[1], tracks[1] + tracks[2]), TRACK_ENTRY)
    parts = children(f.data, entry[1], entry[1] + entry[2])
    codec = one(parts, CODEC_ID)
    assert f.data[codec[1]:codec[1] + codec[2]] == b"V_MJPEG"
    assert not [p for p in parts if p[0] == CODEC_PRIVATE]
    assert [b["body"] for b in f.blocks()] == [data for _, _, data in pictures]
    assert all(b["key"] for b in f.blocks())
    assert [c["pos"] for c in f.clusters] == [pos for _, pos in f.cues()]
    assert [c["time"] for c in f.clusters] == [ms for ms, _ in f.cues()]
    assert len(f.clusters) == SECONDS and all(b["time"] - a["time"] <= 1033 for a, b in zip(f.clusters, f.clusters[1:]))
    assert f.duration() == pictures[-1][0] + 1000 // FPS


@pytest.mark.parametrize("seconds", [1.0, 2.5, 4.0, 5.5])
def test_mjpeg_seeking(host, tmp_path, jpegs, seconds):
    """Seeking lands on the second it asked for."""
    raw, pictures = jpegs
    out = mux(host, tmp_path, pictures, "mjpeg")
    want, _ = frame_md5("-f", "mjpeg", "-framerate", str(FPS), "-i", raw)
    got, errors = frame_md5("-ss", str(seconds), "-i", out, "-frames:v", "3")
    assert errors == "", errors
    at = want.index(got[0])
    assert abs(at - seconds * FPS) <= FPS and got == want[at:at + 3], f"{seconds} s landed on picture {at}"


def test_mjpeg_cut_short(host, tmp_path, jpegs):
    """A crash leaves a file that plays up to where it stops."""
    raw, pictures = jpegs
    data = mux(host, tmp_path, pictures, "mjpeg").read_bytes()
    short = tmp_path / "short.mkv"
    short.write_bytes(data[:File(data).top_one(CUES)[3] // 2])
    want, _ = frame_md5("-f", "mjpeg", "-framerate", str(FPS), "-i", raw)
    got, _ = frame_md5("-i", short)
    assert len(got) >= len(want) // 3 and got == want[:len(got)]


def test_mjpeg_nothing_recorded_writes_nothing(host, tmp_path):
    """No picture: an empty file, as for H.264."""
    assert mux(host, tmp_path, [], "mjpeg").read_bytes() == b""


def test_writing_app(host, tmp_path, stream):
    """The header says who wrote it."""
    _, pictures = stream
    data = mux(host, tmp_path, pictures[:2]).read_bytes()
    assert b"mkv-writer" in data[:512] and b"mkv_host" in data[:512]


@pytest.mark.parametrize("count", [6, 7, 13, 14, 15, 862, 863, 864])
def test_cues_of_any_size(host, tmp_path, count):
    """Cues whose size crosses the one and two byte widths, where libvpx's own size writer goes wrong."""
    f = File(mux(host, tmp_path, tiny_pictures(count)).read_bytes())
    assert len(f.cues()) == count
    assert [pos for _, pos in f.cues()] == [c["pos"] for c in f.clusters]


def test_a_picture_larger_than_the_buffer_is_refused(host, tmp_path):
    """It cannot fit: the error, not a stray write, and the picture before it stays in a finished file."""
    pictures = tiny_pictures(2)
    big = (2000, True, pictures[1][2] + b"\x00\x00\x00\x01\x41" + b"\x55" * (5 << 20))
    out = mux(host, tmp_path, [pictures[0], big], pieces=lambda d: [d[i:i + 65536] for i in range(0, len(d), 65536)], expect=3)
    f = File(out.read_bytes())
    assert len(f.blocks()) == 1 and [pos for _, pos in f.cues()] == [f.clusters[0]["pos"]]


def test_a_bad_picture_costs_only_itself(host, tmp_path):
    """The whole pictures still waiting reach the file, with their Cues and Duration; the error still comes back."""
    pictures = tiny_pictures(1)
    payload = [bytes([0x65]) + bytes([i + 2]) * (700 << 10) for i in range(3)]
    pictures += [((i + 1) * 2000, True, b"\x00\x00\x00\x01" + p) for i, p in enumerate(payload)]
    f = File(mux(host, tmp_path, pictures + [(8000, True, b"\x65\x88\x84")], expect=ERR_STREAM).read_bytes())
    assert [nal_units(b["body"])[0] for b in f.blocks()[1:]] == payload
    assert [pos for _, pos in f.cues()] == [c["pos"] for c in f.clusters]
    assert f.duration() == pictures[-1][0] + 1000 // FPS


def test_a_picture_never_ended(host, tmp_path):
    """The run stops between a key frame's packets: the picture is dropped, and no cue is left pointing at the Cues."""
    pictures = tiny_pictures(4)
    f = File(mux(host, tmp_path, pictures, "unfinished").read_bytes())
    assert len(f.blocks()) == 3
    assert [(ms, pos) for ms, pos in f.cues()] == [(c["time"], c["pos"]) for c in f.clusters]
    assert f.duration() == pictures[2][0] + 1000 // FPS


def test_pictures_larger_than_a_write(host, tmp_path):
    """Pictures over the bounded write, and many of them: the buffer closes up and nothing is lost or reordered."""
    pictures = tiny_pictures(1)
    payload = [bytes([0x41]) + bytes([i % 200 + 2]) * (700 << 10) for i in range(40)]
    pictures += [(100 + i * 33, False, b"\x00\x00\x00\x01" + p) for i, p in enumerate(payload)]
    f = File(mux(host, tmp_path, pictures).read_bytes())
    blocks = f.blocks()[1:]
    assert [nal_units(b["body"])[0] for b in blocks] == payload


@pytest.mark.slow
def test_past_two_gib(host, tmp_path):
    """Past 2 GiB, with a seek like PFS3's: cues up there are right, and the close only seeks near the start."""
    pictures = tiny_pictures(1) + [(2000 + i * 2000, True, b"\x00\x00\x00\x01\x65" + bytes([i % 200 + 2]) * (1 << 20)) for i in range(100)]
    out = mux(host, tmp_path, pictures, "repeat=24")
    size = out.stat().st_size
    assert size > 2200 << 20
    with open(out, "rb") as f:
        head = f.read(4096)
        f.seek(-(1 << 20), 2)
        tail = f.read()
    cues_id = tail.rfind(struct.pack(">I", CUES))
    assert cues_id > 0
    points = File(head[:head.index(struct.pack(">I", CLUSTER))])
    targets = points.seek_targets()
    assert targets[CUES] == size - len(tail) + cues_id
    data = tail[cues_id:]
    eid, length, body = element(data, 0)
    assert eid == CUES and body + length == len(data), "the Cues end the file"
    positions = []
    for point in children(data, body, body + length):
        parts = children(data, point[1], point[1] + point[2])
        track = one(parts, CUE_TRACK_POSITIONS)
        inner = children(data, track[1], track[1] + track[2])
        positions.append(points.segment + number(data, one(inner, CUE_CLUSTER_POSITION)))
    assert len(positions) == 101 * 24 and max(positions) > 1 << 31
    with open(out, "rb") as f:
        for pos in positions[::97]:
            f.seek(pos)
            assert f.read(4) == struct.pack(">I", CLUSTER), f"a cue at {pos}"
    out.unlink()
