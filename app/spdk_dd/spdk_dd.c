/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/event.h"
#include "spdk/fd.h"
#include "spdk/string.h"
#include "spdk/vmd.h"

#include <libaio.h>

#define DD_NSEC_SINCE_X(time_now, time_x) ((1000000000 * time_now.tv_sec + time_now.tv_nsec) \
											- (1000000000 * time_x.tv_sec + time_x.tv_nsec))

struct spdk_dd_opts {
	char		*input_file;
	char		*output_file;
	char		*input_bdev;
	char		*output_bdev;
	uint64_t	input_offset;
	uint64_t	output_offset;
	int64_t		io_unit_size;
	int64_t		io_unit_count;
	uint32_t	queue_depth;
};

static struct spdk_dd_opts g_opts = {
	.io_unit_size = 4096,
	.queue_depth = 2,
};

enum dd_submit_type {
	DD_POPULATE,
	DD_READ,
	DD_WRITE,
};

struct dd_io {
	uint64_t		offset;
	uint64_t		length;
	struct iocb		iocb;
	enum dd_submit_type	type;
	void			*buf;
};

enum dd_target_type {
	DD_TARGET_TYPE_FILE,
	DD_TARGET_TYPE_BDEV,
};

struct dd_target {
	enum dd_target_type	type;

	union {
		struct {
			struct spdk_bdev *bdev;
			struct spdk_bdev_desc *desc;
			struct spdk_io_channel *ch;
		} bdev;

		struct {
			int fd;
			io_context_t io_ctx;
			struct spdk_poller *poller;
		} aio;
	} u;

	/* Block size of underlying device. */
	uint32_t	block_size;

	/* Position of next I/O in bytes */
	uint64_t	pos;

	/* Total size of target in bytes */
	uint64_t	total_size;

	bool open;
};

struct dd_job {
	struct dd_target	input;
	struct dd_target	output;

	struct dd_io		*ios;

	uint32_t		outstanding;
	uint64_t		copy_size;
};

static struct dd_job g_job = {};
static int g_error = 0;
static struct timespec g_start_time;

static void dd_target_populate_buffer(struct dd_io *io);

static void
dd_exit(int rc)
{
	if (g_job.input.type == DD_TARGET_TYPE_FILE) {
		spdk_poller_unregister(&g_job.input.u.aio.poller);
		io_destroy(g_job.input.u.aio.io_ctx);
		close(g_job.input.u.aio.fd);
	} else if (g_job.input.type == DD_TARGET_TYPE_BDEV && g_job.input.open) {
		spdk_put_io_channel(g_job.input.u.bdev.ch);
		spdk_bdev_close(g_job.input.u.bdev.desc);
	}

	if (g_job.output.type == DD_TARGET_TYPE_FILE) {
		spdk_poller_unregister(&g_job.output.u.aio.poller);
		io_destroy(g_job.output.u.aio.io_ctx);
		close(g_job.output.u.aio.fd);
	} else if (g_job.output.type == DD_TARGET_TYPE_BDEV && g_job.output.open) {
		spdk_put_io_channel(g_job.output.u.bdev.ch);
		spdk_bdev_close(g_job.output.u.bdev.desc);
	}

	spdk_app_stop(rc);
}

static void
dd_show_progress(uint64_t offset, uint64_t length, bool finish)
{
	char *unit_str[5] = {"", "k", "M", "G", "T"};
	char *speed_type_str[2] = {"", "average "};
	char *size_unit_str = "";
	char *speed_unit_str = "";
	char *speed_type = "";
	uint64_t size = g_job.copy_size;
	uint64_t size_unit = 1;
	uint64_t speed_unit = 1;
	uint64_t speed, tmp_speed;
	static struct timespec g_time_last = {.tv_nsec = 0};
	static uint64_t g_data_last = 0;
	struct timespec time_now;
	int i = 0;

	clock_gettime(CLOCK_REALTIME, &time_now);

	if (((time_now.tv_sec == g_time_last.tv_sec && offset + length != g_job.copy_size) ||
	     (offset < g_data_last)) && !finish) {
		/* refresh every one second */
		return;
	}

	/* Find the rigth unit for size displaying (B vs kB vs MB vs GB vs TB) */
	while (size > 1024 * 10) {
		size /= 1024;
		size_unit *= 1024;
		size_unit_str = unit_str[++i];
		if (i == 4) {
			break;
		}
	}

	if (!finish) {
		speed_type = speed_type_str[0];
		tmp_speed = speed = (offset - g_data_last) * 1000000000 / DD_NSEC_SINCE_X(time_now, g_time_last);
	} else {
		speed_type = speed_type_str[1];
		tmp_speed = speed = offset * 1000000000 / DD_NSEC_SINCE_X(time_now, g_start_time);
	}

	i = 0;

	/* Find the rigth unit for speed displaying (Bps vs kBps vs MBps vs GBps vs TBps) */
	while (tmp_speed > 1024) {
		tmp_speed /= 1024;
		speed_unit *= 1024;
		speed_unit_str = unit_str[++i];
		if (i == 4) {
			break;
		}
	}

	printf("\33[2K\rCopying: %" PRIu64 "/%" PRIu64 " [%sB] (%s%" PRIu64 " %sBps)",
	       (offset + length) / size_unit, g_job.copy_size / size_unit, size_unit_str, speed_type,
	       speed / speed_unit, speed_unit_str);
	fflush(stdout);

	g_data_last = offset;
	g_time_last = time_now;
}

