/*
 * stream.h - a tiny random-access byte stream over a file or a memory block.
 *
 * Both the tag reader and the decoders read through this interface. That
 * lets the unit tests feed them hand-built files straight from memory, and
 * it keeps every file access in the app on plain Win32 calls (CreateFileW,
 * ReadFile, SetFilePointer). The C runtime's 64-bit stdio functions
 * (_ftelli64, _fseeki64, _wfopen_s) are not exported by Windows XP's
 * msvcrt.dll, so we deliberately avoid stdio for audio files.
 */
#ifndef MP_STREAM_H
#define MP_STREAM_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

enum { MP_SEEK_SET = 0, MP_SEEK_CUR = 1, MP_SEEK_END = 2 };

typedef struct MpStream MpStream;

/* Opens a file for reading. Returns NULL on failure; if err is non-NULL it
 * receives the Win32 error code. The file is opened with FILE_SHARE_READ |
 * FILE_SHARE_WRITE so a file that another program (a tag editor, say) has
 * open can still be played. */
MpStream *mp_stream_open_file(const wchar_t *path, unsigned long *err);

/* Wraps a memory block. The data is not copied and must outlive the stream. */
MpStream *mp_stream_open_memory(const void *data, size_t size);

void mp_stream_close(MpStream *s);

/* Reads up to n bytes at the current position; returns the count read
 * (0 at end of stream or on error). */
size_t mp_stream_read(MpStream *s, void *buf, size_t n);

/* Returns 1 on success. Seeking past the end is allowed (reads then return
 * 0); seeking before the start fails. */
int mp_stream_seek(MpStream *s, int64_t offset, int origin);

int64_t mp_stream_tell(MpStream *s);
int64_t mp_stream_size(MpStream *s);

/* Seek to offset and read exactly n bytes. Returns 1 only if all n bytes
 * were read. */
int mp_stream_read_at(MpStream *s, int64_t offset, void *buf, size_t n);

/* Reads a whole file into a malloc'd buffer (with one extra NUL byte after
 * the data, for convenience). Fails if the file is larger than max_bytes.
 * Returns 1 on success; free the buffer with free(). */
int mp_file_read_all(const wchar_t *path, size_t max_bytes, uint8_t **data, size_t *size);

/* Creates or truncates a file and writes data to it. Returns 1 on success. */
int mp_file_write_all(const wchar_t *path, const void *data, size_t size);

#endif
