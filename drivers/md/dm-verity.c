/*
 * Copyright (C) 2012 Red Hat, Inc.
 *
 * Author: Mikulas Patocka <mpatocka@redhat.com>
 *
 * Based on Chromium dm-verity driver (C) 2011 The Chromium OS Authors
 *
 * This file is released under the GPLv2.
 *
 * In the file "/sys/module/dm_verity/parameters/prefetch_cluster" you can set
 * default prefetch value. Data are read in "prefetch_cluster" chunks from the
 * hash device. Setting this greatly improves performance when data and hash
 * are on the same disk on different partitions on devices with poor random
 * access behavior.
 */

#include "dm-bufio.h"

#include <linux/module.h>
#include <linux/device-mapper.h>
#include <linux/reboot.h>
#include <crypto/hash.h>

#ifdef CONFIG_DM_ANDROID_VERITY
#include "dm-android-verity.h"
#else
#define DM_MSG_PREFIX			"verity"
#endif

#define DM_VERITY_ENV_LENGTH		42
#define DM_VERITY_ENV_VAR_NAME		"DM_VERITY_ERR_BLOCK_NR"

#define DM_VERITY_IO_VEC_INLINE		16
#define DM_VERITY_MEMPOOL_SIZE		4
#define DM_VERITY_DEFAULT_PREFETCH_SIZE	262144

#define DM_VERITY_MAX_LEVELS		63
#define DM_VERITY_MAX_CORRUPTED_ERRS	100

#define DM_VERITY_OPT_LOGGING		"ignore_corruption"
#define DM_VERITY_OPT_RESTART		"restart_on_corruption"

static unsigned dm_verity_prefetch_cluster = DM_VERITY_DEFAULT_PREFETCH_SIZE;

module_param_named(prefetch_cluster, dm_verity_prefetch_cluster, uint, S_IRUGO | S_IWUSR);

enum verity_mode {
	DM_VERITY_MODE_EIO,
	DM_VERITY_MODE_LOGGING,
	DM_VERITY_MODE_RESTART
};

enum verity_block_type {
	DM_VERITY_BLOCK_TYPE_DATA,
	DM_VERITY_BLOCK_TYPE_METADATA
};

struct dm_verity {
	struct dm_dev *data_dev;
	struct dm_dev *hash_dev;
	struct dm_target *ti;
	struct dm_bufio_client *bufio;
	char *alg_name;
	struct crypto_shash *tfm;
	u8 *root_digest;	/* digest of the root block */
	u8 *salt;		/* salt: its size is salt_size */
	unsigned salt_size;
	sector_t data_start;	/* data offset in 512-byte sectors */
	sector_t hash_start;	/* hash start in blocks */
	sector_t data_blocks;	/* the number of data blocks */
	sector_t hash_blocks;	/* the number of hash blocks */
	unsigned char data_dev_block_bits;	/* log2(data blocksize) */
	unsigned char hash_dev_block_bits;	/* log2(hash blocksize) */
	unsigned char hash_per_block_bits;	/* log2(hashes in hash block) */
	unsigned char levels;	/* the number of tree levels */
	unsigned char version;
	unsigned digest_size;	/* digest size for the current hash algorithm */
	unsigned shash_descsize;/* the size of temporary space for crypto */
	int hash_failed;	/* set to 1 if hash of any block failed */
	enum verity_mode mode;	/* mode for handling verification errors */
	unsigned corrupted_errs;/* Number of errors for corrupted blocks */

	mempool_t *io_mempool;	/* mempool of struct dm_verity_io */
	mempool_t *vec_mempool;	/* mempool of bio vector */

	struct workqueue_struct *verify_wq;

	/* starting blocks for each tree level. 0 is the lowest level. */
	sector_t hash_level_block[DM_VERITY_MAX_LEVELS];
};

struct dm_verity_io {
	struct dm_verity *v;
	struct bio *bio;

	/* original values of bio->bi_end_io and bio->bi_private */
	bio_end_io_t *orig_bi_end_io;
	void *orig_bi_private;

	sector_t block;
	unsigned n_blocks;

	/* saved bio vector */
	struct bio_vec *io_vec;
	unsigned io_vec_size;

	struct work_struct work;

	/* A space for short vectors; longer vectors are allocated separately. */
	struct bio_vec io_vec_inline[DM_VERITY_IO_VEC_INLINE];

	/*
	 * Three variably-size fields follow this struct:
	 *
	 * u8 hash_desc[v->shash_descsize];
	 * u8 real_digest[v->digest_size];
	 * u8 want_digest[v->digest_size];
	 *
	 * To access them use: io_hash_desc(), io_real_digest() and io_want_digest().
	 */
};

struct dm_verity_prefetch_work {
	struct work_struct work;
	struct dm_verity *v;
	sector_t block;
	unsigned n_blocks;
};

static struct shash_desc *io_hash_desc(struct dm_verity *v, struct dm_verity_io *io)
{
	return (struct shash_desc *)(io + 1);
}

static u8 *io_real_digest(struct dm_verity *v, struct dm_verity_io *io)
{
	return (u8 *)(io + 1) + v->shash_descsize;
}

static u8 *io_want_digest(struct dm_verity *v, struct dm_verity_io *io)
{
	return (u8 *)(io + 1) + v->shash_descsize + v->digest_size;
}

/*
 * Auxiliary structure appended to each dm-bufio buffer. If the value
 * hash_verified is nonzero, hash of the block has been verified.
 *
 * The variable hash_verified is set to 0 when allocating the buffer, then
 * it can be changed to 1 and it is never reset to 0 again.
 *
 * There is no lock around this value, a race condition can at worst cause
 * that multiple processes verify the hash of the same buffer simultaneously
 * and write 1 to hash_verified simultaneously.
 * This condition is harmless, so we don't need locking.
 */
struct buffer_aux {
	int hash_verified;
};

/*
 * Initialize struct buffer_aux for a freshly created buffer.
 */
static void dm_bufio_alloc_callback(struct dm_buffer *buf)
{
	struct buffer_aux *aux = dm_bufio_get_aux_data(buf);

	aux->hash_verified = 0;
}

/*
 * Translate input sector number to the sector number on the target device.
 */
static sector_t verity_map_sector(struct dm_verity *v, sector_t bi_sector)
{
	return v->data_start + dm_target_offset(v->ti, bi_sector);
}

