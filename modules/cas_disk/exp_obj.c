/*
* Copyright(c) 2012-2019 Intel Corporation
* SPDX-License-Identifier: BSD-3-Clause-Clear
*/
#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/blkpg.h>
#include <linux/elevator.h>
#include <linux/blk-mq.h>

#include "cas_disk_defs.h"
#include "cas_disk.h"
#include "disk.h"
#include "exp_obj.h"
#include "linux_kernel_version.h"

#define CASDSK_DEV_MINORS 16
#define KMEM_CACHE_MIN_SIZE sizeof(void *)

int __init casdsk_init_exp_objs(void)
{
	int ncpus;

	CASDSK_DEBUG_TRACE();

	casdsk_module->exp_obj_cache = kmem_cache_create("casdsk_exp_obj",
			sizeof(struct casdsk_exp_obj), 0, 0, NULL);
	if (!casdsk_module->exp_obj_cache)
		goto error_exp_obj_cache;

	ncpus = num_online_cpus();

	casdsk_module->pending_rqs_cache =
		kmem_cache_create("casdsk_exp_obj_pending_rqs",
			((sizeof(atomic_t) * ncpus) < KMEM_CACHE_MIN_SIZE) ?
			KMEM_CACHE_MIN_SIZE : (sizeof(atomic_t) * ncpus),
			0, 0, NULL);
	if (!casdsk_module->pending_rqs_cache)
		goto error_pending_rqs_cache;

	casdsk_module->pt_io_ctx_cache =
		kmem_cache_create("casdsk_exp_obj_pt_io_ctx",
				sizeof(struct casdsk_exp_obj_pt_io_ctx),
				0, 0, NULL);
	if (!casdsk_module->pt_io_ctx_cache)
		goto error_pt_io_ctx_cache;

	return 0;

error_pt_io_ctx_cache:
	kmem_cache_destroy(casdsk_module->pending_rqs_cache);
error_pending_rqs_cache:
	kmem_cache_destroy(casdsk_module->exp_obj_cache);
error_exp_obj_cache:
	return -ENOMEM;
}

void casdsk_deinit_exp_objs(void)
{
	CASDSK_DEBUG_TRACE();

	kmem_cache_destroy(casdsk_module->pt_io_ctx_cache);
	kmem_cache_destroy(casdsk_module->pending_rqs_cache);
	kmem_cache_destroy(casdsk_module->exp_obj_cache);
}

static inline void _casdsk_exp_obj_handle_bio_att(struct casdsk_disk *dsk,
						struct request_queue *q,
						struct bio *bio)
{
	int status = CASDSK_BIO_NOT_HANDLED;

	if (likely(dsk->exp_obj->ops->make_request_fn))
		status = dsk->exp_obj->ops->
				make_request_fn(dsk, q, bio, dsk->private);

	if (status == CASDSK_BIO_NOT_HANDLED)
		dsk->exp_obj->mk_rq_fn(q, bio);
}

CAS_DECLARE_BLOCK_CALLBACK(_casdsk_exp_obj_bio_pt_io, struct bio *bio,
		unsigned int bytes_done, int error)
{
	struct casdsk_exp_obj_pt_io_ctx *io;

	BUG_ON(!bio);
	CAS_BLOCK_CALLBACK_INIT(bio);

	io = bio->bi_private;
	BUG_ON(!io);
	CAS_BIO_ENDIO(io->bio, CAS_BIO_BISIZE(io->bio),
			CAS_BLOCK_CALLBACK_ERROR(bio, CAS_ERRNO_TO_BLK_STS(error)));

	if (atomic_dec_return(&io->dsk->exp_obj->pt_ios) < 0)
		BUG();

	bio_put(bio);
	kmem_cache_free(casdsk_module->pt_io_ctx_cache, io);
	CAS_BLOCK_CALLBACK_RETURN();
}

static inline void _casdsk_exp_obj_handle_bio_pt(struct casdsk_disk *dsk,
					       struct request_queue *q,
					       struct bio *bio)
{
	struct bio *cloned_bio;
	struct casdsk_exp_obj_pt_io_ctx *io;