static void
_dd_write_bdev_done(struct spdk_bdev_io *bdev_io,
		    bool success,
		    void *cb_arg)
{
	struct dd_io *io = cb_arg;

	g_job.outstanding--;
	spdk_bdev_free_io(bdev_io);
	dd_target_populate_buffer(io);
}

static void
dd_target_write(struct dd_io *io)
{
	struct dd_target *target = &g_job.output;
	uint64_t length = SPDK_CEIL_DIV(io->length, target->block_size) * target->block_size;
	uint64_t read_region_start = g_opts.input_offset * g_opts.io_unit_size;
	uint64_t read_offset = io->offset - read_region_start;
	uint64_t write_region_start = g_opts.output_offset * g_opts.io_unit_size;
	uint64_t write_offset = write_region_start + read_offset;
	int rc = 0;

	if (g_error != 0) {
		if (g_job.outstanding == 0) {
			if (g_error == 0) {
				dd_show_progress(io->offset, io->length, true);
				printf("\n\n");
			}
			dd_exit(g_error);
		}
		return;
	}

	dd_show_progress(read_offset, io->length, false);

	g_job.outstanding++;
	io->type = DD_WRITE;

	if (target->type == DD_TARGET_TYPE_FILE) {
		struct iocb *iocb = &io->iocb;

		io_prep_pwrite(iocb, target->u.aio.fd, io->buf, length, write_offset);
		iocb->data = io;
		if (io_submit(target->u.aio.io_ctx, 1, &iocb) < 0) {
			rc = -errno;
		}
	} else if (target->type == DD_TARGET_TYPE_BDEV) {
		rc = spdk_bdev_write(target->u.bdev.desc, target->u.bdev.ch, io->buf, write_offset, length,
				     _dd_write_bdev_done, io);
	}

	if (rc != 0) {
		SPDK_ERRLOG("%s\n", strerror(-rc));
		g_job.outstanding--;
		if (g_job.outstanding == 0) {
			dd_exit(rc);
		} else {
			g_error = rc;
		}
		return;
	}
}

static void
_dd_read_bdev_done(struct spdk_bdev_io *bdev_io,
		   bool success,
		   void *cb_arg)
{
	struct dd_io *io = cb_arg;

	spdk_bdev_free_io(bdev_io);
	g_job.outstanding--;
	dd_target_write(io);
}

static void
dd_target_read(struct dd_io *io)
{
	struct dd_target *target = &g_job.input;
	int rc = 0;

	if (g_error != 0) {
		if (g_job.outstanding == 0) {
			dd_exit(g_error);
		}
		return;
	}

	g_job.outstanding++;
	io->type = DD_READ;

	if (target->type == DD_TARGET_TYPE_FILE) {
		struct iocb *iocb = &io->iocb;

		io_prep_pread(iocb, target->u.aio.fd, io->buf, io->length, io->offset);
		iocb->data = io;
		if (io_submit(target->u.aio.io_ctx, 1, &iocb) < 0) {
			rc = -errno;
		}
	} else if (target->type == DD_TARGET_TYPE_BDEV) {
		rc = spdk_bdev_read(target->u.bdev.desc, target->u.bdev.ch, io->buf, io->offset, io->length,
				    _dd_read_bdev_done, io);
	}

	if (rc != 0) {
		SPDK_ERRLOG("%s\n", strerror(-rc));
		g_job.outstanding--;
		if (g_job.outstanding == 0) {
			dd_exit(rc);
		} else {
			g_error = rc;
		}
		return;
	}
}

