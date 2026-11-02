/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * mkv.c - one video track into a Matroska file, written as it goes.
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
 * What is its own here is what a recorder needs.  H.264 comes as Annex-B with
 * SPS and PPS in or before the first picture, so the file's header waits for
 * that picture and those two go into the track's avcC; later ones are dropped.
 * Start codes become lengths in place, and a picture may come in pieces.  An
 * MJPEG picture goes in as it is; each is a key frame, so its clusters and
 * cues go by time.
 *
 * The Segment and the Clusters stay of unknown size for good, so a file cut
 * short by a crash or a full disk plays up to the cut.  (A sized Segment over
 * unknown Clusters is what ffmpeg complains of, and sizing every Cluster is
 * a seek backwards per Cluster for nothing.)  What only the end can tell has
 * its room kept in Voids, overwritten at the close: the Duration, and the
 * SeekHead that finds the Cues.  Nothing else ever seeks.
 *
 * Bytes wait in one buffer and leave in bounded writes (mkv_flush()), so the
 * disk holds the recording up in small pieces.  The buffer closes up when it
 * runs out; little is waiting by then, and a picture stays in one piece
 * for its rewriting.
 */
#include <string.h>

#include "mkv.h"

/* Matroska's IDs */
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
#define ID_CLUSTER 0x1F43B675u
#define ID_TIMESTAMP 0xE7u
#define ID_SIMPLE_BLOCK 0xA3u
#define ID_CUES 0x1C53BB6Bu
#define ID_CUE_POINT 0xBBu
#define ID_CUE_TIME 0xB3u
#define ID_CUE_TRACK_POSITIONS 0xB7u
#define ID_CUE_TRACK 0xF7u
#define ID_CUE_CLUSTER_POSITION 0xF1u

#define UNKNOWN_SIZE 0x01FFFFFFFFFFFFFFULL
#define TIMESTAMP_SCALE 1000000u /* nanoseconds: timestamps are milliseconds */
#define CLUSTER_MS 30000u        /* a block's time is 16 bits from its cluster's, so a long pause opens a new one */
#define CLUSTER_MS_MJPEG 1000u   /* every picture could start one: a cluster and a cue a second is what seeking needs */
#define VOID_HEADER 2u           /* a Void's ID and size, for the sizes below */
#define SEEK_BYTES 18u           /* a Seek's content: ID of 2 + 1 + 4, position of 2 + 1 + 8 */
#define SEEK_HEAD_BYTES (5u + 3u * (3u + SEEK_BYTES)) /* Info, Tracks, Cues: the Void kept for it */
#define DURATION_BYTES 11u       /* the float's ID, size and eight bytes: the Void kept for it */
#define CUE_POSITIONS_BYTES 9u   /* CueTrackPositions' content: track of 3, position of 2 + 4 */
#define CUE_POINT_BYTES (2u + 6u + 2u + CUE_POSITIONS_BYTES) /* all numbers four bytes wide, so the Cues' size is known ahead */
#define BLOCK_HEADER 8u          /* a SimpleBlock's size of four, its track, time of two and flags */
#define PICTURE_HEADER_MOST 64u  /* room for a Cluster's start and a block's */
#define HEADER_MOST 512u         /* the file's header, without SPS and PPS */
#define CUE_PART 8u              /* of the memory, for the cue list */

#define NAL_SPS 7u
#define NAL_PPS 8u

/* ---- EBML ----------------------------------------------------------------------- */

/* These write at the tail and need the room asked for first (room()). */

/**
 * put_be() - Write a number, highest byte first.
 * @m:     the muxer
 * @value: the number
 * @bytes: how many of its low bytes
 */
static void put_be(struct mkv *m, uint64_t value, uint32_t bytes)
{
	while (bytes-- != 0)
		m->buffer[m->tail++] = (uint8_t)(value >> (bytes * 8u));
}

/**
 * put_bytes() - Copy bytes in.
 * @m:    the muxer
 * @data: the bytes
 * @size: how many
 */