	io = kmem_cache_zalloc(casdsk_module->pt_io_ctx_cache, GFP_ATOMIC);
	if (!io) {
		CAS_BIO_ENDIO(bio, CAS_BIO_BISIZE(bio), CAS_ERRNO_TO_BLK_STS(-ENOMEM));
		return;
	}

	cloned_bio = cas_bio_clone(bio, GFP_ATOMIC);
	if (!cloned_bio) {
		kmem_cache_free(casdsk_module->pt_io_ctx_cache, io);
		CAS_BIO_ENDIO(bio, CAS_BIO_BISIZE(bio), CAS_ERRNO_TO_BLK_STS(-ENOMEM));
		return;
	}

	io->bio = bio;
	io->dsk = dsk;

	atomic_inc(&dsk->exp_obj->pt_ios);

	CAS_BIO_SET_DEV(cloned_bio, casdsk_disk_get_blkdev(dsk));
	cloned_bio->bi_private = io;
	cloned_bio->bi_end_io = CAS_REFER_BLOCK_CALLBACK(_casdsk_exp_obj_bio_pt_io);
	cas_submit_bio(CAS_BIO_OP_FLAGS(cloned_bio), cloned_bio);
}

static inline void _casdsk_exp_obj_handle_bio(struct casdsk_disk *dsk,
					    struct request_queue *q,
					    struct bio *bio)
{
	if (likely(casdsk_disk_is_attached(dsk)))
		_casdsk_exp_obj_handle_bio_att(dsk, q, bio);
	else if (casdsk_disk_is_pt(dsk))
		_casdsk_exp_obj_handle_bio_pt(dsk, q, bio);
	else if (casdsk_disk_is_shutdown(dsk))
		CAS_BIO_ENDIO(bio, CAS_BIO_BISIZE(bio), CAS_ERRNO_TO_BLK_STS(-EIO));
	else
		BUG();
}

static inline void _casdsk_exp_obj_end_rq(struct casdsk_disk *dsk, unsigned int cpu)
{
	return atomic_dec(&dsk->exp_obj->pending_rqs[cpu]);
}

static inline unsigned int _casdsk_exp_obj_begin_rq(struct casdsk_disk *dsk)
{
	unsigned int cpu;

	BUG_ON(!dsk);

retry:
	while (unlikely(casdsk_disk_in_transition(dsk)))
		io_schedule();

	cpu = smp_processor_id();
	atomic_inc(&dsk->exp_obj->pending_rqs[cpu]);

	if (unlikely(casdsk_disk_in_transition(dsk))) {
		/*
		 * If we are in transition state, decrement pending rqs counter
		 * and retry bio processing
		 */
		_casdsk_exp_obj_end_rq(dsk, cpu);
		goto retry;
	}

	return cpu;
}

static MAKE_RQ_RET_TYPE _casdsk_exp_obj_make_rq_fn(struct request_queue *q,
						 struct bio *bio)
{
	struct casdsk_disk *dsk;
	unsigned int cpu;

	BUG_ON(!bio);
	BUG_ON(!q);
	BUG_ON(!q->queuedata);
	dsk = q->queuedata;

	cpu = _casdsk_exp_obj_begin_rq(dsk);

	_casdsk_exp_obj_handle_bio(dsk, q, bio);

	_casdsk_exp_obj_end_rq(dsk, cpu);

	KRETURN(0);
}

static int _casdsk_get_next_part_no(struct block_device *bd)
{
	int part_no = 0;
	struct gendisk *disk = bd->bd_disk;
	struct disk_part_iter piter;
	struct hd_struct *part;

	mutex_lock(&bd->bd_mutex);

	disk_part_iter_init(&piter, disk, DISK_PITER_INCL_EMPTY);
	while ((part = disk_part_iter_next(&piter))) {
		part_no = part->partno;
		break;
	}
	disk_part_iter_exit(&piter);

	mutex_unlock(&bd->bd_mutex);

	return part_no;
}

