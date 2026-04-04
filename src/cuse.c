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

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fuse.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include "cuse.h"
#include "eloop.h"
#include "shl_log.h"

#define LOG_SUBSYSTEM "cuse"

#define CUSE_READ_BUFFER (FUSE_MIN_READ_BUFFER + 4096)

#define NLONGS(n) (((n) + LONG_BIT - 1) / LONG_BIT)
#define BIT_IS_SET(array, bit) \
	(!!((array)[(bit) / LONG_BIT] & (1UL << ((bit) % LONG_BIT))))

/*
 * We need the *kernel's* struct termios size for FUSE ioctl retry iovecs.
 * glibc's <termios.h> defines a larger struct (NCCS=32, plus c_ispeed and
 * c_ospeed, ~60 bytes) that cannot coexist with the kernel headers, and
 * <linux/termios.h> conflicts with <sys/ioctl.h> over struct winsize.
 *
 * Including <asm/termbits.h> directly gives us the kernel's struct termios
 * (NCCS=19, ~36 bytes) without pulling in the conflicting types.  Using
 * glibc's larger size in the iovec would overflow the 36-byte stack buffer
 * that glibc's tcgetattr/tcsetattr allocate, smashing the stack canary.
 */
#include <asm/termbits.h>

struct kmscon_cuse {
	int cuse_fd;
	int slave_fd;
	struct ev_fd *efd;
	struct ev_fd *slave_efd;
	struct ev_eloop *eloop;
	char devname[32];
	unsigned int open_count;
	bool init_done;
	char buf[CUSE_READ_BUFFER];

	bool read_pending;
	uint64_t read_unique;
	uint32_t read_size;

	uint64_t poll_kh;
	bool poll_registered;

	pid_t fg_pgrp;

	int spkr_fd;
	struct ev_timer *tone_timer;

	int kbd_fd;
	unsigned char led_state;
	unsigned char kbd_flags;
	int kb_mode;
	int kd_mode;

	unsigned int vtnr;
	struct vt_mode vtmode;

	char *func_strings[256];

	char slave_path[128];
};

static unsigned int cuse_counter;

static void update_slave_monitoring(struct kmscon_cuse *cuse);
static void slave_readable(struct ev_fd *fd, int mask, void *data);
static int cuse_reopen_slave(struct kmscon_cuse *cuse);

/*
 * Reply helpers -- compose fuse_out_header + optional payload and write
 * to the CUSE fd in a single writev() call.
 */

static int cuse_reply_err(struct kmscon_cuse *cuse, uint64_t unique, int err)
{
	struct fuse_out_header hdr;

	memset(&hdr, 0, sizeof(hdr));
	hdr.len = sizeof(hdr);
	hdr.error = -err;
	hdr.unique = unique;

	return write(cuse->cuse_fd, &hdr, sizeof(hdr));
}

static int cuse_reply_ok(struct kmscon_cuse *cuse, uint64_t unique)
{
	return cuse_reply_err(cuse, unique, 0);
}

static int cuse_reply_buf(struct kmscon_cuse *cuse, uint64_t unique,
			  const void *data, size_t len)
{
	struct fuse_out_header hdr;
	struct iovec iov[2];
	int cnt = 1;

	memset(&hdr, 0, sizeof(hdr));
	hdr.len = sizeof(hdr) + len;
	hdr.unique = unique;

	iov[0].iov_base = &hdr;
	iov[0].iov_len = sizeof(hdr);

	if (data && len) {
		iov[1].iov_base = (void *)data;
		iov[1].iov_len = len;
		cnt = 2;
	}

	return writev(cuse->cuse_fd, iov, cnt);
}

static int cuse_reply_ioctl(struct kmscon_cuse *cuse, uint64_t unique,
			    int result, const void *data, size_t len)
{
	struct fuse_out_header hdr;
	struct fuse_ioctl_out out;
	struct iovec iov[3];
	int cnt = 2;

	memset(&out, 0, sizeof(out));
	out.result = result;

	memset(&hdr, 0, sizeof(hdr));
	hdr.len = sizeof(hdr) + sizeof(out) + len;
	hdr.unique = unique;

	iov[0].iov_base = &hdr;
	iov[0].iov_len = sizeof(hdr);
	iov[1].iov_base = &out;
	iov[1].iov_len = sizeof(out);

	if (data && len) {
		iov[2].iov_base = (void *)data;
		iov[2].iov_len = len;
		cnt = 3;
	}

	return writev(cuse->cuse_fd, iov, cnt);
}

static int cuse_reply_ioctl_retry(struct kmscon_cuse *cuse, uint64_t unique,
				  const struct fuse_ioctl_iovec *in_iov,
				  unsigned int in_count,
				  const struct fuse_ioctl_iovec *out_iov,
				  unsigned int out_count)
{
	struct fuse_out_header hdr;
	struct fuse_ioctl_out out;
	struct iovec iov[4];
	int cnt = 2;
	size_t in_len = in_count * sizeof(*in_iov);
	size_t out_len = out_count * sizeof(*out_iov);

	memset(&out, 0, sizeof(out));
	out.flags = FUSE_IOCTL_RETRY;
	out.in_iovs = in_count;
	out.out_iovs = out_count;

	memset(&hdr, 0, sizeof(hdr));
	hdr.len = sizeof(hdr) + sizeof(out) + in_len + out_len;
	hdr.unique = unique;

	iov[0].iov_base = &hdr;
	iov[0].iov_len = sizeof(hdr);
	iov[1].iov_base = &out;
	iov[1].iov_len = sizeof(out);

	if (in_count) {
		iov[cnt].iov_base = (void *)in_iov;
		iov[cnt].iov_len = in_len;
		cnt++;
	}
	if (out_count) {
		iov[cnt].iov_base = (void *)out_iov;
		iov[cnt].iov_len = out_len;
		cnt++;
	}

	return writev(cuse->cuse_fd, iov, cnt);
}

