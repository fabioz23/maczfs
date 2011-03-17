/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2007 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 * Portions Copyright 2007-2008 Apple Inc. All rights reserved.
 * Use is subject to license terms.
 */

/* Portions Copyright 2007 Jeremy Teo */

#pragma ident	"%Z%%M%	%I%	%E% SMI"

#ifdef _KERNEL
#include <sys/types.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/systm.h>
#include <sys/sysmacros.h>
#include <sys/resource.h>
#include <sys/mntent.h>
#ifndef __APPLE__
#include <sys/mkdev.h>
#include <sys/vfs_opreg.h>
#endif /*!__APPLE__*/
#include <sys/vfs.h>
#ifdef __APPLE__
#include "maczfs_vnode.h"
#else
#include <sys/vnode.h>
#endif
#include <sys/file.h>
#include <sys/kmem.h>
#include <sys/errno.h>
#include <sys/unistd.h>
#ifndef __APPLE__
#include <sys/mode.h>
#endif /*!__APPLE__*/
#include <sys/atomic.h>
#ifndef __APPLE__
#include <vm/pvn.h>
#include "fs/fs_subr.h"
#endif /*!__APPLE__*/
#include <sys/zfs_dir.h>
#include <sys/zfs_acl.h>
#include <sys/zfs_ioctl.h>
#include <sys/zfs_rlock.h>
#include <sys/zfs_fuid.h>
#include <sys/zfs_i18n.h>
#include <sys/fs/zfs.h>
#ifdef __APPLE__
#include <maczfs/kernel/maczfs_kernel.h>
#endif
#include <sys/kidmap.h>
#endif /* _KERNEL */

#include <sys/dmu.h>
#include <sys/refcount.h>
#include <sys/stat.h>
#include <sys/zap.h>
#include <sys/zfs_znode.h>

/*
 * Functions needed for userland (ie: libzpool) are not put under
 * #ifdef_KERNEL; the rest of the functions have dependencies
 * (such as VFS logic) that will not compile easily in userland.
 */
#ifdef _KERNEL
struct kmem_cache *znode_cache = NULL;

/*ARGSUSED*/
static void
znode_pageout_func(dmu_buf_t *dbuf, void *user_ptr)
{
	znode_t *zp = user_ptr;

#ifdef __APPLE__
#ifdef ZFS_DEBUG
	znode_stalker(zp, N_znode_pageout);
#endif
	mutex_enter(&zp->z_lock);
	/* indicate that this znode can be freed */
	zp->z_dbuf = NULL;

	if (zp->z_zfsvfs && vfs_isforce(zp->z_zfsvfs->z_vfs)) {
		mutex_exit(&zp->z_lock);
		zfs_znode_free(zp);
	} else {
        	mutex_exit(&zp->z_lock);
        }
#else
	vnode_t *vp = ZTOV(zp);

	mutex_enter(&zp->z_lock);
	zp->z_dbuf = NULL;
	if (vp->v_count == 0) {
		mutex_exit(&zp->z_lock);
		vn_invalid(vp);
		zfs_znode_free(zp);
	} else {
		mutex_exit(&zp->z_lock);
	}
#endif /* __APPLE__ */
}

/*ARGSUSED*/
static int
zfs_znode_cache_constructor(void *buf, void *cdrarg, int kmflags)
{
	znode_t *zp = buf;

#ifdef __APPLE__
	/* OS X Note: the vnode is attached later in zfs_attach_vnode() */
	zp->z_vnode = NULLVP;
	cv_init(&zp->z_cv, NULL, CV_DEFAULT, NULL);
	zp->z_link_node.list_next = NULL;
	zp->z_link_node.list_prev = NULL;
#else
	zp->z_vnode = vn_alloc(KM_SLEEP);
	zp->z_vnode->v_data = (caddr_t)zp;
#endif
	mutex_init(&zp->z_lock, NULL, MUTEX_DEFAULT, NULL);
	rw_init(&zp->z_map_lock, NULL, RW_DEFAULT, NULL);
	rw_init(&zp->z_parent_lock, NULL, RW_DEFAULT, NULL);
	rw_init(&zp->z_name_lock, NULL, RW_DEFAULT, NULL);
	mutex_init(&zp->z_acl_lock, NULL, MUTEX_DEFAULT, NULL);

	mutex_init(&zp->z_range_lock, NULL, MUTEX_DEFAULT, NULL);
	avl_create(&zp->z_range_avl, zfs_range_compare,
	    sizeof (rl_t), offsetof(rl_t, r_node));

	zp->z_dbuf = NULL;
	zp->z_dirlocks = 0;
	return (0);
}

/*ARGSUSED*/
static void
zfs_znode_cache_destructor(void *buf, void *cdarg)
{
	znode_t *zp = buf;

	ASSERT(zp->z_dirlocks == 0);
	mutex_destroy(&zp->z_lock);
	rw_destroy(&zp->z_map_lock);
	rw_destroy(&zp->z_parent_lock);
	rw_destroy(&zp->z_name_lock);
	mutex_destroy(&zp->z_acl_lock);
	avl_destroy(&zp->z_range_avl);
	mutex_destroy(&zp->z_range_lock);

	ASSERT(zp->z_dbuf == NULL);
#ifdef __APPLE__
	cv_destroy(&zp->z_cv);
#else
	ASSERT(ZTOV(zp)->v_count == 0);
	vn_free(ZTOV(zp));
#endif
}

void
zfs_znode_init(void)
{
	/*
	 * Initialize zcache
	 */
	ASSERT(znode_cache == NULL);
	znode_cache = kmem_cache_create("zfs_znode_cache",
	    sizeof (znode_t), 0, zfs_znode_cache_constructor,
	    zfs_znode_cache_destructor, NULL, NULL, NULL, 0);
}

void
zfs_znode_fini(void)
{
	/*
	 * Cleanup vfs & vnode ops
	 */
#ifndef __APPLE__
	zfs_remove_op_tables();
#endif /*!__APPLE__*/
	/*
	 * Cleanup zcache
	 */
	if (znode_cache)
		kmem_cache_destroy(znode_cache);
	znode_cache = NULL;
}

#ifdef __APPLE__
extern int (**zfs_dvnodeops) (void *);
extern int (**zfs_evnodeops) (void *);
extern int (**zfs_fvnodeops) (void *);
extern int (**zfs_symvnodeops) (void *);
extern int (**zfs_xdvnodeops) (void *);
extern int (**zfs_fifoops) (void *);

struct kmem_cache * znode_cache_get(void);

struct kmem_cache *
znode_cache_get(void)
{
	return znode_cache;
}

#else
struct vnodeops *zfs_dvnodeops;
struct vnodeops *zfs_fvnodeops;
struct vnodeops *zfs_symvnodeops;
struct vnodeops *zfs_xdvnodeops;
struct vnodeops *zfs_evnodeops;

void
zfs_remove_op_tables()
{
	/*
	 * Remove vfs ops
	 */
	ASSERT(zfsfstype);
	(void) vfs_freevfsops_by_type(zfsfstype);
	zfsfstype = 0;

	/*
	 * Remove vnode ops
	 */
	if (zfs_dvnodeops)
		vn_freevnodeops(zfs_dvnodeops);
	if (zfs_fvnodeops)
		vn_freevnodeops(zfs_fvnodeops);
	if (zfs_symvnodeops)
		vn_freevnodeops(zfs_symvnodeops);
	if (zfs_xdvnodeops)
		vn_freevnodeops(zfs_xdvnodeops);
	if (zfs_evnodeops)
		vn_freevnodeops(zfs_evnodeops);

	zfs_dvnodeops = NULL;
	zfs_fvnodeops = NULL;
	zfs_symvnodeops = NULL;
	zfs_xdvnodeops = NULL;
	zfs_evnodeops = NULL;
}