static void put_bytes(struct mkv *m, const void *data, uint32_t size)
{
	memcpy(m->buffer + m->tail, data, size);
	m->tail += size;
}

/**
 * store_be32() - Fill in a four-byte field, highest byte first.
 * @at:    the field
 * @value: the number
 */
static void store_be32(uint8_t *at, uint32_t value)
{
	at[0] = (uint8_t)(value >> 24);
	at[1] = (uint8_t)(value >> 16);
	at[2] = (uint8_t)(value >> 8);
	at[3] = (uint8_t)value;
}

/**
 * ebml_id() - Write an element's ID, which holds its own length marker.
 * @m:  the muxer
 * @id: the ID
 */
static void ebml_id(struct mkv *m, uint32_t id)
{
	put_be(m, id, id >= 0x01000000u ? 4u : id >= 0x00010000u ? 3u : id >= 0x00000100u ? 2u : 1u);
}

/**
 * ebml_length() - Write a size in the fewest bytes.
 * @m:     the muxer
 * @value: the size
 *
 * All ones in a width means unknown, so that width's largest number takes the next.
 */
static void ebml_length(struct mkv *m, uint64_t value)
{
	uint32_t bytes = 1;
	while (value >= (1ULL << (bytes * 7u)) - 1u)
		bytes++;
	put_be(m, value | (1ULL << (bytes * 7u)), bytes);
}

/**
 * ebml_uint() - Write an unsigned element in the fewest bytes.
 * @m:     the muxer
 * @id:    its ID
 * @value: its value
 */
static void ebml_uint(struct mkv *m, uint32_t id, uint32_t value)
{
	uint32_t bytes = 1;
	while (bytes < 4u && (value >> (bytes * 8u)) != 0)
		bytes++;
	ebml_id(m, id);
	ebml_length(m, bytes);
	put_be(m, value, bytes);
}

/**
 * ebml_string() - Write a string element.
 * @m:    the muxer
 * @id:   its ID
 * @text: the string
 */
static void ebml_string(struct mkv *m, uint32_t id, const char *text)
{
	uint32_t size = (uint32_t)strlen(text);
	ebml_id(m, id);
	ebml_length(m, size);
	put_bytes(m, text, size);
}

/**
 * ebml_void() - Write a Void to keep room.
 * @m:     the muxer
 * @total: its bytes, its header among them
 */
static void ebml_void(struct mkv *m, uint32_t total)
{
	ebml_id(m, ID_VOID);
	ebml_length(m, total - VOID_HEADER);
	memset(m->buffer + m->tail, 0, total - VOID_HEADER);
	m->tail += total - VOID_HEADER;
}

/**
 * ebml_float() - Write a number as an eight-byte float.
 * @m:     the muxer
 * @id:    its ID
 * @value: the number
 *
 * Built by hand, without the float arithmetic of a 68k's library: top bit, exponent, the rest.
 */
static void ebml_float(struct mkv *m, uint32_t id, uint32_t value)
{
	uint64_t bits = 0;
	if (value != 0)
	{
		uint32_t top = 31;
		while ((value >> top) == 0)
			top--;
		bits = ((uint64_t)(1023u + top) << 52) | (((uint64_t)value << (52u - top)) & ((1ULL << 52) - 1u));
	}
	ebml_id(m, id);
	ebml_length(m, 8);
	put_be(m, bits, 8);
}

/**
 * element_begin() - Start an element whose size is known once written: two bytes kept for it.
 * @m:  the muxer
 * @id: its ID
 *
 * Return: where its content starts, for element_end().
 */
static uint32_t element_begin(struct mkv *m, uint32_t id)
{
	ebml_id(m, id);
	m->tail += 2;
	return m->tail;
}

/**
 * element_end() - Fill in the size element_begin() kept room for.
 * @m:     the muxer
 * @start: what element_begin() returned
 */
static void element_end(struct mkv *m, uint32_t start)
{
	uint32_t size = m->tail - start;
	m->buffer[start - 2u] = (uint8_t)(0x40u | (size >> 8));
	m->buffer[start - 1u] = (uint8_t)size;
}

