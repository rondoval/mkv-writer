/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * mkv.c - Matroska muxer for one video track, written as the pictures arrive.
 *
 * The EBML writing and the way clusters and cues are laid out come from
 * libvpx v1.3.0 (third_party/libmkv/EbmlWriter.c, webmenc.c):
 *
 *   Copyright (c) 2010, The WebM Project authors. All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions are
 *   met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of Google, nor the WebM Project, nor the names
 *       of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written
 *       permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * The rest is specific to a recorder:
 *
 * H.264 arrives as Annex-B, with SPS and PPS in or before the first picture.
 * The first SPS and PPS go into the track's avcC (CodecPrivate), so the file
 * header is written only once they have been seen; every SPS and PPS is
 * removed from the stream. Start codes are rewritten to 4-byte NAL lengths in
 * place, and a picture may arrive in any number of pieces. MJPEG and raw
 * pictures are stored as they arrive; each is a key frame, so Clusters and
 * cue points are opened by time instead. A raw track names its pixel format
 * as a FourCC (ColourSpace), as ffmpeg writes and reads it.
 *
 * The Segment and the Clusters are written with unknown size and stay that
 * way, so a file cut short by a crash or a full disk plays up to the cut. (A
 * sized Segment over unknown-size Clusters makes ffmpeg complain, and sizing
 * each Cluster would cost a seek per Cluster.) The Duration and the SeekHead,
 * which points at the Cues, are known only at the end: the header reserves
 * Voids for them, and mkv_close() overwrites those. These are the only seeks.
 *
 * Output is staged in one linear buffer, as offsets in this order:
 *
 *   [0, flushed)          written to the file; reclaimed by compact()
 *   [flushed, committed)  complete pictures, waiting for mkv_flush()
 *   [committed, payload)  the open picture's Cluster header, if it opens one,
 *                         and its SimpleBlock header
 *   [payload, tail)       the open picture's bytes
 *   [tail, capacity)      free
 *
 * mkv_flush() writes in bounded pieces, so a slow disk stalls the caller
 * briefly rather than once for a long write. When the free space runs out,
 * reserve() compacts the buffer and, if that is not enough, writes everything
 * committed first. The buffer is linear rather than a ring so that the open
 * picture stays contiguous for the in-place Annex-B rewrite. Compaction moves
 * the bytes: a pointer into the buffer is valid only until the next reserve().
 */
#include <string.h>

#include "mkv.h"

/* Matroska element IDs */
#define ID_EBML 0x1A45DFA3u
#define ID_EBML_VERSION 0x4286u
#define ID_EBML_READ_VERSION 0x42F7u
#define ID_EBML_MAX_ID_LENGTH 0x42F2u
#define ID_EBML_MAX_SIZE_LENGTH 0x42F3u
#define ID_DOC_TYPE 0x4282u
#define ID_DOC_TYPE_VERSION 0x4287u
#define ID_DOC_TYPE_READ_VERSION 0x4285u
#define ID_VOID 0xECu
#define ID_SEGMENT 0x18538067u
#define ID_SEEK_HEAD 0x114D9B74u
#define ID_SEEK 0x4DBBu
#define ID_SEEK_ID 0x53ABu
#define ID_SEEK_POSITION 0x53ACu
#define ID_INFO 0x1549A966u
#define ID_TIMESTAMP_SCALE 0x2AD7B1u
#define ID_DURATION 0x4489u
#define ID_MUXING_APP 0x4D80u
#define ID_WRITING_APP 0x5741u
#define ID_TRACKS 0x1654AE6Bu
#define ID_TRACK_ENTRY 0xAEu
#define ID_TRACK_NUMBER 0xD7u
#define ID_TRACK_UID 0x73C5u
#define ID_TRACK_TYPE 0x83u
#define ID_FLAG_LACING 0x9Cu
#define ID_DEFAULT_DURATION 0x23E383u
#define ID_CODEC_ID 0x86u
#define ID_CODEC_PRIVATE 0x63A2u
#define ID_VIDEO 0xE0u
#define ID_PIXEL_WIDTH 0xB0u
#define ID_PIXEL_HEIGHT 0xBAu
#define ID_COLOUR_SPACE 0x2EB524u
#define ID_CLUSTER 0x1F43B675u
#define ID_TIMESTAMP 0xE7u
#define ID_SIMPLE_BLOCK 0xA3u
#define ID_CUES 0x1C53BB6Bu
#define ID_CUE_POINT 0xBBu
#define ID_CUE_TIME 0xB3u
#define ID_CUE_TRACK_POSITIONS 0xB7u
#define ID_CUE_TRACK 0xF7u
#define ID_CUE_CLUSTER_POSITION 0xF1u