static int _casdsk_del_partitions(struct block_device *bd)
{
	int result = 0;
	int part_no;
	struct blkpg_partition bpart;
	struct blkpg_ioctl_arg barg;

	memset(&bpart, 0, sizeof(struct blkpg_partition));
	memset(&barg, 0, sizeof(struct blkpg_ioctl_arg));
	barg.data = (void __force __user *) &bpart;
	barg.op = BLKPG_DEL_PARTITION;


	while ((part_no = _casdsk_get_next_part_no(bd))) {
		bpart.pno = part_no;
		result = ioctl_by_bdev(bd, BLKPG, (unsigned long) &barg);
		if (result == 0) {
			printk(CASDSK_KERN_INFO "Partition %d on %s hidden\n",
				part_no, bd->bd_disk->disk_name);
		} else {
			printk(CASDSK_KERN_ERR "Error(%d) hiding the partition %d on %s\n",
				result, part_no, bd->bd_disk->disk_name);
			break;
		}
	}

	return result;
}

#ifdef GENHD_FL_NO_PART_SCAN
static int _casdsk_flags = GENHD_FL_NO_PART_SCAN | GENHD_FL_EXT_DEVT;
#else
static int _casdsk_flags = GENHD_FL_EXT_DEVT;
#endif

static int _casdsk_exp_obj_hide_parts(struct casdsk_disk *dsk)
{
	struct block_device *bd = casdsk_disk_get_blkdev(dsk);
	struct gendisk *gdsk = casdsk_disk_get_gendisk(dsk);

	if (bd != bd->bd_contains)
		/* It is partition, no more job required */
		return 0;

	if (disk_max_parts(dsk->bd->bd_disk) > 1) {
		if (_casdsk_del_partitions(bd)) {
			printk(CASDSK_KERN_ERR "Error deleting a partition on thedevice %s\n",
				gdsk->disk_name);

			/* Try restore previous partitions by rescaning */
			ioctl_by_bdev(bd, BLKRRPART, (unsigned long) NULL);
			return -EINVAL;
		}
	}

	/* Save original flags and minors */
	dsk->gd_flags = gdsk->flags & _casdsk_flags;
	dsk->gd_minors = gdsk->minors;

	/* Setup disk of bottom device as not partitioned device */
	gdsk->flags &= ~_casdsk_flags;
	gdsk->minors = 1;
	/* Rescan partitions */
	ioctl_by_bdev(bd, BLKRRPART, (unsigned long) NULL);

	return 0;
}

static int _casdsk_exp_obj_set_dev_t(struct casdsk_disk *dsk, struct gendisk *gd)
{
	int flags;
	int minors = disk_max_parts(casdsk_disk_get_gendisk(dsk));
	struct block_device *bdev;

	bdev = casdsk_disk_get_blkdev(dsk);
	BUG_ON(!bdev);

	if (bdev->bd_contains != bdev) {
		minors = 1;
		flags = 0;
	} else {
		if (_casdsk_exp_obj_hide_parts(dsk))
			return -EINVAL;
		flags = dsk->gd_flags;
	}

	gd->first_minor = casdsk_disk_allocate_minors(minors);
	if (gd->first_minor < 0) {
		CASDSK_DEBUG_DISK_ERROR(dsk, "Cannot allocate %d minors", minors);
		return -EINVAL;
	}
	gd->minors = minors;

	gd->major = casdsk_module->disk_major;
	gd->flags |= flags;

	return 0;
}

static void _casdsk_exp_obj_clear_dev_t(struct casdsk_disk *dsk)
{
	struct block_device *bdev = casdsk_disk_get_blkdev(dsk);
	struct gendisk *gdsk = casdsk_disk_get_gendisk(dsk);

	if (bdev->bd_contains == bdev) {
		/* Restore previous configuration of bottom disk */
		gdsk->minors = dsk->gd_minors;
		gdsk->flags |= dsk->gd_flags;
		ioctl_by_bdev(bdev, BLKRRPART, (unsigned long) NULL);
	}
}

static const struct block_device_operations _casdsk_exp_obj_ops = {
	.owner = THIS_MODULE,
};