/*
 * Return hash position of a specified block at a specified tree level
 * (0 is the lowest level).
 * The lowest "hash_per_block_bits"-bits of the result denote hash position
 * inside a hash block. The remaining bits denote location of the hash block.
 */
static sector_t verity_position_at_level(struct dm_verity *v, sector_t block,
					 int level)
{
	return block >> (level * v->hash_per_block_bits);
}

static void verity_hash_at_level(struct dm_verity *v, sector_t block, int level,
				 sector_t *hash_block, unsigned *offset)
{
	sector_t position = verity_position_at_level(v, block, level);
	unsigned idx;

	*hash_block = v->hash_level_block[level] + (position >> v->hash_per_block_bits);

	if (!offset)
		return;

	idx = position & ((1 << v->hash_per_block_bits) - 1);
	if (!v->version)
		*offset = idx * v->digest_size;
	else
		*offset = idx << (v->hash_dev_block_bits - v->hash_per_block_bits);
}

/*
 * Handle verification errors.
 */
static int verity_handle_err(struct dm_verity *v, enum verity_block_type type,
			     unsigned long long block)
{
	char verity_env[DM_VERITY_ENV_LENGTH];
	char *envp[] = { verity_env, NULL };
	const char *type_str = "";
	struct mapped_device *md = dm_table_get_md(v->ti->table);

	/* Corruption should be visible in device status in all modes */
	v->hash_failed = 1;

	if (v->corrupted_errs >= DM_VERITY_MAX_CORRUPTED_ERRS)
		goto out;

	v->corrupted_errs++;

	switch (type) {
	case DM_VERITY_BLOCK_TYPE_DATA:
		type_str = "data";
		break;
	case DM_VERITY_BLOCK_TYPE_METADATA:
		type_str = "metadata";
		break;
	default:
		BUG();
	}

	DMERR("%s: %s block %llu is corrupted", v->data_dev->name, type_str,
		block);

	if (v->corrupted_errs == DM_VERITY_MAX_CORRUPTED_ERRS)
		DMERR("%s: reached maximum errors", v->data_dev->name);

	snprintf(verity_env, DM_VERITY_ENV_LENGTH, "%s=%d,%llu",
		DM_VERITY_ENV_VAR_NAME, type, block);

	kobject_uevent_env(&disk_to_dev(dm_disk(md))->kobj, KOBJ_CHANGE, envp);

out:
	if (v->mode == DM_VERITY_MODE_LOGGING)
		return 0;

	if (v->mode == DM_VERITY_MODE_RESTART)
		kernel_restart("dm-verity device corrupted");

	return 1;
}

/*
 * Verify hash of a metadata block pertaining to the specified data block
 * ("block" argument) at a specified level ("level" argument).
 *
 * On successful return, io_want_digest(v, io) contains the hash value for
 * a lower tree level or for the data block (if we're at the lowest leve).
 *
 * If "skip_unverified" is true, unverified buffer is skipped and 1 is returned.
 * If "skip_unverified" is false, unverified buffer is hashed and verified
 * against current value of io_want_digest(v, io).
 */
static int verity_verify_level(struct dm_verity_io *io, sector_t block,
			       int level, bool skip_unverified)
{
	struct dm_verity *v = io->v;
	struct dm_buffer *buf;
	struct buffer_aux *aux;
	u8 *data;
	int r;
	sector_t hash_block;
	unsigned offset;

	verity_hash_at_level(v, block, level, &hash_block, &offset);

	data = dm_bufio_read(v->bufio, hash_block, &buf);
	if (unlikely(IS_ERR(data)))
		return PTR_ERR(data);

	aux = dm_bufio_get_aux_data(buf);

	if (!aux->hash_verified) {
		struct shash_desc *desc;
		u8 *result;

		if (skip_unverified) {
			r = 1;
			goto release_ret_r;
		}

		desc = io_hash_desc(v, io);
		desc->tfm = v->tfm;
		desc->flags = CRYPTO_TFM_REQ_MAY_SLEEP;
		r = crypto_shash_init(desc);
		if (r < 0) {
			DMERR("crypto_shash_init failed: %d", r);
			goto release_ret_r;
		}

		if (likely(v->version >= 1)) {
			r = crypto_shash_update(desc, v->salt, v->salt_size);
			if (r < 0) {
				DMERR("crypto_shash_update failed: %d", r);
				goto release_ret_r;
			}
		}

		r = crypto_shash_update(desc, data, 1 << v->hash_dev_block_bits);
		if (r < 0) {
			DMERR("crypto_shash_update failed: %d", r);
			goto release_ret_r;
		}

		if (!v->version) {
			r = crypto_shash_update(desc, v->salt, v->salt_size);
			if (r < 0) {
				DMERR("crypto_shash_update failed: %d", r);
				goto release_ret_r;
			}
		}

		result = io_real_digest(v, io);
		r = crypto_shash_final(desc, result);
		if (r < 0) {
			DMERR("crypto_shash_final failed: %d", r);
			goto release_ret_r;
		}
		if (unlikely(memcmp(result, io_want_digest(v, io), v->digest_size))) {
			if (verity_handle_err(v, DM_VERITY_BLOCK_TYPE_METADATA,
					      hash_block)) {
				r = -EIO;
				goto release_ret_r;
			}
		} else
			aux->hash_verified = 1;
	}

	data += offset;

	memcpy(io_want_digest(v, io), data, v->digest_size);

	dm_bufio_release(buf);
	return 0;

release_ret_r:
	dm_bufio_release(buf);

	return r;
}

/*
 * Verify one "dm_verity_io" structure.
 */