/* EBML size fields: the leading 1 bit marks the width */
#define SIZE2_MARKER 0x4000u               /* 2-byte size, content up to 0x3FFE */
#define SIZE4_MARKER 0x10000000u           /* 4-byte size */
#define UNKNOWN_SIZE 0x01FFFFFFFFFFFFFFULL /* 8-byte size, all value bits set */

/* Timing */
#define TIMESTAMP_SCALE 1000000u /* ns per tick: timestamps are in ms */
#define CLUSTER_MS 30000u        /* a SimpleBlock's time is a signed 16-bit offset from its Cluster's */
#define CLUSTER_MS_INTRA 1000u   /* every MJPEG or raw picture is a key frame: a Cluster and cue point per second */

/*
 * Elements of fixed layout. The SeekHead has to fit the Void reserved for it,
 * and the Cues' size is written before its CuePoints, so their values use
 * fixed widths. All sizes here fit a 1-byte size field.
 */
#define ELEMENT_BYTES(id_bytes, content) ((id_bytes) + 1u + (content))
#define SEEK_CONTENT (ELEMENT_BYTES(2u, 4u) + ELEMENT_BYTES(2u, 8u))     /* SeekID, SeekPosition */
#define SEEK_HEAD_CONTENT (3u * ELEMENT_BYTES(2u, SEEK_CONTENT))         /* Seeks for Info, Tracks, Cues */
#define SEEK_HEAD_BYTES ELEMENT_BYTES(4u, SEEK_HEAD_CONTENT)
#define CUE_POSITIONS_CONTENT (ELEMENT_BYTES(1u, 1u) + ELEMENT_BYTES(1u, 4u)) /* CueTrack, CueClusterPosition */
#define CUE_POINT_CONTENT (ELEMENT_BYTES(1u, 4u) + ELEMENT_BYTES(1u, CUE_POSITIONS_CONTENT)) /* CueTime, positions */
#define CUE_POINT_BYTES ELEMENT_BYTES(1u, CUE_POINT_CONTENT)
#define DURATION_BYTES ELEMENT_BYTES(2u, 8u)
#define VOID_HEADER ELEMENT_BYTES(1u, 0u)

/* SimpleBlock: ID, 4-byte size, then BLOCK_PREFIX bytes before the payload */
#define BLOCK_PREFIX 4u        /* track number (1), time relative to the Cluster (2), flags (1) */
#define BLOCK_TRACK_1 0x81u    /* track number 1 as a 1-byte EBML number */
#define BLOCK_KEYFRAME 0x80u

/* Space reserved before writing */
#define FILE_HEADER_MOST 512u     /* EBML header, Segment start, Info, Tracks; without app name, SPS, PPS */
#define ELEMENT_HEADERS_MOST 64u  /* a picture's Cluster and SimpleBlock headers, or the Cues' header */

#define CUE_FRACTION 8u /* the cue array gets 1/CUE_FRACTION of the memory */

/* H.264 */
#define NAL_TYPE_MASK 0x1Fu
#define NAL_SPS 7u
#define NAL_PPS 8u
#define PROFILE_BASELINE 66u
#define PROFILE_MAIN 77u
#define PROFILE_EXTENDED 88u

/* ---- Bytes at the tail ---------------------------------------------------------- */

/* The put_*() and ebml_*() writers append at the tail; reserve() the space first. */

/**
 * store_be() - Store a number's low bytes, most significant first.
 * @at:    the destination
 * @value: the number
 * @bytes: how many low bytes
 */
static void store_be(uint8_t *at, uint64_t value, uint32_t bytes)
{
	while (bytes-- != 0)
		*at++ = (uint8_t)(value >> (bytes * 8u));
}

/**
 * put_be() - Append a number's low bytes, most significant first.
 * @m:     the muxer
 * @value: the number
 * @bytes: how many low bytes
 */
static void put_be(struct mkv *m, uint64_t value, uint32_t bytes)
{
	store_be(m->buffer + m->tail, value, bytes);
	m->tail += bytes;
}

/**
 * put_u8() - Append one byte.
 * @m:     the muxer
 * @value: the byte
 */
static void put_u8(struct mkv *m, uint8_t value)
{
	m->buffer[m->tail++] = value;
}

/**
 * put_bytes() - Append a block of bytes.
 * @m:    the muxer
 * @data: the bytes
 * @size: their count
 */
static void put_bytes(struct mkv *m, const void *data, uint32_t size)
{
	memcpy(m->buffer + m->tail, data, size);
	m->tail += size;
}

/* ---- EBML ----------------------------------------------------------------------- */

/**
 * uint_bytes() - Count the bytes a number needs, at least one.
 * @value: the number
 *
 * Return: 1 to 4.
 */