static int casdsk_exp_obj_alloc(struct casdsk_disk *dsk)
{
	struct casdsk_exp_obj *exp_obj;
	int result;

	BUG_ON(!dsk);
	BUG_ON(dsk->exp_obj);

	CASDSK_DEBUG_DISK_TRACE(dsk);

	exp_obj = kmem_cache_zalloc(casdsk_module->exp_obj_cache, GFP_KERNEL);
	if (!exp_obj) {
		CASDSK_DEBUG_ERROR("Cannot allocate memory");
		result = -ENOMEM;
		goto error_exp_obj_alloc;
	}

	exp_obj->pending_rqs = kmem_cache_zalloc(casdsk_module->pending_rqs_cache,
						 GFP_KERNEL);
	if (!exp_obj->pending_rqs) {
		result = -ENOMEM;
		goto error_pending_rqs_alloc;
	}

	dsk->exp_obj = exp_obj;

	return 0;

error_pending_rqs_alloc:
	kmem_cache_free(casdsk_module->exp_obj_cache, exp_obj);
error_exp_obj_alloc:
	return result;
}

void casdsk_exp_obj_free(struct casdsk_disk *dsk)
{
	struct casdsk_exp_obj *exp_obj;

	CASDSK_DEBUG_DISK_TRACE(dsk);

	exp_obj = dsk->exp_obj;

	if (!exp_obj)
		return;

	kobject_put(&exp_obj->kobj);
	dsk->exp_obj = NULL;
}
EXPORT_SYMBOL(casdsk_exp_obj_free);

static void __casdsk_exp_obj_release(struct casdsk_exp_obj *exp_obj)
{
	kfree(exp_obj->dev_name);
	kmem_cache_free(casdsk_module->pending_rqs_cache, exp_obj->pending_rqs);
	kmem_cache_free(casdsk_module->exp_obj_cache, exp_obj);
}

static void _casdsk_exp_obj_release(struct kobject *kobj)
{
	struct casdsk_exp_obj *exp_obj;
	struct module *owner;

	BUG_ON(!kobj);

	exp_obj = casdsk_kobj_to_exp_obj(kobj);
	BUG_ON(!exp_obj);

	CASDSK_DEBUG_TRACE();

	owner = exp_obj->owner;

	__casdsk_exp_obj_release(exp_obj);

	if (owner)
		module_put(owner);
}

static struct kobj_type casdsk_exp_obj_ktype = {
	.release = _casdsk_exp_obj_release
};

static int _casdsk_exp_obj_init_kobject(struct casdsk_disk *dsk)
{
	int result = 0;
	struct casdsk_exp_obj *exp_obj = dsk->exp_obj;

	kobject_init(&exp_obj->kobj, &casdsk_exp_obj_ktype);
	result = kobject_add(&exp_obj->kobj, &dsk->kobj,
			     "%s", exp_obj->dev_name);
	if (result)
		CASDSK_DEBUG_DISK_ERROR(dsk, "Cannot register kobject");

	return result;
}

static CAS_BLK_STATUS_T _casdsk_exp_obj_queue_qr(struct blk_mq_hw_ctx *hctx,
			const struct blk_mq_queue_data *bd)
{
	struct casdsk_disk *dsk = hctx->driver_data;
	struct casdsk_exp_obj *exp_obj = dsk->exp_obj;
	struct request *rq = bd->rq;
	CAS_BLK_STATUS_T result = CAS_BLK_STS_OK;

	if (likely(exp_obj->ops && exp_obj->ops->queue_rq_fn)) {
		exp_obj->ops->pending_rq_inc(dsk, dsk->private);

		result = exp_obj->ops->queue_rq_fn(dsk, rq, dsk->private);

		exp_obj->ops->pending_rq_dec(dsk, dsk->private);
	} else {
		/*
		* queue_rq_fn() is required, as we can't do any default
		* action in attached mode. In PT mode we handle all bios
		* directly in make_request_fn(), so queue_rq_fn() will not
		* be called.
		*/
		BUG_ON(rq);
	}

	return result;
}

static struct blk_mq_ops casdsk_mq_ops = {
	.queue_rq       = _casdsk_exp_obj_queue_qr,
};

