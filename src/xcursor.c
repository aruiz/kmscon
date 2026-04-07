/*
 * kmscon - XCursor theme loader
 *
 * Minimal parser for the Xcursor file format and freedesktop theme
 * resolution.  No external dependencies beyond libc.
 *
 * Copyright (c) 2026 Alberto Ruiz <aruiz@redhat.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <endian.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xcursor.h"

#define XCURSOR_MAGIC		0x72756358 /* "Xcur" in LE */
#define XCURSOR_IMAGE_TYPE	0xfffd0002
#define XCURSOR_IMAGE_HDR_LEN	36
#define XCURSOR_FILE_HDR_LEN	16
#define XCURSOR_TOC_ENTRY_LEN	12
#define XCURSOR_MAX_DIM		1024

#define DEFAULT_PATH \
	"~/.local/share/icons:~/.icons:/usr/share/icons:/usr/share/pixmaps"

/*
 * Sticky-error reader: once a short read occurs, @err is set and all
 * subsequent calls return 0 without touching the file.
 */
static uint32_t read_le32(FILE *f, bool *err)
{
	uint32_t v;

	if (*err)
		return 0;
	if (fread(&v, 4, 1, f) != 1) {
		*err = true;
		return 0;
	}
	return le32toh(v);
}

/*
 * Parse an xcursor file and extract the image whose nominal size is
 * closest to @target_size.  Returns 0 on success.
 */
static int parse_cursor_file(FILE *f, unsigned int target_size,
			     struct xcursor_image *img)
{
	bool err = false;
	uint32_t magic, header_sz, version, ntoc;
	uint32_t best_pos = 0;
	bool found = false;
	unsigned int best_dist = UINT_MAX;
	uint64_t toc_end, npixels;
	uint32_t i, type, subtype, pos;
	unsigned int dist;
	uint32_t chunk_hdr, chunk_type, chunk_sub, chunk_ver, delay;

	magic = read_le32(f, &err);
	if (err || magic != XCURSOR_MAGIC)
		return -EINVAL;

	header_sz = read_le32(f, &err);
	version = read_le32(f, &err);
	ntoc = read_le32(f, &err);
	(void)version;

	if (err)
		return -EIO;
	if (header_sz < XCURSOR_FILE_HDR_LEN)
		return -EINVAL;
	if (ntoc == 0 || ntoc > 0x10000)
		return -EINVAL;

	toc_end = (uint64_t)header_sz +
		  (uint64_t)ntoc * XCURSOR_TOC_ENTRY_LEN;

	if (fseek(f, header_sz, SEEK_SET))
		return -EIO;

	for (i = 0; i < ntoc; i++) {
		type = read_le32(f, &err);
		subtype = read_le32(f, &err);
		pos = read_le32(f, &err);

		if (err)
			return -EIO;
		if (type != XCURSOR_IMAGE_TYPE)
			continue;
		if (pos < toc_end)
			continue;

		dist = (subtype > target_size)
			? subtype - target_size
			: target_size - subtype;
		if (dist < best_dist) {
			best_dist = dist;
			best_pos = pos;
			found = true;
		}
	}

	if (!found)
		return -ENOENT;

	if (fseek(f, best_pos, SEEK_SET))
		return -EIO;

	err = false;
	chunk_hdr = read_le32(f, &err);
	chunk_type = read_le32(f, &err);
	chunk_sub = read_le32(f, &err);
	chunk_ver = read_le32(f, &err);
	(void)chunk_hdr;
	(void)chunk_sub;
	(void)chunk_ver;

	if (err)
		return -EIO;
	if (chunk_type != XCURSOR_IMAGE_TYPE)
		return -EINVAL;

	img->width = read_le32(f, &err);
	img->height = read_le32(f, &err);
	img->xhot = read_le32(f, &err);
	img->yhot = read_le32(f, &err);
	delay = read_le32(f, &err);
	(void)delay;

	if (err)
		return -EIO;
	if (img->width == 0 || img->height == 0 ||
	    img->width > XCURSOR_MAX_DIM || img->height > XCURSOR_MAX_DIM)
		return -EINVAL;
	if (img->xhot > img->width || img->yhot > img->height)
		return -EINVAL;

	npixels = (uint64_t)img->width * img->height;
	img->pixels = malloc(npixels * sizeof(uint32_t));
	if (!img->pixels)
		return -ENOMEM;

	if (fread(img->pixels, sizeof(uint32_t), npixels, f) != npixels) {
		free(img->pixels);
		img->pixels = NULL;
		return -EIO;
	}

	for (i = 0; i < npixels; i++)
		img->pixels[i] = le32toh(img->pixels[i]);

	return 0;
}

/*
 * Try to load a cursor from <dir>/<theme>/cursors/<name>.
 */