/*
 * FUSE_OPEN / FUSE_RELEASE handlers
 */

static void handle_open(struct kmscon_cuse *cuse,
			const struct fuse_in_header *hdr)
{
	struct fuse_open_out out;

	cuse->open_count++;
	log_debug("FUSE_OPEN: count=%u pid=%u", cuse->open_count, hdr->pid);

	if (!cuse->fg_pgrp)
		cuse->fg_pgrp = hdr->pid;

	memset(&out, 0, sizeof(out));
	out.fh = cuse->open_count;
	out.open_flags = FOPEN_DIRECT_IO;

	cuse_reply_buf(cuse, hdr->unique, &out, sizeof(out));
}

static void handle_release(struct kmscon_cuse *cuse,
			   const struct fuse_in_header *hdr)
{
	if (cuse->open_count > 0)
		cuse->open_count--;
	log_debug("FUSE_RELEASE: count=%u", cuse->open_count);

	cuse_reply_ok(cuse, hdr->unique);
}

/*
 * FUSE_GETATTR -- return synthetic char device attributes
 */

static void handle_getattr(struct kmscon_cuse *cuse,
			   const struct fuse_in_header *hdr)
{
	struct fuse_attr_out out;

	memset(&out, 0, sizeof(out));
	out.attr_valid = 3600;
	out.attr.ino = 2;
	out.attr.mode = S_IFCHR | 0666;
	out.attr.nlink = 1;

	cuse_reply_buf(cuse, hdr->unique, &out, sizeof(out));
}

/*
 * FUSE_READ -- read from PTY slave and return to caller
 */

static void handle_read(struct kmscon_cuse *cuse,
			const struct fuse_in_header *hdr,
			const void *payload)
{
	const struct fuse_read_in *in = payload;
	char tmp[CUSE_READ_BUFFER];
	ssize_t n;
	size_t size;

	size = in->size;
	if (size > sizeof(tmp))
		size = sizeof(tmp);

	n = read(cuse->slave_fd, tmp, size);
	if (n < 0 && errno == EIO && cuse_reopen_slave(cuse) == 0)
		n = read(cuse->slave_fd, tmp, size);
	if (n < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			/*
			 * No data on the PTY slave yet.  Defer the reply so
			 * the child's read() blocks until input arrives.
			 * Enable slave_fd readability monitoring.
			 */
			cuse->read_pending = true;
			cuse->read_unique = hdr->unique;
			cuse->read_size = size;
			update_slave_monitoring(cuse);
			log_debug("FUSE_READ: deferred (size=%zu)", size);
			return;
		}
		cuse_reply_err(cuse, hdr->unique, errno);
		return;
	}

	log_debug("FUSE_READ: immediate %zd bytes", n);
	cuse_reply_buf(cuse, hdr->unique, tmp, n);
}

/*
 * FUSE_WRITE -- write caller data to PTY slave
 */

static void handle_write(struct kmscon_cuse *cuse,
			 const struct fuse_in_header *hdr,
			 const void *payload)
{
	const struct fuse_write_in *in = payload;
	const void *data = (const char *)in + sizeof(*in);
	struct fuse_write_out out;
	ssize_t n;

	log_debug("FUSE_WRITE: %u bytes", in->size);
	n = write(cuse->slave_fd, data, in->size);
	if (n < 0 && errno == EIO && cuse_reopen_slave(cuse) == 0)
		n = write(cuse->slave_fd, data, in->size);
	if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
		cuse_reply_err(cuse, hdr->unique, errno);
		return;
	}

	memset(&out, 0, sizeof(out));
	if (n > 0)
		out.size = n;
	cuse_reply_buf(cuse, hdr->unique, &out, sizeof(out));
}

/*
 * FUSE_IOCTL -- dispatch terminal ioctls to PTY slave using the
 * unrestricted ioctl retry mechanism for pointer arguments.
 */

enum ioctl_dir {
	IOCTL_DIR_NONE,
	IOCTL_DIR_READ,
	IOCTL_DIR_WRITE,
	IOCTL_DIR_UNKNOWN,
};

struct ioctl_info {
	enum ioctl_dir dir;
	size_t size;
};

static struct ioctl_info classify_ioctl(unsigned int cmd)
{
	switch (cmd) {
	case TCGETS:
		return (struct ioctl_info){ IOCTL_DIR_READ,
					   sizeof(struct termios) };
	case TCSETS:
	case TCSETSW:
	case TCSETSF:
		return (struct ioctl_info){ IOCTL_DIR_WRITE,
					   sizeof(struct termios) };
	case TIOCGWINSZ:
		return (struct ioctl_info){ IOCTL_DIR_READ,
					   sizeof(struct winsize) };
	case TIOCSWINSZ:
		return (struct ioctl_info){ IOCTL_DIR_WRITE,
					   sizeof(struct winsize) };
	case TIOCGSID:
		return (struct ioctl_info){ IOCTL_DIR_READ, sizeof(pid_t) };
	case TIOCSCTTY:
	case TIOCNOTTY:
		return (struct ioctl_info){ IOCTL_DIR_NONE, 0 };
	default:
		return (struct ioctl_info){ IOCTL_DIR_UNKNOWN, 0 };
	}
}

/*
 * Find an evdev device that advertises a specific capability bit.
 * Used to locate the PC speaker (EV_SND + SND_TONE) and a keyboard
 * with LEDs (EV_LED + LED_NUML).
 */