static void _casdsk_init_queues(struct casdsk_disk *dsk)
{
	struct request_queue *q = dsk->exp_obj->queue;
	struct blk_mq_hw_ctx *hctx;
	int i;

	queue_for_each_hw_ctx(q, hctx, i) {
		if (!hctx->nr_ctx || !hctx->tags)
			continue;

		hctx->driver_data = dsk;
	}
}

static int _casdsk_init_tag_set(struct casdsk_disk *dsk, struct blk_mq_tag_set *set)
{
	BUG_ON(!dsk);
	BUG_ON(!set);

	set->ops = &casdsk_mq_ops;
	set->nr_hw_queues = num_online_cpus();
	set->numa_node = NUMA_NO_NODE;
	/*TODO: Should we inherit qd from core device? */
	set->queue_depth = BLKDEV_MAX_RQ;

	set->cmd_size = 0;
	set->flags = BLK_MQ_F_SHOULD_MERGE;

	set->driver_data = dsk;

	return blk_mq_alloc_tag_set(set);
}

int casdsk_exp_obj_create(struct casdsk_disk *dsk, const char *dev_name,
			struct module *owner, struct casdsk_exp_obj_ops *ops)
{
	struct casdsk_exp_obj *exp_obj;
	struct request_queue *queue;
	struct gendisk *gd;
	int result = 0;

	BUG_ON(!owner);
	BUG_ON(!dsk);
	BUG_ON(!ops);
	BUG_ON(dsk->exp_obj);

	CASDSK_DEBUG_DISK_TRACE(dsk);

	if (strlen(dev_name) >= DISK_NAME_LEN)
		return -EINVAL;

	result = casdsk_exp_obj_alloc(dsk);
	if (result)
		goto error_alloc;

	exp_obj = dsk->exp_obj;

	exp_obj->dev_name = kstrdup(dev_name, GFP_KERNEL);
	if (!exp_obj->dev_name) {
		__casdsk_exp_obj_release(exp_obj);
		result = -ENOMEM;
		goto error_strdup;
	}

	result = _casdsk_exp_obj_init_kobject(dsk);
	if (result) {
		__casdsk_exp_obj_release(exp_obj);
		goto error_kobject;
	}

	if (!try_module_get(owner)) {
		CASDSK_DEBUG_DISK_ERROR(dsk, "Cannot get reference to module");
		result = -ENAVAIL;
		goto error_module;
	}
	exp_obj->owner = owner;
	exp_obj->ops = ops;

	gd = alloc_disk(1);
	if (!gd) {
		result = -ENOMEM;
		goto error_alloc_disk;
	}
	exp_obj->gd = gd;

	result = _casdsk_exp_obj_set_dev_t(dsk, gd);
	if (result)
		goto error_dev_t;

	result = _casdsk_init_tag_set(dsk, &dsk->tag_set);
	if (result) {
		goto error_init_tag_set;
	}

	queue = blk_mq_init_queue(&dsk->tag_set);
	if (!queue) {
		result = -ENOMEM;
		goto error_init_queue;
	}

	BUG_ON(queue->queuedata);
	queue->queuedata = dsk;
	exp_obj->queue = queue;

	_casdsk_init_queues(dsk);

	gd->fops = &_casdsk_exp_obj_ops;
	gd->queue = queue;
	gd->private_data = dsk;
	strlcpy(gd->disk_name, exp_obj->dev_name, sizeof(gd->disk_name));

	dsk->exp_obj->mk_rq_fn = queue->make_request_fn;
	blk_queue_make_request(queue, _casdsk_exp_obj_make_rq_fn);

	if (exp_obj->ops->set_geometry) {
		result = exp_obj->ops->set_geometry(dsk, dsk->private);
		if (result)
			goto error_set_geometry;
	}

	return 0;

error_set_geometry:
	if (exp_obj->ops->cleanup_queue)
		exp_obj->ops->cleanup_queue(dsk, queue, dsk->private);
error_init_queue:
	blk_mq_free_tag_set(&dsk->tag_set);
error_init_tag_set:
	_casdsk_exp_obj_clear_dev_t(dsk);
error_dev_t:
	put_disk(gd);
error_alloc_disk:
	module_put(owner);
	dsk->exp_obj->owner = NULL;
error_module:
	casdsk_exp_obj_free(dsk);
error_kobject:
error_strdup:
	dsk->exp_obj = NULL;
error_alloc:
	return result;
}
EXPORT_SYMBOL(casdsk_exp_obj_create);

