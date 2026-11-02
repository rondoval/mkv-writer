/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * mkv.h - one video track into a Matroska file: plain C, the file behind two
 * callbacks, so that the tests run it on the host (tests/test_mkv.py).
 */
#ifndef MKV_H
#define MKV_H

#include <stdint.h>

#define MKV_MEMORY (4u << 20)       /* what the muxer takes: pictures on their way, the cue list */
#define MKV_WRITE_MOST (128u << 10) /* what one pass should give the file: a slow disk then holds up one tick, not many */

/* Errors: after one, only mkv_close() is left, which saves the whole pictures unless it was MKV_ERR_WRITE */
#define MKV_OK 0
#define MKV_ERR_WRITE 1  /* the file took no more: a full disk */
#define MKV_ERR_STREAM 2 /* H.264 that is not Annex-B, or brought no SPS or PPS before its first picture */
#define MKV_ERR_ROOM 3   /* a picture larger than the memory */

/* What goes in a picture */
#define MKV_H264 0  /* Annex-B, SPS and PPS in or before the first picture */
#define MKV_MJPEG 1 /* one complete JPEG: every picture is a key frame */

/* The track */
struct mkv_track
{
	int codec;       /* MKV_H264 or MKV_MJPEG */
	uint32_t width;  /* the track's picture */
	uint32_t height;
	uint32_t fps;    /* the pictures a second it is meant to have */
	const char *app; /* who writes the file, for its header; kept until the first picture is whole */
};

/* The file */
struct mkv_io
{
	void *user;
	/* @size bytes go after what was written, or at the position sought: nonzero when they did */
	int (*write)(void *user, const void *data, uint32_t size);
	/* The next write goes to @position, near the start of the file: nonzero when it will */
	int (*seek)(void *user, uint32_t position);
};

struct mkv_cue
{
	uint32_t ms;       /* the cluster's time */
	uint32_t position; /* where it starts, from the Segment's first byte */
};

struct mkv
{
	const struct mkv_io *io;
	struct mkv_track track;
	int error;

	/* the bytes on their way to the file: head..done are whole pictures, done..tail the one being put together */
	uint8_t *buffer;
	uint32_t size, head, done, tail;

	struct mkv_cue *cues;
	uint32_t cue_count, cue_max;

	/* the picture being put together: where its data starts in the buffer, its time and kind, and whether it wants a cue */
	uint32_t data, ms;
	int key, cue;

	int cluster_open;
	uint32_t cluster_ms;
	uint32_t written; /* bytes in the file: 0 until the header is */
	uint32_t pictures;
	uint32_t last_ms;

	/* from the file's first byte: the Segment's content, which starts with the SeekHead's Void, and the Duration's Void */
	uint32_t segment, duration;
	uint32_t tracks; /* from the Segment's first byte */

	uint8_t sps[128], pps[128];
	uint32_t sps_size, pps_size;
};

void mkv_open(struct mkv *m, const struct mkv_io *io, const struct mkv_track *track, void *memory, uint32_t size);
int mkv_picture_begin(struct mkv *m, uint32_t ms, int key);
int mkv_picture_bytes(struct mkv *m, const void *data, uint32_t size);
int mkv_picture_end(struct mkv *m);
int mkv_flush(struct mkv *m, uint32_t most);
uint32_t mkv_bytes(const struct mkv *m);
int mkv_close(struct mkv *m);

#endif /* MKV_H */