extern const fs_operation_def_t zfs_dvnodeops_template[];
extern const fs_operation_def_t zfs_fvnodeops_template[];
extern const fs_operation_def_t zfs_xdvnodeops_template[];
extern const fs_operation_def_t zfs_symvnodeops_template[];
extern const fs_operation_def_t zfs_evnodeops_template[];

int
zfs_create_op_tables()
{
	int error;

	/*
	 * zfs_dvnodeops can be set if mod_remove() calls mod_installfs()
	 * due to a failure to remove the the 2nd modlinkage (zfs_modldrv).
	 * In this case we just return as the ops vectors are already set up.
	 */
	if (zfs_dvnodeops)
		return (0);

	error = vn_make_ops(MNTTYPE_ZFS, zfs_dvnodeops_template,
	    &zfs_dvnodeops);
	if (error)
		return (error);

	error = vn_make_ops(MNTTYPE_ZFS, zfs_fvnodeops_template,
	    &zfs_fvnodeops);
	if (error)
		return (error);

	error = vn_make_ops(MNTTYPE_ZFS, zfs_symvnodeops_template,
	    &zfs_symvnodeops);
	if (error)
		return (error);

	error = vn_make_ops(MNTTYPE_ZFS, zfs_xdvnodeops_template,
	    &zfs_xdvnodeops);
	if (error)
		return (error);

	error = vn_make_ops(MNTTYPE_ZFS, zfs_evnodeops_template,
	    &zfs_evnodeops);

	return (error);
}
#endif /*__APPLE__*/

/*
 * zfs_init_fs - Initialize the zfsvfs struct and the file system
 *	incore "master" object.  Verify version compatibility.
 */
int
zfs_init_fs(zfsvfs_t *zfsvfs, znode_t **zpp, cred_t *cr)
{
	extern int zfsfstype;

	objset_t	*os = zfsvfs->z_os;
	int		i, error;
	dmu_object_info_t doi;
#ifndef __APPLE__
	uint64_t fsid_guid;
#endif

	*zpp = NULL;

	/*
	 * XXX - hack to auto-create the pool root filesystem at
	 * the first attempted mount.
	 */
	if (dmu_object_info(os, MASTER_NODE_OBJ, &doi) == ENOENT) {
		dmu_tx_t *tx = dmu_tx_create(os);
		uint64_t zpl_version;

		dmu_tx_hold_zap(tx, DMU_NEW_OBJECT, TRUE, NULL); /* master */
		dmu_tx_hold_zap(tx, DMU_NEW_OBJECT, TRUE, NULL); /* del queue */
		dmu_tx_hold_bonus(tx, DMU_NEW_OBJECT); /* root node */
		error = dmu_tx_assign(tx, TXG_WAIT);
		ASSERT3U(error, ==, 0);
		if (spa_version(dmu_objset_spa(os)) >= SPA_VERSION_FUID)
			zpl_version = ZPL_VERSION;
		else
			zpl_version = ZPL_VERSION_FUID - 1;
		zfs_create_fs(os, cr, zpl_version, 0, tx);
		dmu_tx_commit(tx);
	}

	error = zap_lookup(os, MASTER_NODE_OBJ, ZPL_VERSION_STR, 8, 1,
	    &zfsvfs->z_version);
	if (error) {
		return (error);
	} else if (zfsvfs->z_version > ZPL_VERSION) {
		(void) printf("Mismatched versions:  File system "
		    "is version %lld on-disk format, which is "
		    "incompatible with this software version %lld!",
		    (u_longlong_t)zfsvfs->z_version, ZPL_VERSION);
		return (ENOTSUP);
	}

#ifndef __APPLE__
	/*
	 * The fsid is 64 bits, composed of an 8-bit fs type, which
	 * separates our fsid from any other filesystem types, and a
	 * 56-bit objset unique ID.  The objset unique ID is unique to
	 * all objsets open on this system, provided by unique_create().
	 * The 8-bit fs type must be put in the low bits of fsid[1]
	 * because that's where other Solaris filesystems put it.
	 */
	fsid_guid = dmu_objset_fsid_guid(os);
	ASSERT((fsid_guid & ~((1ULL<<56)-1)) == 0);
	zfsvfs->z_vfs->vfs_fsid.val[0] = fsid_guid;
	zfsvfs->z_vfs->vfs_fsid.val[1] = ((fsid_guid>>32) << 8) |
	    zfsfstype & 0xFF;
#endif /*!__APPLE__*/
	
	error = zap_lookup(os, MASTER_NODE_OBJ, ZFS_ROOT_OBJ, 8, 1,
	    &zfsvfs->z_root);
	if (error)
		return (error);
	ASSERT(zfsvfs->z_root != 0);

	error = zap_lookup(os, MASTER_NODE_OBJ, ZFS_UNLINKED_SET, 8, 1,
	    &zfsvfs->z_unlinkedobj);
	if (error)
		return (error);

	/*
	 * Initialize zget mutex's
	 */
	for (i = 0; i != ZFS_OBJ_MTX_SZ; i++)
		mutex_init(&zfsvfs->z_hold_mtx[i], NULL, MUTEX_DEFAULT, NULL);

	error = zfs_zget(zfsvfs, zfsvfs->z_root, zpp);
	if (error) {
		/*
		 * On error, we destroy the mutexes here since it's not
		 * possible for the caller to determine if the mutexes were
		 * initialized properly.
		 */
		for (i = 0; i != ZFS_OBJ_MTX_SZ; i++)
			mutex_destroy(&zfsvfs->z_hold_mtx[i]);
		return (error);
	}
	ASSERT3U((*zpp)->z_id, ==, zfsvfs->z_root);

#ifndef __APPLE__
	/* OS X not quite ready for FUID mapping */
	error = zap_lookup(os, MASTER_NODE_OBJ, ZFS_FUID_TABLES, 8, 1,
	    &zfsvfs->z_fuid_obj);
	if (error == ENOENT)
		error = 0;
#endif

	return (0);
}

/*
 * define a couple of values we need available
 * for both 64 and 32 bit environments.
 */
#ifndef NBITSMINOR64
#define	NBITSMINOR64	32
#endif
#ifndef MAXMAJ64
#define	MAXMAJ64	0xffffffffUL
#endif
#ifndef	MAXMIN64
#define	MAXMIN64	0xffffffffUL
#endif

/*
 * Create special expldev for ZFS private use.
 * Can't use standard expldev since it doesn't do
 * what we want.  The standard expldev() takes a
 * dev32_t in LP64 and expands it to a long dev_t.
 * We need an interface that takes a dev32_t in ILP32
 * and expands it to a long dev_t.
 */
static uint64_t
zfs_expldev(dev_t dev)
{
#ifndef _LP64
	major_t major = (major_t)dev >> NBITSMINOR32 & MAXMAJ32;
	return (((uint64_t)major << NBITSMINOR64) |
	    ((minor_t)dev & MAXMIN32));
#else
	return (dev);
#endif
}

/*
 * Special cmpldev for ZFS private use.
 * Can't use standard cmpldev since it takes
 * a long dev_t and compresses it to dev32_t in
 * LP64.  We need to do a compaction of a long dev_t
 * to a dev32_t in ILP32.
 */
dev_t
zfs_cmpldev(uint64_t dev)
{
#ifndef _LP64
	minor_t minor = (minor_t)dev & MAXMIN64;
	major_t major = (major_t)(dev >> NBITSMINOR64) & MAXMAJ64;

	if (major > MAXMAJ32 || minor > MAXMIN32)
		return (NODEV32);

	return (((dev32_t)major << NBITSMINOR32) | minor);
#else
	return (dev);
#endif
}