static void
_dd_target_populate_buffer_done(struct spdk_bdev_io *bdev_io,
				bool success,
				void *cb_arg)
{
	struct dd_io *io = cb_arg;

	g_job.outstanding--;
	spdk_bdev_free_io(bdev_io);
	dd_target_read(io);
}

static void
dd_target_populate_buffer(struct dd_io *io)
{
	struct dd_target *target = &g_job.output;
	uint64_t read_region_start = g_opts.input_offset * g_opts.io_unit_size;
	uint64_t read_offset = g_job.input.pos - read_region_start;
	uint64_t write_region_start = g_opts.output_offset * g_opts.io_unit_size;
	uint64_t write_offset = write_region_start + read_offset;
	uint64_t length;
	int rc = 0;

	io->offset = g_job.input.pos;
	io->length = spdk_min((uint64_t)g_opts.io_unit_size, g_job.copy_size - read_offset);

	if (io->length == 0 || g_error != 0) {
		if (g_job.outstanding == 0) {
			if (g_error == 0) {
				dd_show_progress(read_offset, io->length, true);
				printf("\n\n");
			}
			dd_exit(g_error);
		}
		return;
	}

	g_job.input.pos += io->length;

	if ((io->length % target->block_size) == 0) {
		dd_target_read(io);
		return;
	}

	/* Read whole blocks from output to combine buffers later */
	g_job.outstanding++;
	io->type = DD_POPULATE;

	length = SPDK_CEIL_DIV(io->length, target->block_size) * target->block_size;

	if (target->type == DD_TARGET_TYPE_FILE) {
		struct iocb *iocb = &io->iocb;

		io_prep_pread(iocb, target->u.aio.fd, io->buf, length, write_offset);
		iocb->data = io;
		if (io_submit(target->u.aio.io_ctx, 1, &iocb) < 0) {
			rc = -errno;
		}
	} else if (target->type == DD_TARGET_TYPE_BDEV) {
		rc = spdk_bdev_read(target->u.bdev.desc, target->u.bdev.ch, io->buf, write_offset, length,
				    _dd_target_populate_buffer_done, io);
	}

	if (rc != 0) {
		SPDK_ERRLOG("%s\n", strerror(-rc));
		g_job.outstanding--;
		if (g_job.outstanding == 0) {
			dd_exit(rc);
		} else {
			g_error = rc;
		}
		return;
	}
}

static void
dd_complete_poll(struct dd_io *io)
{
	g_job.outstanding--;

	switch (io->type) {
	case DD_POPULATE:
		dd_target_read(io);
		break;
	case DD_READ:
		dd_target_write(io);
		break;
	case DD_WRITE:
		dd_target_populate_buffer(io);
		break;
	default:
		assert(false);
		break;
	}
}

static int
dd_aio_poll(io_context_t io_ctx)
{
	struct io_event events[32];
	int rc = 0;
	int i;
	struct timespec timeout;
	struct dd_io *io;

	timeout.tv_sec = 0;
	timeout.tv_nsec = 0;

	rc = io_getevents(io_ctx, 0, 32, events, &timeout);

	if (rc < 0) {
		SPDK_ERRLOG("%s\n", strerror(-rc));
		dd_exit(rc);
	}

	for (i = 0; i < rc; i++) {
		io = events[i].data;
		if (events[i].res != io->length) {
			g_error = rc = -ENOSPC;
		}

		dd_complete_poll(io);
	}

	return rc;
}

static int
dd_input_poll(void *ctx)
{
	int rc = 0;

	if (g_job.input.type == DD_TARGET_TYPE_FILE) {
		rc = dd_aio_poll(g_job.input.u.aio.io_ctx);
		if (rc == -ENOSPC) {
			SPDK_ERRLOG("No more file content to read\n");
		}
	}

	return rc;
}

static int
dd_output_poll(void *ctx)
{
	int rc = 0;

	if (g_job.output.type == DD_TARGET_TYPE_FILE) {
		rc = dd_aio_poll(g_job.output.u.aio.io_ctx);
		if (rc == -ENOSPC) {
			SPDK_ERRLOG("No space left on device\n");
		}
	}

	return rc;
}