static int verity_verify_io(struct dm_verity_io *io)
{
	struct dm_verity *v = io->v;
	unsigned b;
	int i;
	unsigned vector = 0, offset = 0;

	for (b = 0; b < io->n_blocks; b++) {
		struct shash_desc *desc;
		u8 *result;
		int r;
		unsigned todo;

		if (likely(v->levels)) {
			/*
			 * First, we try to get the requested hash for
			 * the current block. If the hash block itself is
			 * verified, zero is returned. If it isn't, this
			 * function returns 0 and we fall back to whole
			 * chain verification.
			 */
			int r = verity_verify_level(io, io->block + b, 0, true);
			if (likely(!r))
				goto test_block_hash;
			if (r < 0)
				return r;
		}

		memcpy(io_want_digest(v, io), v->root_digest, v->digest_size);

		for (i = v->levels - 1; i >= 0; i--) {
			int r = verity_verify_level(io, io->block + b, i, false);
			if (unlikely(r))
				return r;
		}

test_block_hash:
		desc = io_hash_desc(v, io);
		desc->tfm = v->tfm;
		desc->flags = CRYPTO_TFM_REQ_MAY_SLEEP;
		r = crypto_shash_init(desc);
		if (r < 0) {
			DMERR("crypto_shash_init failed: %d", r);
			return r;
		}

		if (likely(v->version >= 1)) {
			r = crypto_shash_update(desc, v->salt, v->salt_size);
			if (r < 0) {
				DMERR("crypto_shash_update failed: %d", r);
				return r;
			}
		}

		todo = 1 << v->data_dev_block_bits;
		do {
			struct bio_vec *bv;
			u8 *page;
			unsigned len;

			BUG_ON(vector >= io->io_vec_size);
			bv = &io->io_vec[vector];
			page = kmap_atomic(bv->bv_page);
			len = bv->bv_len - offset;
			if (likely(len >= todo))
				len = todo;
			r = crypto_shash_update(desc,
					page + bv->bv_offset + offset, len);
			kunmap_atomic(page);
			if (r < 0) {
				DMERR("crypto_shash_update failed: %d", r);
				return r;
			}
			offset += len;
			if (likely(offset == bv->bv_len)) {
				offset = 0;
				vector++;
			}
			todo -= len;
		} while (todo);

		if (!v->version) {
			r = crypto_shash_update(desc, v->salt, v->salt_size);
			if (r < 0) {
				DMERR("crypto_shash_update failed: %d", r);
				return r;
			}
		}

		result = io_real_digest(v, io);
		r = crypto_shash_final(desc, result);
		if (r < 0) {
			DMERR("crypto_shash_final failed: %d", r);
			return r;
		}
		if (unlikely(memcmp(result, io_want_digest(v, io), v->digest_size))) {
			if (verity_handle_err(v, DM_VERITY_BLOCK_TYPE_DATA,
					      io->block + b))
				return -EIO;
		}
	}
	BUG_ON(vector != io->io_vec_size);
	BUG_ON(offset);

	return 0;
}

/*
 * End one "io" structure with a given error.
 */
static void verity_finish_io(struct dm_verity_io *io, int error)
{
	struct bio *bio = io->bio;
	struct dm_verity *v = io->v;

	bio->bi_end_io = io->orig_bi_end_io;
	bio->bi_private = io->orig_bi_private;

	if (io->io_vec != io->io_vec_inline)
		mempool_free(io->io_vec, v->vec_mempool);

	mempool_free(io, v->io_mempool);

	bio_endio(bio, error);
}

static void verity_work(struct work_struct *w)
{
	struct dm_verity_io *io = container_of(w, struct dm_verity_io, work);

	verity_finish_io(io, verity_verify_io(io));
}

static void verity_end_io(struct bio *bio, int error)
{
	struct dm_verity_io *io = bio->bi_private;

	if (error) {
		verity_finish_io(io, error);
		return;
	}

	INIT_WORK(&io->work, verity_work);
	queue_work(io->v->verify_wq, &io->work);
}

/*
 * Prefetch buffers for the specified io.
 * The root buffer is not prefetched, it is assumed that it will be cached
 * all the time.
 */
static void verity_prefetch_io(struct work_struct *work)
{
	struct dm_verity_prefetch_work *pw =
		container_of(work, struct dm_verity_prefetch_work, work);
	struct dm_verity *v = pw->v;
	int i;

	for (i = v->levels - 2; i >= 0; i--) {
		sector_t hash_block_start;
		sector_t hash_block_end;
		verity_hash_at_level(v, pw->block, i, &hash_block_start, NULL);
		verity_hash_at_level(v, pw->block + pw->n_blocks - 1, i, &hash_block_end, NULL);
		if (!i) {
			unsigned cluster = ACCESS_ONCE(dm_verity_prefetch_cluster);

			cluster >>= v->data_dev_block_bits;
			if (unlikely(!cluster))
				goto no_prefetch_cluster;

			if (unlikely(cluster & (cluster - 1)))
				cluster = 1 << __fls(cluster);

			hash_block_start &= ~(sector_t)(cluster - 1);
			hash_block_end |= cluster - 1;
			if (unlikely(hash_block_end >= v->hash_blocks))
				hash_block_end = v->hash_blocks - 1;
		}
no_prefetch_cluster:
		dm_bufio_prefetch(v->bufio, hash_block_start,
				  hash_block_end - hash_block_start + 1);
	}

	kfree(pw);
}

static void verity_submit_prefetch(struct dm_verity *v, struct dm_verity_io *io)
{
	struct dm_verity_prefetch_work *pw;

	pw = kmalloc(sizeof(struct dm_verity_prefetch_work),
		GFP_NOIO | __GFP_NORETRY | __GFP_NOMEMALLOC | __GFP_NOWARN);

	if (!pw)
		return;

	INIT_WORK(&pw->work, verity_prefetch_io);
	pw->v = v;
	pw->block = io->block;
	pw->n_blocks = io->n_blocks;
	queue_work(v->verify_wq, &pw->work);
}

/*
 * Bio map function. It allocates dm_verity_io structure and bio vector and
 * fills them. Then it issues prefetches and the I/O.
 */
static int verity_map(struct dm_target *ti, struct bio *bio,
		      union map_info *map_context)
{
	struct dm_verity *v = ti->private;
	struct dm_verity_io *io;

	bio->bi_bdev = v->data_dev->bdev;
	bio->bi_sector = verity_map_sector(v, bio->bi_sector);

	if (((unsigned)bio->bi_sector | bio_sectors(bio)) &
	    ((1 << (v->data_dev_block_bits - SECTOR_SHIFT)) - 1)) {
		DMERR_LIMIT("unaligned io");
		return -EIO;
	}

	if ((bio->bi_sector + bio_sectors(bio)) >>
	    (v->data_dev_block_bits - SECTOR_SHIFT) > v->data_blocks) {
		DMERR_LIMIT("io out of range");
		return -EIO;
	}

	if (bio_data_dir(bio) == WRITE)
		return -EIO;

	io = mempool_alloc(v->io_mempool, GFP_NOIO);
	io->v = v;
	io->bio = bio;
	io->orig_bi_end_io = bio->bi_end_io;
	io->orig_bi_private = bio->bi_private;
	io->block = bio->bi_sector >> (v->data_dev_block_bits - SECTOR_SHIFT);
	io->n_blocks = bio->bi_size >> v->data_dev_block_bits;