static void
zfs_znode_dmu_init(znode_t *zp, dmu_buf_t *db)
{
	znode_t		*nzp;
#ifndef __APPLE__
	zfsvfs_t	*zfsvfs = zp->z_zfsvfs;
#endif

	ASSERT(MUTEX_HELD(ZFS_OBJ_MUTEX(zp)));

	mutex_enter(&zp->z_lock);

	ASSERT(zp->z_dbuf == NULL);
	zp->z_dbuf = db;
	nzp = dmu_buf_set_user_ie(db, zp, &zp->z_phys, znode_pageout_func);

	/*
	 * there should be no
	 * concurrent zgets on this object.
	 */
	if (nzp != NULL)
		panic("existing znode %p for dbuf %p", nzp, db);

#ifndef __APPLE__
	/*
	 * Slap on VROOT if we are the root znode
	 */
	if (zp->z_id == zfsvfs->z_root)
		ZTOV(zp)->v_flag |= VROOT;
#endif

	mutex_exit(&zp->z_lock);
#ifndef __APPLE__
	vn_exists(ZTOV(zp));
#endif
}

static void
zfs_znode_dmu_fini(znode_t *zp)
{
	dmu_buf_t *db = zp->z_dbuf;
	ASSERT(MUTEX_HELD(ZFS_OBJ_MUTEX(zp)));
	ASSERT(zp->z_dbuf != NULL);
	zp->z_dbuf = NULL;
	dmu_buf_rele(db, NULL);
}

/*
 * Construct a new znode/vnode and intialize.
 *
 * This does not do a call to dmu_set_user() that is
 * up to the caller to do, in case you don't want to
 * return the znode
 */
static znode_t *
zfs_znode_alloc(zfsvfs_t *zfsvfs, dmu_buf_t *db, int blksz)
{
	znode_t	*zp;
#ifndef __APPLE__
	vnode_t *vp;
#endif

	zp = kmem_cache_alloc(znode_cache, KM_SLEEP);

	ASSERT(zp->z_dirlocks == NULL);
	ASSERT(zp->z_dbuf == NULL);

	zp->z_phys = NULL;
	zp->z_zfsvfs = zfsvfs;
	zp->z_unlinked = 0;
	zp->z_atime_dirty = 0;
#ifdef __APPLE__
	zp->z_mmapped = 0;
#else
	zp->z_mapcnt = 0;
#endif
	zp->z_last_itx = 0;
	zp->z_id = db->db_object;
	zp->z_blksz = blksz;
	zp->z_seq = 0x7A4653;
	zp->z_sync_cnt = 0;

#ifdef __APPLE__
	/* OS X - vnode initialization takes place in zfs_attach_vnode() */
	ASSERT((zp->z_vid == 0) && (zp->z_vnode == NULLVP));
#else
	vp = ZTOV(zp);
	vn_reinit(vp);
#endif
	zfs_znode_dmu_init(zp, db);

	zp->z_gen = zp->z_phys->zp_gen;

#ifdef __APPLE__
	/* Note: this zp is not yet in the zfsvfs->z_all_znodes list */
	zp->z_zfsvfs = zfsvfs;
	
#ifdef ZFS_DEBUG
	list_create(&zp->z_stalker, sizeof (findme_t),
				offsetof(findme_t, n_elem));
	znode_stalker(zp, N_znode_alloc);
#endif /* ZFS_DEBUG */
	
#else /* Open Solaris */
	
	vp->v_vfsp = zfsvfs->z_parent->z_vfs;
	vp->v_type = IFTOVT((mode_t)zp->z_phys->zp_mode);

	switch (vp->v_type) {
	case VDIR:
		if (zp->z_phys->zp_flags & ZFS_XATTR) {
			vn_setops(vp, zfs_xdvnodeops);
			vp->v_flag |= V_XATTRDIR;
		} else {
			vn_setops(vp, zfs_dvnodeops);
		}
		zp->z_zn_prefetch = B_TRUE; /* z_prefetch default is enabled */
		break;
	case VBLK:
	case VCHR:
		vp->v_rdev = zfs_cmpldev(zp->z_phys->zp_rdev);
		/*FALLTHROUGH*/
	case VFIFO:
	case VSOCK:
	case VDOOR:
		vn_setops(vp, zfs_fvnodeops);
		break;
	case VREG:
		vp->v_flag |= VMODSORT;
		vn_setops(vp, zfs_fvnodeops);
		break;
	case VLNK:
		vn_setops(vp, zfs_symvnodeops);
		break;
	default:
		vn_setops(vp, zfs_evnodeops);
		break;
	}

#endif /* __APPLE__ */

	/* it can be NULL from zfs_create_fs */
	if (zfsvfs->z_vfs)
		VFS_HOLD(zfsvfs->z_vfs);
	return (zp);
}

#ifdef __APPLE__
/*
 * OS X - Attach a vnode to a znode.
 *
 * No global resources should be held (like ZFS_OBJ_HOLD_ENTER) and
 * the caller should not be inside a transaction.
 */
int
zfs_attach_vnode(znode_t *zp)
{
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	struct vnode_fsparam vfsp;
	int error;
	
	bzero(&vfsp, sizeof (vfsp));
	vfsp.vnfs_str = "zfs";
	vfsp.vnfs_mp = zfsvfs->z_parent->z_vfs;
	vfsp.vnfs_vtype = IFTOVT((mode_t)zp->z_phys->zp_mode);
	vfsp.vnfs_fsnode = zp;
	vfsp.vnfs_flags = VNFS_ADDFSREF;
	
	/*
	 * OS X HACK - workaround missing vnode_setnoflush() KPI...
	 */
	/* Tag system files */
	if ((zp->z_phys->zp_flags & ZFS_XATTR) &&
		(zfsvfs->z_last_unmount_time == 0xBADC0DE) &&
		(zfsvfs->z_last_mtime_synced == zp->z_phys->zp_parent)) {
		vfsp.vnfs_marksystem = 1;
	}
	
	/* Tag root directory */
	if (zp->z_id == zfsvfs->z_root) {
		vfsp.vnfs_markroot = 1;
	}
	
	switch (vfsp.vnfs_vtype) {
        case VDIR:
			if (zp->z_phys->zp_flags & ZFS_XATTR) {
				vfsp.vnfs_vops = zfs_xdvnodeops;
			} else
				vfsp.vnfs_vops = zfs_dvnodeops;
			zp->z_zn_prefetch = B_TRUE; /* z_prefetch default is enabled */
			break;
        case VBLK:
        case VCHR:
			vfsp.vnfs_rdev = zfs_cmpldev(zp->z_phys->zp_rdev);
			/*FALLTHROUGH*/
        case VSOCK:
			vfsp.vnfs_vops = zfs_fvnodeops;
			break;
        case VFIFO:
			vfsp.vnfs_vops = zfs_fifoops;
			break;
        case VREG:
			vfsp.vnfs_vops = zfs_fvnodeops;
			vfsp.vnfs_filesize = zp->z_phys->zp_size;
			break;
        case VLNK:
			vfsp.vnfs_vops = zfs_symvnodeops;
			vfsp.vnfs_filesize = 0;
			break;
        default:
			vfsp.vnfs_vops = zfs_evnodeops;
			break;
	}
	
	/*
	 * Ask VFS for a vnode.  Note that this request can fail.
	 */
	error = vnode_create(VNCREATE_FLAVOR, VCREATESIZE, &vfsp, &zp->z_vnode);
	if (error != 0) {
		printf("zfs_attach_vnode: no more vnodes available!\n");
		zp->z_vnode = NULL;  /* just in case */
		return (error);
	}
	
	vnode_settag(zp->z_vnode, VT_ZFS);
	
	mutex_enter(&zp->z_lock);
	zp->z_vid = vnode_vid(zp->z_vnode);
	/* Wake up any waiters. */
	cv_broadcast(&zp->z_cv);
	mutex_exit(&zp->z_lock);
	
	/* Insert it on our list of active znodes */
	ASSERT(!list_link_active(&zp->z_link_node));
	mutex_enter(&zfsvfs->z_znodes_lock);
	list_insert_tail(&zfsvfs->z_all_znodes, zp);
	membar_producer();
	mutex_exit(&zfsvfs->z_znodes_lock);
	
	return (0);
}