static int find_evdev(unsigned int cap_type, unsigned int cap_bit)
{
	DIR *dir;
	struct dirent *ent;
	char path[PATH_MAX];
	int fd;
	unsigned long evbits[NLONGS(EV_CNT)] = { 0 };
	unsigned long capbits[NLONGS(SND_MAX + 1)] = { 0 };

	dir = opendir("/dev/input");
	if (!dir)
		return -1;

	while ((ent = readdir(dir))) {
		if (strncmp(ent->d_name, "event", 5))
			continue;

		snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
		fd = open(path, O_WRONLY | O_CLOEXEC);
		if (fd < 0)
			continue;

		memset(evbits, 0, sizeof(evbits));
		if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0 ||
		    !BIT_IS_SET(evbits, cap_type)) {
			close(fd);
			continue;
		}

		memset(capbits, 0, sizeof(capbits));
		if (ioctl(fd, EVIOCGBIT(cap_type, sizeof(capbits)),
			  capbits) < 0 ||
		    !BIT_IS_SET(capbits, cap_bit)) {
			close(fd);
			continue;
		}

		closedir(dir);
		return fd;
	}

	closedir(dir);
	return -1;
}

static void spkr_tone(struct kmscon_cuse *cuse, unsigned int hz)
{
	struct input_event ev;

	if (cuse->spkr_fd < 0)
		return;

	memset(&ev, 0, sizeof(ev));
	ev.type = EV_SND;
	ev.code = SND_TONE;
	ev.value = hz;

	if (write(cuse->spkr_fd, &ev, sizeof(ev)) < 0)
		log_debug("spkr_tone(%u): write failed: %m", hz);
}

static void tone_timeout(struct ev_timer *timer, uint64_t num, void *data)
{
	struct kmscon_cuse *cuse = data;

	spkr_tone(cuse, 0);
	ev_timer_disable(timer);
}

static void sync_leds(struct kmscon_cuse *cuse)
{
	struct input_event ev[3];

	if (cuse->kbd_fd < 0)
		return;

	memset(ev, 0, sizeof(ev));
	ev[0].type = EV_LED;
	ev[0].code = LED_SCROLLL;
	ev[0].value = !!(cuse->led_state & LED_SCR);
	ev[1].type = EV_LED;
	ev[1].code = LED_NUML;
	ev[1].value = !!(cuse->led_state & LED_NUM);
	ev[2].type = EV_LED;
	ev[2].code = LED_CAPSL;
	ev[2].value = !!(cuse->led_state & LED_CAP);

	if (write(cuse->kbd_fd, ev, sizeof(ev)) < 0)
		log_debug("sync_leds: write failed: %m");
}

static void handle_ioctl_kiocsound(struct kmscon_cuse *cuse, uint64_t unique,
				   unsigned long arg)
{
	unsigned int hz = arg ? (unsigned)(1193182UL / arg) : 0;

	if (cuse->tone_timer)
		ev_timer_disable(cuse->tone_timer);

	spkr_tone(cuse, hz);
	log_debug("KIOCSOUND: %s", hz ? "on" : "off");

	cuse_reply_ioctl(cuse, unique, 0, NULL, 0);
}

static void handle_ioctl_kdmktone(struct kmscon_cuse *cuse, uint64_t unique,
				  unsigned long arg)
{
	unsigned int count = arg & 0xffff;
	unsigned int ms = (arg >> 16) & 0xffff;
	unsigned int hz = count ? (unsigned)(1193182UL / count) : 0;

	if (cuse->tone_timer)
		ev_timer_disable(cuse->tone_timer);

	spkr_tone(cuse, hz);

	if (hz && ms && cuse->tone_timer) {
		struct itimerspec spec;

		memset(&spec, 0, sizeof(spec));
		spec.it_value.tv_sec = ms / 1000;
		spec.it_value.tv_nsec = (ms % 1000) * 1000000L;
		ev_timer_update(cuse->tone_timer, &spec);
	}

	log_debug("KDMKTONE: %u Hz, %u ms", hz, ms);
	cuse_reply_ioctl(cuse, unique, 0, NULL, 0);
}

/*
 * Ioctl helpers -- factor out the retry-or-reply dance for returning
 * a fixed value to userspace and for fetching pointer input.
 */

static void ioctl_read_val(struct kmscon_cuse *cuse,
			   const struct fuse_in_header *hdr,
			   const struct fuse_ioctl_in *in,
			   const void *val, size_t size)
{
	if (in->out_size < size) {
		struct fuse_ioctl_iovec fiov = {
			.base = in->arg,
			.len = size,
		};
		cuse_reply_ioctl_retry(cuse, hdr->unique,
				       NULL, 0, &fiov, 1);
	} else {
		cuse_reply_ioctl(cuse, hdr->unique, 0, val, size);
	}
}

static const void *ioctl_fetch_input(struct kmscon_cuse *cuse,
				     const struct fuse_in_header *hdr,
				     const struct fuse_ioctl_in *in,
				     size_t size)
{
	if (in->in_size >= size)
		return (const char *)in + sizeof(*in);

	struct fuse_ioctl_iovec fiov = {
		.base = in->arg,
		.len = size,
	};
	cuse_reply_ioctl_retry(cuse, hdr->unique, &fiov, 1, NULL, 0);
	return NULL;
}