static uint32_t uint_bytes(uint32_t value)
{
	if (value < 0x100u)
		return 1;
	if (value < 0x10000u)
		return 2;
	if (value < 0x1000000u)
		return 3;
	return 4;
}

/**
 * ebml_id() - Append an element ID.
 * @m:  the muxer
 * @id: the ID, whose leading bits already mark its width
 */
static void ebml_id(struct mkv *m, uint32_t id)
{
	put_be(m, id, uint_bytes(id));
}

/**
 * ebml_size() - Append an element size as an EBML number of the fewest bytes.
 * @m:    the muxer
 * @size: the content's size
 *
 * A w-byte number is the marker bit 1 << 7w over 7w value bits. The all-ones
 * value means unknown size, so each width stops one short of its maximum.
 */
static void ebml_size(struct mkv *m, uint32_t size)
{
	if (size < 0x7Fu)
		put_be(m, 0x80u | size, 1);
	else if (size < 0x3FFFu)
		put_be(m, 0x4000u | size, 2);
	else if (size < 0x1FFFFFu)
		put_be(m, 0x200000u | size, 3);
	else if (size < 0xFFFFFFFu)
		put_be(m, 0x10000000u | size, 4);
	else
		put_be(m, 0x800000000ULL | size, 5);
}

/**
 * element_header() - Append an element's ID and size.
 * @m:    the muxer
 * @id:   its ID
 * @size: its content's size
 */
static void element_header(struct mkv *m, uint32_t id, uint32_t size)
{
	ebml_id(m, id);
	ebml_size(m, size);
}

/**
 * ebml_uint_fixed() - Append an unsigned integer element of a given width.
 * @m:     the muxer
 * @id:    its ID
 * @value: its value
 * @bytes: the value's width
 */
static void ebml_uint_fixed(struct mkv *m, uint32_t id, uint64_t value, uint32_t bytes)
{
	element_header(m, id, bytes);
	put_be(m, value, bytes);
}

/**
 * ebml_uint() - Append an unsigned integer element in the fewest bytes.
 * @m:     the muxer
 * @id:    its ID
 * @value: its value
 */
static void ebml_uint(struct mkv *m, uint32_t id, uint32_t value)
{
	ebml_uint_fixed(m, id, value, uint_bytes(value));
}

/**
 * ebml_float_from_uint() - Append an integer as an 8-byte IEEE 754 float element.
 * @m:     the muxer
 * @id:    its ID
 * @value: the integer
 *
 * Built from the integer's bits, so no floating-point code is linked.
 */
static void ebml_float_from_uint(struct mkv *m, uint32_t id, uint32_t value)
{
	uint64_t bits = 0; /* +0.0 */
	if (value != 0)
	{
		uint32_t msb = 31;
		while ((value >> msb) == 0)
			msb--;
		uint64_t exponent = 1023u + msb;                                            /* biased */
		uint64_t fraction = ((uint64_t)value << (52u - msb)) & ((1ULL << 52) - 1u); /* implicit 1 dropped */
		bits = (exponent << 52) | fraction;
	}
	ebml_uint_fixed(m, id, bits, 8);
}

/**
 * ebml_string() - Append a string element.
 * @m:    the muxer
 * @id:   its ID
 * @text: the string, without its NUL
 */
static void ebml_string(struct mkv *m, uint32_t id, const char *text)
{
	uint32_t size = (uint32_t)strlen(text);
	element_header(m, id, size);
	put_bytes(m, text, size);
}

/**
 * ebml_void() - Append a zero-filled Void, reserving space to overwrite later.
 * @m:     the muxer
 * @bytes: the whole element's size, header included
 */
static void ebml_void(struct mkv *m, uint32_t bytes)
{
	uint32_t content = bytes - VOID_HEADER;
	element_header(m, ID_VOID, content);
	memset(m->buffer + m->tail, 0, content);
	m->tail += content;
}

/**
 * element_open() - Append an element's ID and a 2-byte size field for element_close() to fill in.
 * @m:  the muxer
 * @id: its ID
 *
 * The content must stay within 0x3FFE bytes.
 *
 * Return: the content's offset, for element_close().
 */
static uint32_t element_open(struct mkv *m, uint32_t id)
{
	ebml_id(m, id);
	m->tail += 2;
	return m->tail;
}

/**
 * element_close() - Fill in the size field element_open() reserved: the content is what has been appended since.
 * @m:       the muxer
 * @content: what element_open() returned
 */
static void element_close(struct mkv *m, uint32_t content)
{
	store_be(m->buffer + content - 2u, SIZE2_MARKER | (m->tail - content), 2);
}

/* ---- Output buffer -------------------------------------------------------------- */

/**
 * tail_room() - Tell how many bytes are free after the tail.
 * @m: the muxer
 *
 * Return: the bytes.
 */