#endif /* __APPLE__ */
			
/*
 * Create a new DMU object to hold a zfs znode.
 *
 *	IN:	dzp	- parent directory for new znode
 *		vap	- file attributes for new znode
 *		tx	- dmu transaction id for zap operations
 *		cr	- credentials of caller
 *		flag	- flags:
 *			  IS_ROOT_NODE	- new object will be root
 *			  IS_XATTR	- new object is an attribute
 *			  IS_REPLAY	- intent log replay
 *		bonuslen - length of bonus buffer
 *		setaclp  - File/Dir initial ACL
 *		fuidp	 - Tracks fuid allocation.
 *
 *	OUT:	zpp	- allocated znode
 *
 * OSX implementation:
 *
 * The caller of zfs_mknode() is expected to call zfs_attach_vnode()
 * AFTER the dmu_tx_commit() is performed.  This prevents deadlocks
 * since vnode_create can indirectly attempt to clean a dirty vnode.
 *
 * The current list of callers includes:
 *	zfs_vnop_create
 *	zfs_vnop_mkdir
 *	zfs_vnop_symlink
 *	zfs_obtain_xattr
 *	zfs_make_xattrdir
 */
void
zfs_mknode(znode_t *dzp, vattr_t *vap, dmu_tx_t *tx, cred_t *cr,
    uint_t flag, znode_t **zpp, int bonuslen, zfs_acl_t *setaclp,
    zfs_fuid_info_t **fuidp)
{
	dmu_buf_t	*db;
	znode_phys_t	*pzp;
	zfsvfs_t	*zfsvfs = dzp->z_zfsvfs;
	timestruc_t	now;
	uint64_t	gen, obj;
	int		err;

	ASSERT(vap && (vap->va_mask & (AT_TYPE|AT_MODE)) == (AT_TYPE|AT_MODE));

	if (zfsvfs->z_assign >= TXG_INITIAL) {		/* ZIL replay */
		obj = vap->va_nodeid;
		flag |= IS_REPLAY;
		now = vap->va_ctime;		/* see zfs_replay_create() */
		gen = vap->va_nblocks;		/* ditto */
	} else {
		obj = 0;
		gethrestime(&now);
		gen = dmu_tx_get_txg(tx);
	}

	/*
	 * Create a new DMU object.
	 */
	/*
	 * There's currently no mechanism for pre-reading the blocks that will
	 * be to needed allocate a new object, so we accept the small chance
	 * that there will be an i/o error and we will fail one of the
	 * assertions below.
	 */
	if (vap->va_type == VDIR) {
		if (flag & IS_REPLAY) {
			err = zap_create_claim_norm(zfsvfs->z_os, obj,
			    zfsvfs->z_norm, DMU_OT_DIRECTORY_CONTENTS,
			    DMU_OT_ZNODE, sizeof (znode_phys_t) + bonuslen, tx);
			ASSERT3U(err, ==, 0);
		} else {
			obj = zap_create_norm(zfsvfs->z_os,
			    zfsvfs->z_norm, DMU_OT_DIRECTORY_CONTENTS,
			    DMU_OT_ZNODE, sizeof (znode_phys_t) + bonuslen, tx);
		}
	} else {
		if (flag & IS_REPLAY) {
			err = dmu_object_claim(zfsvfs->z_os, obj,
			    DMU_OT_PLAIN_FILE_CONTENTS, 0,
			    DMU_OT_ZNODE, sizeof (znode_phys_t) + bonuslen, tx);
			ASSERT3U(err, ==, 0);
		} else {
			obj = dmu_object_alloc(zfsvfs->z_os,
			    DMU_OT_PLAIN_FILE_CONTENTS, 0,
			    DMU_OT_ZNODE, sizeof (znode_phys_t) + bonuslen, tx);
		}
	}
	VERIFY(0 == dmu_bonus_hold(zfsvfs->z_os, obj, NULL, &db));
	dmu_buf_will_dirty(db, tx);

	/*
	 * Initialize the znode physical data to zero.
	 */
	ASSERT(db->db_size >= sizeof (znode_phys_t));
	bzero(db->db_data, db->db_size);
	pzp = db->db_data;

	/*
	 * If this is the root, fix up the half-initialized parent pointer
	 * to reference the just-allocated physical data area.
	 */
	if (flag & IS_ROOT_NODE) {
		dzp->z_phys = pzp;
		dzp->z_id = obj;
	}

	/*
	 * If parent is an xattr, so am I.
	 */
	if (dzp->z_phys->zp_flags & ZFS_XATTR)
		flag |= IS_XATTR;

	if (vap->va_type == VBLK || vap->va_type == VCHR) {
		pzp->zp_rdev = zfs_expldev(vap->va_rdev);
	}

	if (zfsvfs->z_use_fuids)
		pzp->zp_flags = ZFS_ARCHIVE | ZFS_AV_MODIFIED;

	if (vap->va_type == VDIR) {
		pzp->zp_size = 2;		/* contents ("." and "..") */
		pzp->zp_links = (flag & (IS_ROOT_NODE | IS_XATTR)) ? 2 : 1;
	}

	pzp->zp_parent = dzp->z_id;
	if (flag & IS_XATTR)
		pzp->zp_flags |= ZFS_XATTR;

	pzp->zp_gen = gen;

	ZFS_TIME_ENCODE(&now, pzp->zp_crtime);
	ZFS_TIME_ENCODE(&now, pzp->zp_ctime);

	if (vap->va_mask & AT_ATIME) {
		ZFS_TIME_ENCODE(&vap->va_atime, pzp->zp_atime);
	} else {
		ZFS_TIME_ENCODE(&now, pzp->zp_atime);
	}

	if (vap->va_mask & AT_MTIME) {
		ZFS_TIME_ENCODE(&vap->va_mtime, pzp->zp_mtime);
	} else {
		ZFS_TIME_ENCODE(&now, pzp->zp_mtime);
	}

	pzp->zp_mode = MAKEIMODE(vap->va_type, vap->va_mode);
	ZFS_OBJ_HOLD_ENTER(zfsvfs, obj)
	*zpp = zfs_znode_alloc(zfsvfs, db, 0);
	ZFS_OBJ_HOLD_EXIT(zfsvfs, obj);
	zfs_perm_init(*zpp, dzp, flag, vap, tx, cr, setaclp, fuidp);
}

