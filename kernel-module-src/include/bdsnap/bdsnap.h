#ifndef BDSNAP_H
#define BDSNAP_H

#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,9,0)
#include <linux/blk_types.h>
#elif KERNEL_VERSION(2,3,37) <= LINUX_VERSION_CODE && LINUX_VERSION_CODE < KERNEL_VERSION(5,9,0)
#include <linux/fs.h>
#else
#error it's time to upgrade, don't you think? (unknown struct block_device)
#endif

/**
 * exported to the fs snapshot implementor, 
 * both meant to be run in interrupt context
 */

/**
 * bdsnap_search_device - lookup a registered device and get an handle to it
 * @bdev: the block device to search for
 * @saved_cpu_flags: ptr to an unsigned long to save CPU's FLAGS register
 *
 * Returns a valid handle (!= NULL) if bdev is activated (aka registered),
 * and no deactivation request from userspace is in progress. 
 * FS-specific part implementor should check: if returned value is NULL then, 
 * nothing to do, do cleanup, issue a bdsnap_end() and return immediately, 
 * otherwise proceed.
 *
 * IMPORTANT NOTE: BEFORE AND AFTER bdsnap_search_device, bdsnap_make_snapshot you must have
 * a RCU-protected section, and you should take care of this aspect.
 * The sequence is RCU_LOCK -> SEARCH_DEVICE -> MAKE_SNAPSHOT -> RCU_UNLOCK
 */
void* bdsnap_search_device(
		const struct block_device* bdev, 
		unsigned long *saved_cpu_flags);

/**
 * bdsnap_make_snapshot - defer work to record block of some size
 * @handle: the valid handle retrieved via bdsnap_search_device
 * @block: the block to save
 * @blocknr: the block number
 * @blocksize: the size of the block
 * @cpu_flags: CPU FLAGS to restore
 *
 * Returns true if we could schedule the deferred work, false otherwise.
 * This can happen because in between bdsnap_search_device and bdsnap_make_snapshot
 * the user may have requested a deactivation for the bdev you searched for
 * (the deferred work wq is in draining and will be destroyed).
 *
 * IMPORTANT NOTE: BEFORE AND AFTER bdsnap_search_device, bdsnap_make_snapshot you must have
 * a RCU-protected section, and you should take care of this aspect.
 * The sequence is RCU_LOCK -> SEARCH_DEVICE -> MAKE_SNAPSHOT -> RCU_UNLOCK
 */
bool bdsnap_make_snapshot(
		void* handle, const char* block, 
		sector_t blocknr, unsigned blocksize, 
		unsigned long cpu_flags);

#endif