static void handle_ioctl(struct kmscon_cuse *cuse,
			 const struct fuse_in_header *hdr,
			 const void *payload)
{
	const struct fuse_ioctl_in *in = payload;
	struct ioctl_info info;
	int ret;

	switch (in->cmd) {
	case KIOCSOUND:
		handle_ioctl_kiocsound(cuse, hdr->unique,
				       (unsigned long)in->arg);
		return;
	case KDMKTONE:
		handle_ioctl_kdmktone(cuse, hdr->unique,
				      (unsigned long)in->arg);
		return;
	case KDGETLED: {
		unsigned char val = cuse->led_state;
		ioctl_read_val(cuse, hdr, in, &val, sizeof(val));
		return;
	}
	case KDSETLED:
		cuse->led_state = (unsigned char)in->arg & 0x07;
		sync_leds(cuse);
		cuse_reply_ioctl(cuse, hdr->unique, 0, NULL, 0);
		return;
	case KDGKBLED: {
		unsigned char val = cuse->kbd_flags;
		ioctl_read_val(cuse, hdr, in, &val, sizeof(val));
		return;
	}
	case KDSKBLED:
		cuse->kbd_flags = (unsigned char)in->arg & 0x77;
		cuse_reply_ioctl(cuse, hdr->unique, 0, NULL, 0);
		return;
	case KDGKBTYPE: {
		unsigned char val = KB_101;

		if (in->out_size < sizeof(val)) {
			struct fuse_ioctl_iovec fiov = {
				.base = in->arg,
				.len = sizeof(val),
			};
			cuse_reply_ioctl_retry(cuse, hdr->unique,
					       NULL, 0, &fiov, 1);
		} else {
			cuse_reply_ioctl(cuse, hdr->unique, 0,
					 &val, sizeof(val));
		}
		return;
	}
	case KDGETMODE: {
		int val = cuse->kd_mode;

		if (in->out_size < sizeof(val)) {
			struct fuse_ioctl_iovec fiov = {
				.base = in->arg,
				.len = sizeof(val),
			};
			cuse_reply_ioctl_retry(cuse, hdr->unique,
					       NULL, 0, &fiov, 1);
		} else {
			cuse_reply_ioctl(cuse, hdr->unique, 0,
					 &val, sizeof(val));
		}
		return;
	}
	case KDSETMODE:
		if ((int)in->arg == KD_TEXT || (int)in->arg == KD_GRAPHICS)
			cuse->kd_mode = (int)in->arg;
		cuse_reply_ioctl(cuse, hdr->unique, 0, NULL, 0);
		return;
	case KDGKBMODE: {
		int val = cuse->kb_mode;

		if (in->out_size < sizeof(val)) {
			struct fuse_ioctl_iovec fiov = {
				.base = in->arg,
				.len = sizeof(val),
			};
			cuse_reply_ioctl_retry(cuse, hdr->unique,
					       NULL, 0, &fiov, 1);
		} else {
			cuse_reply_ioctl(cuse, hdr->unique, 0,
					 &val, sizeof(val));
		}
		return;
	}
	case KDSKBMODE:
		cuse->kb_mode = (int)in->arg;
		cuse_reply_ioctl(cuse, hdr->unique, 0, NULL, 0);
		return;
	case KDGKBSENT: {
		struct kbsentry kbs;

		if (in->in_size < sizeof(kbs) || in->out_size < sizeof(kbs)) {
			struct fuse_ioctl_iovec fiov = {
				.base = in->arg,
				.len = sizeof(kbs),
			};
			cuse_reply_ioctl_retry(cuse, hdr->unique,
					       &fiov, 1, &fiov, 1);
		} else {
			const struct kbsentry *req = (const void *)
				(cuse->buf + sizeof(struct fuse_in_header) +
				 sizeof(struct fuse_ioctl_in));

			memset(&kbs, 0, sizeof(kbs));
			kbs.kb_func = req->kb_func;
			if (cuse->func_strings[req->kb_func])
				strncpy((char *)kbs.kb_string,
					cuse->func_strings[req->kb_func],
					sizeof(kbs.kb_string) - 1);
			cuse_reply_ioctl(cuse, hdr->unique, 0,
					 &kbs, sizeof(kbs));
		}
		return;
	}
	case KDSKBSENT: {
		if (in->in_size < sizeof(struct kbsentry)) {
			struct fuse_ioctl_iovec fiov = {
				.base = in->arg,
				.len = sizeof(struct kbsentry),
			};
			cuse_reply_ioctl_retry(cuse, hdr->unique,
					       &fiov, 1, NULL, 0);
		} else {
			const struct kbsentry *req = (const void *)
				(cuse->buf + sizeof(struct fuse_in_header) +
				 sizeof(struct fuse_ioctl_in));
			unsigned char idx = req->kb_func;

			free(cuse->func_strings[idx]);
			cuse->func_strings[idx] = strndup(
				(const char *)req->kb_string,
				sizeof(req->kb_string) - 1);
			cuse_reply_ioctl(cuse, hdr->unique, 0, NULL, 0);
		}
		return;
	}
	/*
	 * VT management ioctls.  kmscon owns the real VT underneath,
	 * so we present a self-consistent view to programs running
	 * inside the CUSE terminal.
	 */
	case VT_GETSTATE: {
		struct vt_stat vs;

		if (in->out_size < sizeof(vs)) {
			struct fuse_ioctl_iovec fiov = {
				.base = in->arg,
				.len = sizeof(vs),
			};
			cuse_reply_ioctl_retry(cuse, hdr->unique,
					       NULL, 0, &fiov, 1);
		} else {
			memset(&vs, 0, sizeof(vs));
			vs.v_active = cuse->vtnr;
			vs.v_state = 1 << cuse->vtnr;
			cuse_reply_ioctl(cuse, hdr->unique, 0,
					 &vs, sizeof(vs));
		}
		return;
	}
	case VT_OPENQRY: {
		/*
		 * Report no free VT.  kmscon manages VTs itself;
		 * child processes should not allocate new ones.
		 */
		int vt = -1;

		if (in->out_size < sizeof(vt)) {
			struct fuse_ioctl_iovec fiov = {
				.base = in->arg,
				.len = sizeof(vt),
			};
			cuse_reply_ioctl_retry(cuse, hdr->unique,
					       NULL, 0, &fiov, 1);
		} else {
			cuse_reply_ioctl(cuse, hdr->unique, 0,
					 &vt, sizeof(vt));
		}
		return;
	}
	case VT_GETMODE: {
		if (in->out_size < sizeof(struct vt_mode)) {
			struct fuse_ioctl_iovec fiov = {
				.base = in->arg,
				.len = sizeof(struct vt_mode),
			};
			cuse_reply_ioctl_retry(cuse, hdr->unique,
					       NULL, 0, &fiov, 1);
		} else {
			cuse_reply_ioctl(cuse, hdr->unique, 0,
					 &cuse->vtmode,
					 sizeof(cuse->vtmode));
		}
		return;
	}
	case VT_SETMODE: {
		if (in->in_size < sizeof(struct vt_mode)) {
			struct fuse_ioctl_iovec fiov = {
				.base = in->arg,
				.len = sizeof(struct vt_mode),
			};
			cuse_reply_ioctl_retry(cuse, hdr->unique,
					       &fiov, 1, NULL, 0);
		} else {
			const void *data = cuse->buf +
				sizeof(struct fuse_in_header) +
				sizeof(struct fuse_ioctl_in);

			memcpy(&cuse->vtmode, data,
			       sizeof(cuse->vtmode));
			cuse_reply_ioctl(cuse, hdr->unique, 0,
					 NULL, 0);
		}
		return;
	}
	case VT_ACTIVATE:
	case VT_WAITACTIVE:
		/* Silently succeed -- kmscon controls VT switching. */
		cuse_reply_ioctl(cuse, hdr->unique, 0, NULL, 0);
		return;
	case VT_RELDISP:
		/*
		 * Acknowledge the VT release/acquire.  In VT_PROCESS
		 * mode the kernel expects this; we always succeed.
		 */
		cuse_reply_ioctl(cuse, hdr->unique, 0, NULL, 0);
		return;

	/*
	 * TIOCGPGRP and TIOCSPGRP check that the fd is the caller's
	 * controlling terminal.  Since the daemon's ctty is not the
	 * PTY slave, proxying fails with ENOTTY.  Handle them locally.
	 */
	case TIOCGPGRP: {
		pid_t pgrp = cuse->fg_pgrp;
		ioctl_read_val(cuse, hdr, in, &pgrp, sizeof(pgrp));
		return;
	}
	case TIOCSPGRP: {
		const void *data = ioctl_fetch_input(cuse, hdr, in,
						     sizeof(pid_t));
		pid_t pgrp;

		if (!data)
			return;
		memcpy(&pgrp, data, sizeof(pgrp));
		cuse->fg_pgrp = pgrp;
		log_debug("TIOCSPGRP: fg_pgrp=%d", (int)pgrp);
		cuse_reply_ioctl(cuse, hdr->unique, 0, NULL, 0);
		return;
	}
	}

	info = classify_ioctl(in->cmd);

	if (info.dir == IOCTL_DIR_UNKNOWN) {
		cuse_reply_err(cuse, hdr->unique, ENOTTY);
		return;
	}

	switch (info.dir) {
	case IOCTL_DIR_NONE:
		ret = ioctl(cuse->slave_fd, in->cmd,
			    (unsigned long)in->arg);
		if (ret < 0 && errno == EIO && cuse_reopen_slave(cuse) == 0)
			ret = ioctl(cuse->slave_fd, in->cmd,
				    (unsigned long)in->arg);
		if (ret < 0)
			cuse_reply_err(cuse, hdr->unique, errno);
		else
			cuse_reply_ioctl(cuse, hdr->unique, ret, NULL, 0);
		break;

	case IOCTL_DIR_WRITE: {
		const void *arg = ioctl_fetch_input(cuse, hdr, in,
						    info.size);
		if (!arg)
			break;
		ret = ioctl(cuse->slave_fd, in->cmd, arg);
		if (ret < 0 && errno == EIO &&
		    cuse_reopen_slave(cuse) == 0)
			ret = ioctl(cuse->slave_fd, in->cmd, arg);
		if (ret < 0)
			cuse_reply_err(cuse, hdr->unique, errno);
		else
			cuse_reply_ioctl(cuse, hdr->unique, ret, NULL, 0);
		break;
	}

	case IOCTL_DIR_READ: {
		char out_data[256];

		if (in->out_size < info.size) {
			struct fuse_ioctl_iovec fiov = {
				.base = in->arg,
				.len = info.size,
			};
			cuse_reply_ioctl_retry(cuse, hdr->unique,
					       NULL, 0, &fiov, 1);
			break;
		}
		ret = ioctl(cuse->slave_fd, in->cmd, out_data);
		if (ret < 0 && errno == EIO &&
		    cuse_reopen_slave(cuse) == 0)
			ret = ioctl(cuse->slave_fd, in->cmd, out_data);
		if (ret < 0)
			cuse_reply_err(cuse, hdr->unique, errno);
		else
			cuse_reply_ioctl(cuse, hdr->unique, ret,
					 out_data, info.size);
		break;
	}

	default:
		cuse_reply_err(cuse, hdr->unique, ENOTTY);
		break;
	}
}

