/*
 * kmscon - XCursor theme loader
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

#ifndef KMSCON_XCURSOR_H
#define KMSCON_XCURSOR_H

#include <stdint.h>

struct xcursor_image {
	uint32_t width;
	uint32_t height;
	uint32_t xhot;
	uint32_t yhot;
	uint32_t *pixels;
};

/*
 * Load a named cursor from installed xcursor themes.
 * Searches $XCURSOR_THEME (or "default"), following theme inheritance,
 * across the standard freedesktop icon search paths.
 *
 * Returns 0 on success, negative errno on failure.
 * On success the caller must free img->pixels with free().
 */
int xcursor_load(const char *name, unsigned int size,
		 struct xcursor_image *img);

#endif /* KMSCON_XCURSOR_H */