#ifndef __APPLE__
void
zfs_xvattr_set(znode_t *zp, xvattr_t *xvap)
{
	xoptattr_t *xoap;

	xoap = xva_getxoptattr(xvap);
	ASSERT(xoap);

	if (XVA_ISSET_REQ(xvap, XAT_CREATETIME)) {
		ZFS_TIME_ENCODE(&xoap->xoa_createtime, zp->z_phys->zp_crtime);
		XVA_SET_RTN(xvap, XAT_CREATETIME);
	}
	if (XVA_ISSET_REQ(xvap, XAT_READONLY)) {
		ZFS_ATTR_SET(zp, ZFS_READONLY, xoap->xoa_readonly);
		XVA_SET_RTN(xvap, XAT_READONLY);
	}
	if (XVA_ISSET_REQ(xvap, XAT_HIDDEN)) {
		ZFS_ATTR_SET(zp, ZFS_HIDDEN, xoap->xoa_hidden);
		XVA_SET_RTN(xvap, XAT_HIDDEN);
	}
	if (XVA_ISSET_REQ(xvap, XAT_SYSTEM)) {
		ZFS_ATTR_SET(zp, ZFS_SYSTEM, xoap->xoa_system);
		XVA_SET_RTN(xvap, XAT_SYSTEM);
	}
	if (XVA_ISSET_REQ(xvap, XAT_ARCHIVE)) {
		ZFS_ATTR_SET(zp, ZFS_ARCHIVE, xoap->xoa_archive);
		XVA_SET_RTN(xvap, XAT_ARCHIVE);
	}
	if (XVA_ISSET_REQ(xvap, XAT_IMMUTABLE)) {
		ZFS_ATTR_SET(zp, ZFS_IMMUTABLE, xoap->xoa_immutable);
		XVA_SET_RTN(xvap, XAT_IMMUTABLE);
	}
	if (XVA_ISSET_REQ(xvap, XAT_NOUNLINK)) {
		ZFS_ATTR_SET(zp, ZFS_NOUNLINK, xoap->xoa_nounlink);
		XVA_SET_RTN(xvap, XAT_NOUNLINK);
	}
	if (XVA_ISSET_REQ(xvap, XAT_APPENDONLY)) {
		ZFS_ATTR_SET(zp, ZFS_APPENDONLY, xoap->xoa_appendonly);
		XVA_SET_RTN(xvap, XAT_APPENDONLY);
	}
	if (XVA_ISSET_REQ(xvap, XAT_NODUMP)) {
		ZFS_ATTR_SET(zp, ZFS_NODUMP, xoap->xoa_nodump);
		XVA_SET_RTN(xvap, XAT_NODUMP);
	}
	if (XVA_ISSET_REQ(xvap, XAT_OPAQUE)) {
		ZFS_ATTR_SET(zp, ZFS_OPAQUE, xoap->xoa_opaque);
		XVA_SET_RTN(xvap, XAT_OPAQUE);
	}
	if (XVA_ISSET_REQ(xvap, XAT_AV_QUARANTINED)) {
		ZFS_ATTR_SET(zp, ZFS_AV_QUARANTINED,
		    xoap->xoa_av_quarantined);
		XVA_SET_RTN(xvap, XAT_AV_QUARANTINED);
	}
	if (XVA_ISSET_REQ(xvap, XAT_AV_MODIFIED)) {
		ZFS_ATTR_SET(zp, ZFS_AV_MODIFIED, xoap->xoa_av_modified);
		XVA_SET_RTN(xvap, XAT_AV_MODIFIED);
	}
	if (XVA_ISSET_REQ(xvap, XAT_AV_SCANSTAMP)) {
		(void) memcpy(zp->z_phys + 1, xoap->xoa_av_scanstamp,
		    sizeof (xoap->xoa_av_scanstamp));
		zp->z_phys->zp_flags |= ZFS_BONUS_SCANSTAMP;
		XVA_SET_RTN(xvap, XAT_AV_SCANSTAMP);
	}
}
#endif /* !__APPLE__ */

#ifdef __APPLE__
/*
 * Some callers don't require a vnode, so allow them to
 * get a znode without attaching a vnode to it.
 */
int
zfs_zget_sans_vnode(zfsvfs_t *zfsvfs, uint64_t obj_num, znode_t **zpp)
{
	return zfs_zget_internal(zfsvfs, obj_num, zpp, 1);
}
#endif /* __APPLE__ */

/*
 * Get a znode from cache or create one if necessary.
 */
int
zfs_zget(zfsvfs_t *zfsvfs, uint64_t obj_num, znode_t **zpp)
#ifdef __APPLE__
{
	return zfs_zget_internal(zfsvfs, obj_num, zpp, 0);
}

zfs_zget_internal(zfsvfs_t *zfsvfs, uint64_t obj_num, znode_t **zpp, int skip_vnode)
#endif /* __APPLE__ */
{
	dmu_object_info_t doi;
	dmu_buf_t	*db;
	znode_t		*zp;
	int err;

	*zpp = NULL;

#ifdef __APPLE__
again:
#endif

	ZFS_OBJ_HOLD_ENTER(zfsvfs, obj_num);

	err = dmu_bonus_hold(zfsvfs->z_os, obj_num, NULL, &db);
	if (err) {
		ZFS_OBJ_HOLD_EXIT(zfsvfs, obj_num);
		return (err);
	}

	dmu_object_info_from_db(db, &doi);
	if (doi.doi_bonus_type != DMU_OT_ZNODE ||
	    doi.doi_bonus_size < sizeof (znode_phys_t)) {
		dmu_buf_rele(db, NULL);
		ZFS_OBJ_HOLD_EXIT(zfsvfs, obj_num);
		return (EINVAL);
	}

	zp = dmu_buf_get_user(db);
	if (zp != NULL) {
		mutex_enter(&zp->z_lock);

#ifdef __APPLE__
		/* 
		 * Since zp may disappear after we unlock, we save a copy of 
		 * vp and vid before we unlock 
		 */
		uint32_t vid = zp->z_vid;
		vnode_t *vp = ZTOV(zp);
#endif /* __APPLE__ */
		/*
		 * Since we do immediate eviction of the z_dbuf, we
		 * should never find a dbuf with a znode that doesn't
		 * know about the dbuf.
		 */
		ASSERT3P(zp->z_dbuf, ==, db);
		ASSERT3U(zp->z_id, ==, obj_num);

		if (zp->z_unlinked) {
			err = ENOENT;
		} else {
			VN_HOLD(ZTOV(zp));
			*zpp = zp;
			err = 0;
		}
		dmu_buf_rele(db, NULL);
		mutex_exit(&zp->z_lock);
		ZFS_OBJ_HOLD_EXIT(zfsvfs, obj_num);
		return (err);
		// TODO Some changes from 10a286 could be merged in here
	}

	/*
	 * Not found create new znode/vnode
	 */
	zp = zfs_znode_alloc(zfsvfs, db, doi.doi_data_block_size);
	ZFS_OBJ_HOLD_EXIT(zfsvfs, obj_num);
#ifdef __APPLE__
	if (skip_vnode) {
		mutex_enter(&zfsvfs->z_znodes_lock);
		list_insert_tail(&zfsvfs->z_all_znodes, zp);
		mutex_exit(&zfsvfs->z_znodes_lock);
	} else {
		zfs_attach_vnode(zp);
	}
#endif
	*zpp = zp;
	return (0);
}

int
zfs_rezget(znode_t *zp)
{
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	dmu_object_info_t doi;
	dmu_buf_t *db;
	uint64_t obj_num = zp->z_id;
	int err;

	ZFS_OBJ_HOLD_ENTER(zfsvfs, obj_num);

	err = dmu_bonus_hold(zfsvfs->z_os, obj_num, NULL, &db);
	if (err) {
		ZFS_OBJ_HOLD_EXIT(zfsvfs, obj_num);
		return (err);
	}

	dmu_object_info_from_db(db, &doi);
	if (doi.doi_bonus_type != DMU_OT_ZNODE ||
	    doi.doi_bonus_size < sizeof (znode_phys_t)) {
		dmu_buf_rele(db, NULL);
		ZFS_OBJ_HOLD_EXIT(zfsvfs, obj_num);
		return (EINVAL);
	}

	if (((znode_phys_t *)db->db_data)->zp_gen != zp->z_gen) {
		dmu_buf_rele(db, NULL);
		ZFS_OBJ_HOLD_EXIT(zfsvfs, obj_num);
		return (EIO);
	}

	zfs_znode_dmu_init(zp, db);
	zp->z_unlinked = (zp->z_phys->zp_links == 0);

	ZFS_OBJ_HOLD_EXIT(zfsvfs, obj_num);

	return (0);
}