/*
 * Poll notification -- write a FUSE_NOTIFY_POLL wakeup to kick any
 * blocked poll/select on the CUSE device.
 */

static void cuse_notify_poll(struct kmscon_cuse *cuse)
{
	struct {
		struct fuse_out_header hdr;
		struct fuse_notify_poll_wakeup_out notify;
	} msg;

	memset(&msg, 0, sizeof(msg));
	msg.hdr.len = sizeof(msg);
	msg.hdr.unique = 0;
	msg.hdr.error = FUSE_NOTIFY_POLL;
	msg.notify.kh = cuse->poll_kh;

	if (write(cuse->cuse_fd, &msg, sizeof(msg)) < 0)
		log_warn("FUSE_NOTIFY_POLL write failed: %m");
}

/*
 * Enable/disable slave_efd monitoring depending on whether we have a
 * deferred FUSE_READ or a registered poll notification to deliver.
 */
static void update_slave_monitoring(struct kmscon_cuse *cuse)
{
	if (cuse->read_pending || cuse->poll_registered)
		ev_fd_update(cuse->slave_efd, EV_READABLE);
	else
		ev_fd_update(cuse->slave_efd, 0);
}

/*
 * Re-open the PTY slave after vhangup().  /bin/login calls vhangup() to
 * revoke all existing file descriptors on the controlling terminal before
 * prompting for credentials.  Our slave_fd gets permanently revoked (EIO),
 * but a fresh open() on the same pts device returns a working fd.
 *
 * The old fd must be closed BEFORE opening the new one: the kernel's
 * tty_reopen() rejects opens while TTY_HUPPED is set, and that flag
 * only clears when the tty's open count drops to zero via tty_release().
 */