	bio->bi_end_io = verity_end_io;
	bio->bi_private = io;
	io->io_vec_size = bio->bi_vcnt - bio->bi_idx;
	if (io->io_vec_size < DM_VERITY_IO_VEC_INLINE)
		io->io_vec = io->io_vec_inline;
	else
		io->io_vec = mempool_alloc(v->vec_mempool, GFP_NOIO);
	memcpy(io->io_vec, bio_iovec(bio),
	       io->io_vec_size * sizeof(struct bio_vec));

	verity_submit_prefetch(v, io);

	generic_make_request(bio);

	return DM_MAPIO_SUBMITTED;
}

/*
 * Status: V (valid) or C (corruption found)
 */
static void verity_status(struct dm_target *ti, status_type_t type,
			  char *result, unsigned maxlen)
{
	struct dm_verity *v = ti->private;
	unsigned sz = 0;
	unsigned x;

	switch (type) {
	case STATUSTYPE_INFO:
		DMEMIT("%c", v->hash_failed ? 'C' : 'V');
		break;
	case STATUSTYPE_TABLE:
		DMEMIT("%u %s %s %u %u %llu %llu %s ",
			v->version,
			v->data_dev->name,
			v->hash_dev->name,
			1 << v->data_dev_block_bits,
			1 << v->hash_dev_block_bits,
			(unsigned long long)v->data_blocks,
			(unsigned long long)v->hash_start,
			v->alg_name
			);
		for (x = 0; x < v->digest_size; x++)
			DMEMIT("%02x", v->root_digest[x]);
		DMEMIT(" ");
		if (!v->salt_size)
			DMEMIT("-");
		else
			for (x = 0; x < v->salt_size; x++)
				DMEMIT("%02x", v->salt[x]);
		if (v->mode != DM_VERITY_MODE_EIO) {
			DMEMIT(" 1 ");
			switch (v->mode) {
			case DM_VERITY_MODE_LOGGING:
				DMEMIT(DM_VERITY_OPT_LOGGING);
				break;
			case DM_VERITY_MODE_RESTART:
				DMEMIT(DM_VERITY_OPT_RESTART);
				break;
			default:
				BUG();
			}
		}
		break;
	}
}

static int verity_ioctl(struct dm_target *ti, unsigned cmd,
			unsigned long arg)
{
	struct dm_verity *v = ti->private;
	int r = 0;

	if (v->data_start ||
	    ti->len != i_size_read(v->data_dev->bdev->bd_inode) >> SECTOR_SHIFT)
		r = scsi_verify_blk_ioctl(NULL, cmd);

	return r ? : __blkdev_driver_ioctl(v->data_dev->bdev, v->data_dev->mode,
				     cmd, arg);
}

static int verity_merge(struct dm_target *ti, struct bvec_merge_data *bvm,
			struct bio_vec *biovec, int max_size)
{
	struct dm_verity *v = ti->private;
	struct request_queue *q = bdev_get_queue(v->data_dev->bdev);

	if (!q->merge_bvec_fn)
		return max_size;

	bvm->bi_bdev = v->data_dev->bdev;
	bvm->bi_sector = verity_map_sector(v, bvm->bi_sector);

	return min(max_size, q->merge_bvec_fn(q, bvm, biovec));
}

static int verity_iterate_devices(struct dm_target *ti,
				  iterate_devices_callout_fn fn, void *data)
{
	struct dm_verity *v = ti->private;

	return fn(ti, v->data_dev, v->data_start, ti->len, data);
}

static void verity_io_hints(struct dm_target *ti, struct queue_limits *limits)
{
	struct dm_verity *v = ti->private;

	if (limits->logical_block_size < 1 << v->data_dev_block_bits)
		limits->logical_block_size = 1 << v->data_dev_block_bits;

	if (limits->physical_block_size < 1 << v->data_dev_block_bits)
		limits->physical_block_size = 1 << v->data_dev_block_bits;

	blk_limits_io_min(limits, limits->logical_block_size);
}

static void verity_dtr(struct dm_target *ti)
{
	struct dm_verity *v = ti->private;

	if (v->verify_wq)
		destroy_workqueue(v->verify_wq);

	if (v->vec_mempool)
		mempool_destroy(v->vec_mempool);

	if (v->io_mempool)
		mempool_destroy(v->io_mempool);

	if (v->bufio)
		dm_bufio_client_destroy(v->bufio);

	kfree(v->salt);
	kfree(v->root_digest);

	if (v->tfm)
		crypto_free_shash(v->tfm);

	kfree(v->alg_name);

	if (v->hash_dev)
		dm_put_device(ti, v->hash_dev);

	if (v->data_dev)
		dm_put_device(ti, v->data_dev);

	kfree(v);
}

/*
 * Target parameters:
 *	<version>	The current format is version 1.
 *			Vsn 0 is compatible with original Chromium OS releases.
 *	<data device>
 *	<hash device>
 *	<data block size>
 *	<hash block size>
 *	<the number of data blocks>
 *	<hash start block>
 *	<algorithm>
 *	<digest>
 *	<salt>		Hex string or "-" if no salt.
 */