struct request_queue *casdsk_exp_obj_get_queue(struct casdsk_disk *dsk)
{
	BUG_ON(!dsk);
	BUG_ON(!dsk->exp_obj);
	return dsk->exp_obj->queue;
}
EXPORT_SYMBOL(casdsk_exp_obj_get_queue);

struct gendisk *casdsk_exp_obj_get_gendisk(struct casdsk_disk *dsk)
{
	BUG_ON(!dsk);
	BUG_ON(!dsk->exp_obj);
	return dsk->exp_obj->gd;
}
EXPORT_SYMBOL(casdsk_exp_obj_get_gendisk);

static bool _casdsk_exp_obj_exists(const char *path)
{
	struct file *exported;

	exported = filp_open(path, O_RDONLY, 0);

	if (!exported || IS_ERR(exported)) {
		/*failed to open file - it is safe to assume,
		 * it does not exist
		 */
		return false;
	}

	filp_close(exported, NULL);
	return true;
}

int casdsk_exp_obj_activate(struct casdsk_disk *dsk)
{
	char *path;
	int result;

	BUG_ON(!dsk);
	BUG_ON(!dsk->exp_obj);
	BUG_ON(!dsk->exp_obj->gd);
	BUG_ON(dsk->exp_obj->activated);

	CASDSK_DEBUG_DISK_TRACE(dsk);

	path = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!path)
		return -ENOMEM;

	snprintf(path, PATH_MAX, "/dev/%s", dsk->exp_obj->dev_name);
	if (_casdsk_exp_obj_exists(path)) {
		printk(CASDSK_KERN_ERR "Could not activate exported object, "
				"because file %s exists.\n", path);
		kfree(path);
		return -EEXIST;
	}
	kfree(path);

	dsk->exp_obj->activated = true;
	atomic_set(&dsk->mode, CASDSK_MODE_ATTACHED);
	add_disk(dsk->exp_obj->gd);

	result = bd_claim_by_disk(dsk->bd, dsk, dsk->exp_obj->gd);
	if (result)
		goto error_bd_claim;

	result = sysfs_create_link(&dsk->exp_obj->kobj,
				   &disk_to_dev(dsk->exp_obj->gd)->kobj,
				   "blockdev");
	if (result)
		goto error_sysfs_link;

	CASDSK_DEBUG_DISK(dsk, "Activated exp object %s", dsk->exp_obj->dev_name);

	return 0;

error_sysfs_link:
	bd_release_from_disk(dsk->bd, dsk->exp_obj->gd);
error_bd_claim:
	del_gendisk(dsk->exp_obj->gd);
	dsk->exp_obj->activated = false;
	return result;
}
EXPORT_SYMBOL(casdsk_exp_obj_activate);

bool casdsk_exp_obj_activated(struct casdsk_disk *dsk)
{
	BUG_ON(!dsk);
	return dsk->exp_obj->activated;
}
EXPORT_SYMBOL(casdsk_exp_obj_activated);

int casdsk_exp_obj_lock(struct casdsk_disk *dsk)
{
	struct casdsk_exp_obj *exp_obj;

	BUG_ON(!dsk);
	BUG_ON(!dsk->exp_obj);

	CASDSK_DEBUG_DISK_TRACE(dsk);

	exp_obj = dsk->exp_obj;

	exp_obj->locked_bd = bdget_disk(exp_obj->gd, 0);
	if (!exp_obj->locked_bd)
		return -ENAVAIL;

	mutex_lock(&exp_obj->locked_bd->bd_mutex);

	if (exp_obj->locked_bd->bd_openers) {
		printk(CASDSK_KERN_DEBUG "Device %s in use (openers=%d). Refuse to stop\n",
		       exp_obj->locked_bd->bd_disk->disk_name,
		       exp_obj->locked_bd->bd_openers);

		casdsk_exp_obj_unlock(dsk);
		return -EBUSY;
	}

	return 0;
}
EXPORT_SYMBOL(casdsk_exp_obj_lock);