static int cuse_reopen_slave(struct kmscon_cuse *cuse)
{
	int fd, ret;

	if (!cuse->slave_path[0])
		return -ENOENT;

	if (cuse->slave_efd) {
		ev_eloop_rm_fd(cuse->slave_efd);
		cuse->slave_efd = NULL;
	}
	if (cuse->slave_fd >= 0) {
		close(cuse->slave_fd);
		cuse->slave_fd = -1;
	}

	fd = open(cuse->slave_path, O_RDWR | O_NONBLOCK | O_NOCTTY |
					     O_CLOEXEC);
	if (fd < 0) {
		log_warn("cannot re-open slave %s after vhangup: %m",
			 cuse->slave_path);
		return -errno;
	}

	cuse->slave_fd = fd;

	ret = ev_eloop_new_fd(cuse->eloop, &cuse->slave_efd, fd,
			      0, slave_readable, cuse);
	if (ret) {
		log_err("cannot re-register slave fd after vhangup");
		return ret;
	}
	update_slave_monitoring(cuse);

	log_info("re-opened slave %s after vhangup (fd=%d)",
		 cuse->slave_path, fd);
	return 0;
}

/*
 * FUSE_POLL -- poll the PTY slave and, if the kernel requests it,
 * register for asynchronous poll-wakeup notification.
 */

static void handle_poll(struct kmscon_cuse *cuse,
			const struct fuse_in_header *hdr,
			const void *payload)
{
	const struct fuse_poll_in *in = payload;
	struct fuse_poll_out out;
	struct pollfd pfd;

	if (in->flags & FUSE_POLL_SCHEDULE_NOTIFY) {
		cuse->poll_kh = in->kh;
		cuse->poll_registered = true;
	}

	memset(&pfd, 0, sizeof(pfd));
	pfd.fd = cuse->slave_fd;
	pfd.events = POLLIN | POLLOUT | POLLERR | POLLHUP;

	poll(&pfd, 1, 0);

	memset(&out, 0, sizeof(out));
	out.revents = pfd.revents;

	cuse_reply_buf(cuse, hdr->unique, &out, sizeof(out));

	update_slave_monitoring(cuse);
}

/*
 * Called when the PTY slave fd becomes readable while a FUSE_READ is
 * deferred.  Reads the available data and replies to the pending request.
 */
static void slave_readable(struct ev_fd *fd, int mask, void *data)
{
	struct kmscon_cuse *cuse = data;
	char tmp[CUSE_READ_BUFFER];
	ssize_t n;

	if (!(mask & EV_READABLE))
		return;

	if (cuse->poll_registered) {
		cuse_notify_poll(cuse);
		cuse->poll_registered = false;
	}

	if (cuse->read_pending) {
		n = read(cuse->slave_fd, tmp, cuse->read_size);
		if (n < 0 && errno == EIO && cuse_reopen_slave(cuse) == 0)
			n = read(cuse->slave_fd, tmp, cuse->read_size);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				goto out;
			log_warn("slave_readable: error %m");
			cuse_reply_err(cuse, cuse->read_unique, errno);
		} else {
			log_debug("slave_readable: delivering %zd bytes", n);
			cuse_reply_buf(cuse, cuse->read_unique, tmp, n);
		}
		cuse->read_pending = false;
	}

out:
	update_slave_monitoring(cuse);
}

/*
 * Request dispatcher -- called by eloop when the CUSE fd is readable.
 * Reads one fuse_in_header + payload and routes to the appropriate handler.
 */

static void cuse_event(struct ev_fd *fd, int mask, void *data)
{
	struct kmscon_cuse *cuse = data;
	struct fuse_in_header *hdr;
	const void *payload;
	ssize_t len;
	int num;

	if (mask & EV_ERR)
		log_warn("error on CUSE fd");
	if (mask & EV_HUP)
		log_debug("HUP on CUSE fd");
	if (!(mask & (EV_READABLE | EV_HUP | EV_ERR)))
		return;

	/*
	 * The CUSE fd is edge-triggered, so we must drain all pending
	 * messages in one shot -- no further notification until new data
	 * arrives after we empty the queue.
	 */
	num = 50;
	while (num--) {
		len = read(cuse->cuse_fd, cuse->buf, sizeof(cuse->buf));
		if (len < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK)
				log_warn("read from CUSE fd failed: %m");
			break;
		}

		if ((size_t)len < sizeof(struct fuse_in_header)) {
			log_warn("short CUSE read: %zd bytes", len);
			break;
		}

		hdr = (struct fuse_in_header *)cuse->buf;
		payload = cuse->buf + sizeof(*hdr);

		switch (hdr->opcode) {
		case FUSE_OPEN:
			handle_open(cuse, hdr);
			break;
		case FUSE_RELEASE:
			handle_release(cuse, hdr);
			break;
		case FUSE_GETATTR:
			handle_getattr(cuse, hdr);
			break;
		case FUSE_READ:
			handle_read(cuse, hdr, payload);
			break;
		case FUSE_WRITE:
			handle_write(cuse, hdr, payload);
			break;
		case FUSE_IOCTL:
			handle_ioctl(cuse, hdr, payload);
			break;
		case FUSE_POLL:
			handle_poll(cuse, hdr, payload);
			break;
		case FUSE_FLUSH:
			cuse_reply_ok(cuse, hdr->unique);
			break;
		case FUSE_INTERRUPT:
		case FUSE_FORGET:
			break;
		case FUSE_DESTROY:
			cuse_reply_ok(cuse, hdr->unique);
			break;
		default:
			cuse_reply_err(cuse, hdr->unique, ENOSYS);
			break;
		}
	}
}