static int verity_ctr(struct dm_target *ti, unsigned argc, char **argv)
{
	struct dm_verity *v;
	struct dm_arg_set as;
	const char *opt_string;
	unsigned int num, opt_params;
	unsigned long long num_ll;
	int r;
	int i;
	sector_t hash_position;
	char dummy;

	static struct dm_arg _args[] = {
		{0, 1, "Invalid number of feature args"},
	};

	v = kzalloc(sizeof(struct dm_verity), GFP_KERNEL);
	if (!v) {
		ti->error = "Cannot allocate verity structure";
		return -ENOMEM;
	}
	ti->private = v;
	v->ti = ti;

	if ((dm_table_get_mode(ti->table) & ~FMODE_READ)) {
		ti->error = "Device must be readonly";
		r = -EINVAL;
		goto bad;
	}

	if (argc < 10) {
		ti->error = "Not enough arguments";
		r = -EINVAL;
		goto bad;
	}

	if (sscanf(argv[0], "%u%c", &num, &dummy) != 1 ||
	    num > 1) {
		ti->error = "Invalid version";
		r = -EINVAL;
		goto bad;
	}
	v->version = num;

	r = dm_get_device(ti, argv[1], FMODE_READ, &v->data_dev);
	if (r) {
		ti->error = "Data device lookup failed";
		goto bad;
	}

	r = dm_get_device(ti, argv[2], FMODE_READ, &v->hash_dev);
	if (r) {
		ti->error = "Data device lookup failed";
		goto bad;
	}

	if (sscanf(argv[3], "%u%c", &num, &dummy) != 1 ||
	    !num || (num & (num - 1)) ||
	    num < bdev_logical_block_size(v->data_dev->bdev) ||
	    num > PAGE_SIZE) {
		ti->error = "Invalid data device block size";
		r = -EINVAL;
		goto bad;
	}
	v->data_dev_block_bits = __ffs(num);

	if (sscanf(argv[4], "%u%c", &num, &dummy) != 1 ||
	    !num || (num & (num - 1)) ||
	    num < bdev_logical_block_size(v->hash_dev->bdev) ||
	    num > INT_MAX) {
		ti->error = "Invalid hash device block size";
		r = -EINVAL;
		goto bad;
	}
	v->hash_dev_block_bits = __ffs(num);

	if (sscanf(argv[5], "%llu%c", &num_ll, &dummy) != 1 ||
	    (sector_t)(num_ll << (v->data_dev_block_bits - SECTOR_SHIFT))
	    >> (v->data_dev_block_bits - SECTOR_SHIFT) != num_ll) {
		ti->error = "Invalid data blocks";
		r = -EINVAL;
		goto bad;
	}
	v->data_blocks = num_ll;

	if (ti->len > (v->data_blocks << (v->data_dev_block_bits - SECTOR_SHIFT))) {
		ti->error = "Data device is too small";
		r = -EINVAL;
		goto bad;
	}

	if (sscanf(argv[6], "%llu%c", &num_ll, &dummy) != 1 ||
	    (sector_t)(num_ll << (v->hash_dev_block_bits - SECTOR_SHIFT))
	    >> (v->hash_dev_block_bits - SECTOR_SHIFT) != num_ll) {
		ti->error = "Invalid hash start";
		r = -EINVAL;
		goto bad;
	}
	v->hash_start = num_ll;

	v->alg_name = kstrdup(argv[7], GFP_KERNEL);
	if (!v->alg_name) {
		ti->error = "Cannot allocate algorithm name";
		r = -ENOMEM;
		goto bad;
	}

	v->tfm = crypto_alloc_shash(v->alg_name, 0, 0);
	if (IS_ERR(v->tfm)) {
		ti->error = "Cannot initialize hash function";
		r = PTR_ERR(v->tfm);
		v->tfm = NULL;
		goto bad;
	}
	v->digest_size = crypto_shash_digestsize(v->tfm);
	if ((1 << v->hash_dev_block_bits) < v->digest_size * 2) {
		ti->error = "Digest size too big";
		r = -EINVAL;
		goto bad;
	}
	v->shash_descsize =
		sizeof(struct shash_desc) + crypto_shash_descsize(v->tfm);

	v->root_digest = kmalloc(v->digest_size, GFP_KERNEL);
	if (!v->root_digest) {
		ti->error = "Cannot allocate root digest";
		r = -ENOMEM;
		goto bad;
	}
	if (strlen(argv[8]) != v->digest_size * 2 ||
	    hex2bin(v->root_digest, argv[8], v->digest_size)) {
		ti->error = "Invalid root digest";
		r = -EINVAL;
		goto bad;
	}

	if (strcmp(argv[9], "-")) {
		v->salt_size = strlen(argv[9]) / 2;
		v->salt = kmalloc(v->salt_size, GFP_KERNEL);
		if (!v->salt) {
			ti->error = "Cannot allocate salt";
			r = -ENOMEM;
			goto bad;
		}
		if (strlen(argv[9]) != v->salt_size * 2 ||
		    hex2bin(v->salt, argv[9], v->salt_size)) {
			ti->error = "Invalid salt";
			r = -EINVAL;
			goto bad;
		}
	}

	argv += 10;
	argc -= 10;

	/* Optional parameters */
	if (argc) {
		as.argc = argc;
		as.argv = argv;

		r = dm_read_arg_group(_args, &as, &opt_params, &ti->error);
		if (r)
			goto bad;

		while (opt_params) {
			opt_params--;
			opt_string = dm_shift_arg(&as);
			if (!opt_string) {
				ti->error = "Not enough feature arguments";
				r = -EINVAL;
				goto bad;
			}

			if (!strcasecmp(opt_string, DM_VERITY_OPT_LOGGING))
				v->mode = DM_VERITY_MODE_LOGGING;
			else if (!strcasecmp(opt_string, DM_VERITY_OPT_RESTART))
				v->mode = DM_VERITY_MODE_RESTART;
			else {
				ti->error = "Invalid feature arguments";
				r = -EINVAL;
				goto bad;
			}
		}
	}

	v->hash_per_block_bits =
		__fls((1 << v->hash_dev_block_bits) / v->digest_size);

	v->levels = 0;
	if (v->data_blocks)
		while (v->hash_per_block_bits * v->levels < 64 &&
		       (unsigned long long)(v->data_blocks - 1) >>
		       (v->hash_per_block_bits * v->levels))
			v->levels++;

	if (v->levels > DM_VERITY_MAX_LEVELS) {
		ti->error = "Too many tree levels";
		r = -E2BIG;
		goto bad;
	}

	hash_position = v->hash_start;
	for (i = v->levels - 1; i >= 0; i--) {
		sector_t s;
		v->hash_level_block[i] = hash_position;
		s = (v->data_blocks + ((sector_t)1 << ((i + 1) * v->hash_per_block_bits)) - 1)
					>> ((i + 1) * v->hash_per_block_bits);
		if (hash_position + s < hash_position) {
			ti->error = "Hash device offset overflow";
			r = -E2BIG;
			goto bad;
		}
		hash_position += s;
	}
	v->hash_blocks = hash_position;

	v->bufio = dm_bufio_client_create(v->hash_dev->bdev,
		1 << v->hash_dev_block_bits, 1, sizeof(struct buffer_aux),
		dm_bufio_alloc_callback, NULL);
	if (IS_ERR(v->bufio)) {
		ti->error = "Cannot initialize dm-bufio";
		r = PTR_ERR(v->bufio);
		v->bufio = NULL;
		goto bad;
	}

	if (dm_bufio_get_device_size(v->bufio) < v->hash_blocks) {
		ti->error = "Hash device is too small";
		r = -E2BIG;
		goto bad;
	}

	v->io_mempool = mempool_create_kmalloc_pool(DM_VERITY_MEMPOOL_SIZE,
	  sizeof(struct dm_verity_io) + v->shash_descsize + v->digest_size * 2);
	if (!v->io_mempool) {
		ti->error = "Cannot allocate io mempool";
		r = -ENOMEM;
		goto bad;
	}

	v->vec_mempool = mempool_create_kmalloc_pool(DM_VERITY_MEMPOOL_SIZE,
					BIO_MAX_PAGES * sizeof(struct bio_vec));
	if (!v->vec_mempool) {
		ti->error = "Cannot allocate vector mempool";
		r = -ENOMEM;
		goto bad;
	}

	/* WQ_UNBOUND greatly improves performance when running on ramdisk */
	v->verify_wq = alloc_workqueue("kverityd",
				       WQ_HIGHPRI | WQ_MEM_RECLAIM | WQ_UNBOUND,
				       num_online_cpus());
	if (!v->verify_wq) {
		ti->error = "Cannot allocate workqueue";
		r = -ENOMEM;
		goto bad;
	}

	return 0;

bad:
	verity_dtr(ti);

	return r;
}