/* ---- the bytes on their way ----------------------------------------------------- */

/**
 * buffer_compact() - Close up the gap the file left at the buffer's front.
 * @m: the muxer
 */
static void buffer_compact(struct mkv *m)
{
	uint32_t by = m->head;
	if (by == 0)
		return;
	memmove(m->buffer, m->buffer + by, m->tail - by);
	m->head = 0;
	m->done -= by;
	m->tail -= by;
	m->data -= by;
}

/**
 * mkv_flush() - Give the file some of the whole pictures waiting.
 * @m:    the muxer
 * @most: bytes at most
 *
 * Return: MKV_OK or the error.
 */
int mkv_flush(struct mkv *m, uint32_t most)
{
	uint32_t size = m->done - m->head;
	if (size > most)
		size = most;
	if (size == 0 || m->error != MKV_OK)
		return m->error;
	if (m->io->write(m->io->user, m->buffer + m->head, size))
	{
		m->head += size;
		m->written += size;
	}
	else
		m->error = MKV_ERR_WRITE;
	return m->error;
}

/**
 * room() - Make bytes free at the tail, waiting for the file if it must come to that.
 * @m:    the muxer
 * @need: the bytes
 *
 * Return: MKV_OK or the error.
 */
static int room(struct mkv *m, uint32_t need)
{
	if (m->error != MKV_OK || m->size - m->tail >= need)
		return m->error;
	buffer_compact(m);
	if (m->size - m->tail >= need)
		return MKV_OK;
	mkv_flush(m, m->done - m->head);
	buffer_compact(m);
	if (m->error == MKV_OK && m->size - m->tail < need)
		m->error = MKV_ERR_ROOM;
	return m->error;
}

/* ---- the file's header ---------------------------------------------------------- */

/**
 * avcc_write() - Write the track's avcC: how a reader learns SPS and PPS, the stream having lost them.
 * @m: the muxer
 */
static void avcc_write(struct mkv *m)
{
	const uint8_t *sps = m->sps;
	uint32_t avcc = element_begin(m, ID_CODEC_PRIVATE);
	put_be(m, 1, 1);     /* version */
	put_be(m, sps[1], 1); /* profile, its compatibility, level */
	put_be(m, sps[2], 1);
	put_be(m, sps[3], 1);
	put_be(m, 0xFFu, 1); /* lengths of four bytes */
	put_be(m, 0xE1u, 1); /* one SPS */
	put_be(m, m->sps_size, 2);
	put_bytes(m, m->sps, m->sps_size);
	put_be(m, 1, 1); /* one PPS */
	put_be(m, m->pps_size, 2);
	put_bytes(m, m->pps, m->pps_size);
	if (sps[1] != 66u && sps[1] != 77u && sps[1] != 88u)
		put_be(m, 0xFDF8F800u, 4); /* the High profiles' fields: 4:2:0, 8 bits both, no SPS extension */
	element_end(m, avcc);
}

/**
 * file_header() - Write the file's header, straight to the file, ahead of the first picture that made it possible.
 * @m: the muxer, SPS and PPS known, the room asked for
 */