/*
 * Open /dev/cuse, perform the CUSE_INIT handshake, and switch to
 * non-blocking mode.  On success cuse->cuse_fd is ready; on failure
 * it is closed and set to -1.  Returns 0 or negative errno.
 */
static int cuse_handshake(struct kmscon_cuse *cuse)
{
	struct fuse_in_header *hdr;
	struct cuse_init_in *init_in;
	struct fuse_out_header *out_hdr;
	struct cuse_init_out *init_out;
	char *info;
	char reply_buf[sizeof(struct fuse_out_header) +
		       sizeof(struct cuse_init_out) + 64];
	size_t info_len, reply_len;
	ssize_t n;
	int fl;

	cuse->cuse_fd = open("/dev/cuse", O_RDWR | O_CLOEXEC);
	if (cuse->cuse_fd < 0)
		return -errno;

	n = read(cuse->cuse_fd, cuse->buf, sizeof(cuse->buf));
	if (n < (ssize_t)(sizeof(struct fuse_in_header) +
			  sizeof(struct cuse_init_in))) {
		log_err("short CUSE_INIT read: %zd", n);
		goto fail;
	}

	hdr = (struct fuse_in_header *)cuse->buf;
	if (hdr->opcode != CUSE_INIT) {
		log_err("expected CUSE_INIT (opcode %u), got %u",
			CUSE_INIT, hdr->opcode);
		goto fail;
	}

	init_in = (struct cuse_init_in *)(cuse->buf + sizeof(*hdr));
	log_debug("CUSE_INIT request: kernel FUSE %u.%u, flags=0x%x",
		  init_in->major, init_in->minor, init_in->flags);

	memset(reply_buf, 0, sizeof(reply_buf));
	out_hdr = (struct fuse_out_header *)reply_buf;
	init_out = (struct cuse_init_out *)(reply_buf + sizeof(*out_hdr));
	info = reply_buf + sizeof(*out_hdr) + sizeof(*init_out);

	info_len = snprintf(info, 64, "DEVNAME=%s", cuse->devname) + 1;
	reply_len = sizeof(*out_hdr) + sizeof(*init_out) + info_len;

	out_hdr->len = reply_len;
	out_hdr->unique = hdr->unique;

	init_out->major = FUSE_KERNEL_VERSION;
	init_out->minor = FUSE_KERNEL_MINOR_VERSION;
	init_out->flags = CUSE_UNRESTRICTED_IOCTL;
	init_out->max_read = sizeof(cuse->buf);
	init_out->max_write = sizeof(cuse->buf) -
			      sizeof(struct fuse_in_header) -
			      sizeof(struct fuse_write_in);

	n = write(cuse->cuse_fd, reply_buf, reply_len);
	if (n < 0 || (size_t)n != reply_len) {
		log_err("CUSE_INIT reply write failed: %m");
		goto fail;
	}

	/*
	 * The kernel processes the CUSE_INIT reply synchronously during
	 * write().  If device_add() failed (e.g. stale sysfs entry with
	 * the same name), fuse_abort_conn() was called and the next read
	 * will return ENODEV.  Probe for this before committing.
	 */
	fl = fcntl(cuse->cuse_fd, F_GETFL);
	if (fl >= 0)
		fcntl(cuse->cuse_fd, F_SETFL, fl | O_NONBLOCK);

	n = read(cuse->cuse_fd, cuse->buf, sizeof(cuse->buf));
	if (n < 0 && errno == ENODEV) {
		close(cuse->cuse_fd);
		cuse->cuse_fd = -1;
		return -ENODEV;
	}

	/*
	 * If we read an actual FUSE request, process it later -- leave
	 * the data in cuse->buf.  Store the length so the event loop
	 * can pick it up.  If EAGAIN, the buffer is empty.
	 */
	cuse->init_done = true;
	return 0;

fail:
	close(cuse->cuse_fd);
	cuse->cuse_fd = -1;
	return -EIO;
}

/*
 * Constructor -- open /dev/cuse, perform CUSE_INIT handshake, register
 * with the event loop.
 *
 * The device is named ttyK<vtnr> to mirror the VT it serves.  If the
 * kernel rejects that name (stale sysfs entry from a leaked device --
 * see CUSE_KERNEL_BUG.md), we retry with a PID-qualified fallback.
 */