static uint32_t tail_room(const struct mkv *m)
{
	return m->capacity - m->tail;
}

/**
 * compact() - Move the unwritten bytes to the buffer's start, reclaiming the written ones.
 * @m: the muxer
 */
static void compact(struct mkv *m)
{
	uint32_t by = m->flushed;
	if (by == 0)
		return;
	memmove(m->buffer, m->buffer + by, m->tail - by);
	m->flushed = 0;
	m->committed -= by;
	m->payload -= by;
	m->tail -= by;
}

/**
 * flush_all() - Write everything committed.
 * @m: the muxer
 */
static void flush_all(struct mkv *m)
{
	mkv_flush(m, m->committed - m->flushed);
}

/**
 * reserve() - Make bytes free after the tail.
 * @m:    the muxer
 * @need: the bytes
 *
 * Compacts the buffer, and if that is not enough, writes everything committed
 * and compacts again. Either way the open picture may move.
 *
 * Return: MKV_OK, MKV_ERR_ROOM if the open picture leaves too little, or an earlier error.
 */
static int reserve(struct mkv *m, uint32_t need)
{
	if (m->error != MKV_OK || tail_room(m) >= need)
		return m->error;
	compact(m);
	if (tail_room(m) >= need)
		return MKV_OK;
	flush_all(m);
	compact(m);
	if (m->error == MKV_OK && tail_room(m) < need)
		m->error = MKV_ERR_ROOM;
	return m->error;
}

/**
 * picture_open() - Tell whether a picture has been begun and not yet ended.
 * @m: the muxer
 *
 * Return: nonzero while one is.
 */
static int picture_open(const struct mkv *m)
{
	return m->tail != m->committed;
}

/**
 * header_sent() - Tell whether the file header has been written: it is the file's first write.
 * @m: the muxer
 *
 * Return: nonzero once it is.
 */
static int header_sent(const struct mkv *m)
{
	return m->written != 0;
}

/* ---- File header ---------------------------------------------------------------- */

/**
 * avcc_write() - Append the CodecPrivate holding the avcC: the only copy of SPS and PPS, which the stream loses.
 * @m: the muxer
 */
static void avcc_write(struct mkv *m)
{
	uint8_t profile = m->sps.data[1];
	uint32_t avcc = element_open(m, ID_CODEC_PRIVATE);
	put_u8(m, 1);              /* configurationVersion */
	put_u8(m, profile);        /* AVCProfileIndication */
	put_u8(m, m->sps.data[2]); /* profile_compatibility */
	put_u8(m, m->sps.data[3]); /* AVCLevelIndication */
	put_u8(m, 0xFCu | 3u);     /* reserved bits set, lengthSizeMinusOne: 4-byte NAL lengths */
	put_u8(m, 0xE0u | 1u);     /* reserved bits set, numOfSequenceParameterSets */
	put_be(m, m->sps.size, 2);
	put_bytes(m, m->sps.data, m->sps.size);
	put_u8(m, 1); /* numOfPictureParameterSets */
	put_be(m, m->pps.size, 2);
	put_bytes(m, m->pps.data, m->pps.size);
	if (profile != PROFILE_BASELINE && profile != PROFILE_MAIN && profile != PROFILE_EXTENDED)
	{
		put_u8(m, 0xFCu | 1u); /* chroma_format: 4:2:0 */
		put_u8(m, 0xF8u | 0u); /* bit_depth_luma_minus8 */
		put_u8(m, 0xF8u | 0u); /* bit_depth_chroma_minus8 */
		put_u8(m, 0);          /* numOfSequenceParameterSetExt */
	}
	element_close(m, avcc);
}

/**
 * header_build() - Append the EBML header, the Segment's start, Info and Tracks.
 * @m:     the muxer; for H.264, SPS and PPS known
 * @start: the tail's offset, which becomes file offset 0
 *
 * Records the file offsets of what mkv_close() overwrites or points at.
 */