static void file_header(struct mkv *m)
{
	uint32_t start = m->tail;

	uint32_t e = element_begin(m, ID_EBML);
	ebml_uint(m, ID_EBML_VERSION, 1);
	ebml_uint(m, ID_EBML_READ_VERSION, 1);
	ebml_uint(m, ID_EBML_MAX_ID_LENGTH, 4);
	ebml_uint(m, ID_EBML_MAX_SIZE_LENGTH, 8);
	ebml_string(m, ID_DOC_TYPE, "matroska");
	ebml_uint(m, ID_DOC_TYPE_VERSION, 2);
	ebml_uint(m, ID_DOC_TYPE_READ_VERSION, 2);
	element_end(m, e);

	ebml_id(m, ID_SEGMENT);
	put_be(m, UNKNOWN_SIZE, 8);
	m->segment = m->tail - start;

	ebml_void(m, SEEK_HEAD_BYTES);

	uint32_t info = element_begin(m, ID_INFO);
	ebml_uint(m, ID_TIMESTAMP_SCALE, TIMESTAMP_SCALE);
	m->duration = m->tail - start;
	ebml_void(m, DURATION_BYTES);
	ebml_string(m, ID_MUXING_APP, "mkv-writer");
	ebml_string(m, ID_WRITING_APP, m->track.app);
	element_end(m, info);

	m->tracks = m->tail - start - m->segment;
	uint32_t tracks = element_begin(m, ID_TRACKS);
	uint32_t entry = element_begin(m, ID_TRACK_ENTRY);
	ebml_uint(m, ID_TRACK_NUMBER, 1);
	ebml_uint(m, ID_TRACK_UID, 1);
	ebml_uint(m, ID_TRACK_TYPE, 1);
	ebml_uint(m, ID_FLAG_LACING, 0);
	ebml_uint(m, ID_DEFAULT_DURATION, 1000000000u / m->track.fps);
	ebml_string(m, ID_CODEC_ID, m->track.codec == MKV_H264 ? "V_MPEG4/ISO/AVC" : "V_MJPEG");
	if (m->track.codec == MKV_H264)
		avcc_write(m);
	uint32_t video = element_begin(m, ID_VIDEO);
	ebml_uint(m, ID_PIXEL_WIDTH, m->track.width);
	ebml_uint(m, ID_PIXEL_HEIGHT, m->track.height);
	element_end(m, video);
	element_end(m, entry);
	element_end(m, tracks);

	uint32_t size = m->tail - start;
	m->tail = start;
	if (m->io->write(m->io->user, m->buffer + start, size))
		m->written += size;
	else
		m->error = MKV_ERR_WRITE;
}

/**
 * header_send() - Send the file's header, with the first picture: for H.264, the one that brought SPS and PPS.
 * @m: the muxer
 *
 * Return: MKV_OK or the error.
 */
static int header_send(struct mkv *m)
{
	if (m->track.codec == MKV_H264 && (m->sps_size == 0 || m->pps_size == 0))
		m->error = MKV_ERR_STREAM;
	else if (room(m, HEADER_MOST + (uint32_t)strlen(m->track.app) + m->sps_size + m->pps_size) == MKV_OK)
		file_header(m);
	return m->error;
}

/* ---- a picture ------------------------------------------------------------------ */

/**
 * start_code() - Measure the start code that should be at @p.
 * @p:   where it should be
 * @end: the picture's end
 *
 * Any zero bytes ahead of the three that count are part of it.
 *
 * Return: its bytes, 0 for none.
 */
static uint32_t start_code(const uint8_t *p, const uint8_t *end)
{
	const uint8_t *at = p;
	while (at < end && *at == 0)
		at++;
	return at - p >= 2 && at < end && *at == 1u ? (uint32_t)(at - p) + 1u : 0;
}

/**
 * nal_end() - Find where a NAL unit ends: where the next start code's zeros begin, or the end.
 * @p:   its first byte
 * @end: the picture's end
 *
 * Return: the byte after its last.
 */
static uint8_t *nal_end(uint8_t *p, uint8_t *end)
{
	for (uint8_t *at = p; at + 3 <= end; at++)
	{
		if (at[2] > 1u)
		{
			at += 2; /* no start code can begin in these three */
			continue;
		}
		if (at[0] == 0 && at[1] == 0 && at[2] == 1u)
		{
			while (at > p && at[-1] == 0)
				at--; /* a NAL unit never ends in a zero */
			return at;
		}
	}
	return end;
}

/**
 * parameter_set() - Keep an SPS or PPS for the avcC, out of the stream; the first of each.
 * @out:        where it goes
 * @out_size:   its size there, 0 until it has one
 * @room_bytes: the room at @out
 * @nal:        the NAL unit
 * @size:       its bytes
 *
 * Return: MKV_OK or MKV_ERR_STREAM.
 */
