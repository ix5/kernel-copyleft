/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write the Free Software Foundation,
 * Inc.,  51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */
/*
 * NOTE: This file has been modified by Sony Mobile Communications Inc.
 * Modifications are Copyright (c) 2015 Sony Mobile Communications Inc,
 * and licensed under the license of the file.
 */
#include <linux/mm.h>
#include <linux/sched/mm.h>
#include <linux/highmem.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/blkdev.h>
#include <linux/backing-dev.h>
#include "kmem.h"
#include "xfs_message.h"

void *
kmem_alloc(size_t size, xfs_km_flags_t flags)
{
	int	retries = 0;
	gfp_t	lflags = kmem_flags_convert(flags);
	void	*ptr;

	do {
		ptr = kmalloc(size, lflags);
		if (ptr || (flags & (KM_MAYFAIL|KM_NOSLEEP)))
			return ptr;
		if (!(++retries % 100))
			xfs_err(NULL,
	"%s(%u) possible memory allocation deadlock size %u in %s (mode:0x%x)",
				current->comm, current->pid,
				(unsigned int)size, __func__, lflags);
		congestion_wait(BLK_RW_ASYNC, HZ/50);
	} while (1);
}

void *
kmem_zalloc_large(size_t size, xfs_km_flags_t flags)
{
	unsigned nofs_flag = 0;
	void	*ptr;
	gfp_t	lflags;

	ptr = kmem_zalloc(size, flags | KM_MAYFAIL);
	if (ptr)
		return ptr;

	/*
	 * __vmalloc() will allocate data pages and auxillary structures (e.g.
	 * pagetables) with GFP_KERNEL, yet we may be under GFP_NOFS context
	 * here. Hence we need to tell memory reclaim that we are in such a
	 * context via PF_MEMALLOC_NOFS to prevent memory reclaim re-entering
	 * the filesystem here and potentially deadlocking.
	 */
	if (flags & KM_NOFS)
		nofs_flag = memalloc_nofs_save();

	lflags = kmem_flags_convert(flags);
	ptr = __vmalloc(size, lflags | __GFP_ZERO, PAGE_KERNEL);

	if (flags & KM_NOFS)
		memalloc_nofs_restore(nofs_flag);

	return ptr;
}

void *
kmem_realloc(const void *old, size_t newsize, xfs_km_flags_t flags)
{
	int	retries = 0;
	gfp_t	lflags = kmem_flags_convert(flags);
	void	*ptr;

	do {
		ptr = krealloc(old, newsize, lflags);
		if (ptr || (flags & (KM_MAYFAIL|KM_NOSLEEP)))
			return ptr;
		if (!(++retries % 100))
			xfs_err(NULL,
	"%s(%u) possible memory allocation deadlock size %zu in %s (mode:0x%x)",
				current->comm, current->pid,
				newsize, __func__, lflags);
		congestion_wait(BLK_RW_ASYNC, HZ/50);
	} while (1);
}

void *
kmem_zone_alloc(kmem_zone_t *zone, xfs_km_flags_t flags)
{
	int	retries = 0;
	gfp_t	lflags = kmem_flags_convert(flags);
	void	*ptr;

	do {
		ptr = kmem_cache_alloc(zone, lflags);
		if (ptr || (flags & (KM_MAYFAIL|KM_NOSLEEP)))
			return ptr;
		if (!(++retries % 100))
			xfs_err(NULL,
		"%s(%u) possible memory allocation deadlock in %s (mode:0x%x)",
				current->comm, current->pid,
				__func__, lflags);
		congestion_wait(BLK_RW_ASYNC, HZ/50);
	} while (1);
}
