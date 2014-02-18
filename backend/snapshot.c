/*
 * Copyright (C) 2014 John Crispin <blogic@openwrt.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2.1
 * as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <sys/stat.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <mtd/mtd-user.h>

#include <glob.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <libgen.h>
#include <unistd.h>
#include <string.h>

#include <libubox/list.h>
#include <libubox/blob.h>
#include <libubox/md5.h>

#include "../fs-state.h"
#include "../lib/mtd.h"

#define PATH_MAX	256
#define OWRT		0x4f575254
#define DATA		0x44415441
#define CONF		0x434f4e46

struct file_header {
	uint32_t magic;
	uint32_t type;
	uint32_t seq;
	uint32_t length;
	uint32_t md5[4];
};

static inline int
is_config(struct file_header *h)
{
	return ((h->magic == OWRT) && (h->type == CONF));
}

static inline int
valid_file_size(int fs)
{
	if ((fs > 8 * 1024 * 1204) || (fs <= 0))
		return -1;

	return 0;
}

static void
hdr_to_be32(struct file_header *hdr)
{
	uint32_t *h = (uint32_t *) hdr;
	int i;

	for (i = 0; i < sizeof(struct file_header) / sizeof(uint32_t); i++)
		h[i] = cpu_to_be32(h[i]);
}

static void
be32_to_hdr(struct file_header *hdr)
{
	uint32_t *h = (uint32_t *) hdr;
	int i;

	for (i = 0; i < sizeof(struct file_header) / sizeof(uint32_t); i++)
		h[i] = be32_to_cpu(h[i]);
}

static int
pad_file_size(int size)
{
	int mod;

	size += sizeof(struct file_header);
	mod = size % erasesize;
	if (mod) {
		size -= mod;
		size += erasesize;
	}

	return size;
}

static int
verify_file_hash(char *file, uint32_t *hash)
{
	uint32_t md5[4];

	if (md5sum(file, md5)) {
		fprintf(stderr, "failed to generate md5 sum\n");
		return -1;
	}

	if (memcmp(md5, hash, sizeof(md5))) {
		fprintf(stderr, "failed to verify hash of %s.\n", file);
		return -1;
	}

	return 0;
}

static int
snapshot_next_free(int fd, uint32_t *seq)
{
	struct file_header hdr = { 0 };
	int block = 0;

	*seq = rand();

	do {
		if (mtd_read_buffer(fd, &hdr, block * erasesize, sizeof(struct file_header))) {
			fprintf(stderr, "scanning for next free block failed\n");
			return 0;
		}

		be32_to_hdr(&hdr);

		if (hdr.magic != OWRT)
			break;

		if (hdr.type == DATA && !valid_file_size(hdr.length)) {
			if (*seq + 1 != hdr.seq && block)
				return block;
			*seq = hdr.seq;
			block += pad_file_size(hdr.length) / erasesize;
		}
	} while (hdr.type == DATA);

	return block;
}

static int
config_find(int fd, struct file_header *conf, struct file_header *sentinel)
{
	uint32_t seq;
	int i, next = snapshot_next_free(fd, &seq);

	conf->magic = sentinel->magic = 0;

	if (!mtd_read_buffer(fd, conf, next, sizeof(*conf)))
		be32_to_hdr(conf);

	for (i = (mtdsize / erasesize) - 1; i > 0; i--) {
		if (mtd_read_buffer(fd, sentinel,  i * erasesize, sizeof(*sentinel))) {
			fprintf(stderr, "failed to read header\n");
			return -1;
		}
		be32_to_hdr(sentinel);

		if (sentinel->magic == OWRT && sentinel->type == CONF && !valid_file_size(sentinel->length)) {
			if (next == i)
				return -1;
			return i;
		}
	}

	return -1;
}

static int
snapshot_info(void)
{
	int fd = mtd_load("rootfs_data");
	struct file_header hdr = { 0 }, conf;
	int block = 0;

	if (fd < 1)
		return -1;

	fprintf(stderr, "sectors:\t%d, erasesize:\t%dK\n", mtdsize / erasesize, erasesize / 1024);
	do {
		if (mtd_read_buffer(fd, &hdr, block * erasesize, sizeof(struct file_header))) {
			fprintf(stderr, "scanning for next free block failed\n");
			close(fd);
			return 0;
		}

		be32_to_hdr(&hdr);

		if (hdr.magic != OWRT)
			break;

		if (hdr.type == DATA)
			fprintf(stderr, "block %d:\tsnapshot entry, size: %d, sectors: %d, sequence: %d\n", block,  hdr.length, pad_file_size(hdr.length) / erasesize, hdr.seq);
		else if (hdr.type == CONF)
			fprintf(stderr, "block %d:\tvolatile entry, size: %d, sectors: %d, sequence: %d\n", block,  hdr.length, pad_file_size(hdr.length) / erasesize, hdr.seq);

		if (hdr.type == DATA && !valid_file_size(hdr.length))
			block += pad_file_size(hdr.length) / erasesize;
	} while (hdr.type == DATA);
	block = config_find(fd, &conf, &hdr);
	if (block > 0)
		fprintf(stderr, "block %d:\tsentinel entry, size: %d, sectors: %d, sequence: %d\n", block, hdr.length, pad_file_size(hdr.length) / erasesize, hdr.seq);
	close(fd);
	return 0;
}

static int
snapshot_write_file(int fd, int block, char *file, uint32_t seq, uint32_t type)
{
	uint32_t md5[4] = { 0 };
	struct file_header hdr;
	struct stat s;
        char buffer[256];
	int in = 0, len, offset;
	int ret = -1;

	if (stat(file, &s) || md5sum(file, md5)) {
		fprintf(stderr, "stat failed on %s\n", file);
		goto out;
	}

	if ((block * erasesize) + pad_file_size(s.st_size) > mtdsize) {
		fprintf(stderr, "upgrade is too big for the flash\n");
		goto out;
	}
	mtd_erase(fd, block, (pad_file_size(s.st_size) / erasesize));
	mtd_erase(fd, block + (pad_file_size(s.st_size) / erasesize), 1);

	hdr.length = s.st_size;
	hdr.magic = OWRT;
	hdr.type = type;
	hdr.seq = seq;
	memcpy(hdr.md5, md5, sizeof(md5));
	hdr_to_be32(&hdr);

	if (mtd_write_buffer(fd, &hdr, block * erasesize, sizeof(struct file_header))) {
		fprintf(stderr, "failed to write header\n");
		goto out;
	}

	in = open(file, O_RDONLY);
	if (in < 1) {
		fprintf(stderr, "failed to open %s\n", file);
		goto out;
	}

	offset = (block * erasesize) + sizeof(struct file_header);

	while ((len = read(in, buffer, sizeof(buffer))) > 0) {
		if (mtd_write_buffer(fd, buffer, offset, len) < 0)
			goto out;
		offset += len;
	}

	ret = 0;

out:
	if (in > 0)
		close(in);

	return ret;
}

static int
snapshot_read_file(int fd, int block, char *file, uint32_t type)
{
	struct file_header hdr;
	char buffer[256];
	int out;

	if (mtd_read_buffer(fd, &hdr, block * erasesize, sizeof(struct file_header))) {
		fprintf(stderr, "failed to read header\n");
		return -1;
	}
	be32_to_hdr(&hdr);

	if (hdr.magic != OWRT)
		return -1;

	if (hdr.type != type)
		return -1;

	if (valid_file_size(hdr.length))
		return -1;

	out = open(file, O_WRONLY | O_CREAT, 0700);
	if (!out) {
		fprintf(stderr, "failed to open %s\n", file);
		return -1;
	}

	while (hdr.length > 0) {
		int len = sizeof(buffer);

		if (hdr.length < len)
			len = hdr.length;

		if ((read(fd, buffer, len) != len) || (write(out, buffer, len) != len)) {
			return -1;
		}

		hdr.length -= len;
	}

	close(out);

	if (verify_file_hash(file, hdr.md5)) {
		fprintf(stderr, "md5 verification failed\n");
		unlink(file);
		return 0;
	}

        block += pad_file_size(hdr.length) / erasesize;

	return block;
}

static int
sentinel_write(int fd, uint32_t _seq)
{
	int ret, block;
	struct stat s;
	uint32_t seq;

	if (stat("/tmp/config.tar.gz", &s)) {
		fprintf(stderr, "failed to stat /tmp/config.tar.gz\n");
		return -1;
	}

	snapshot_next_free(fd, &seq);
	if (_seq)
		seq = _seq;
	block = mtdsize / erasesize;
	block -= pad_file_size(s.st_size) / erasesize;
	if (block < 0)
		block = 0;

	ret = snapshot_write_file(fd, block, "/tmp/config.tar.gz", seq, CONF);
	if (ret)
		fprintf(stderr, "failed to write sentinel\n");
	else
		fprintf(stderr, "wrote /tmp/config.tar.gz sentinel\n");
	return ret;
}

static int
volatile_write(int fd, uint32_t _seq)
{
	int block, ret;
	uint32_t seq;

	block = snapshot_next_free(fd, &seq);
	if (_seq)
		seq = _seq;
	if (block < 0)
		block = 0;

	ret = snapshot_write_file(fd, block, "/tmp/config.tar.gz", seq, CONF);
	if (ret)
		fprintf(stderr, "failed to write /tmp/config.tar.gz\n");
	else
		fprintf(stderr, "wrote /tmp/config.tar.gz\n");
	return ret;
}

static int
config_write(int argc, char **argv)
{
	int fd, ret;

	fd = mtd_load("rootfs_data");
	if (fd < 1) {
		fprintf(stderr, "failed to open rootfs_config\n");
		return -1;
	}

	ret = volatile_write(fd, 0);
	if (!ret)
		ret = sentinel_write(fd, 0);

	close(fd);

	return ret;
}

static int
config_read(int argc, char **argv)
{
	struct file_header conf, sentinel;
	int fd, next, block, ret = 0;
	uint32_t seq;

	fd = mtd_load("rootfs_data");
	if (fd < 1) {
		fprintf(stderr, "failed to open rootfs_data\n");
		return -1;
	}

	block = config_find(fd, &conf, &sentinel);
	next = snapshot_next_free(fd, &seq);
	if (is_config(&conf) && conf.seq == seq)
		block = next;
	else if (!is_config(&sentinel) || sentinel.seq != seq)
		return -1;

	unlink("/tmp/config.tar.gz");
	ret = snapshot_read_file(fd, block, "/tmp/config.tar.gz", CONF);

	if (ret < 1)
		fprintf(stderr, "failed to read /tmp/config.tar.gz\n");
	close(fd);
	return ret;
}

static int
snapshot_write(int argc, char **argv)
{
	int mtd, block, ret;
	uint32_t seq;

	mtd = mtd_load("rootfs_data");
	if (mtd < 1) {
		fprintf(stderr, "failed to open rootfs_data\n");
		return -1;
	}

	block = snapshot_next_free(mtd, &seq);
	if (block < 0)
		block = 0;

	ret = snapshot_write_file(mtd, block, "/tmp/snapshot.tar.gz", seq + 1, DATA);
	if (ret)
		fprintf(stderr, "failed to write /tmp/snapshot.tar.gz\n");
	else
		fprintf(stderr, "wrote /tmp/snapshot.tar.gz\n");

	close(mtd);

	return ret;
}

static int
snapshot_mark(int argc, char **argv)
{
	FILE *fp;
	__be32 owrt = cpu_to_be32(OWRT);
	char mtd[32];
	size_t sz;

	fprintf(stderr, "This will remove all snapshot data stored on the system. Are you sure? [N/y]\n");
	if (getchar() != 'y')
		return -1;

	if (find_mtd_block("rootfs_data", mtd, sizeof(mtd))) {
		fprintf(stderr, "no rootfs_data was found\n");
		return -1;
	}

	fp = fopen(mtd, "w");
	fprintf(stderr, "%s - marking with 0x4f575254\n", mtd);
	if (!fp) {
		fprintf(stderr, "opening %s failed\n", mtd);
		return -1;
	}

	sz = fwrite(&owrt, sizeof(owrt), 1, fp);
	fclose(fp);

	if (sz != 1) {
		fprintf(stderr, "writing %s failed: %s\n", mtd, strerror(errno));
		return -1;
	}

	return 0;
}

static int
snapshot_read(int argc, char **argv)
{
	char file[64];
	int block = 0, fd, ret = 0;

	fd = mtd_load("rootfs_data");
	if (fd < 1) {
		fprintf(stderr, "failed to open rootfs_data\n");
		return -1;
	}

	if (argc > 1) {
		block = atoi(argv[1]);
		if (block >= (mtdsize / erasesize)) {
			fprintf(stderr, "invalid block %d > %d\n", block, mtdsize / erasesize);
			goto out;
		}
		snprintf(file, sizeof(file), "/tmp/snapshot/block%d.tar.gz", block);

		ret = snapshot_read_file(fd, block, file, DATA);
		goto out;
	}

	do {
		snprintf(file, sizeof(file), "/tmp/snapshot/block%d.tar.gz", block);
		block = snapshot_read_file(fd, block, file, DATA);
	} while (block > 0);

out:
	close(fd);
	return ret;
}

static int
snapshot_sync(void)
{
	int fd = mtd_load("rootfs_data");
	struct file_header sentinel, conf;
	int next, block = 0;
	uint32_t seq;

	if (fd < 1)
		return -1;

	next = snapshot_next_free(fd, &seq);
	block = config_find(fd, &conf, &sentinel);
	if (is_config(&conf) && conf.seq != seq) {
		conf.magic = 0;
		mtd_erase(fd, next, 2);
	}

	if (is_config(&sentinel) && (sentinel.seq != seq)) {
		sentinel.magic = 0;
		mtd_erase(fd, block, 1);
	}

	if (!is_config(&conf) && !is_config(&sentinel)) {
	//	fprintf(stderr, "no config found\n");
	} else if (((is_config(&conf) && is_config(&sentinel)) &&
				(memcmp(conf.md5, sentinel.md5, sizeof(conf.md5)) || (conf.seq != sentinel.seq))) ||
			(is_config(&conf) && !is_config(&sentinel))) {
		uint32_t seq;
		int next = snapshot_next_free(fd, &seq);
		int ret = snapshot_read_file(fd, next, "/tmp/config.tar.gz", CONF);
		if (ret > 0) {
			if (sentinel_write(fd, conf.seq))
				fprintf(stderr, "failed to write sentinel data");
		}
	} else if (!is_config(&conf) && is_config(&sentinel) && next) {
		int ret = snapshot_read_file(fd, block, "/tmp/config.tar.gz", CONF);
		if (ret > 0)
			if (volatile_write(fd, sentinel.seq))
				fprintf(stderr, "failed to write sentinel data");
	} else
		fprintf(stderr, "config in sync\n");

	unlink("/tmp/config.tar.gz");
	close(fd);

	return 0;
}

static int
_ramoverlay(char *rom, char *overlay)
{
	mount("tmpfs", overlay, "tmpfs", MS_NOATIME, "mode=0755");
	return fopivot(overlay, rom);
}

static int
snapshot_mount(void)
{
	snapshot_sync();
	setenv("SNAPSHOT", "magic", 1);
	_ramoverlay("/rom", "/overlay");
	system("/sbin/snapshot unpack");
	foreachdir("/overlay/", handle_whiteout);
	mkdir("/volatile", 0700);
	_ramoverlay("/rom", "/volatile");
	mount_move("/rom/volatile", "/volatile", "");
	mount_move("/rom/rom", "/rom", "");
	system("/sbin/snapshot config_unpack");
	foreachdir("/volatile/", handle_whiteout);
	unsetenv("SNAPSHOT");
	return -1;
}

static struct backend_handler snapshot_handlers[] = {
{
	.name = "config_read",
	.cli = config_read,
}, {
	.name = "config_write",
	.cli = config_write,
}, {
	.name = "read",
	.cli = snapshot_read,
}, {
	.name = "write",
	.cli = snapshot_write,
}, {
	.name = "mark",
	.cli = snapshot_mark,
}};

static struct backend snapshot_backend = {
	.name = "snapshot",
	.num_handlers = ARRAY_SIZE(snapshot_handlers),
	.handlers = snapshot_handlers,
	.mount = snapshot_mount,
	.info = snapshot_info,
};
BACKEND(snapshot_backend);