void
zfs_znode_delete(znode_t *zp, dmu_tx_t *tx)
{
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	uint64_t obj = zp->z_id;

#ifdef __APPLE__
#ifdef ZFS_DEBUG
	znode_stalker(zp, N_znode_delete);
#endif
#endif /* __APPLE__ */

	ZFS_OBJ_HOLD_ENTER(zfsvfs, obj);
	if (zp->z_phys->zp_acl.z_acl_extern_obj) {
		VERIFY(0 == dmu_object_free(zfsvfs->z_os,
		    zp->z_phys->zp_acl.z_acl_extern_obj, tx));
	}
	VERIFY(0 == dmu_object_free(zfsvfs->z_os, obj, tx));
	zfs_znode_dmu_fini(zp);
	ZFS_OBJ_HOLD_EXIT(zfsvfs, obj);
}

void
zfs_zinactive(znode_t *zp)
{
#ifndef __APPLE__
	vnode_t	*vp = ZTOV(zp);
#endif
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	uint64_t z_id = zp->z_id;

	ASSERT(zp->z_dbuf && zp->z_phys);

	/*
	 * Don't allow a zfs_zget() while were trying to release this znode
	 */
	ZFS_OBJ_HOLD_ENTER(zfsvfs, z_id);

	mutex_enter(&zp->z_lock);
#ifndef __APPLE__
	mutex_enter(&vp->v_lock);
	vp->v_count--;
	if (vp->v_count > 0 || vn_has_cached_data(vp)) {
		/*
		 * If the hold count is greater than zero, somebody has
		 * obtained a new reference on this znode while we were
		 * processing it here, so we are done.  If we still have
		 * mapped pages then we are also done, since we don't
		 * want to inactivate the znode until the pages get pushed.
		 *
		 * XXX - if vn_has_cached_data(vp) is true, but count == 0,
		 * this seems like it would leave the znode hanging with
		 * no chance to go inactive...
		 */
		mutex_exit(&vp->v_lock);
		mutex_exit(&zp->z_lock);
		ZFS_OBJ_HOLD_EXIT(zfsvfs, z_id);
		return;
	}
	mutex_exit(&vp->v_lock);
#endif /* !__APPLE__ */

	/*
	 * If this was the last reference to a file with no links,
	 * remove the file from the file system.
	 */
	if (zp->z_unlinked) {
		mutex_exit(&zp->z_lock);
		ZFS_OBJ_HOLD_EXIT(zfsvfs, z_id);
		zfs_rmnode(zp);
		return;
	}
	mutex_exit(&zp->z_lock);
	zfs_znode_dmu_fini(zp);
	ZFS_OBJ_HOLD_EXIT(zfsvfs, z_id);
	/* it can be NULL from zfs_create_fs */
	if (zfsvfs->z_vfs)
		VFS_RELE(zfsvfs->z_vfs);
}

void
zfs_znode_free(znode_t *zp)
{
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;

#ifdef __APPLE__
	boolean_t forced = vfs_isforce(zfsvfs->z_vfs);
	
	zp->z_vid = 0;
	zp->z_vnode = NULLVP;
	
	if (!forced)
		mutex_enter(&zfsvfs->z_znodes_lock);
	/* 
	 * Note: the znode isn't inserted into the zfsvfs->z_all_znodes
	 * list until after the vnode is attached so make sure its in
	 * the list before attempting to remove it.
	 */
	if (list_link_active(&zp->z_link_node))
		list_remove(&zfsvfs->z_all_znodes, zp);

	if (!forced)
		mutex_exit(&zfsvfs->z_znodes_lock);

#ifdef ZFS_DEBUG
	znode_stalker_fini(zp);
#endif /* ZFS_DEBUG */
	
#else
	mutex_enter(&zfsvfs->z_znodes_lock);
	list_remove(&zfsvfs->z_all_znodes, zp);
	mutex_exit(&zfsvfs->z_znodes_lock);
	
#endif /* __APPLE__ */

	kmem_cache_free(znode_cache, zp);

#ifdef __APPLE__
	/*
	 * If we're beyond our target footprint, start up a reclaim thread
	 */
	if (zfs_footprint.current > zfs_footprint.target) {
		static struct timeval lastreap = {0, 0};

		struct timeval tv;

		microuptime(&tv);
		if (tv.tv_sec > lastreap.tv_sec + 15) {
			lastreap = tv;
			kmem_reap();
		}
	}
#endif /* __APPLE__ */
}

void
zfs_time_stamper_locked(znode_t *zp, uint_t flag, dmu_tx_t *tx)
{
	timestruc_t	now;

	ASSERT(MUTEX_HELD(&zp->z_lock));

	gethrestime(&now);

	if (tx) {
		dmu_buf_will_dirty(zp->z_dbuf, tx);
		zp->z_atime_dirty = 0;
		zp->z_seq++;
	} else {
		zp->z_atime_dirty = 1;
	}

	if (flag & AT_ATIME)
		ZFS_TIME_ENCODE(&now, zp->z_phys->zp_atime);

	if (flag & AT_MTIME) {
		ZFS_TIME_ENCODE(&now, zp->z_phys->zp_mtime);
		if (zp->z_zfsvfs->z_use_fuids)
			zp->z_phys->zp_flags |= (ZFS_ARCHIVE | ZFS_AV_MODIFIED);
	}

	if (flag & AT_CTIME) {
		ZFS_TIME_ENCODE(&now, zp->z_phys->zp_ctime);
		if (zp->z_zfsvfs->z_use_fuids)
			zp->z_phys->zp_flags |= ZFS_ARCHIVE;
	}
	
#ifdef __APPLE__
	/*
	 * Mac OS X needs a file system modify time
	 *
	 * We use the mtime of the "com.apple.system.mtime" 
	 * extended attribute, which is associated with the
	 * file system root directory.
	 *
	 * We take the znode mutex for this special file last.
	 * No other section of code should ever hold this mutex
	 * and attempt to acquire another znode mutex.
	 */
	if ((flag & AT_MTIME) &&
	    (zp->z_zfsvfs->z_mtime_vp != NULL) &&
	    (VTOZ(zp->z_zfsvfs->z_mtime_vp) != zp)) {
		znode_t *mzp = VTOZ(zp->z_zfsvfs->z_mtime_vp);

		mutex_enter(&mzp->z_lock);
		ZFS_TIME_ENCODE(&now, mzp->z_phys->zp_mtime);
		mutex_exit(&mzp->z_lock);
	}
#endif /* __APPLE__ */
}

/*
 * Update the requested znode timestamps with the current time.
 * If we are in a transaction, then go ahead and mark the znode
 * dirty in the transaction so the timestamps will go to disk.
 * Otherwise, we will get pushed next time the znode is updated
 * in a transaction, or when this znode eventually goes inactive.
 *
 * Why is this OK?
 *  1 - Only the ACCESS time is ever updated outside of a transaction.
 *  2 - Multiple consecutive updates will be collapsed into a single
 *	znode update by the transaction grouping semantics of the DMU.
 */
void
zfs_time_stamper(znode_t *zp, uint_t flag, dmu_tx_t *tx)
{
	mutex_enter(&zp->z_lock);
	zfs_time_stamper_locked(zp, flag, tx);
	mutex_exit(&zp->z_lock);
}

/*
 * Grow the block size for a file.
 *
 *	IN:	zp	- znode of file to free data in.
 *		size	- requested block size
 *		tx	- open transaction.
 *
 * NOTE: this function assumes that the znode is write locked.
 */