static void header_build(struct mkv *m, uint32_t start)
{
	uint32_t ebml = element_open(m, ID_EBML);
	ebml_uint(m, ID_EBML_VERSION, 1);
	ebml_uint(m, ID_EBML_READ_VERSION, 1);
	ebml_uint(m, ID_EBML_MAX_ID_LENGTH, 4);
	ebml_uint(m, ID_EBML_MAX_SIZE_LENGTH, 8);
	ebml_string(m, ID_DOC_TYPE, "matroska");
	ebml_uint(m, ID_DOC_TYPE_VERSION, 2);
	ebml_uint(m, ID_DOC_TYPE_READ_VERSION, 2);
	element_close(m, ebml);

	ebml_id(m, ID_SEGMENT);
	put_be(m, UNKNOWN_SIZE, 8);
	m->segment_pos = m->tail - start;
	ebml_void(m, SEEK_HEAD_BYTES);

	uint32_t info = element_open(m, ID_INFO);
	ebml_uint(m, ID_TIMESTAMP_SCALE, TIMESTAMP_SCALE);
	m->duration_pos = m->tail - start;
	ebml_void(m, DURATION_BYTES);
	ebml_string(m, ID_MUXING_APP, "mkv-writer");
	ebml_string(m, ID_WRITING_APP, m->track.app);
	element_close(m, info);

	m->tracks_pos = m->tail - start;
	uint32_t tracks = element_open(m, ID_TRACKS);
	uint32_t entry = element_open(m, ID_TRACK_ENTRY);
	ebml_uint(m, ID_TRACK_NUMBER, 1);
	ebml_uint(m, ID_TRACK_UID, 1);
	ebml_uint(m, ID_TRACK_TYPE, 1); /* video */
	ebml_uint(m, ID_FLAG_LACING, 0);
	ebml_uint(m, ID_DEFAULT_DURATION, 1000000000u / m->track.fps); /* ns */
	if (m->track.codec == MKV_H264)
	{
		ebml_string(m, ID_CODEC_ID, "V_MPEG4/ISO/AVC");
		avcc_write(m);
	}
	else
		ebml_string(m, ID_CODEC_ID, m->track.codec == MKV_MJPEG ? "V_MJPEG" : "V_UNCOMPRESSED");
	uint32_t video = element_open(m, ID_VIDEO);
	ebml_uint(m, ID_PIXEL_WIDTH, m->track.width);
	ebml_uint(m, ID_PIXEL_HEIGHT, m->track.height);
	if (m->track.codec == MKV_RAW)
	{
		element_header(m, ID_COLOUR_SPACE, 4);
		put_be(m, m->track.fourcc, 4);
	}
	element_close(m, video);
	element_close(m, entry);
	element_close(m, tracks);
}

/**
 * header_send() - Write the file header, ahead of the first complete picture.
 * @m: the muxer, the first picture complete at the tail
 *
 * The header is built in the free space after the picture and written
 * directly, so it reaches the file first; the buffer holds it only meanwhile.
 *
 * Return: MKV_OK, MKV_ERR_STREAM if H.264 brought no SPS or PPS, or reserve()'s or the write's error.
 */
static int header_send(struct mkv *m)
{
	if (m->track.codec == MKV_H264 && (m->sps.size == 0 || m->pps.size == 0))
	{
		m->error = MKV_ERR_STREAM;
		return m->error;
	}
	if (reserve(m, FILE_HEADER_MOST + (uint32_t)strlen(m->track.app) + m->sps.size + m->pps.size) != MKV_OK)
		return m->error;
	uint32_t start = m->tail;
	header_build(m, start);
	if (m->io->write(m->io->user, m->buffer + start, m->tail - start))
		m->written += m->tail - start;
	else
		m->error = MKV_ERR_WRITE;
	m->tail = start;
	return m->error;
}

/* ---- Annex-B to NAL lengths ----------------------------------------------------- */

/**
 * start_code_bytes() - Measure the Annex-B start code at @p: two or more zero bytes, then a one.
 * @p:   where it should be
 * @end: the picture's end
 *
 * Return: its length, 0 if there is none.
 */
static uint32_t start_code_bytes(const uint8_t *p, const uint8_t *end)
{
	const uint8_t *at = p;
	while (at < end && *at == 0)
		at++;
	uint32_t zeros = (uint32_t)(at - p);
	if (zeros < 2u || at == end || *at != 1u)
		return 0;
	return zeros + 1u;
}

/**
 * nal_end() - Find the end of a NAL unit: the first zero of the next start code, or the picture's end.
 * @p:   its first byte
 * @end: the picture's end
 *
 * Return: the byte after its last.
 */
static const uint8_t *nal_end(const uint8_t *p, const uint8_t *end)
{
	for (const uint8_t *at = p; at + 3 <= end; at++)
	{
		if (at[2] > 1u)
		{
			at += 2; /* 00 00 01 at at, at + 1 or at + 2 would need at[2] to be 0 or 1 */
			continue;
		}
		if (at[0] == 0 && at[1] == 0 && at[2] == 1u)
		{
			while (at > p && at[-1] == 0)
				at--; /* a NAL unit never ends in a zero byte: these belong to the start code */
			return at;
		}
	}
	return end;
}

/**
 * keep_parameter_set() - Copy the first SPS and the first PPS for the avcC; later ones are dropped.
 * @m:    the muxer
 * @nal:  the SPS or PPS NAL unit
 * @size: its size
 *
 * Return: MKV_OK, or MKV_ERR_STREAM if it is shorter than 4 bytes or does not fit.
 */