static int
dd_open_file(struct dd_target *target, const char *fname, uint64_t skip_blocks, bool input)
{
	target->type = DD_TARGET_TYPE_FILE;
	target->u.aio.fd = open(fname, O_RDWR);
	if (target->u.aio.fd < 0) {
		SPDK_ERRLOG("Could not open file %s: %s\n", fname, strerror(errno));
		return target->u.aio.fd;
	}

	target->block_size = spdk_max(spdk_fd_get_blocklen(target->u.aio.fd), 1);
	target->total_size = spdk_fd_get_size(target->u.aio.fd);

	if (input == true) {
		g_opts.queue_depth = spdk_min(g_opts.queue_depth,
					      (target->total_size / g_opts.io_unit_size) - skip_blocks + 1);
	}

	if (g_opts.io_unit_count != 0) {
		g_opts.queue_depth = spdk_min(g_opts.queue_depth, g_opts.io_unit_count);
	}

	return io_setup(g_opts.queue_depth, &target->u.aio.io_ctx);
}

static int
dd_open_bdev(struct dd_target *target, const char *bdev_name, uint64_t skip_blocks)
{
	int rc;

	target->type = DD_TARGET_TYPE_BDEV;
	target->u.bdev.bdev = spdk_bdev_get_by_name(bdev_name);
	if (target->u.bdev.bdev == NULL) {
		SPDK_ERRLOG("Could not find bdev %s\n", bdev_name);
		return -EINVAL;
	}

	target->block_size = spdk_bdev_get_block_size(target->u.bdev.bdev);
	target->total_size = spdk_bdev_get_num_blocks(target->u.bdev.bdev) * target->block_size;

	rc = spdk_bdev_open(target->u.bdev.bdev, true, NULL, NULL, &target->u.bdev.desc);
	if (rc < 0) {
		SPDK_ERRLOG("Could not open bdev %s: %s\n", bdev_name, strerror(-rc));
		return rc;
	}

	target->open = true;

	target->u.bdev.ch = spdk_bdev_get_io_channel(target->u.bdev.desc);
	if (target->u.bdev.ch == NULL) {
		spdk_bdev_close(target->u.bdev.desc);
		SPDK_ERRLOG("Could not get I/O channel: %s\n", strerror(ENOMEM));
		return -ENOMEM;
	}

	g_opts.queue_depth = spdk_min(g_opts.queue_depth,
				      (target->total_size / g_opts.io_unit_size) - skip_blocks + 1);

	if (g_opts.io_unit_count != 0) {
		g_opts.queue_depth = spdk_min(g_opts.queue_depth, g_opts.io_unit_count);
	}

	return 0;
}