void
zfs_grow_blocksize(znode_t *zp, uint64_t size, dmu_tx_t *tx)
{
	int		error;
	u_longlong_t	dummy;

	if (size <= zp->z_blksz)
		return;
	/*
	 * If the file size is already greater than the current blocksize,
	 * we will not grow.  If there is more than one block in a file,
	 * the blocksize cannot change.
	 */
	if (zp->z_blksz && zp->z_phys->zp_size > zp->z_blksz)
		return;

	error = dmu_object_set_blocksize(zp->z_zfsvfs->z_os, zp->z_id,
	    size, 0, tx);
	if (error == ENOTSUP)
		return;
	ASSERT3U(error, ==, 0);

	/* What blocksize did we actually get? */
	dmu_object_size_from_db(zp->z_dbuf, &zp->z_blksz, &dummy);
}

#ifndef __APPLE__
/*
 * This is a dummy interface used when pvn_vplist_dirty() should *not*
 * be calling back into the fs for a putpage().  E.g.: when truncating
 * a file, the pages being "thrown away" don't need to be written out.
 */
/* ARGSUSED */
static int
zfs_no_putpage(vnode_t *vp, page_t *pp, u_offset_t *offp, size_t *lenp,
    int flags, cred_t *cr)
{
	ASSERT(0);
	return (0);
}
#endif /* !__APPLE__ */

/*
 * Free space in a file.
 *
 *	IN:	zp	- znode of file to free data in.
 *		off	- start of section to free.
 *		len	- length of section to free (0 => to EOF).
 *		flag	- current file open mode flags.
 *
 * 	RETURN:	0 if success
 *		error code if failure
 */
int
zfs_freesp(znode_t *zp, uint64_t off, uint64_t len, int flag, boolean_t log)
{
	vnode_t *vp = ZTOV(zp);
	dmu_tx_t *tx;
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	zilog_t *zilog = zfsvfs->z_log;
	rl_t *rl;
	uint64_t end = off + len;
	uint64_t size, new_blksz;
	uint64_t pflags = zp->z_phys->zp_flags;
	int error;

	if ((pflags & (ZFS_IMMUTABLE|ZFS_READONLY)) ||
	    off < zp->z_phys->zp_size && (pflags & ZFS_APPENDONLY))
		return (EPERM);

#ifdef __APPLE__
	if (vnode_isfifo(ZTOV(zp)))
#else
	if (ZTOV(zp)->v_type == VFIFO)
#endif
		return (0);

	/*
	 * If we will change zp_size then lock the whole file,
	 * otherwise just lock the range being freed.
	 */
	if (len == 0 || off + len > zp->z_phys->zp_size) {
		rl = zfs_range_lock(zp, 0, UINT64_MAX, RL_WRITER);
	} else {
		rl = zfs_range_lock(zp, off, len, RL_WRITER);
		/* recheck, in case zp_size changed */
		if (off + len > zp->z_phys->zp_size) {
			/* lost race: file size changed, lock whole file */
			zfs_range_unlock(rl);
			rl = zfs_range_lock(zp, 0, UINT64_MAX, RL_WRITER);
		}
	}

	/*
	 * Nothing to do if file already at desired length.
	 */
	size = zp->z_phys->zp_size;
	if (len == 0 && size == off && off != 0) {
		zfs_range_unlock(rl);
		return (0);
	}

	/*
	 * Check for any locks in the region to be freed.
	 */
	if (MANDLOCK(vp, (mode_t)zp->z_phys->zp_mode)) {
		uint64_t start = off;
		uint64_t extent = len;

		if (off > size) {
			start = size;
			extent += off - size;
		} else if (len == 0) {
			extent = size - off;
		}
		if (error = chklock(vp, FWRITE, start, extent, flag, NULL)) {
			zfs_range_unlock(rl);
			return (error);
		}
	}

	tx = dmu_tx_create(zfsvfs->z_os);
	dmu_tx_hold_bonus(tx, zp->z_id);
	new_blksz = 0;
	if (end > size &&
	    (!ISP2(zp->z_blksz) || zp->z_blksz < zfsvfs->z_max_blksz)) {
		/*
		 * We are growing the file past the current block size.
		 */
		if (zp->z_blksz > zp->z_zfsvfs->z_max_blksz) {
			ASSERT(!ISP2(zp->z_blksz));
			new_blksz = MIN(end, SPA_MAXBLOCKSIZE);
		} else {
			new_blksz = MIN(end, zp->z_zfsvfs->z_max_blksz);
		}
		dmu_tx_hold_write(tx, zp->z_id, 0, MIN(end, new_blksz));
	} else if (off < size) {
		/*
		 * If len == 0, we are truncating the file.
		 */
		dmu_tx_hold_free(tx, zp->z_id, off, len ? len : DMU_OBJECT_END);
	}

	error = dmu_tx_assign(tx, zfsvfs->z_assign);
	if (error) {
		if (error == ERESTART && zfsvfs->z_assign == TXG_NOWAIT)
			dmu_tx_wait(tx);
		dmu_tx_abort(tx);
		zfs_range_unlock(rl);
		return (error);
	}

	if (new_blksz)
		zfs_grow_blocksize(zp, new_blksz, tx);

	if (end > size || len == 0)
		zp->z_phys->zp_size = end;

	if (off < size) {
		objset_t *os = zfsvfs->z_os;
		uint64_t rlen = len;

		if (len == 0)
			rlen = -1;
		else if (end > size)
			rlen = size - off;
		VERIFY(0 == dmu_free_range(os, zp->z_id, off, rlen, tx));
	}

	if (log) {
		zfs_time_stamper(zp, CONTENT_MODIFIED, tx);
		zfs_log_truncate(zilog, tx, TX_TRUNCATE, zp, off, len);
	}

	zfs_range_unlock(rl);

	dmu_tx_commit(tx);

	/*
	 * Clear any mapped pages in the truncated region.  This has to
	 * happen outside of the transaction to avoid the possibility of
	 * a deadlock with someone trying to push a page that we are
	 * about to invalidate.
	 */
/* ### APPLE ZFS TODO ### */
#ifndef __APPLE__
	rw_enter(&zp->z_map_lock, RW_WRITER);
	if (off < size && vn_has_cached_data(vp)) {
		page_t *pp;
		uint64_t start = off & PAGEMASK;
		int poff = off & PAGEOFFSET;

		if (poff != 0 && (pp = page_lookup(vp, start, SE_SHARED))) {
			/*
			 * We need to zero a partial page.
			 */
			pagezero(pp, poff, PAGESIZE - poff);
			start += PAGESIZE;
			page_unlock(pp);
		}
		error = pvn_vplist_dirty(vp, start, zfs_no_putpage,
		    B_INVAL | B_TRUNC, NULL);
		ASSERT(error == 0);
	}
	rw_exit(&zp->z_map_lock);
#endif

	return (0);
}