#ifdef CONFIG_DM_ANDROID_VERITY
static void bi_complete(struct bio *bio, int error)
{
        complete((struct completion *)bio->bi_private);
}

static int submit_bio_wait(int rw, struct bio *bio)
{
        struct completion event;
        rw |= REQ_SYNC;

        init_completion(&event);
        bio->bi_private = &event;
        bio->bi_end_io = bi_complete;
        submit_bio(rw, bio);
        wait_for_completion(&event);

        return test_bit(BIO_UPTODATE, &bio->bi_flags);
}

static int read_block_dev(struct bio_read *payload, struct block_device *bdev,
		sector_t offset, int length)
{
	struct bio *bio;
	int err = 0, i;

	payload->number_of_pages = DIV_ROUND_UP(length, PAGE_SIZE);

	bio = bio_alloc(GFP_KERNEL, payload->number_of_pages);
	if (!bio) {
		DMERR("Error while allocating bio");
		return -ENOMEM;
	}

	bio->bi_bdev = bdev;
	bio->bi_sector = offset;

	payload->page_io = kzalloc(sizeof(struct page *) *
		payload->number_of_pages, GFP_KERNEL);
	if (!payload->page_io) {
		DMERR("page_io array alloc failed");
		err = -ENOMEM;
		goto free_bio;
	}

	for (i = 0; i < payload->number_of_pages; i++) {
		payload->page_io[i] = alloc_page(GFP_KERNEL);
		if (!payload->page_io[i]) {
			DMERR("alloc_page failed");
			err = -ENOMEM;
			goto free_pages;
		}
		if (!bio_add_page(bio, payload->page_io[i], PAGE_SIZE, 0)) {
			DMERR("bio_add_page error");
			err = -EIO;
			goto free_pages;
		}
	}

	if (!submit_bio_wait(READ, bio))
		/* success */
		goto free_bio;
	DMERR("bio read failed");
	err = -EIO;

free_pages:
	for (i = 0; i < payload->number_of_pages; i++)
		if (payload->page_io[i])
			__free_page(payload->page_io[i]);
	kfree(payload->page_io);
free_bio:
	bio_put(bio);
	return err;
}

static inline u64 div64_u64_rem(u64 dividend, u64 divisor, u64 *remainder)
{
        *remainder = dividend % divisor;
        return dividend / divisor;
}

static inline u64 fec_div_round_up(u64 x, u64 y)
{
	u64 remainder;

	return div64_u64_rem(x, y, &remainder) +
		(remainder > 0 ? 1 : 0);
}

static inline void populate_fec_metadata(struct fec_header *header,
				struct fec_ecc_metadata *ecc)
{
	ecc->blocks = fec_div_round_up(le64_to_cpu(header->inp_size),
			FEC_BLOCK_SIZE);
	ecc->roots = le32_to_cpu(header->roots);
	ecc->start = le64_to_cpu(header->inp_size);
}

static inline int validate_fec_header(struct fec_header *header, u64 offset)
{
	/* move offset to make the sanity check work for backup header
	 * as well. */
	offset -= offset % FEC_BLOCK_SIZE;
	if (le32_to_cpu(header->magic) != FEC_MAGIC ||
		le32_to_cpu(header->version) != FEC_VERSION ||
		le32_to_cpu(header->size) != sizeof(struct fec_header) ||
		le32_to_cpu(header->roots) == 0 ||
		le32_to_cpu(header->roots) >= FEC_RSM ||
		offset < le32_to_cpu(header->fec_size) ||
		offset - le32_to_cpu(header->fec_size) !=
		le64_to_cpu(header->inp_size))
		return -EINVAL;

	return 0;
}

static int extract_fec_header(dev_t dev, struct fec_header *fec,
				struct fec_ecc_metadata *ecc)
{
	u64 device_size;
	struct bio_read payload;
	int i, err = 0;
	struct block_device *bdev;

	bdev = blkdev_get_by_dev(dev, FMODE_READ, NULL);

	if (IS_ERR(bdev)) {
		DMERR("bdev get error");
		return PTR_ERR(bdev);
	}

	device_size = i_size_read(bdev->bd_inode);

	/* fec metadata size is a power of 2 and PAGE_SIZE
	 * is a power of 2 as well.
	 */
	BUG_ON(FEC_BLOCK_SIZE > PAGE_SIZE);
	/* 512 byte sector alignment */
	BUG_ON(((device_size - FEC_BLOCK_SIZE) % (1 << SECTOR_SHIFT)) != 0);

	err = read_block_dev(&payload, bdev, (device_size -
		FEC_BLOCK_SIZE) / (1 << SECTOR_SHIFT), FEC_BLOCK_SIZE);
	if (err) {
		DMERR("Error while reading verity metadata");
		goto error;
	}

	BUG_ON(sizeof(struct fec_header) > PAGE_SIZE);
	memcpy(fec, page_address(payload.page_io[0]),
			sizeof(*fec));