static int keep_parameter_set(struct mkv *m, const uint8_t *nal, uint32_t size)
{
	struct mkv_parameter_set *set = (nal[0] & NAL_TYPE_MASK) == NAL_SPS ? &m->sps : &m->pps;
	if (set->size != 0)
		return MKV_OK;
	if (size < 4u || size > sizeof(set->data))
		return MKV_ERR_STREAM;
	memcpy(set->data, nal, size);
	set->size = size;
	return MKV_OK;
}

/**
 * short_start_codes() - Count the 3-byte start codes in the open picture.
 * @m:     the muxer
 * @count: out: the count
 *
 * Return: MKV_OK, or MKV_ERR_STREAM if the picture does not begin with a start code.
 */
static int short_start_codes(const struct mkv *m, uint32_t *count)
{
	const uint8_t *at = m->buffer + m->payload, *end = m->buffer + m->tail;
	*count = 0;
	while (at < end)
	{
		uint32_t code = start_code_bytes(at, end);
		if (code == 0)
			return MKV_ERR_STREAM;
		*count += code == 3u;
		at = nal_end(at + code, end);
	}
	return MKV_OK;
}

/**
 * picture_move_up() - Move the open picture's bytes up, leaving a gap before them.
 * @m:  the muxer
 * @by: the gap's size
 *
 * Return: MKV_OK or reserve()'s error.
 */
static int picture_move_up(struct mkv *m, uint32_t by)
{
	if (by == 0)
		return MKV_OK;
	if (reserve(m, by) != MKV_OK)
		return m->error;
	uint8_t *picture = m->buffer + m->payload;
	memmove(picture + by, picture, m->tail - m->payload);
	m->tail += by;
	return MKV_OK;
}

/**
 * annexb_to_lengths() - Rewrite the open picture from Annex-B to 4-byte NAL lengths, in place.
 * @m:    the muxer
 * @size: out: the picture's size afterwards
 *
 * Each NAL unit is copied down to the output behind its length. A length
 * fills a 4-byte start code exactly but is a byte longer than a 3-byte one,
 * so the picture first moves up by one byte per 3-byte start code: then the
 * output never overtakes the input. SPS and PPS are taken out
 * (keep_parameter_set()), and zero bytes after the last NAL unit dropped.
 *
 * Return: MKV_OK, MKV_ERR_STREAM for a malformed stream, or reserve()'s error.
 */
static int annexb_to_lengths(struct mkv *m, uint32_t *size)
{
	while (m->tail > m->payload && m->buffer[m->tail - 1u] == 0)
		m->tail--; /* trailing_zero_8bits */
	uint32_t gap = 0;
	int error = short_start_codes(m, &gap);
	if (error == MKV_OK)
		error = picture_move_up(m, gap);
	if (error != MKV_OK)
		return error;

	uint8_t *out = m->buffer + m->payload;
	const uint8_t *in = out + gap, *end = m->buffer + m->tail;
	while (in < end)
	{
		const uint8_t *nal = in + start_code_bytes(in, end);
		const uint8_t *next = nal_end(nal, end);
		uint32_t nal_size = (uint32_t)(next - nal);
		if (nal_size == 0)
			return MKV_ERR_STREAM;
		uint32_t type = nal[0] & NAL_TYPE_MASK;
		if (type == NAL_SPS || type == NAL_PPS)
		{
			error = keep_parameter_set(m, nal, nal_size);
			if (error != MKV_OK)
				return error;
		}
		else
		{
			store_be(out, nal_size, 4);
			memmove(out + 4, nal, nal_size);
			out += 4u + nal_size;
		}
		in = next;
	}
	*size = (uint32_t)(out - (m->buffer + m->payload));
	return MKV_OK;
}

/* ---- Cues and SeekHead ---------------------------------------------------------- */

/**
 * cue_add() - Record a cue point for the open picture's Cluster, while the cue array has room.
 * @m: the muxer
 *
 * Called only for a picture that opened its Cluster, so the Cluster starts at @committed.
 */
static void cue_add(struct mkv *m)
{
	if (m->cue_count == m->cue_max)
		return;
	struct mkv_cue *cue = &m->cues[m->cue_count++];
	cue->ms = m->picture_ms;
	cue->position = mkv_bytes(m) - m->segment_pos;
}

/**
 * cues_write() - Append the Cues after the last picture and write them.
 * @m: the muxer, everything before written
 *
 * The Cues' size goes first, so each CuePoint has the fixed size CUE_POINT_BYTES.
 */