void
zfs_create_fs(objset_t *os, cred_t *cr, uint64_t version,
    int norm, dmu_tx_t *tx)
{
	zfsvfs_t	zfsvfs;
	uint64_t	moid, doid;
	int		error;
	znode_t		*rootzp = NULL;
	vnode_t		*vp;
	vattr_t		vattr;
	znode_t		*zp;

	/*
	 * First attempt to create master node.
	 */
	/*
	 * In an empty objset, there are no blocks to read and thus
	 * there can be no i/o errors (which we assert below).
	 */
	moid = MASTER_NODE_OBJ;
	error = zap_create_claim(os, moid, DMU_OT_MASTER_NODE,
	    DMU_OT_NONE, 0, tx);
	ASSERT(error == 0);

	/*
	 * Set starting attributes.
	 */

	error = zap_update(os, moid, ZPL_VERSION_STR, 8, 1, &version, tx);
	ASSERT(error == 0);

	/*
	 * Create a delete queue.
	 */
	doid = zap_create(os, DMU_OT_UNLINKED_SET, DMU_OT_NONE, 0, tx);

	error = zap_add(os, moid, ZFS_UNLINKED_SET, 8, 1, &doid, tx);
	ASSERT(error == 0);

	/*
	 * Create root znode.  Create minimal znode/vnode/zfsvfs
	 * to allow zfs_mknode to work.
	 */
	vattr.va_mask = AT_MODE|AT_UID|AT_GID|AT_TYPE;
	vattr.va_type = VDIR;
	vattr.va_mode = S_IFDIR|0755;
	vattr.va_uid = crgetuid(cr);
	vattr.va_gid = crgetgid(cr);

	rootzp = kmem_cache_alloc(znode_cache, KM_SLEEP);
	rootzp->z_zfsvfs = &zfsvfs;
	rootzp->z_unlinked = 0;
	rootzp->z_atime_dirty = 0;

#ifndef __APPLE__
	vp = ZTOV(rootzp);
	vn_reinit(vp);
	vp->v_type = VDIR;
#endif

	bzero(&zfsvfs, sizeof (zfsvfs_t));

	zfsvfs.z_os = os;
	zfsvfs.z_assign = TXG_NOWAIT;
	zfsvfs.z_parent = &zfsvfs;
	zfsvfs.z_version = version;
	zfsvfs.z_use_fuids = USE_FUIDS(version, os);
	zfsvfs.z_norm = norm;

	mutex_init(&zfsvfs.z_znodes_lock, NULL, MUTEX_DEFAULT, NULL);
	list_create(&zfsvfs.z_all_znodes, sizeof (znode_t),
	    offsetof(znode_t, z_link_node));

	zfs_mknode(rootzp, &vattr, tx, cr, IS_ROOT_NODE,
	    &zp, 0, NULL, NULL);
	VN_RELE(ZTOV(zp));
	error = zap_add(os, moid, ZFS_ROOT_OBJ, 8, 1, &rootzp->z_id, tx);
	ASSERT(error == 0);

#ifndef __APPLE__
	ZTOV(rootzp)->v_count = 0;
#endif
	kmem_cache_free(znode_cache, rootzp);
}

#endif /* _KERNEL */
/*
 * Given an object number, return its parent object number and whether
 * or not the object is an extended attribute directory.
 */
static int
zfs_obj_to_pobj(objset_t *osp, uint64_t obj, uint64_t *pobjp, int *is_xattrdir)
{
	dmu_buf_t *db;
	dmu_object_info_t doi;
	znode_phys_t *zp;
	int error;

	if ((error = dmu_bonus_hold(osp, obj, FTAG, &db)) != 0)
		return (error);

	dmu_object_info_from_db(db, &doi);
	if (doi.doi_bonus_type != DMU_OT_ZNODE ||
	    doi.doi_bonus_size < sizeof (znode_phys_t)) {
		dmu_buf_rele(db, FTAG);
		return (EINVAL);
	}

	zp = db->db_data;
	*pobjp = zp->zp_parent;
	*is_xattrdir = ((zp->zp_flags & ZFS_XATTR) != 0) &&
	    S_ISDIR(zp->zp_mode);
	dmu_buf_rele(db, FTAG);

	return (0);
}

int
zfs_obj_to_path(objset_t *osp, uint64_t obj, char *buf, int len)
{
	char *path = buf + len - 1;
	int error;

	*path = '\0';

	for (;;) {
		uint64_t pobj;
		char component[MAXNAMELEN + 2];
		size_t complen;
		int is_xattrdir;

		if ((error = zfs_obj_to_pobj(osp, obj, &pobj,
		    &is_xattrdir)) != 0)
			break;

		if (pobj == obj) {
			if (path[0] != '/')
				*--path = '/';
			break;
		}

		component[0] = '/';
		if (is_xattrdir) {
			(void) sprintf(component + 1, "<xattrdir>");
		} else {
			error = zap_value_search(osp, pobj, obj,
			    ZFS_DIRENT_OBJ(-1ULL), component + 1);
			if (error != 0)
				break;
		}

		complen = strlen(component);
		path -= complen;
		ASSERT(path >= buf);
		bcopy(component, path, complen);
		obj = pobj;
	}

	if (error == 0)
		(void) memmove(buf, path, buf + len - path);
	return (error);
}

#ifdef __APPLE__
uint32_t
zfs_getbsdflags(znode_t *zp)
{
	uint64_t  zflags = zp->z_phys->zp_flags;
	uint32_t  bsdflags = 0;

	if (zflags & ZFS_NODUMP)
		bsdflags |= UF_NODUMP;
	if (zflags & ZFS_IMMUTABLE)
		bsdflags |= UF_IMMUTABLE;
	if (zflags & ZFS_APPENDONLY)
		bsdflags |= UF_APPEND;
	if (zflags & ZFS_OPAQUE)
		bsdflags |= UF_OPAQUE;
	if (zflags & ZFS_HIDDEN)
		bsdflags |= UF_HIDDEN;
	if (zflags & ZFS_ARCHIVE)
		bsdflags |= SF_ARCHIVED;

	return (bsdflags);
}

void
zfs_setbsdflags(znode_t *zp, uint32_t bsdflags)
{
	uint64_t  zflags = zp->z_phys->zp_flags;

	if (bsdflags & UF_NODUMP)
		zflags |= ZFS_NODUMP;
	else
		zflags &= ~ZFS_NODUMP;

	if (bsdflags & UF_IMMUTABLE)
		zflags |= ZFS_IMMUTABLE;
	else
		zflags &= ~ZFS_IMMUTABLE;

	if (bsdflags & UF_APPEND)
		zflags |= ZFS_APPENDONLY;
	else
		zflags &= ~ZFS_APPENDONLY;

	if (bsdflags & UF_OPAQUE)
		zflags |= ZFS_OPAQUE;
	else
		zflags &= ~ZFS_OPAQUE;

	if (bsdflags & UF_HIDDEN)
		zflags |= ZFS_HIDDEN;
	else
		zflags &= ~ZFS_HIDDEN;

	if (bsdflags & SF_ARCHIVED)
		zflags |= ZFS_ARCHIVE;
	else
		zflags &= ~ZFS_ARCHIVE;

	zp->z_phys->zp_flags = zflags;
}

#ifdef _KERNEL
#ifdef ZFS_DEBUG
char *
n_event_to_str(whereami_t event); // the prototype that removes gcc warning
char *
n_event_to_str(whereami_t event)
{
        switch (event) {
        case N_znode_alloc:
                return("N_znode_alloc");
        case N_vnop_inactive:
                return("N_vnop_inactive");
        case N_zinactive:
                return("N_zinactive");
        case N_vnop_reclaim:
                return("N_vnop_reclaim");
        case N_znode_delete:
                return("N_znode_delete");
        case N_znode_pageout:
                return("N_znode_pageout");
        case N_zfs_nolink_add:
                return("N_zfs_nolink_add");
        case N_mknode_err:
                return("N_mknode_err");
        case N_zinact_retearly:
                return("N_zinact_retearly");
        case N_zfs_rmnode:
                return("N_zfs_rmnode");
        case N_vnop_fsync_zil:
                return("N_vnop_fsync_zil");
        default:
                return("don't know");
        }
}

void 
znode_stalker(znode_t *zp, whereami_t event)
{
	findme_t *n;
	if( k_maczfs_debug_stalk ) {
		n = kmem_alloc(sizeof (findme_t), KM_SLEEP);
		n->event = event;
		mutex_enter(&zp->z_lock);
		list_insert_tail(&zp->z_stalker, n);
		mutex_exit(&zp->z_lock);
		printf("stalk: zp %p %s\n", zp, n_event_to_str(event));
	}
}

void 
znode_stalker_fini(znode_t *zp)
{
	findme_t *n;

	while (n = list_head(&zp->z_stalker)) {
                list_remove(&zp->z_stalker, n);
                kmem_free(n, sizeof(findme_t));
        }
	list_destroy(&zp->z_stalker);
}
#endif /* ZFS_DEBUG */
#endif /* _KERNEL */
#endif /* __APPLE__ */
