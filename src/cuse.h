/*
 * kmscon - CUSE Character Device Engine
 *
 * Copyright (c) 2025 Alberto Ruiz <aruiz@gnome.org>
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

#ifndef KMSCON_CUSE_H
#define KMSCON_CUSE_H

struct kmscon_cuse;
struct ev_eloop;

int kmscon_cuse_new(struct kmscon_cuse **out, struct ev_eloop *eloop,
		    int slave_fd, const char *slave_path,
		    unsigned int vtnr);
void kmscon_cuse_free(struct kmscon_cuse *cuse);
const char *kmscon_cuse_get_devname(struct kmscon_cuse *cuse);
int kmscon_cuse_set_slave(struct kmscon_cuse *cuse, int new_slave_fd,
			  const char *slave_path);

#endif /* KMSCON_CUSE_H */