static int parameter_set(uint8_t *out, uint32_t *out_size, uint32_t room_bytes, const uint8_t *nal, uint32_t size)
{
	if (*out_size != 0)
		return MKV_OK;
	if (size > room_bytes || size < 4u)
		return MKV_ERR_STREAM;
	memcpy(out, nal, size);
	*out_size = size;
	return MKV_OK;
}

/**
 * gap_open() - Move what is left of a picture up, by a byte for each three-byte start code in it.
 * @m:    the muxer
 * @read: where it starts, from the picture's first byte
 * @by:   where how far it moved goes
 *
 * A length is four bytes.  This happens at most once a picture, at its first
 * such start code, and never to the four-byte ones an encoder writes.
 *
 * Return: MKV_OK or the error.
 */
static int gap_open(struct mkv *m, uint32_t read, uint32_t *by)
{
	uint8_t *at = m->buffer + m->data + read, *end = m->buffer + m->tail;
	uint32_t count = 0;
	while (at < end)
	{
		uint32_t code = start_code(at, end);
		if (code == 0)
			return MKV_ERR_STREAM;
		count += code == 3u;
		at = nal_end(at + code, end);
	}
	if (room(m, count) != MKV_OK)
		return m->error;
	at = m->buffer + m->data + read;
	memmove(at + count, at, m->tail - m->data - read);
	m->tail += count;
	*by = count;
	return MKV_OK;
}

/**
 * nal_lengths() - Rewrite Annex-B to lengths in place, SPS and PPS taken out.
 * @m:    the muxer
 * @size: where the picture's new size goes
 *
 * A four-byte start code and a length are the same size, so such a stream
 * moves only where something was taken out.
 *
 * Return: MKV_OK or the error.
 */
static int nal_lengths(struct mkv *m, uint32_t *size)
{
	uint32_t read = 0, write = 0;
	while (m->tail > m->data && m->buffer[m->tail - 1u] == 0)
		m->tail--; /* padding after the last NAL unit */
	while (m->data + read < m->tail)
	{
		uint8_t *start = m->buffer + m->data, *end = m->buffer + m->tail;
		uint32_t code = start_code(start + read, end);
		if (code == 0)
			return MKV_ERR_STREAM;
		if (write + 4u > read + code)
		{
			uint32_t by = 0;
			int error = gap_open(m, read, &by);
			if (error != MKV_OK)
				return error;
			read += by;
			continue;
		}
		uint8_t *nal = start + read + code;
		uint8_t *next = nal_end(nal, end);
		uint32_t nal_size = (uint32_t)(next - nal);
		if (nal_size == 0)
			return MKV_ERR_STREAM;
		uint32_t type = nal[0] & 0x1Fu;
		if (type == NAL_SPS || type == NAL_PPS)
		{
			int error = type == NAL_SPS ? parameter_set(m->sps, &m->sps_size, sizeof(m->sps), nal, nal_size)
			                            : parameter_set(m->pps, &m->pps_size, sizeof(m->pps), nal, nal_size);
			if (error != MKV_OK)
				return error;
		}
		else
		{
			if (start + write + 4 != nal)
				memmove(start + write + 4, nal, nal_size);
			store_be32(start + write, nal_size);
			write += 4u + nal_size;
		}
		read = (uint32_t)(next - start);
	}
	*size = write;
	return MKV_OK;
}

/**
 * mkv_picture_begin() - Start a picture: its cluster if it needs one, and its block's header.
 * @m:   the muxer
 * @ms:  its time; never less than the last picture's
 * @key: nonzero for a key frame; MJPEG pictures all are
 *
 * An H.264 key frame opens a cluster, so that seeking lands on one; MJPEG
 * opens one a second.  A key frame that opens one gets a cue, which waits for
 * the picture to be whole (mkv_picture_end()).  While a picture is being put
 * together this does nothing, so each of its packets may call it.
 *
 * Return: MKV_OK or the error.
 */
