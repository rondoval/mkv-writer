/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * mkv.h - Matroska muxer for one video track, H.264, H.265, MJPEG or raw. It never
 * allocates or calls the OS: the caller supplies the memory and two file
 * callbacks, so the tests run it on the host (tests/test_mkv.py).
 */
#ifndef MKV_H
#define MKV_H

#include <stdint.h>

#define MKV_MEMORY (4u << 20)       /* memory for mkv_open() that suits most video */
#define MKV_WRITE_MOST (128u << 10) /* mkv_flush() limit per call: a slow disk stalls one call, not many */

/* Errors. After one only mkv_close() is left, which still writes the complete pictures unless it is MKV_ERR_WRITE. */
#define MKV_OK 0
#define MKV_ERR_WRITE 1  /* mkv_io.write() failed: a full disk */
#define MKV_ERR_STREAM 2 /* H.264 or H.265 not in Annex-B, or without its parameter sets by its first picture */
#define MKV_ERR_ROOM 3   /* a picture larger than the buffer */

/* Codecs */
#define MKV_H264 0  /* Annex-B, SPS and PPS in or before the first picture */
#define MKV_MJPEG 1 /* one complete JPEG per picture; every picture is a key frame */
#define MKV_RAW 2   /* one uncompressed picture, in the pixel format mkv_track.fourcc names; every picture is a key frame */
#define MKV_H265 3  /* Annex-B, VPS, SPS and PPS in or before the first picture */

struct mkv_track
{
	int codec;       /* MKV_H264, MKV_H265, MKV_MJPEG or MKV_RAW */
	uint32_t width;  /* pixels */
	uint32_t height;
	uint32_t fps;    /* nominal rate: sets DefaultDuration and the last picture's duration */
	const char *app; /* WritingApp, a short name; read when the first picture is complete */
	uint32_t fourcc; /* MKV_RAW: the pixel format as a FourCC, its first character the most significant byte ('YUY2') */
};

struct mkv_io
{
	void *user; /* passed to both callbacks */
	/* Write @size bytes at the current position: the end, or where seek() moved it. Return nonzero if all went. */
	int (*write)(void *user, const void *data, uint32_t size);
	/* Move the write position to @position, near the file's start. Return nonzero on success. */
	int (*seek)(void *user, uint32_t position);
};

struct mkv_cue
{
	uint32_t ms;       /* the Cluster's timestamp */
	uint32_t position; /* the Cluster's offset from the Segment's content */
};

struct mkv_parameter_set
{
	uint8_t data[128]; /* the NAL unit, without its start code */
	uint32_t size;     /* 0 until one is kept */
};

/* The caller may read error, pictures, written and cue_count; the rest is internal. */
struct mkv
{
	const struct mkv_io *io;
	struct mkv_track track;
	int error;         /* the first error, MKV_OK until then */
	uint32_t pictures; /* pictures stored */
	uint32_t written;  /* bytes written to the file; 0 until the header is */

	/* Output buffer; the offsets are in this order (see mkv.c) */
	uint8_t *buffer;
	uint32_t capacity;
	uint32_t flushed;   /* [0, flushed): written to the file */
	uint32_t committed; /* [flushed, committed): complete pictures */
	uint32_t payload;   /* [committed, payload): the open picture's Cluster and SimpleBlock headers */
	uint32_t tail;      /* [payload, tail): the open picture's bytes */

	/* The open picture */
	uint32_t picture_ms;
	int picture_cue; /* a key frame that opened its Cluster: gets a cue point */

	int cluster_open;
	uint32_t cluster_ms; /* the open Cluster's timestamp */
	uint32_t last_ms;    /* the last complete picture's timestamp */

	/* Cue points for the Cues, 1/8 of the memory */
	struct mkv_cue *cues;
	uint32_t cue_count, cue_max;

	/* File offsets recorded with the header */
	uint32_t segment_pos;  /* the Segment's content, which starts with the SeekHead's Void */
	uint32_t duration_pos; /* the Duration's Void */
	uint32_t tracks_pos;   /* the Tracks element */

	/* The first parameter sets, for the avcC or hvcC; vps is H.265's only */
	struct mkv_parameter_set vps, sps, pps;
};

void mkv_open(struct mkv *m, const struct mkv_io *io, const struct mkv_track *track, void *memory, uint32_t size);
int mkv_picture_begin(struct mkv *m, uint32_t ms, int key);
int mkv_picture_bytes(struct mkv *m, const void *data, uint32_t size);
int mkv_picture_end(struct mkv *m);
int mkv_flush(struct mkv *m, uint32_t most);
uint32_t mkv_bytes(const struct mkv *m);
int mkv_close(struct mkv *m);

#endif /* MKV_H */
