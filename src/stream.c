/*
 * stream.c - file and memory streams. See stream.h.
 */
#include "stream.h"

#include <stdlib.h>
#include <string.h>
#include <windows.h>

struct MpStream {
	/* Exactly one of these is in use. */
	HANDLE file;
	const uint8_t *mem;
	int64_t size;
	int64_t pos;
};

MpStream *mp_stream_open_file(const wchar_t *path, unsigned long *err)
{
	MpStream *s;
	LARGE_INTEGER size;
	HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		if (err)
			*err = GetLastError();
		return NULL;
	}
	/* GetFileSizeEx exists from Windows 2000 on, so it is safe for XP. */
	if (!GetFileSizeEx(h, &size)) {
		if (err)
			*err = GetLastError();
		CloseHandle(h);
		return NULL;
	}
	s = (MpStream *)calloc(1, sizeof(*s));
	if (s == NULL) {
		if (err)
			*err = ERROR_NOT_ENOUGH_MEMORY;
		CloseHandle(h);
		return NULL;
	}
	s->file = h;
	s->size = size.QuadPart;
	return s;
}

MpStream *mp_stream_open_memory(const void *data, size_t size)
{
	MpStream *s = (MpStream *)calloc(1, sizeof(*s));
	if (s == NULL)
		return NULL;
	s->file = INVALID_HANDLE_VALUE;
	s->mem = (const uint8_t *)data;
	s->size = (int64_t)size;
	return s;
}

void mp_stream_close(MpStream *s)
{
	if (s == NULL)
		return;
	if (s->mem == NULL && s->file != INVALID_HANDLE_VALUE && s->file != NULL)
		CloseHandle(s->file);
	free(s);
}

size_t mp_stream_read(MpStream *s, void *buf, size_t n)
{
	size_t total = 0;
	if (s == NULL || n == 0 || s->pos >= s->size)
		return 0;
	if ((int64_t)n > s->size - s->pos)
		n = (size_t)(s->size - s->pos);
	if (s->mem != NULL) {
		memcpy(buf, s->mem + s->pos, n);
		s->pos += (int64_t)n;
		return n;
	}
	/* We keep our own position rather than trusting the handle's, so set
	 * it explicitly; this also makes read_at trivially correct. */
	{
		LARGE_INTEGER li;
		li.QuadPart = s->pos;
		if (!SetFilePointerEx(s->file, li, NULL, FILE_BEGIN))
			return 0;
	}
	while (total < n) {
		DWORD chunk = (n - total > 0x10000000u) ? 0x10000000u : (DWORD)(n - total);
		DWORD got = 0;
		if (!ReadFile(s->file, (uint8_t *)buf + total, chunk, &got, NULL) || got == 0)
			break;
		total += got;
	}
	s->pos += (int64_t)total;
	return total;
}

int mp_stream_seek(MpStream *s, int64_t offset, int origin)
{
	int64_t base;
	if (s == NULL)
		return 0;
	switch (origin) {
	case MP_SEEK_SET: base = 0; break;
	case MP_SEEK_CUR: base = s->pos; break;
	case MP_SEEK_END: base = s->size; break;
	default: return 0;
	}
	if (base + offset < 0)
		return 0;
	s->pos = base + offset;
	return 1;
}

int64_t mp_stream_tell(MpStream *s)
{
	return s ? s->pos : 0;
}

int64_t mp_stream_size(MpStream *s)
{
	return s ? s->size : 0;
}

int mp_stream_read_at(MpStream *s, int64_t offset, void *buf, size_t n)
{
	if (!mp_stream_seek(s, offset, MP_SEEK_SET))
		return 0;
	return mp_stream_read(s, buf, n) == n;
}

int mp_file_read_all(const wchar_t *path, size_t max_bytes, uint8_t **data, size_t *size)
{
	MpStream *s = mp_stream_open_file(path, NULL);
	uint8_t *buf;
	size_t n;
	*data = NULL;
	*size = 0;
	if (s == NULL)
		return 0;
	if (s->size < 0 || (uint64_t)s->size > (uint64_t)max_bytes) {
		mp_stream_close(s);
		return 0;
	}
	n = (size_t)s->size;
	buf = (uint8_t *)malloc(n + 1);
	if (buf == NULL) {
		mp_stream_close(s);
		return 0;
	}
	if (mp_stream_read(s, buf, n) != n) {
		free(buf);
		mp_stream_close(s);
		return 0;
	}
	buf[n] = 0;
	mp_stream_close(s);
	*data = buf;
	*size = n;
	return 1;
}

int mp_file_write_all(const wchar_t *path, const void *data, size_t size)
{
	DWORD written = 0;
	int ok;
	HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return 0;
	ok = WriteFile(h, data, (DWORD)size, &written, NULL) && written == (DWORD)size;
	if (!CloseHandle(h))
		ok = 0;
	return ok;
}
