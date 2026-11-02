# mkv-writer

Writes one video track into a Matroska (`.mkv`) file. Plain C with no dependencies beyond
`<string.h>` and `<stdint.h>`: it was written for 68k AmigaOS tools and runs anywhere.

- **H.264:** give it Annex-B, with three- or four-byte start codes. SPS and PPS go into the
  track's avcC, and start codes become lengths in place.
- **MJPEG:** give it complete JPEGs.
- **Raw video:** give it whole uncompressed pictures and name their pixel format as a FourCC,
  such as `YUY2` or `NV12`. It is stored as `V_UNCOMPRESSED`, the way ffmpeg writes and reads
  it.

A file cut short by a crash or a full disk plays up to the cut. Past that point nothing is
lost: the Segment and Clusters stay of unknown size, and only the Duration and SeekHead are
filled in at the close.

## Using it

Add `mkv.c` and `mkv.h` to your build; a git submodule works well. You give it the memory and
two file callbacks, and it never allocates or calls the OS.

```c
static const struct mkv_io io = {&file, my_write, my_seek};
const struct mkv_track track = {.codec = MKV_H264, .width = 1280, .height = 720, .fps = 30, .app = "MyProgram"};
struct mkv m;

mkv_open(&m, &io, &track, memory, MKV_MEMORY);
/* for each picture */
mkv_picture_begin(&m, ms, key);
mkv_picture_bytes(&m, data, size);   /* as many pieces as you like */
mkv_picture_end(&m);
mkv_flush(&m, MKV_WRITE_MOST);       /* bounded, so a slow disk holds up one frame */
/* at the end */
mkv_close(&m);
```

Every call returns `MKV_OK` or an error (see `mkv.h`). After an error only `mkv_close()` is
left; it saves the whole pictures, unless the error was a failed write. `memory` must hold the
largest picture you will send with room to spare: `MKV_MEMORY` is enough for most video, and an
eighth of what you give it holds the cue list.

## Limits

One track, no audio, 4 GiB. A picture's time is in milliseconds and must not go backwards, so
no B-frame reordering. Past 2 GiB it never seeks there: the close only seeks near the start of
the file.

## Tests

```sh
python3 -m pytest          # needs a C compiler and ffmpeg with libx264
python3 -m pytest -m "not slow"   # skips the one that writes 2 GiB
```

They build `mkv.c` for the host with the address and undefined-behaviour sanitizers, and check
what it writes with their own EBML reader and with ffmpeg.

## Licence

BSD 3-Clause. The EBML writing comes from libvpx, whose notice is at the top of `mkv.c`; it has
to travel with binaries too.