	ecc->valid = true;
	if (validate_fec_header(fec, device_size - FEC_BLOCK_SIZE)) {
		/* Try the backup header */
		memcpy(fec, page_address(payload.page_io[0]) + FEC_BLOCK_SIZE
			- sizeof(*fec) ,
			sizeof(*fec));
		if (validate_fec_header(fec, device_size -
			sizeof(struct fec_header)))
			ecc->valid = false;
	}

	if (ecc->valid)
		populate_fec_metadata(fec, ecc);

	for (i = 0; i < payload.number_of_pages; i++)
		__free_page(payload.page_io[i]);
	kfree(payload.page_io);

error:
	blkdev_put(bdev, FMODE_READ);
	return err;
}
static void find_metadata_offset(struct fec_header *fec,
		struct block_device *bdev, u64 *metadata_offset)
{
	u64 device_size;

	device_size = i_size_read(bdev->bd_inode);

	if (le32_to_cpu(fec->magic) == FEC_MAGIC)
		*metadata_offset = le64_to_cpu(fec->inp_size) -
					VERITY_METADATA_SIZE;
	else
		*metadata_offset = device_size - VERITY_METADATA_SIZE;
}

static struct android_metadata *extract_metadata(dev_t dev,
				struct fec_header *fec)
{
	struct block_device *bdev;
	struct android_metadata_header *header;
	struct android_metadata *uninitialized_var(metadata);
	int i;
	u32 table_length, copy_length, offset;
	u64 metadata_offset;
	struct bio_read payload;
	int err = 0;

	bdev = blkdev_get_by_dev(dev, FMODE_READ, NULL);

	if (IS_ERR(bdev)) {
		DMERR("blkdev_get_by_dev failed");
		return ERR_CAST(bdev);
	}

	find_metadata_offset(fec, bdev, &metadata_offset);

	/* Verity metadata size is a power of 2 and PAGE_SIZE
	 * is a power of 2 as well.
	 * PAGE_SIZE is also a multiple of 512 bytes.
	*/
	if (VERITY_METADATA_SIZE > PAGE_SIZE)
		BUG_ON(VERITY_METADATA_SIZE % PAGE_SIZE != 0);
	/* 512 byte sector alignment */
	BUG_ON(metadata_offset % (1 << SECTOR_SHIFT) != 0);

	err = read_block_dev(&payload, bdev, metadata_offset /
		(1 << SECTOR_SHIFT), VERITY_METADATA_SIZE);
	if (err) {
		DMERR("Error while reading verity metadata");
		metadata = ERR_PTR(err);
		goto blkdev_release;
	}

	header = kzalloc(sizeof(*header), GFP_KERNEL);
	if (!header) {
		DMERR("kzalloc failed for header");
		err = -ENOMEM;
		goto free_payload;
	}

	memcpy(header, page_address(payload.page_io[0]),
		sizeof(*header));

	DMINFO("bio magic_number:%u protocol_version:%d table_length:%u",
		le32_to_cpu(header->magic_number),
		le32_to_cpu(header->protocol_version),
		le32_to_cpu(header->table_length));

	metadata = kzalloc(sizeof(*metadata), GFP_KERNEL);
	if (!metadata) {
		DMERR("kzalloc for metadata failed");
		err = -ENOMEM;
		goto free_header;
	}

	metadata->header = header;
	table_length = le32_to_cpu(header->table_length);

	if (table_length == 0 ||
		table_length > (VERITY_METADATA_SIZE -
			sizeof(struct android_metadata_header)))
		goto free_metadata;

	metadata->verity_table = kzalloc(table_length + 1, GFP_KERNEL);

	if (!metadata->verity_table) {
		DMERR("kzalloc verity_table failed");
		err = -ENOMEM;
		goto free_metadata;
	}

	if (sizeof(struct android_metadata_header) +
			table_length <= PAGE_SIZE) {
		memcpy(metadata->verity_table, page_address(payload.page_io[0])
			+ sizeof(struct android_metadata_header),
			table_length);
	} else {
		copy_length = PAGE_SIZE -
			sizeof(struct android_metadata_header);
		memcpy(metadata->verity_table, page_address(payload.page_io[0])
			+ sizeof(struct android_metadata_header),
			copy_length);
		table_length -= copy_length;
		offset = copy_length;
		i = 1;
		while (table_length != 0) {
			if (table_length > PAGE_SIZE) {
				memcpy(metadata->verity_table + offset,
					page_address(payload.page_io[i]),
					PAGE_SIZE);
				offset += PAGE_SIZE;
				table_length -= PAGE_SIZE;
			} else {
				memcpy(metadata->verity_table + offset,
					page_address(payload.page_io[i]),
					table_length);
				table_length = 0;
			}
			i++;
		}
	}
	metadata->verity_table[table_length] = '\0';

	goto free_payload;

free_metadata:
	kfree(metadata);
free_header:
	kfree(header);
	metadata = ERR_PTR(err);
free_payload:
	for (i = 0; i < payload.number_of_pages; i++)
		if (payload.page_io[i])
			__free_page(payload.page_io[i]);
	kfree(payload.page_io);

	DMINFO("verity_table: %s", metadata->verity_table);
blkdev_release:
	blkdev_put(bdev, FMODE_READ);
	return metadata;
}

static inline bool test_mult_overflow(sector_t a, u32 b)
{
	sector_t r = (sector_t)~0ULL;

	sector_div(r, b);
	return a > r;
}

/*
 * Target parameters:
 *	<key id>	Key id of the public key in the system keyring.
 *			Verity metadata's signature would be verified against
 *			this. If the key id contains spaces, replace them
 *			with '#'.
 *	<block device>	The block device for which dm-verity is being setup.
 */