static void cues_write(struct mkv *m)
{
	if (reserve(m, ELEMENT_HEADERS_MOST) != MKV_OK)
		return;
	element_header(m, ID_CUES, m->cue_count * CUE_POINT_BYTES);
	for (uint32_t i = 0; i < m->cue_count; i++)
	{
		m->committed = m->tail; /* reserve() may write what is appended so far */
		if (reserve(m, CUE_POINT_BYTES) != MKV_OK)
			return;
		element_header(m, ID_CUE_POINT, CUE_POINT_CONTENT);
		ebml_uint_fixed(m, ID_CUE_TIME, m->cues[i].ms, 4);
		element_header(m, ID_CUE_TRACK_POSITIONS, CUE_POSITIONS_CONTENT);
		ebml_uint_fixed(m, ID_CUE_TRACK, 1, 1);
		ebml_uint_fixed(m, ID_CUE_CLUSTER_POSITION, m->cues[i].position, 4);
	}
	m->committed = m->tail;
	flush_all(m);
}

/**
 * seek_entry() - Append one Seek of the SeekHead.
 * @m:        the muxer
 * @id:       the element it points at
 * @position: that element's offset from the Segment's content
 */
static void seek_entry(struct mkv *m, uint32_t id, uint32_t position)
{
	element_header(m, ID_SEEK, SEEK_CONTENT);
	ebml_uint_fixed(m, ID_SEEK_ID, id, 4);
	ebml_uint_fixed(m, ID_SEEK_POSITION, position, 8);
}

/**
 * overwrite() - Write what is at the buffer's start over a Void in the file, then empty the buffer.
 * @m:        the muxer
 * @position: the Void's file offset
 *
 * A failure sets MKV_ERR_WRITE unless an earlier error is set.
 */
static void overwrite(struct mkv *m, uint32_t position)
{
	int ok = m->io->seek(m->io->user, position) && m->io->write(m->io->user, m->buffer, m->tail);
	if (!ok && m->error == MKV_OK)
		m->error = MKV_ERR_WRITE;
	m->tail = 0;
}

/* ---- API ------------------------------------------------------------------------ */

/**
 * mkv_open() - Set up a muxer. Nothing is written before the first picture is complete.
 * @m:      the muxer
 * @io:     the file's callbacks; must stay valid until mkv_close()
 * @track:  the track; copied
 * @memory: the muxer's memory until mkv_close(), pointer-aligned
 * @size:   its size; MKV_MEMORY suits most video
 *
 * 1/CUE_FRACTION of the memory holds the cue array, the rest the output
 * buffer, which must hold the largest picture with its headers.
 */
void mkv_open(struct mkv *m, const struct mkv_io *io, const struct mkv_track *track, void *memory, uint32_t size)
{
	memset(m, 0, sizeof(*m));
	m->io = io;
	m->track = *track;
	uint32_t cue_bytes = size / CUE_FRACTION;
	m->cues = memory;
	m->cue_max = cue_bytes / (uint32_t)sizeof(struct mkv_cue);
	m->buffer = (uint8_t *)memory + cue_bytes;
	m->capacity = size - cue_bytes;
}

/**
 * mkv_picture_begin() - Begin a picture: append a Cluster header if it opens one, and its SimpleBlock header.
 * @m:   the muxer
 * @ms:  its timestamp in ms; never less than the previous picture's
 * @key: nonzero for a key frame; every MJPEG or raw picture is one regardless
 *
 * A picture opens a Cluster if it is the first, an H.264 key frame (so that
 * seeking lands on one), or CLUSTER_MS (MJPEG, raw: CLUSTER_MS_INTRA) after the
 * Cluster's start. A key frame that opens a Cluster gets a cue point once it
 * is complete (mkv_picture_end()). While a picture is open this does nothing,
 * so the caller may call it for every packet.
 *
 * Return: MKV_OK or the error.
 */
int mkv_picture_begin(struct mkv *m, uint32_t ms, int key)
{
	if (picture_open(m) || reserve(m, ELEMENT_HEADERS_MOST) != MKV_OK)
		return m->error;
	int intra = m->track.codec != MKV_H264;
	int key_frame = key || intra;
	uint32_t cluster_span = intra ? CLUSTER_MS_INTRA : CLUSTER_MS;
	int new_cluster = !m->cluster_open || (key_frame && !intra) || ms - m->cluster_ms >= cluster_span;
	if (new_cluster)
	{
		ebml_id(m, ID_CLUSTER);
		put_be(m, UNKNOWN_SIZE, 8);
		ebml_uint(m, ID_TIMESTAMP, ms);
		m->cluster_open = 1;
		m->cluster_ms = ms;
	}
	ebml_id(m, ID_SIMPLE_BLOCK);
	put_be(m, 0, 4); /* size, filled in by mkv_picture_end() */
	put_u8(m, BLOCK_TRACK_1);
	put_be(m, ms - m->cluster_ms, 2);
	put_u8(m, key_frame ? BLOCK_KEYFRAME : 0);
	m->payload = m->tail;
	m->picture_ms = ms;
	m->picture_cue = new_cluster && key_frame;
	return MKV_OK;
}