static void
dd_run(void *arg1)
{
	uint64_t write_size;
	uint32_t i;
	int rc;

	if (g_opts.input_file) {
		if (dd_open_file(&g_job.input, g_opts.input_file, g_opts.input_offset, true) < 0) {
			SPDK_ERRLOG("%s: %s\n", g_opts.input_file, strerror(errno));
			dd_exit(-errno);
			return;
		}
		g_job.input.u.aio.poller = spdk_poller_register(dd_input_poll, NULL, 0);
	} else if (g_opts.input_bdev) {
		rc = dd_open_bdev(&g_job.input, g_opts.input_bdev, g_opts.input_offset);
		if (rc < 0) {
			SPDK_ERRLOG("%s: %s\n", g_opts.input_bdev, strerror(-rc));
			dd_exit(rc);
			return;
		}
	}

	write_size = g_opts.io_unit_count * g_opts.io_unit_size;
	g_job.input.pos = g_opts.input_offset * g_opts.io_unit_size;

	/* We cannot check write size for input files because /dev/zeros, /dev/random, etc would not work.
	 * We will handle that during copying */
	if (g_opts.input_bdev && g_job.input.pos > g_job.input.total_size) {
		SPDK_ERRLOG("--skip value too big (%" PRIu64 ") - only %" PRIu64 " blocks available in input\n",
			    g_opts.input_offset, g_job.input.total_size / g_opts.io_unit_size);
		dd_exit(-ENOSPC);
		return;
	}

	if (g_opts.io_unit_count != 0 && g_opts.input_bdev &&
	    write_size + g_job.input.pos > g_job.input.total_size) {
		SPDK_ERRLOG("--count value too big (%" PRIu64 ") - only %" PRIu64 " blocks available from input\n",
			    g_opts.io_unit_count, (g_job.input.total_size - g_job.input.pos) / g_opts.io_unit_size);
		dd_exit(-ENOSPC);
		return;
	}

	if (g_opts.io_unit_count != 0) {
		g_job.copy_size = write_size;
	} else {
		g_job.copy_size = g_job.input.total_size - g_job.input.pos;
	}

	g_job.output.pos = g_opts.output_offset * g_opts.io_unit_size;

	if (g_opts.output_file) {
		if (dd_open_file(&g_job.output, g_opts.output_file, g_opts.output_offset, false) < 0) {
			SPDK_ERRLOG("%s: %s\n", g_opts.output_file, strerror(errno));
			dd_exit(-errno);
			return;
		}
		g_job.output.u.aio.poller = spdk_poller_register(dd_output_poll, NULL, 0);
	} else if (g_opts.output_bdev) {
		rc = dd_open_bdev(&g_job.output, g_opts.output_bdev, g_opts.output_offset);
		if (rc < 0) {
			SPDK_ERRLOG("%s: %s\n", g_opts.output_bdev, strerror(-rc));
			dd_exit(rc);
			return;
		}

		if (g_job.output.pos > g_job.output.total_size) {
			SPDK_ERRLOG("--seek value too big (%" PRIu64 ") - only %" PRIu64 " blocks available in output\n",
				    g_opts.output_offset, g_job.output.total_size / g_opts.io_unit_size);
			dd_exit(-ENOSPC);
			return;
		}

		if (g_opts.io_unit_count != 0 && write_size + g_job.output.pos > g_job.output.total_size) {
			SPDK_ERRLOG("--count value too big (%" PRIu64 ") - only %" PRIu64 " blocks available in output\n",
				    g_opts.io_unit_count, (g_job.output.total_size - g_job.output.pos) / g_opts.io_unit_size);
			dd_exit(-ENOSPC);
			return;
		}
	}

	if ((g_job.output.block_size > g_opts.io_unit_size) ||
	    (g_job.input.block_size > g_opts.io_unit_size)) {
		SPDK_ERRLOG("--bs value cannot be less than input (%d) neither output (%d) native block size\n",
			    g_job.input.block_size, g_job.output.block_size);
		dd_exit(-EINVAL);
		return;
	}

	g_job.ios = calloc(g_opts.queue_depth, sizeof(struct dd_io));
	if (g_job.ios == NULL) {
		SPDK_ERRLOG("%s\n", strerror(ENOMEM));
		dd_exit(-ENOMEM);
		return;
	}

	for (i = 0; i < g_opts.queue_depth; i++) {
		g_job.ios[i].buf = spdk_malloc(g_opts.io_unit_size, 0x1000, NULL, 0, SPDK_MALLOC_DMA);
		if (g_job.ios[i].buf == NULL) {
			SPDK_ERRLOG("%s - try smaller block size value\n", strerror(ENOMEM));
			dd_exit(-ENOMEM);
			return;
		}
	}

	clock_gettime(CLOCK_REALTIME, &g_start_time);

	for (i = 0; i < g_opts.queue_depth; i++) {
		dd_target_populate_buffer(&g_job.ios[i]);
	}

}

enum dd_cmdline_opts {
	DD_OPTION_IF = 0x1000,
	DD_OPTION_OF,
	DD_OPTION_IB,
	DD_OPTION_OB,
	DD_OPTION_SKIP,
	DD_OPTION_SEEK,
	DD_OPTION_BS,
	DD_OPTION_QD,
	DD_OPTION_COUNT,
};

static struct option g_cmdline_opts[] = {
	{
		.name = "if",
		.has_arg = 1,
		.flag = NULL,
		.val = DD_OPTION_IF,
	},
	{
		.name = "of",
		.has_arg = 1,
		.flag = NULL,
		.val = DD_OPTION_OF,
	},
	{
		.name = "ib",
		.has_arg = 1,
		.flag = NULL,
		.val = DD_OPTION_IB,
	},
	{
		.name = "ob",
		.has_arg = 1,
		.flag = NULL,
		.val = DD_OPTION_OB,
	},
	{
		.name = "skip",
		.has_arg = 1,
		.flag = NULL,
		.val = DD_OPTION_SKIP,
	},
	{
		.name = "seek",
		.has_arg = 1,
		.flag = NULL,
		.val = DD_OPTION_SEEK,
	},
	{
		.name = "bs",
		.has_arg = 1,
		.flag = NULL,
		.val = DD_OPTION_BS,
	},
	{
		.name = "qd",
		.has_arg = 1,
		.flag = NULL,
		.val = DD_OPTION_QD,
	},
	{
		.name = "count",
		.has_arg = 1,
		.flag = NULL,
		.val = DD_OPTION_COUNT,
	},
	{
		.name = NULL
	}
};