int mkv_picture_begin(struct mkv *m, uint32_t ms, int key)
{
	if (m->tail != m->done || room(m, PICTURE_HEADER_MOST) != MKV_OK)
		return m->error;
	int mjpeg = m->track.codec == MKV_MJPEG;
	int cluster = !m->cluster_open || (key && !mjpeg) || ms - m->cluster_ms >= (mjpeg ? CLUSTER_MS_MJPEG : CLUSTER_MS);
	key = key || mjpeg;
	if (cluster)
	{
		ebml_id(m, ID_CLUSTER);
		put_be(m, UNKNOWN_SIZE, 8);
		ebml_uint(m, ID_TIMESTAMP, ms);
		m->cluster_open = 1;
		m->cluster_ms = ms;
	}
	ebml_id(m, ID_SIMPLE_BLOCK);
	put_be(m, 0, 4); /* its size, from mkv_picture_end() */
	put_be(m, 0x81u, 1); /* track 1 */
	put_be(m, ms - m->cluster_ms, 2);
	put_be(m, key ? 0x80u : 0u, 1);
	m->data = m->tail;
	m->ms = ms;
	m->key = key;
	m->cue = cluster && key;
	return MKV_OK;
}

/**
 * mkv_picture_bytes() - Add to the picture: as many pieces as the encoder's packets cut it into.
 * @m:    the muxer
 * @data: the bytes: H.264 as Annex-B, or a JPEG's
 * @size: how many
 *
 * Return: MKV_OK or the error.
 */
int mkv_picture_bytes(struct mkv *m, const void *data, uint32_t size)
{
	if (room(m, size) != MKV_OK)
		return m->error;
	put_bytes(m, data, size);
	return MKV_OK;
}

/**
 * cue_add() - Note the whole key frame's cluster for the Cues, while the list has room.
 * @m: the muxer
 */
static void cue_add(struct mkv *m)
{
	if (m->cue_count == m->cue_max)
		return;
	m->cues[m->cue_count].ms = m->ms;
	m->cues[m->cue_count].position = mkv_bytes(m) - m->segment;
	m->cue_count++;
}

/**
 * mkv_picture_end() - Finish the picture: rewrite it, close its block, and let it leave.
 * @m: the muxer
 *
 * The first one brings what the file's header needs, so it goes out here.
 *
 * Return: MKV_OK or the error.
 */
int mkv_picture_end(struct mkv *m)
{
	if (m->error != MKV_OK)
		return m->error;
	uint32_t size = m->tail - m->data;
	if (m->track.codec == MKV_H264)
		m->error = nal_lengths(m, &size);
	if (m->error != MKV_OK)
		return m->error;
	if (size == 0)
	{
		/* parameter sets alone: no block, and the next picture opens the cluster */
		m->tail = m->done;
		m->cluster_open = 0;
		return MKV_OK;
	}
	m->tail = m->data + size;
	if (m->written == 0 && header_send(m) != MKV_OK)
		return m->error;
	store_be32(m->buffer + m->data - BLOCK_HEADER, (size + 4u) | 0x10000000u);
	if (m->cue)
		cue_add(m);
	m->done = m->tail;
	m->last_ms = m->ms;
	m->pictures++;
	return MKV_OK;
}

/**
 * mkv_bytes() - Tell how large the file is so far, with what still waits.
 * @m: the muxer
 *
 * Return: the bytes.
 */
uint32_t mkv_bytes(const struct mkv *m)
{
	return m->written + (m->done - m->head);
}

/**
 * mkv_open() - Begin a file; nothing reaches it before the first picture is whole.
 * @m:      the muxer
 * @io:     the file
 * @track:  what it holds; copied
 * @memory: for the muxer's own use until mkv_close(), aligned for a pointer
 * @size:   its bytes: MKV_MEMORY or thereabouts, or more for pictures that need it
 */
void mkv_open(struct mkv *m, const struct mkv_io *io, const struct mkv_track *track, void *memory, uint32_t size)
{
	memset(m, 0, sizeof(*m));
	m->io = io;
	m->track = *track;
	uint32_t cue_bytes = size / CUE_PART;
	m->cues = memory;
	m->cue_max = cue_bytes / (uint32_t)sizeof(struct mkv_cue);
	m->buffer = (uint8_t *)memory + cue_bytes;
	m->size = size - cue_bytes;
}