/**
 * mkv_picture_bytes() - Append bytes to the open picture, in as many pieces as they come.
 * @m:    the muxer
 * @data: H.264 Annex-B, or JPEG
 * @size: their count
 *
 * Return: MKV_OK or the error.
 */
int mkv_picture_bytes(struct mkv *m, const void *data, uint32_t size)
{
	if (reserve(m, size) != MKV_OK)
		return m->error;
	put_bytes(m, data, size);
	return MKV_OK;
}

/**
 * mkv_picture_end() - Complete the open picture: rewrite H.264 to NAL lengths and fill in the SimpleBlock's size.
 * @m: the muxer
 *
 * The first complete picture also sends the file header ahead of it. A
 * picture of SPS and PPS alone leaves no SimpleBlock.
 *
 * Return: MKV_OK or the error.
 */
int mkv_picture_end(struct mkv *m)
{
	if (m->error != MKV_OK)
		return m->error;
	uint32_t size = m->tail - m->payload;
	if (m->track.codec == MKV_H264)
		m->error = annexb_to_lengths(m, &size);
	if (m->error != MKV_OK)
		return m->error;
	if (size == 0)
	{
		/* parameter sets alone: drop the headers, the Cluster's too, so the next picture opens it */
		m->tail = m->committed;
		m->cluster_open = 0;
		return MKV_OK;
	}
	m->tail = m->payload + size;
	if (!header_sent(m) && header_send(m) != MKV_OK)
		return m->error;
	store_be(m->buffer + m->payload - BLOCK_PREFIX - 4u, SIZE4_MARKER | (BLOCK_PREFIX + size), 4);
	if (m->picture_cue)
		cue_add(m);
	m->committed = m->tail;
	m->last_ms = m->picture_ms;
	m->pictures++;
	return MKV_OK;
}

/**
 * mkv_flush() - Write committed bytes to the file.
 * @m:    the muxer
 * @most: the most bytes to write; MKV_WRITE_MOST bounds the time one call takes
 *
 * Return: MKV_OK or the error.
 */
int mkv_flush(struct mkv *m, uint32_t most)
{
	uint32_t size = m->committed - m->flushed;
	if (size > most)
		size = most;
	if (size == 0 || m->error != MKV_OK)
		return m->error;
	if (!m->io->write(m->io->user, m->buffer + m->flushed, size))
	{
		m->error = MKV_ERR_WRITE;
		return m->error;
	}
	m->flushed += size;
	m->written += size;
	return MKV_OK;
}

/**
 * mkv_bytes() - Tell the file's size so far, counting committed bytes not yet written.
 * @m: the muxer
 *
 * Return: the bytes.
 */
uint32_t mkv_bytes(const struct mkv *m)
{
	return m->written + (m->committed - m->flushed);
}

/**
 * mkv_close() - Finish the file: write what is committed and the Cues, then the Duration and SeekHead over their Voids.
 * @m: the muxer
 *
 * An open picture is dropped. After MKV_ERR_STREAM or MKV_ERR_ROOM the
 * complete pictures before the error are still written, with their Cues.
 * After MKV_ERR_WRITE (a full disk) there are no Cues and no SeekHead, and
 * the file still plays. If no picture was complete the file stays empty.
 *
 * Return: MKV_OK or the first error.
 */
int mkv_close(struct mkv *m)
{
	int first = m->error;
	if (first != MKV_ERR_WRITE)
		m->error = MKV_OK; /* the error spoiled only its own picture */
	if (!header_sent(m))
		return first;
	m->tail = m->committed;
	flush_all(m);
	uint32_t cues_pos = m->written;
	if (m->error == MKV_OK)
		cues_write(m);
	int cues_written = m->error == MKV_OK;

	/* these overwrite bytes the file holds already, so a full disk does not stop them */
	m->flushed = m->committed = m->tail = 0;
	ebml_float_from_uint(m, ID_DURATION, m->last_ms + 1000u / m->track.fps); /* the last picture's end */
	overwrite(m, m->duration_pos);
	if (cues_written)
	{
		element_header(m, ID_SEEK_HEAD, SEEK_HEAD_CONTENT);
		seek_entry(m, ID_INFO, SEEK_HEAD_BYTES); /* Info follows the SeekHead's Void */
		seek_entry(m, ID_TRACKS, m->tracks_pos - m->segment_pos);
		seek_entry(m, ID_CUES, cues_pos - m->segment_pos);
		overwrite(m, m->segment_pos);
	}
	return first != MKV_OK ? first : m->error;
}