static void
usage(void)
{
	printf("[--------- DD Options ---------]\n");
	printf(" --if Input file. Must specify either --if or --ib.\n");
	printf(" --ib Input bdev. Must specifier either --if or --ib\n");
	printf(" --of Output file. Must specify either --of or --ob.\n");
	printf(" --ob Output bdev. Must specify either --of or --ob.\n");
	printf(" --bs I/O unit size (default: %" PRId64 ")\n", g_opts.io_unit_size);
	printf(" --qd Queue depth (default: %d)\n", g_opts.queue_depth);
	printf(" --count I/O unit count. The number of I/O units to copy. (default: all)\n");
	printf(" --skip Skip this many I/O units at start of input. (default: 0)\n");
	printf(" --seek Skip this many I/O units at start of output. (default: 0)\n");
}

static int
parse_args(int argc, char *argv)
{
	switch (argc) {
	case DD_OPTION_IF:
		g_opts.input_file = strdup(argv);
		break;
	case DD_OPTION_OF:
		g_opts.output_file = strdup(argv);
		break;
	case DD_OPTION_IB:
		g_opts.input_bdev = strdup(argv);
		break;
	case DD_OPTION_OB:
		g_opts.output_bdev = strdup(argv);
		break;
	case DD_OPTION_SKIP:
		g_opts.input_offset = spdk_strtol(optarg, 10);
		break;
	case DD_OPTION_SEEK:
		g_opts.output_offset = spdk_strtol(optarg, 10);
		break;
	case DD_OPTION_BS:
		g_opts.io_unit_size = spdk_strtol(optarg, 10);
		break;
	case DD_OPTION_QD:
		g_opts.queue_depth = spdk_strtol(optarg, 10);
		break;
	case DD_OPTION_COUNT:
		g_opts.io_unit_count = spdk_strtol(optarg, 10);
		break;
	default:
		usage();
		return 1;
	}
	return 0;
}

static void
dd_free(void)
{
	uint32_t i;

	free(g_opts.input_file);
	free(g_opts.output_file);
	free(g_opts.input_bdev);
	free(g_opts.output_bdev);

	if (g_job.ios) {
		for (i = 0; i < g_opts.queue_depth; i++) {
			spdk_free(g_job.ios[i].buf);
		}

		free(g_job.ios);
	}
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	int rc = 1;

	spdk_app_opts_init(&opts);
	opts.reactor_mask = "0x1";
	rc = spdk_app_parse_args(argc, argv, &opts, "", g_cmdline_opts, parse_args, usage);
	if (rc == SPDK_APP_PARSE_ARGS_FAIL) {
		SPDK_ERRLOG("Invalid arguments\n");
		goto end;
	} else if (rc == SPDK_APP_PARSE_ARGS_HELP) {
		goto end;
	}

	if (g_opts.input_file != NULL && g_opts.input_bdev != NULL) {
		SPDK_ERRLOG("You may specify either --if or --ib, but not both.\n");
		rc = EINVAL;
		goto end;
	}

	if (g_opts.output_file != NULL && g_opts.output_bdev != NULL) {
		SPDK_ERRLOG("You may specify either --of or --ob, but not both.\n");
		rc = EINVAL;
		goto end;
	}

	if (g_opts.input_file == NULL && g_opts.input_bdev == NULL) {
		SPDK_ERRLOG("You must specify either --if or --ib\n");
		rc = EINVAL;
		goto end;
	}

	if (g_opts.output_file == NULL && g_opts.output_bdev == NULL) {
		SPDK_ERRLOG("You must specify either --of or --ob\n");
		rc = EINVAL;
		goto end;
	}

	if (g_opts.io_unit_size <= 0) {
		SPDK_ERRLOG("Invalid --bs value\n");
		rc = EINVAL;
		goto end;
	}

	if (g_opts.io_unit_count < 0) {
		SPDK_ERRLOG("Invalid --count value\n");
		rc = EINVAL;
		goto end;
	}

	rc = spdk_app_start(&opts, dd_run, NULL);
	if (rc) {
		SPDK_ERRLOG("Error occured while performing copy\n");
	}

	dd_free();
	spdk_app_fini();

end:
	return rc;
}