static int try_cursor_file(const char *dir, const char *theme,
			   const char *name, unsigned int size,
			   struct xcursor_image *img)
{
	char path[PATH_MAX];
	FILE *f;
	int n, ret;

	n = snprintf(path, sizeof(path), "%s/%s/cursors/%s",
		     dir, theme, name);
	if (n < 0 || (unsigned)n >= sizeof(path))
		return -ENAMETOOLONG;

	f = fopen(path, "rb");
	if (!f)
		return -errno;

	ret = parse_cursor_file(f, size, img);
	fclose(f);
	return ret;
}

/*
 * Read the Inherits= line from <dir>/<theme>/index.theme.
 * Writes the parent theme name into @buf.  Returns 0 on success.
 */
static int read_theme_inherit(const char *dir, const char *theme,
			      char *buf, size_t bufsz)
{
	char path[PATH_MAX];
	char line[512];
	FILE *f;
	char *p;
	size_t len, i;
	int n, ret;
	bool in_icon_theme;

	n = snprintf(path, sizeof(path), "%s/%s/index.theme", dir, theme);
	if (n < 0 || (unsigned)n >= sizeof(path))
		return -ENAMETOOLONG;

	f = fopen(path, "r");
	if (!f)
		return -errno;

	ret = -ENOENT;
	in_icon_theme = false;
	while (fgets(line, sizeof(line), f)) {
		p = line;
		while (*p == ' ' || *p == '\t')
			p++;

		if (strncmp(p, "[Icon Theme]", 12) == 0) {
			in_icon_theme = true;
			continue;
		}
		if (*p == '[') {
			in_icon_theme = false;
			continue;
		}
		if (!in_icon_theme)
			continue;
		if (strncmp(p, "Inherits", 8) != 0)
			continue;
		p += 8;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p != '=')
			continue;
		p++;
		while (*p == ' ' || *p == '\t')
			p++;

		len = strlen(p);
		while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r' ||
				    p[len - 1] == ' ' || p[len - 1] == '\t' ||
				    p[len - 1] == ',' || p[len - 1] == ';'))
			len--;

		for (i = 0; i < len; i++) {
			if (p[i] == ',' || p[i] == ';' || p[i] == ':') {
				len = i;
				break;
			}
		}

		if (len == 0 || len >= bufsz)
			break;

		memcpy(buf, p, len);
		buf[len] = '\0';
		ret = 0;
		break;
	}

	fclose(f);
	return ret;
}

/*
 * Expand a leading ~ to $HOME.  Returns a pointer to @buf, or @dir
 * unchanged if no expansion was needed.
 */
static const char *expand_home(const char *dir, char *buf, size_t bufsz)
{
	const char *home;
	int n;

	if (dir[0] != '~' || dir[1] != '/')
		return dir;

	home = getenv("HOME");
	if (!home)
		return dir;

	n = snprintf(buf, bufsz, "%s%s", home, dir + 1);
	if (n < 0 || (unsigned)n >= bufsz)
		return dir;

	return buf;
}

static bool name_is_safe(const char *s)
{
	const char *p;

	if (!s || !*s)
		return false;
	if (s[0] == '.')
		return false;
	for (p = s; *p; p++) {
		if (*p == '/')
			return false;
	}
	if (strstr(s, ".."))
		return false;
	return true;
}

int xcursor_load(const char *name, unsigned int size,
		 struct xcursor_image *img)
{
	const char *theme;
	const char *search;
	char *pathbuf;
	char *saveptr;
	char *dir;
	char expanded[PATH_MAX];
	const char *resolved;
	char parent[256];
	int ret;

	if (!name_is_safe(name))
		return -EINVAL;

	theme = getenv("XCURSOR_THEME");
	if (!theme || !*theme || !name_is_safe(theme))
		theme = "default";

	search = getenv("XCURSOR_PATH");
	if (!search || !*search)
		search = DEFAULT_PATH;

	pathbuf = strdup(search);
	if (!pathbuf)
		return -ENOMEM;

	ret = -ENOENT;
	dir = strtok_r(pathbuf, ":", &saveptr);

	while (dir) {
		resolved = expand_home(dir, expanded, sizeof(expanded));

		ret = try_cursor_file(resolved, theme, name, size, img);
		if (ret == 0)
			goto out;

		if (read_theme_inherit(resolved, theme, parent,
				       sizeof(parent)) == 0 &&
		    name_is_safe(parent)) {
			ret = try_cursor_file(resolved, parent, name,
					      size, img);
			if (ret == 0)
				goto out;
		}

		if (strcmp(theme, "default") != 0) {
			ret = try_cursor_file(resolved, "default", name,
					      size, img);
			if (ret == 0)
				goto out;
		}

		dir = strtok_r(NULL, ":", &saveptr);
	}

out:
	free(pathbuf);
	return ret;
}