int kmscon_cuse_new(struct kmscon_cuse **out, struct ev_eloop *eloop,
		    int slave_fd, const char *slave_path,
		    unsigned int vtnr)
{
	struct kmscon_cuse *cuse;
	int ret;

	if (!out || !eloop || slave_fd < 0)
		return -EINVAL;

	cuse = calloc(1, sizeof(*cuse));
	if (!cuse)
		return -ENOMEM;

	cuse->cuse_fd = -1;
	cuse->spkr_fd = -1;
	cuse->kbd_fd = -1;
	cuse->kb_mode = K_UNICODE;
	cuse->kd_mode = KD_TEXT;
	cuse->vtnr = vtnr;
	cuse->vtmode.mode = VT_AUTO;
	cuse->slave_fd = slave_fd;
	if (slave_path)
		snprintf(cuse->slave_path, sizeof(cuse->slave_path),
			 "%s", slave_path);

	static const struct { unsigned char idx; const char *str; } fkey_defaults[] = {
		{  0, "\033[[A"  }, {  1, "\033[[B"  }, {  2, "\033[[C"  },
		{  3, "\033[[D"  }, {  4, "\033[[E"  }, {  5, "\033[17~" },
		{  6, "\033[18~" }, {  7, "\033[19~" }, {  8, "\033[20~" },
		{  9, "\033[21~" }, { 10, "\033[23~" }, { 11, "\033[24~" },
		{ 12, "\033[25~" }, { 13, "\033[26~" }, { 14, "\033[28~" },
		{ 15, "\033[29~" }, { 16, "\033[31~" }, { 17, "\033[32~" },
		{ 18, "\033[33~" }, { 19, "\033[34~" },
		{ 20, "\033[1~"  }, { 21, "\033[2~"  }, { 22, "\033[3~"  },
		{ 23, "\033[4~"  }, { 24, "\033[5~"  }, { 25, "\033[6~"  },
	};
	for (size_t i = 0; i < sizeof(fkey_defaults) / sizeof(fkey_defaults[0]); i++)
		cuse->func_strings[fkey_defaults[i].idx] =
			strdup(fkey_defaults[i].str);

	snprintf(cuse->devname, sizeof(cuse->devname), "ttyK%u", vtnr);

	ret = cuse_handshake(cuse);
	if (ret == -ENODEV) {
		/*
		 * The clean name is poisoned by a stale sysfs entry
		 * (kernel bug in cuse_process_init_reply -- see
		 * CUSE_KERNEL_BUG.md).  Retry with a unique fallback.
		 */
		log_warn("CUSE device name %s rejected (stale sysfs "
			 "entry?), retrying with fallback name",
			 cuse->devname);
		snprintf(cuse->devname, sizeof(cuse->devname),
			 "ttyK%u_%u_%u", vtnr, (unsigned)getpid(),
			 cuse_counter++);
		ret = cuse_handshake(cuse);
	}
	if (ret) {
		log_err("CUSE handshake failed for %s: %d",
			cuse->devname, ret);
		goto err_free;
	}

	log_info("CUSE device %s created", cuse->devname);

	ev_eloop_ref(eloop);
	cuse->eloop = eloop;

	ret = ev_eloop_new_fd(eloop, &cuse->efd, cuse->cuse_fd,
			      EV_ET | EV_READABLE, cuse_event, cuse);
	if (ret) {
		log_err("cannot register CUSE fd with eloop");
		goto err_eloop;
	}

	ret = ev_eloop_new_fd(eloop, &cuse->slave_efd, cuse->slave_fd,
			      0, slave_readable, cuse);
	if (ret) {
		log_err("cannot register slave fd with eloop");
		goto err_cuse_efd;
	}

	cuse->spkr_fd = find_evdev(EV_SND, SND_TONE);
	if (cuse->spkr_fd >= 0) {
		ret = ev_eloop_new_timer(eloop, &cuse->tone_timer, NULL,
					 tone_timeout, cuse);
		if (ret) {
			log_warn("cannot create tone timer: %d", ret);
			close(cuse->spkr_fd);
			cuse->spkr_fd = -1;
		}
	}

	cuse->kbd_fd = find_evdev(EV_LED, LED_NUML);

	*out = cuse;
	return 0;

err_cuse_efd:
	ev_eloop_rm_fd(cuse->efd);
	cuse->efd = NULL;
err_eloop:
	ev_eloop_unref(eloop);
	cuse->eloop = NULL;
	close(cuse->cuse_fd);
err_free:
	free(cuse);
	return ret;
}

void kmscon_cuse_free(struct kmscon_cuse *cuse)
{
	if (!cuse)
		return;

	log_info("destroying CUSE device %s", cuse->devname);

	if (cuse->tone_timer) {
		spkr_tone(cuse, 0);
		ev_eloop_rm_timer(cuse->tone_timer);
		ev_timer_unref(cuse->tone_timer);
	}
	if (cuse->spkr_fd >= 0)
		close(cuse->spkr_fd);
	if (cuse->kbd_fd >= 0)
		close(cuse->kbd_fd);
	if (cuse->slave_efd)
		ev_eloop_rm_fd(cuse->slave_efd);
	if (cuse->efd)
		ev_eloop_rm_fd(cuse->efd);
	if (cuse->cuse_fd >= 0)
		close(cuse->cuse_fd);
	for (int i = 0; i < 256; i++)
		free(cuse->func_strings[i]);
	if (cuse->eloop)
		ev_eloop_unref(cuse->eloop);
	free(cuse);
}

/*
 * Swap the underlying PTY slave fd without tearing down the CUSE device.
 * Used during child restart so the /dev/ttyK<N> device stays alive and
 * no blocking kernel operations (device_del, devtmpfs, CUSE_INIT handshake)
 * run inside the event loop dispatch chain.
 */
int kmscon_cuse_set_slave(struct kmscon_cuse *cuse, int new_slave_fd,
			  const char *slave_path)
{
	int ret;

	if (!cuse || new_slave_fd < 0)
		return -EINVAL;

	if (slave_path)
		snprintf(cuse->slave_path, sizeof(cuse->slave_path),
			 "%s", slave_path);

	if (cuse->read_pending) {
		cuse_reply_err(cuse, cuse->read_unique, EIO);
		cuse->read_pending = false;
	}

	cuse->poll_registered = false;

	if (cuse->slave_efd) {
		ev_eloop_rm_fd(cuse->slave_efd);
		cuse->slave_efd = NULL;
	}

	if (cuse->slave_fd >= 0)
		close(cuse->slave_fd);

	cuse->slave_fd = new_slave_fd;

	ret = ev_eloop_new_fd(cuse->eloop, &cuse->slave_efd, new_slave_fd,
			      0, slave_readable, cuse);
	if (ret) {
		log_err("cannot re-register slave fd with eloop");
		return ret;
	}

	cuse->fg_pgrp = 0;
	cuse->kd_mode = KD_TEXT;
	cuse->kb_mode = K_UNICODE;
	cuse->open_count = 0;
	memset(&cuse->vtmode, 0, sizeof(cuse->vtmode));
	cuse->vtmode.mode = VT_AUTO;

	update_slave_monitoring(cuse);

	log_info("CUSE device %s: slave fd swapped to %d",
		 cuse->devname, new_slave_fd);
	return 0;
}

const char *kmscon_cuse_get_devname(struct kmscon_cuse *cuse)
{
	if (!cuse)
		return NULL;

	return cuse->devname;
}
