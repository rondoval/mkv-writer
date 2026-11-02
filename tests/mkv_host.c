/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * mkv_host.c - mkv.c on the host, for test_mkv.py: a recording read from a
 * script, written to a file as the Amiga's would be.
 *
 *   mkv_host script out.mkv WIDTH HEIGHT FPS [full=BYTES] [repeat=N] [unfinished] [mjpeg] [raw=FOURCC]
 *
 * The script is the calls the recorder makes, in 32-bit little-endian words:
 * for each picture its time in ms, whether it is a key frame and how many
 * pieces it comes in, then each piece's size and its bytes.
 *
 * full=  the disk fills up after that many bytes, part of a write getting in
 * repeat= the script over again, later rounds' times after the first's
 * unfinished  the script's last picture is never ended, as when a run stops
 *        between a picture's packets
 * mjpeg  the pictures are JPEGs, not H.264
 * raw=   the pictures are uncompressed, in this pixel format (YUY2, NV12)
 *
 * The seek refuses what PFS3's refuses: a target at or above 2 GiB.
 *
 * Prints what it did; the exit code is mkv_close()'s result, or 99 for a
 * mistake of its own.
 */
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mkv.h"

#define PFS3_SEEK_LIMIT 0x80000000UL

struct disk
{
	FILE *file;
	unsigned long long used, full;
	int appending;
};

/**
 * disk_write() - The muxer's write: to the file, as far as the disk has room.
 * @user: the disk
 * @data: the bytes
 * @size: how many
 *
 * Return: nonzero when they all went.
 */
static int disk_write(void *user, const void *data, uint32_t size)
{
	struct disk *d = user;
	if (!d->appending)
		return fwrite(data, 1, size, d->file) == size; /* over bytes it holds: a full disk has room for that */
	unsigned long long room = d->full != 0 && d->full > d->used ? d->full - d->used : d->full != 0 ? 0 : size;
	size_t fits = room < size ? (size_t)room : size;
	if (fwrite(data, 1, fits, d->file) != fits)
		return 0;
	d->used += fits;
	return fits == size;
}

/**
 * disk_seek() - The muxer's seek: refused from 2 GiB on, as PFS3's is.
 * @user:     the disk
 * @position: where the next write goes
 *
 * Return: nonzero when it will.
 */
static int disk_seek(void *user, uint32_t position)
{
	struct disk *d = user;
	if (position >= PFS3_SEEK_LIMIT)
		return 0;
	d->appending = 0;
	return fseeko(d->file, (off_t)position, SEEK_SET) == 0;
}

/**
 * word() - Read one of the script's words.
 * @f:     the script
 * @value: where it goes
 *
 * Return: nonzero when there was one.
 */
static int word(FILE *f, uint32_t *value)
{
	return fread(value, sizeof(*value), 1, f) == 1;
}

/**
 * at_end() - Whether the script has no more.
 * @f: the script
 *
 * Return: nonzero at its end.
 */
static int at_end(FILE *f)
{
	int c = fgetc(f);
	if (c == EOF)
		return 1;
	ungetc(c, f);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 6)
		return 99;
	unsigned long long full = 0;
	unsigned long repeat = 1;
	int unfinished = 0;
	struct mkv_track track = {.codec = MKV_H264, .width = (uint32_t)atoi(argv[3]), .height = (uint32_t)atoi(argv[4]),
	                          .fps = (uint32_t)atoi(argv[5]), .app = "mkv_host"};
	for (int i = 6; i < argc; i++)
	{
		if (strncmp(argv[i], "full=", 5) == 0)
			full = strtoull(argv[i] + 5, NULL, 10);
		else if (strncmp(argv[i], "repeat=", 7) == 0)
			repeat = strtoul(argv[i] + 7, NULL, 10);
		else if (strcmp(argv[i], "unfinished") == 0)
			unfinished = 1;
		else if (strcmp(argv[i], "mjpeg") == 0)
			track.codec = MKV_MJPEG;
		else if (strncmp(argv[i], "raw=", 4) == 0 && strlen(argv[i]) == 8)
		{
			const unsigned char *f = (const unsigned char *)argv[i] + 4;
			track.codec = MKV_RAW;
			track.fourcc = (uint32_t)f[0] << 24 | (uint32_t)f[1] << 16 | (uint32_t)f[2] << 8 | f[3];
		}
		else
			return 99;
	}
	FILE *script = fopen(argv[1], "rb");
	struct disk d = {fopen(argv[2], "wb"), 0, full, 1};
	void *memory = malloc(MKV_MEMORY);
	uint32_t piece_room = 1u << 24;
	uint8_t *piece = malloc(piece_room);
	if (script == NULL || d.file == NULL || memory == NULL || piece == NULL)
		return 99;
	static struct mkv m;
	struct mkv_io io = {&d, disk_write, disk_seek};
	mkv_open(&m, &io, &track, memory, MKV_MEMORY);

	int error = MKV_OK;
	uint32_t base = 0, last = 0;
	for (unsigned long round = 0; round < repeat && error == MKV_OK; round++)
	{
		rewind(script);
		uint32_t ms, key, pieces;
		while (error == MKV_OK && word(script, &ms) && word(script, &key) && word(script, &pieces))
		{
			last = base + ms;
			error = mkv_picture_begin(&m, last, key != 0);
			for (uint32_t i = 0; i < pieces; i++)
			{
				uint32_t size;
				if (!word(script, &size) || size > piece_room || fread(piece, 1, size, script) != size)
					return 99;
				if (error == MKV_OK)
					error = mkv_picture_bytes(&m, piece, size);
			}
			if (unfinished && at_end(script))
				break;
			if (error == MKV_OK)
				error = mkv_picture_end(&m);
			if (error == MKV_OK)
				error = mkv_flush(&m, MKV_WRITE_MOST);
		}
		base = last + 33u;
	}
	int closed = mkv_close(&m);
	printf("pictures=%lu bytes=%lu cues=%lu error=%d\n", (unsigned long)m.pictures, (unsigned long)m.written, (unsigned long)m.cue_count, error);
	fclose(d.file);
	fclose(script);
	free(piece);
	free(memory);
	return closed;
}