/* ---- the end -------------------------------------------------------------------- */

/**
 * cues_write() - Write the Cues after the last picture.
 * @m: the muxer
 */
static void cues_write(struct mkv *m)
{
	if (room(m, PICTURE_HEADER_MOST) != MKV_OK)
		return;
	ebml_id(m, ID_CUES);
	ebml_length(m, m->cue_count * CUE_POINT_BYTES);
	for (uint32_t i = 0; i < m->cue_count; i++)
	{
		m->done = m->tail;
		if (room(m, CUE_POINT_BYTES) != MKV_OK)
			return;
		ebml_id(m, ID_CUE_POINT);
		ebml_length(m, CUE_POINT_BYTES - 2u);
		ebml_id(m, ID_CUE_TIME);
		ebml_length(m, 4);
		put_be(m, m->cues[i].ms, 4);
		ebml_id(m, ID_CUE_TRACK_POSITIONS);
		ebml_length(m, CUE_POSITIONS_BYTES);
		ebml_uint(m, ID_CUE_TRACK, 1);
		ebml_id(m, ID_CUE_CLUSTER_POSITION);
		ebml_length(m, 4);
		put_be(m, m->cues[i].position, 4);
	}
	m->done = m->tail;
	mkv_flush(m, m->done - m->head);
}

/**
 * patch() - Overwrite a Void near the file's start with what was built at the buffer's start.
 * @m:        the muxer
 * @position: the Void, in the file
 */
static void patch(struct mkv *m, uint32_t position)
{
	if ((!m->io->seek(m->io->user, position) || !m->io->write(m->io->user, m->buffer, m->tail)) && m->error == MKV_OK)
		m->error = MKV_ERR_WRITE;
	m->tail = 0;
}

/**
 * seek_entry() - Write one Seek of the SeekHead.
 * @m:        the muxer
 * @id:       the element it finds
 * @position: where it is, from the Segment's first byte
 */
static void seek_entry(struct mkv *m, uint32_t id, uint32_t position)
{
	ebml_id(m, ID_SEEK);
	ebml_length(m, SEEK_BYTES);
	ebml_id(m, ID_SEEK_ID);
	ebml_length(m, 4);
	put_be(m, id, 4);
	ebml_id(m, ID_SEEK_POSITION);
	ebml_length(m, 8);
	put_be(m, position, 8);
}

/**
 * mkv_close() - End the file: what still waits, the Cues, then the Duration and the SeekHead over their Voids.
 * @m: the muxer
 *
 * A picture not ended is dropped, and a bad one spoils only itself: the
 * whole pictures before it still go, with their Cues.  With the disk full
 * the Cues do not get written, so nothing is sought for them, and the file
 * plays regardless.  Nothing goes to the file if no picture was whole.
 *
 * Return: MKV_OK or the first error.
 */
int mkv_close(struct mkv *m)
{
	int first = m->error;
	if (first != MKV_ERR_WRITE)
		m->error = MKV_OK;
	if (m->written == 0)
		return first;
	m->tail = m->done;
	mkv_flush(m, m->done - m->head);
	uint32_t cues_at = m->written - m->segment;
	if (m->error == MKV_OK)
		cues_write(m);
	int cues_written = m->error == MKV_OK;

	/* what follows goes straight to the file, overwriting bytes it holds: a full disk does not stop it */
	m->head = m->done = m->tail = 0;
	ebml_float(m, ID_DURATION, m->last_ms + 1000u / m->track.fps);
	patch(m, m->duration);
	if (cues_written)
	{
		ebml_id(m, ID_SEEK_HEAD);
		ebml_length(m, SEEK_HEAD_BYTES - 5u);
		seek_entry(m, ID_INFO, SEEK_HEAD_BYTES);
		seek_entry(m, ID_TRACKS, m->tracks);
		seek_entry(m, ID_CUES, cues_at);
		patch(m, m->segment);
	}
	return first != MKV_OK ? first : m->error;
}