int casdsk_exp_obj_unlock(struct casdsk_disk *dsk)
{
	BUG_ON(!dsk);
	BUG_ON(!dsk->exp_obj);
	BUG_ON(!dsk->exp_obj->locked_bd);

	CASDSK_DEBUG_DISK_TRACE(dsk);

	mutex_unlock(&dsk->exp_obj->locked_bd->bd_mutex);
	bdput(dsk->exp_obj->locked_bd);
	dsk->exp_obj->locked_bd = NULL;

	return 0;
}
EXPORT_SYMBOL(casdsk_exp_obj_unlock);

int casdsk_exp_obj_destroy(struct casdsk_disk *dsk)
{
	struct casdsk_exp_obj *exp_obj;

	BUG_ON(!dsk);

	if (!dsk->exp_obj)
		return -ENODEV;

	BUG_ON(!dsk->exp_obj->locked_bd);

	CASDSK_DEBUG_DISK_TRACE(dsk);

	exp_obj = dsk->exp_obj;

	if (casdsk_exp_obj_activated(dsk)) {
		sysfs_remove_link(&exp_obj->kobj, "blockdev");
		bd_release_from_disk(dsk->bd, exp_obj->gd);
		_casdsk_exp_obj_clear_dev_t(dsk);
		del_gendisk(exp_obj->gd);
	}

	if (exp_obj->queue)
		blk_cleanup_queue(exp_obj->queue);

	blk_mq_free_tag_set(&dsk->tag_set);

	atomic_set(&dsk->mode, CASDSK_MODE_UNKNOWN);
	put_disk(exp_obj->gd);

	return 0;

}
EXPORT_SYMBOL(casdsk_exp_obj_destroy);

int casdsk_exp_obj_dettach(struct casdsk_disk *dsk)
{
	module_put(dsk->exp_obj->owner);

	dsk->exp_obj->owner = NULL;
	dsk->exp_obj->ops = NULL;

	return 0;
}

int casdsk_exp_obj_attach(struct casdsk_disk *dsk, struct module *owner,
			struct casdsk_exp_obj_ops *ops)
{
	if (!try_module_get(owner)) {
		CASDSK_DEBUG_DISK_ERROR(dsk, "Cannot get reference to module");
		return -ENAVAIL;
	}
	dsk->exp_obj->owner = owner;
	dsk->exp_obj->ops = ops;

	return 0;
}

static void _casdsk_exp_obj_wait_for_pending_rqs(struct casdsk_disk *dsk)
{
	int i, ncpus;
	struct casdsk_exp_obj *exp_obj = dsk->exp_obj;

	ncpus = num_online_cpus();
	for (i = 0; i < ncpus; i++)
		while (atomic_read(&exp_obj->pending_rqs[i]))
			schedule();
}

static void _casdsk_exp_obj_flush_queue(struct casdsk_disk *dsk)
{
	struct casdsk_exp_obj *exp_obj = dsk->exp_obj;
	struct request_queue *q = exp_obj->queue;

	blk_mq_run_hw_queues(q, false);
	blk_sync_queue(q);
}

void casdsk_exp_obj_prepare_pt(struct casdsk_disk *dsk)
{
	_casdsk_exp_obj_wait_for_pending_rqs(dsk);
	_casdsk_exp_obj_flush_queue(dsk);
}

void casdsk_exp_obj_prepare_attached(struct casdsk_disk *dsk)
{
	_casdsk_exp_obj_wait_for_pending_rqs(dsk);

	while (atomic_read(&dsk->exp_obj->pt_ios))
		schedule_timeout(msecs_to_jiffies(200));
}

void casdsk_exp_obj_prepare_shutdown(struct casdsk_disk *dsk)
{
	_casdsk_exp_obj_wait_for_pending_rqs(dsk);

	while (atomic_read(&dsk->exp_obj->pt_ios))
		schedule_timeout(msecs_to_jiffies(200));
}