static int android_verity_ctr(struct dm_target *ti, unsigned argc, char **argv)
{
	dev_t uninitialized_var(dev);
	struct android_metadata *uninitialized_var(metadata);
	int err = 0, i, mode;
	char *key_id, *table_ptr, dummy,
	*verity_table_args[VERITY_TABLE_ARGS + 2 + VERITY_TABLE_OPT_FEC_ARGS];
	/* One for specifying number of opt args and one for mode */
	sector_t data_sectors;
	u32 data_block_size;
	unsigned int major, minor,
	no_of_args = VERITY_TABLE_ARGS + 2 + VERITY_TABLE_OPT_FEC_ARGS;
	struct fec_header fec;
	struct fec_ecc_metadata uninitialized_var(ecc);
	char buf[FEC_ARG_LENGTH], *buf_ptr;
	unsigned long long tmpll;

	if (argc != 2) {
		DMERR("Incorrect number of arguments");
		return -EINVAL;
	}

	/* should come as one of the arguments for the verity target */
	key_id = argv[0];
	strreplace(argv[0], '#', ' ');

	if (sscanf(argv[1], "%u:%u%c", &major, &minor, &dummy) == 2) {
		dev = MKDEV(major, minor);
		if (MAJOR(dev) != major || MINOR(dev) != minor) {
			DMERR("Incorrect bdev major minor number");
			return -EOVERFLOW;
		}
	}

	DMINFO("key:%s dev:%s", argv[0], argv[1]);

	if (extract_fec_header(dev, &fec, &ecc)) {
		DMERR("Error while extracting fec header");
		return -EINVAL;
	}

	metadata = extract_metadata(dev, &fec);

	if (IS_ERR(metadata)) {
		DMERR("Error while extracting metadata");
		return -EINVAL;
	}

	table_ptr = metadata->verity_table;

	for (i = 0; i < VERITY_TABLE_ARGS; i++) {
		verity_table_args[i] = strsep(&table_ptr, " ");
		if (verity_table_args[i] == NULL)
			break;
	}

	if (i != VERITY_TABLE_ARGS) {
		DMERR("Verity table not in the expected format");
		err = -EINVAL;
		goto free_metadata;
	}

	if (sscanf(verity_table_args[5], "%llu%c", &tmpll, &dummy)
							!= 1) {
		DMERR("Verity table not in the expected format");
		err = -EINVAL;
		goto free_metadata;
	}

	if (tmpll > ULONG_MAX) {
		DMERR("<num_data_blocks> too large. Forgot to turn on CONFIG_LBDAF?");
		err = -EINVAL;
		goto free_metadata;
	}

	data_sectors = tmpll;

	if (sscanf(verity_table_args[3], "%u%c", &data_block_size, &dummy)
								!= 1) {
		DMERR("Verity table not in the expected format");
		err = -EINVAL;
		goto free_metadata;
	}

	if (test_mult_overflow(data_sectors, data_block_size >>
							SECTOR_SHIFT)) {
		DMERR("data_sectors too large");
		err = -EOVERFLOW;
		goto free_metadata;
	}

	data_sectors *= data_block_size >> SECTOR_SHIFT;
	DMINFO("Data sectors %llu", (unsigned long long)data_sectors);

	/* update target length */
	ti->len = data_sectors;

	/*substitute data_dev and hash_dev*/
	verity_table_args[1] = argv[1];
	verity_table_args[2] = argv[1];

	mode = DM_VERITY_MODE_EIO;

	if (ecc.valid && IS_BUILTIN(CONFIG_DM_VERITY_FEC)) {
		if (mode) {
			err = snprintf(buf, FEC_ARG_LENGTH,
			"%u %s " VERITY_TABLE_OPT_FEC_FORMAT,
			1 + VERITY_TABLE_OPT_FEC_ARGS,
			mode == DM_VERITY_MODE_RESTART ?
			VERITY_TABLE_OPT_RESTART : VERITY_TABLE_OPT_LOGGING,
			argv[1], ecc.start / FEC_BLOCK_SIZE, ecc.blocks,
			ecc.roots);
		} else {
			err = snprintf(buf, FEC_ARG_LENGTH,
			"%u " VERITY_TABLE_OPT_FEC_FORMAT,
			VERITY_TABLE_OPT_FEC_ARGS, argv[1],
			ecc.start / FEC_BLOCK_SIZE, ecc.blocks, ecc.roots);
		}
	} else if (mode) {
		err = snprintf(buf, FEC_ARG_LENGTH,
			"2 " VERITY_TABLE_OPT_IGNZERO " %s",
			mode == DM_VERITY_MODE_RESTART ?
			VERITY_TABLE_OPT_RESTART : VERITY_TABLE_OPT_LOGGING);
	} else {
		err = snprintf(buf, FEC_ARG_LENGTH, "1 %s",
				 "ignore_zero_blocks");
	}

	if (err < 0 || err >= FEC_ARG_LENGTH)
		goto free_metadata;

	buf_ptr = buf;

	for (i = VERITY_TABLE_ARGS; i < (VERITY_TABLE_ARGS +
		VERITY_TABLE_OPT_FEC_ARGS + 2); i++) {
		verity_table_args[i] = strsep(&buf_ptr, " ");
		if (verity_table_args[i] == NULL) {
			no_of_args = i;
			break;
		}
	}

	err = verity_ctr(ti, no_of_args, verity_table_args);

free_metadata:
	kfree(metadata->header);
	kfree(metadata->verity_table);
	kfree(metadata);
	return err;
}

static struct target_type android_verity_target = {
	.name			= "android-verity",
	.version		= {1, 0, 0},
	.module			= THIS_MODULE,
	.ctr			= android_verity_ctr,
	.dtr			= verity_dtr,
	.map			= verity_map,
	.status			= verity_status,
	.ioctl			= verity_ioctl,
	.merge			= verity_merge,
	.iterate_devices	= verity_iterate_devices,
	.io_hints		= verity_io_hints,
};
#endif

static struct target_type verity_target = {
	.name		= "verity",
	.version	= {1, 2, 0},
	.module		= THIS_MODULE,
	.ctr		= verity_ctr,
	.dtr		= verity_dtr,
	.map		= verity_map,
	.status		= verity_status,
	.ioctl		= verity_ioctl,
	.merge		= verity_merge,
	.iterate_devices = verity_iterate_devices,
	.io_hints	= verity_io_hints,
};

static int __init dm_verity_init(void)
{
	int r;

	r = dm_register_target(&verity_target);
	if (r < 0)
		DMERR("register failed %d", r);

#ifdef CONFIG_DM_ANDROID_VERITY
	r = dm_register_target(&android_verity_target);
	if (r < 0)
		DMERR("register failed %d", r);
#endif

	return r;
}

static void __exit dm_verity_exit(void)
{
	dm_unregister_target(&verity_target);
}

module_init(dm_verity_init);
module_exit(dm_verity_exit);

MODULE_AUTHOR("Mikulas Patocka <mpatocka@redhat.com>");
MODULE_AUTHOR("Mandeep Baines <msb@chromium.org>");
MODULE_AUTHOR("Will Drewry <wad@chromium.org>");
MODULE_DESCRIPTION(DM_NAME " target for transparent disk integrity checking");
MODULE_LICENSE("GPL");
