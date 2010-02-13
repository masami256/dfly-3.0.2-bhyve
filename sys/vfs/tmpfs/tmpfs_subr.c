/*	$NetBSD: tmpfs_subr.c,v 1.35 2007/07/09 21:10:50 ad Exp $	*/

/*-
 * Copyright (c) 2005 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Julio M. Merino Vidal, developed as part of Google's Summer of Code
 * 2005 program.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Efficient memory file system supporting functions.
 */
#include <sys/cdefs.h>

#include <sys/kernel.h>
#include <sys/param.h>
#include <sys/namei.h>
#include <sys/priv.h>
#include <sys/proc.h>
#include <sys/spinlock2.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/vmmeter.h>

#include <sys/mplock2.h>

#include <vm/vm.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pager.h>
#include <vm/vm_extern.h>

#include <vfs/tmpfs/tmpfs.h>
#include <vfs/tmpfs/tmpfs_fifoops.h>
#include <vfs/tmpfs/tmpfs_vnops.h>

static ino_t t_ino = 2;
static struct spinlock	ino_lock;
static ino_t tmpfs_fetch_ino(void);

/* --------------------------------------------------------------------- */

/*
 * Allocates a new node of type 'type' inside the 'tmp' mount point, with
 * its owner set to 'uid', its group to 'gid' and its mode set to 'mode',
 * using the credentials of the process 'p'.
 *
 * If the node type is set to 'VDIR', then the parent parameter must point
 * to the parent directory of the node being created.  It may only be NULL
 * while allocating the root node.
 *
 * If the node type is set to 'VBLK' or 'VCHR', then the rdev parameter
 * specifies the device the node represents.
 *
 * If the node type is set to 'VLNK', then the parameter target specifies
 * the file name of the target file for the symbolic link that is being
 * created.
 *
 * Note that new nodes are retrieved from the available list if it has
 * items or, if it is empty, from the node pool as long as there is enough
 * space to create them.
 *
 * Returns zero on success or an appropriate error code on failure.
 */
int
tmpfs_alloc_node(struct tmpfs_mount *tmp, enum vtype type,
    uid_t uid, gid_t gid, mode_t mode, struct tmpfs_node *parent,
    char *target, int rmajor, int rminor, struct tmpfs_node **node)
{
	struct tmpfs_node *nnode;
	struct timespec ts;
	udev_t rdev;

	/* If the root directory of the 'tmp' file system is not yet
	 * allocated, this must be the request to do it. */
	KKASSERT(IMPLIES(tmp->tm_root == NULL, parent == NULL && type == VDIR));

	KKASSERT(IFF(type == VLNK, target != NULL));
	KKASSERT(IFF(type == VBLK || type == VCHR, rmajor != VNOVAL));

	if (tmp->tm_nodes_inuse > tmp->tm_nodes_max)
		return (ENOSPC);

	nnode = (struct tmpfs_node *)objcache_get(tmp->tm_node_pool, M_WAITOK);

	/* Generic initialization. */
	nnode->tn_type = type;
	vfs_timestamp(&ts);
	nnode->tn_ctime = nnode->tn_mtime = nnode->tn_atime
		= ts.tv_sec;
	nnode->tn_ctimensec = nnode->tn_mtimensec = nnode->tn_atimensec
		= ts.tv_nsec;
	nnode->tn_uid = uid;
	nnode->tn_gid = gid;
	nnode->tn_mode = mode;
	nnode->tn_id = tmpfs_fetch_ino();

	/* Type-specific initialization. */
	switch (nnode->tn_type) {
	case VBLK:
	case VCHR:
		rdev = makeudev(rmajor, rminor);
		if (rdev == NOUDEV) {
			return(EINVAL);
		}
		nnode->tn_rdev = rdev;
		break;

	case VDIR:
		TAILQ_INIT(&nnode->tn_dir.tn_dirhead);
		KKASSERT(parent != nnode);
		KKASSERT(IMPLIES(parent == NULL, tmp->tm_root == NULL));
		nnode->tn_dir.tn_parent = parent;
		nnode->tn_dir.tn_readdir_lastn = 0;
		nnode->tn_dir.tn_readdir_lastp = NULL;
		nnode->tn_links++;
		nnode->tn_size = 0;
		if (parent) {
			TMPFS_NODE_LOCK(parent);
			parent->tn_links++;
			TMPFS_NODE_UNLOCK(parent);
		}
		break;

	case VFIFO:
		/* FALLTHROUGH */
	case VSOCK:
		break;

	case VLNK:
		KKASSERT((strlen(target) +1) < MAXPATHLEN);
		nnode->tn_size = strlen(target);
		nnode->tn_link = kmalloc(nnode->tn_size + 1, M_TMPFSNAME,
		    M_WAITOK);
		bcopy(target, nnode->tn_link, nnode->tn_size);
		nnode->tn_link[nnode->tn_size] = '\0';
		break;

	case VREG:
		nnode->tn_reg.tn_aobj =
		    vm_pager_allocate(OBJT_SWAP, NULL, 0, VM_PROT_DEFAULT, 0);
		nnode->tn_reg.tn_aobj_pages = 0;
		nnode->tn_size = 0;
		break;

	default:
		panic("tmpfs_alloc_node: type %p %d", nnode, (int)nnode->tn_type);
	}

	TMPFS_NODE_LOCK(nnode);
	TMPFS_LOCK(tmp);
	LIST_INSERT_HEAD(&tmp->tm_nodes_used, nnode, tn_entries);
	tmp->tm_nodes_inuse++;
	TMPFS_UNLOCK(tmp);
	TMPFS_NODE_UNLOCK(nnode);

	*node = nnode;
	return 0;
}

/* --------------------------------------------------------------------- */

/*
 * Destroys the node pointed to by node from the file system 'tmp'.
 * If the node does not belong to the given mount point, the results are
 * unpredicted.
 *
 * If the node references a directory; no entries are allowed because
 * their removal could need a recursive algorithm, something forbidden in
 * kernel space.  Furthermore, there is not need to provide such
 * functionality (recursive removal) because the only primitives offered
 * to the user are the removal of empty directories and the deletion of
 * individual files.
 *
 * Note that nodes are not really deleted; in fact, when a node has been
 * allocated, it cannot be deleted during the whole life of the file
 * system.  Instead, they are moved to the available list and remain there
 * until reused.
 */
void
tmpfs_free_node(struct tmpfs_mount *tmp, struct tmpfs_node *node)
{
	size_t pages = 0;

#ifdef INVARIANTS
	TMPFS_ASSERT_ELOCKED(node);
	KKASSERT(node->tn_vnode == NULL);
	KKASSERT((node->tn_vpstate & TMPFS_VNODE_ALLOCATING) == 0);
#endif

	TMPFS_LOCK(tmp);
	LIST_REMOVE(node, tn_entries);
	tmp->tm_nodes_inuse--;
	TMPFS_UNLOCK(tmp);
	TMPFS_NODE_UNLOCK(node);

	switch (node->tn_type) {
	case VNON:
		/* Do not do anything.  VNON is provided to let the
		 * allocation routine clean itself easily by avoiding
		 * duplicating code in it. */
		/* FALLTHROUGH */
	case VBLK:
		/* FALLTHROUGH */
	case VCHR:
		/* FALLTHROUGH */
		break;
	case VDIR:
		/*
		 * The parent link can be NULL if this is the root
		 * node.
		 */
		node->tn_links--;
		node->tn_size = 0;
		KKASSERT(node->tn_dir.tn_parent || node == tmp->tm_root);
		if (node->tn_dir.tn_parent) {
			TMPFS_NODE_LOCK(node->tn_dir.tn_parent);
			node->tn_dir.tn_parent->tn_links--;

			/*
			 * If the parent directory has no more links and
			 * no vnode ref nothing is going to come along
			 * and clean it up unless we do it here.
			 */
			if (node->tn_dir.tn_parent->tn_links == 0 &&
			    node->tn_dir.tn_parent->tn_vnode == NULL) {
				tmpfs_free_node(tmp, node->tn_dir.tn_parent);
				/* eats parent lock */
			} else {
				TMPFS_NODE_UNLOCK(node->tn_dir.tn_parent);
			}
			node->tn_dir.tn_parent = NULL;
		}

		/*
		 * If the root node is being destroyed don't leave a
		 * dangling pointer in tmpfs_mount.
		 */
		if (node == tmp->tm_root)
			tmp->tm_root = NULL;
		break;
	case VFIFO:
		/* FALLTHROUGH */
	case VSOCK:
		break;

	case VLNK:
		kfree(node->tn_link, M_TMPFSNAME);
		node->tn_link = NULL;
		break;

	case VREG:
		if (node->tn_reg.tn_aobj != NULL)
			vm_pager_deallocate(node->tn_reg.tn_aobj);
		node->tn_reg.tn_aobj = NULL;
		pages = node->tn_reg.tn_aobj_pages;
		break;

	default:
		panic("tmpfs_free_node: type %p %d", node, (int)node->tn_type);
	}

	/*
	 * Clean up fields for the next allocation.  The objcache only ctors
	 * new allocations.
	 */
	tmpfs_node_ctor(node, NULL, 0);
	objcache_put(tmp->tm_node_pool, node);
	/* node is now invalid */

	TMPFS_LOCK(tmp);
	tmp->tm_pages_used -= pages;
	TMPFS_UNLOCK(tmp);
}

/* --------------------------------------------------------------------- */

/*
 * Allocates a new directory entry for the node node with a name of name.
 * The new directory entry is returned in *de.
 *
 * The link count of node is increased by one to reflect the new object
 * referencing it.
 *
 * Returns zero on success or an appropriate error code on failure.
 */
int
tmpfs_alloc_dirent(struct tmpfs_mount *tmp, struct tmpfs_node *node,
    const char *name, uint16_t len, struct tmpfs_dirent **de)
{
	struct tmpfs_dirent *nde;


	nde = (struct tmpfs_dirent *)objcache_get(tmp->tm_dirent_pool, M_WAITOK);
	nde->td_name = kmalloc(len + 1, M_TMPFSNAME, M_WAITOK);
	nde->td_namelen = len;
	bcopy(name, nde->td_name, len);
	nde->td_name[len] = '\0';

	nde->td_node = node;

	TMPFS_NODE_LOCK(node);
	node->tn_links++;
	TMPFS_NODE_UNLOCK(node);

	*de = nde;

	return 0;
}

/* --------------------------------------------------------------------- */

/*
 * Frees a directory entry.  It is the caller's responsibility to destroy
 * the node referenced by it if needed.
 *
 * The link count of node is decreased by one to reflect the removal of an
 * object that referenced it.  This only happens if 'node_exists' is true;
 * otherwise the function will not access the node referred to by the
 * directory entry, as it may already have been released from the outside.
 */
void
tmpfs_free_dirent(struct tmpfs_mount *tmp, struct tmpfs_dirent *de)
{
	struct tmpfs_node *node;

	node = de->td_node;

	TMPFS_NODE_LOCK(node);
	TMPFS_ASSERT_ELOCKED(node);
	KKASSERT(node->tn_links > 0);
	node->tn_links--;
	TMPFS_NODE_UNLOCK(node);

	kfree(de->td_name, M_TMPFSNAME);
	de->td_namelen = 0;
	de->td_name = NULL;
	de->td_node = NULL;
	objcache_put(tmp->tm_dirent_pool, de);
}

/* --------------------------------------------------------------------- */

/*
 * Allocates a new vnode for the node node or returns a new reference to
 * an existing one if the node had already a vnode referencing it.  The
 * resulting locked vnode is returned in *vpp.
 *
 * Returns zero on success or an appropriate error code on failure.
 */
int
tmpfs_alloc_vp(struct mount *mp, struct tmpfs_node *node, int lkflag,
    struct vnode **vpp)
{
	int error = 0;
	struct vnode *vp;

loop:
	TMPFS_NODE_LOCK(node);
	if ((vp = node->tn_vnode) != NULL) {
		KKASSERT((node->tn_vpstate & TMPFS_VNODE_DOOMED) == 0);
		TMPFS_NODE_UNLOCK(node);
		(void) vget(vp, lkflag | LK_EXCLUSIVE | LK_RETRY);

		/*
		 * Make sure the vnode is still there after
		 * getting the interlock to avoid racing a free.
		 */
		if (node->tn_vnode == NULL || node->tn_vnode != vp) {
			vput(vp);
			goto loop;
		}

		goto out;
	}

	/*
	 * This should never happen.
	 */
	if (node->tn_vpstate & TMPFS_VNODE_DOOMED) {
		TMPFS_NODE_UNLOCK(node);
		error = ENOENT;
		vp = NULL;
		goto out;
	}

	/*
	 * otherwise lock the vp list while we call getnewvnode
	 * since that can block.
	 */
	if (node->tn_vpstate & TMPFS_VNODE_ALLOCATING) {
		node->tn_vpstate |= TMPFS_VNODE_WANT;
		error = tsleep((caddr_t) &node->tn_vpstate,
		    PINTERLOCKED | PCATCH, "tmpfs_alloc_vp", 0);
		TMPFS_NODE_UNLOCK(node);
		if (error)
			return error;

		goto loop;
	} else
		node->tn_vpstate |= TMPFS_VNODE_ALLOCATING;

	TMPFS_NODE_UNLOCK(node);

	/* Get a new vnode and associate it with our node. */
	error = getnewvnode(VT_TMPFS, mp, &vp, VLKTIMEOUT, LK_CANRECURSE);
	if (error != 0)
		goto unlock;
	KKASSERT(vp != NULL);

	vp->v_data = node;
	vp->v_type = node->tn_type;

	/* Type-specific initialization. */
	switch (node->tn_type) {
	case VBLK:
		/* FALLTHROUGH */
	case VCHR:
		/* FALLTHROUGH */
	case VSOCK:
		break;
	case VREG:
		vinitvmio(vp, (node->tn_size + BMASK) & ~(off_t)BMASK);
		break;
	case VLNK:
		break;
	case VFIFO:
		vp->v_ops = &mp->mnt_vn_fifo_ops;
		break;
	case VDIR:
		break;

	default:
		panic("tmpfs_alloc_vp: type %p %d", node, (int)node->tn_type);
	}

	insmntque(vp, mp);

unlock:
	TMPFS_NODE_LOCK(node);

	KKASSERT(node->tn_vpstate & TMPFS_VNODE_ALLOCATING);
	node->tn_vpstate &= ~TMPFS_VNODE_ALLOCATING;
	node->tn_vnode = vp;

	if (node->tn_vpstate & TMPFS_VNODE_WANT) {
		node->tn_vpstate &= ~TMPFS_VNODE_WANT;
		TMPFS_NODE_UNLOCK(node);
		wakeup((caddr_t) &node->tn_vpstate);
	} else
		TMPFS_NODE_UNLOCK(node);

out:
	*vpp = vp;

	KKASSERT(IFF(error == 0, *vpp != NULL && vn_islocked(*vpp)));
#ifdef INVARIANTS
	TMPFS_NODE_LOCK(node);
	KKASSERT(*vpp == node->tn_vnode);
	TMPFS_NODE_UNLOCK(node);
#endif

	return error;
}

/* --------------------------------------------------------------------- */

/*
 * Destroys the association between the vnode vp and the node it
 * references.
 */
void
tmpfs_free_vp(struct vnode *vp)
{
	struct tmpfs_node *node;

	node = VP_TO_TMPFS_NODE(vp);

	TMPFS_NODE_LOCK(node);
	KKASSERT(lockcount(TMPFS_NODE_MTX(node)) > 0);
	node->tn_vnode = NULL;
	TMPFS_NODE_UNLOCK(node);
	vp->v_data = NULL;
}

/* --------------------------------------------------------------------- */

/*
 * Allocates a new file of type 'type' and adds it to the parent directory
 * 'dvp'; this addition is done using the component name given in 'cnp'.
 * The ownership of the new file is automatically assigned based on the
 * credentials of the caller (through 'cnp'), the group is set based on
 * the parent directory and the mode is determined from the 'vap' argument.
 * If successful, *vpp holds a vnode to the newly created file and zero
 * is returned.  Otherwise *vpp is NULL and the function returns an
 * appropriate error code.
 */
int
tmpfs_alloc_file(struct vnode *dvp, struct vnode **vpp, struct vattr *vap,
    struct namecache *ncp, struct ucred *cred, char *target)
{
	int error;
	struct tmpfs_dirent *de;
	struct tmpfs_mount *tmp;
	struct tmpfs_node *dnode;
	struct tmpfs_node *node;
	struct tmpfs_node *parent;

	tmp = VFS_TO_TMPFS(dvp->v_mount);
	dnode = VP_TO_TMPFS_DIR(dvp);
	*vpp = NULL;

	/* If the entry we are creating is a directory, we cannot overflow
	 * the number of links of its parent, because it will get a new
	 * link. */
	if (vap->va_type == VDIR) {
		/* Ensure that we do not overflow the maximum number of links
		 * imposed by the system. */
		KKASSERT(dnode->tn_links <= LINK_MAX);
		if (dnode->tn_links == LINK_MAX) {
			return EMLINK;
		}

		parent = dnode;
		KKASSERT(parent != NULL);
	} else
		parent = NULL;

	/* Allocate a node that represents the new file. */
	error = tmpfs_alloc_node(tmp, vap->va_type, cred->cr_uid,
	    dnode->tn_gid, vap->va_mode, parent, target, vap->va_rmajor, vap->va_rminor, &node);
	if (error != 0)
		return error;
	TMPFS_NODE_LOCK(node);

	/* Allocate a directory entry that points to the new file. */
	error = tmpfs_alloc_dirent(tmp, node, ncp->nc_name, ncp->nc_nlen, &de);
	if (error != 0) {
		tmpfs_free_node(tmp, node);
		/* eats node lock */
		return error;
	}

	/* Allocate a vnode for the new file. */
	error = tmpfs_alloc_vp(dvp->v_mount, node, LK_EXCLUSIVE, vpp);
	if (error != 0) {
		tmpfs_free_dirent(tmp, de);
		tmpfs_free_node(tmp, node);
		/* eats node lock */
		return error;
	}

	/* Now that all required items are allocated, we can proceed to
	 * insert the new node into the directory, an operation that
	 * cannot fail. */
	tmpfs_dir_attach(dnode, de);
	TMPFS_NODE_UNLOCK(node);

	return error;
}

/* --------------------------------------------------------------------- */

/*
 * Attaches the directory entry de to the directory represented by vp.
 * Note that this does not change the link count of the node pointed by
 * the directory entry, as this is done by tmpfs_alloc_dirent.
 */
void
tmpfs_dir_attach(struct tmpfs_node *dnode, struct tmpfs_dirent *de)
{
	TMPFS_NODE_LOCK(dnode);
	TAILQ_INSERT_TAIL(&dnode->tn_dir.tn_dirhead, de, td_entries);

	TMPFS_ASSERT_ELOCKED(dnode);
	dnode->tn_size += sizeof(struct tmpfs_dirent);
	dnode->tn_status |= TMPFS_NODE_ACCESSED | TMPFS_NODE_CHANGED |
			    TMPFS_NODE_MODIFIED;
	TMPFS_NODE_UNLOCK(dnode);
}

/* --------------------------------------------------------------------- */

/*
 * Detaches the directory entry de from the directory represented by vp.
 * Note that this does not change the link count of the node pointed by
 * the directory entry, as this is done by tmpfs_free_dirent.
 */
void
tmpfs_dir_detach(struct tmpfs_node *dnode, struct tmpfs_dirent *de)
{
	TMPFS_NODE_LOCK(dnode);
	if (dnode->tn_dir.tn_readdir_lastp == de) {
		dnode->tn_dir.tn_readdir_lastn = 0;
		dnode->tn_dir.tn_readdir_lastp = NULL;
	}
	TAILQ_REMOVE(&dnode->tn_dir.tn_dirhead, de, td_entries);

	TMPFS_ASSERT_ELOCKED(dnode);
	dnode->tn_size -= sizeof(struct tmpfs_dirent);
	dnode->tn_status |= TMPFS_NODE_ACCESSED | TMPFS_NODE_CHANGED |
			    TMPFS_NODE_MODIFIED;
	TMPFS_NODE_UNLOCK(dnode);
}

/* --------------------------------------------------------------------- */

/*
 * Looks for a directory entry in the directory represented by node.
 * 'ncp' describes the name of the entry to look for.  Note that the .
 * and .. components are not allowed as they do not physically exist
 * within directories.
 *
 * Returns a pointer to the entry when found, otherwise NULL.
 */
struct tmpfs_dirent *
tmpfs_dir_lookup(struct tmpfs_node *node, struct tmpfs_node *f,
    struct namecache *ncp)
{
	struct tmpfs_dirent *de;
	int len = ncp->nc_nlen;

	TMPFS_VALIDATE_DIR(node);

	TAILQ_FOREACH(de, &node->tn_dir.tn_dirhead, td_entries) {
		if (f != NULL && de->td_node != f)
		    continue;
		if (len == de->td_namelen) {
			if (!memcmp(ncp->nc_name, de->td_name, len))
				break;
		}
	}

	TMPFS_NODE_LOCK(node);
	node->tn_status |= TMPFS_NODE_ACCESSED;
	TMPFS_NODE_UNLOCK(node);

	return de;
}

/* --------------------------------------------------------------------- */

/*
 * Helper function for tmpfs_readdir.  Creates a '.' entry for the given
 * directory and returns it in the uio space.  The function returns 0
 * on success, -1 if there was not enough space in the uio structure to
 * hold the directory entry or an appropriate error code if another
 * error happens.
 */
int
tmpfs_dir_getdotdent(struct tmpfs_node *node, struct uio *uio)
{
	int error;
	struct dirent dent;
	int dirsize;

	TMPFS_VALIDATE_DIR(node);
	KKASSERT(uio->uio_offset == TMPFS_DIRCOOKIE_DOT);

	dent.d_ino = node->tn_id;
	dent.d_type = DT_DIR;
	dent.d_namlen = 1;
	dent.d_name[0] = '.';
	dent.d_name[1] = '\0';
	dirsize = _DIRENT_DIRSIZ(&dent);

	if (dirsize > uio->uio_resid)
		error = -1;
	else {
		error = uiomove((caddr_t)&dent, dirsize, uio);
		if (error == 0)
			uio->uio_offset = TMPFS_DIRCOOKIE_DOTDOT;
	}

	node->tn_status |= TMPFS_NODE_ACCESSED;

	return error;
}

/* --------------------------------------------------------------------- */

/*
 * Helper function for tmpfs_readdir.  Creates a '..' entry for the given
 * directory and returns it in the uio space.  The function returns 0
 * on success, -1 if there was not enough space in the uio structure to
 * hold the directory entry or an appropriate error code if another
 * error happens.
 */
int
tmpfs_dir_getdotdotdent(struct tmpfs_mount *tmp, struct tmpfs_node *node,
			struct uio *uio)
{
	int error;
	struct dirent dent;
	int dirsize;

	TMPFS_VALIDATE_DIR(node);
	KKASSERT(uio->uio_offset == TMPFS_DIRCOOKIE_DOTDOT);

	if (node->tn_dir.tn_parent) {
		TMPFS_NODE_LOCK(node->tn_dir.tn_parent);
		dent.d_ino = node->tn_dir.tn_parent->tn_id;
		TMPFS_NODE_UNLOCK(node->tn_dir.tn_parent);
	} else {
		dent.d_ino = tmp->tm_root->tn_id;
	}

	dent.d_type = DT_DIR;
	dent.d_namlen = 2;
	dent.d_name[0] = '.';
	dent.d_name[1] = '.';
	dent.d_name[2] = '\0';
	dirsize = _DIRENT_DIRSIZ(&dent);

	if (dirsize > uio->uio_resid)
		error = -1;
	else {
		error = uiomove((caddr_t)&dent, dirsize, uio);
		if (error == 0) {
			struct tmpfs_dirent *de;

			de = TAILQ_FIRST(&node->tn_dir.tn_dirhead);
			if (de == NULL)
				uio->uio_offset = TMPFS_DIRCOOKIE_EOF;
			else
				uio->uio_offset = tmpfs_dircookie(de);
		}
	}

	node->tn_status |= TMPFS_NODE_ACCESSED;

	return error;
}

/* --------------------------------------------------------------------- */

/*
 * Lookup a directory entry by its associated cookie.
 */
struct tmpfs_dirent *
tmpfs_dir_lookupbycookie(struct tmpfs_node *node, off_t cookie)
{
	struct tmpfs_dirent *de;

	if (cookie == node->tn_dir.tn_readdir_lastn &&
	    node->tn_dir.tn_readdir_lastp != NULL) {
		return node->tn_dir.tn_readdir_lastp;
	}

	TAILQ_FOREACH(de, &node->tn_dir.tn_dirhead, td_entries) {
		if (tmpfs_dircookie(de) == cookie) {
			break;
		}
	}

	return de;
}

/* --------------------------------------------------------------------- */

/*
 * Helper function for tmpfs_readdir.  Returns as much directory entries
 * as can fit in the uio space.  The read starts at uio->uio_offset.
 * The function returns 0 on success, -1 if there was not enough space
 * in the uio structure to hold the directory entry or an appropriate
 * error code if another error happens.
 */
int
tmpfs_dir_getdents(struct tmpfs_node *node, struct uio *uio, off_t *cntp)
{
	int error;
	off_t startcookie;
	struct tmpfs_dirent *de;

	TMPFS_VALIDATE_DIR(node);

	/* Locate the first directory entry we have to return.  We have cached
	 * the last readdir in the node, so use those values if appropriate.
	 * Otherwise do a linear scan to find the requested entry. */
	startcookie = uio->uio_offset;
	KKASSERT(startcookie != TMPFS_DIRCOOKIE_DOT);
	KKASSERT(startcookie != TMPFS_DIRCOOKIE_DOTDOT);
	if (startcookie == TMPFS_DIRCOOKIE_EOF) {
		return 0;
	} else {
		de = tmpfs_dir_lookupbycookie(node, startcookie);
	}
	if (de == NULL) {
		return EINVAL;
	}

	/* Read as much entries as possible; i.e., until we reach the end of
	 * the directory or we exhaust uio space. */
	do {
		struct dirent d;
		int reclen;

		/* Create a dirent structure representing the current
		 * tmpfs_node and fill it. */
		d.d_ino = de->td_node->tn_id;
		switch (de->td_node->tn_type) {
		case VBLK:
			d.d_type = DT_BLK;
			break;

		case VCHR:
			d.d_type = DT_CHR;
			break;

		case VDIR:
			d.d_type = DT_DIR;
			break;

		case VFIFO:
			d.d_type = DT_FIFO;
			break;

		case VLNK:
			d.d_type = DT_LNK;
			break;

		case VREG:
			d.d_type = DT_REG;
			break;

		case VSOCK:
			d.d_type = DT_SOCK;
			break;

		default:
			panic("tmpfs_dir_getdents: type %p %d",
			    de->td_node, (int)de->td_node->tn_type);
		}
		d.d_namlen = de->td_namelen;
		KKASSERT(de->td_namelen < sizeof(d.d_name));
		bcopy(de->td_name, d.d_name, d.d_namlen);
		d.d_name[d.d_namlen] = '\0';
		reclen = _DIRENT_RECLEN(d.d_namlen);

		/* Stop reading if the directory entry we are treating is
		 * bigger than the amount of data that can be returned. */
		if (reclen > uio->uio_resid) {
			error = -1;
			break;
		}

		/* Copy the new dirent structure into the output buffer and
		 * advance pointers. */
		error = uiomove((caddr_t)&d, reclen, uio);

		(*cntp)++;
		de = TAILQ_NEXT(de, td_entries);
	} while (error == 0 && uio->uio_resid > 0 && de != NULL);

	/* Update the offset and cache. */
	if (de == NULL) {
		uio->uio_offset = TMPFS_DIRCOOKIE_EOF;
		node->tn_dir.tn_readdir_lastn = 0;
		node->tn_dir.tn_readdir_lastp = NULL;
	} else {
		node->tn_dir.tn_readdir_lastn = uio->uio_offset = tmpfs_dircookie(de);
		node->tn_dir.tn_readdir_lastp = de;
	}
	node->tn_status |= TMPFS_NODE_ACCESSED;

	return error;
}

/* --------------------------------------------------------------------- */

/*
 * Resizes the aobj associated to the regular file pointed to by vp to
 * the size newsize.  'vp' must point to a vnode that represents a regular
 * file.  'newsize' must be positive.
 *
 * pass trivial as 1 when buf content will be overwritten, otherwise set 0
 * to be zero filled.
 *
 * Returns zero on success or an appropriate error code on failure.
 */
int
tmpfs_reg_resize(struct vnode *vp, off_t newsize, int trivial)
{
	int error;
	size_t newpages, oldpages;
	struct tmpfs_mount *tmp;
	struct tmpfs_node *node;
	off_t oldsize;

#ifdef INVARIANTS
	KKASSERT(vp->v_type == VREG);
	KKASSERT(newsize >= 0);
#endif

	node = VP_TO_TMPFS_NODE(vp);
	tmp = VFS_TO_TMPFS(vp->v_mount);

	/* Convert the old and new sizes to the number of pages needed to
	 * store them.  It may happen that we do not need to do anything
	 * because the last allocated page can accommodate the change on
	 * its own. */
	oldsize = node->tn_size;
	oldpages = round_page(oldsize) / PAGE_SIZE;
	KKASSERT(oldpages == node->tn_reg.tn_aobj_pages);
	newpages = round_page(newsize) / PAGE_SIZE;

	if (newpages > oldpages &&
	    newpages - oldpages > TMPFS_PAGES_AVAIL(tmp)) {
		error = ENOSPC;
		goto out;
	}

	TMPFS_LOCK(tmp);
	tmp->tm_pages_used += (newpages - oldpages);
	TMPFS_UNLOCK(tmp);

	TMPFS_NODE_LOCK(node);
	node->tn_reg.tn_aobj_pages = newpages;
	node->tn_size = newsize;
	TMPFS_NODE_UNLOCK(node);

	/*
	 * When adjusting the vnode filesize and its VM object we must
	 * also adjust our backing VM object (aobj).  The blocksize
	 * used must match the block sized we use for the buffer cache.
	 *
	 * The backing VM object contains no VM pages, only swap
	 * assignments.
	 */
	if (newsize < oldsize) {
		vm_pindex_t osize;
		vm_pindex_t nsize;
		vm_object_t aobj;

		error = nvtruncbuf(vp, newsize, BSIZE, -1);
		aobj = node->tn_reg.tn_aobj;
		if (aobj) {
			osize = aobj->size;
			nsize = vp->v_object->size;
			if (nsize < osize) {
				aobj->size = osize;
				swap_pager_freespace(aobj, nsize,
						     osize - nsize);
			}
		}
	} else {
		vm_object_t aobj;

		error = nvextendbuf(vp, oldsize, newsize, BSIZE, BSIZE,
				    -1, -1, trivial);
		aobj = node->tn_reg.tn_aobj;
		if (aobj)
			aobj->size = vp->v_object->size;
	}

out:
	return error;
}

/* --------------------------------------------------------------------- */

/*
 * Change flags of the given vnode.
 * Caller should execute tmpfs_update on vp after a successful execution.
 * The vnode must be locked on entry and remain locked on exit.
 */
int
tmpfs_chflags(struct vnode *vp, int flags, struct ucred *cred)
{
	int error;
	struct tmpfs_node *node;
	int fmode, mode;

	KKASSERT(vn_islocked(vp));

	node = VP_TO_TMPFS_NODE(vp);

	/* Disallow this operation if the file system is mounted read-only. */
	if (vp->v_mount->mnt_flag & MNT_RDONLY)
		return EROFS;

	fmode = FFLAGS(node->tn_flags);
	mode = 0;
	if (((fmode & (FREAD | FWRITE)) == 0) || (fmode & O_CREAT))
		return EINVAL;
	if (fmode & (FWRITE | O_TRUNC)) {
		if (vp->v_type == VDIR) {
			return EISDIR;
		}
		error = vn_writechk(vp, NULL);
		if (error)
			return (error);

		mode |= VWRITE;
	}
	if (fmode & FREAD)
		mode |= VREAD;
	if (mode) {
		error = VOP_ACCESS(vp, mode, cred);
		if (error)
			return (error);
	}
	/*
	 * Unprivileged processes are not permitted to unset system
	 * flags, or modify flags if any system flags are set.
	 */
	TMPFS_NODE_LOCK(node);
	if (!priv_check_cred(cred, PRIV_VFS_SYSFLAGS, 0)) {
#if 0
		if (node->tn_flags
		  & (SF_NOUNLINK | SF_IMMUTABLE | SF_APPEND)) {
			error = securelevel_gt(cred, 0);
			if (error)
				return (error);
		}
		/* Snapshot flag cannot be set or cleared */
		if (((flags & SF_SNAPSHOT) != 0 &&
		  (node->tn_flags & SF_SNAPSHOT) == 0) ||
		  ((flags & SF_SNAPSHOT) == 0 &&
		  (node->tn_flags & SF_SNAPSHOT) != 0))
			return (EPERM);
#endif
		node->tn_flags = flags;
	} else {
		if (node->tn_flags
		  & (SF_NOUNLINK | SF_IMMUTABLE | SF_APPEND) ||
		  (flags & UF_SETTABLE) != flags)
			return (EPERM);
		node->tn_flags &= SF_SETTABLE;
		node->tn_flags |= (flags & UF_SETTABLE);
	}
	node->tn_status |= TMPFS_NODE_CHANGED;
	TMPFS_NODE_UNLOCK(node);

	KKASSERT(vn_islocked(vp));

	return 0;
}

/* --------------------------------------------------------------------- */

/*
 * Change access mode on the given vnode.
 * Caller should execute tmpfs_update on vp after a successful execution.
 * The vnode must be locked on entry and remain locked on exit.
 */
int
tmpfs_chmod(struct vnode *vp, mode_t mode, struct ucred *cred)
{
	int error;
	struct tmpfs_node *node;
	int fmode, accmode;

	KKASSERT(vn_islocked(vp));

	node = VP_TO_TMPFS_NODE(vp);

	/* Disallow this operation if the file system is mounted read-only. */
	if (vp->v_mount->mnt_flag & MNT_RDONLY)
		return EROFS;

	/* Immutable or append-only files cannot be modified, either. */
	if (node->tn_flags & (IMMUTABLE | APPEND))
		return EPERM;

	fmode = FFLAGS(node->tn_flags);
	accmode = 0;
	if (((fmode & (FREAD | FWRITE)) == 0) || (fmode & O_CREAT))
		return EINVAL;
	if (fmode & (FWRITE | O_TRUNC)) {
		if (vp->v_type == VDIR) {
			return EISDIR;
		}
		error = vn_writechk(vp, NULL);
		if (error)
			return (error);

		accmode |= VWRITE;
	}
	if (fmode & FREAD)
		accmode |= VREAD;
	if (accmode) {
		error = VOP_ACCESS(vp, accmode, cred);
		if (error)
			return (error);
	}

	/*
	 * Privileged processes may set the sticky bit on non-directories,
	 * as well as set the setgid bit on a file with a group that the
	 * process is not a member of.
	 */
	if (vp->v_type != VDIR && (mode & S_ISTXT)) {
		if (priv_check_cred(cred, PRIV_VFS_STICKYFILE, 0))
			return (EFTYPE);
	}
	if (!groupmember(node->tn_gid, cred) && (mode & S_ISGID)) {
		error = priv_check_cred(cred, PRIV_VFS_SETGID, 0);
		if (error)
			return (error);
	}


	TMPFS_NODE_LOCK(node);
	node->tn_mode &= ~ALLPERMS;
	node->tn_mode |= mode & ALLPERMS;

	node->tn_status |= TMPFS_NODE_CHANGED;
	TMPFS_NODE_UNLOCK(node);

	KKASSERT(vn_islocked(vp));

	return 0;
}

/* --------------------------------------------------------------------- */

/*
 * Change ownership of the given vnode.  At least one of uid or gid must
 * be different than VNOVAL.  If one is set to that value, the attribute
 * is unchanged.
 * Caller should execute tmpfs_update on vp after a successful execution.
 * The vnode must be locked on entry and remain locked on exit.
 */
int
tmpfs_chown(struct vnode *vp, uid_t uid, gid_t gid, struct ucred *cred)
{
	int error;
	struct tmpfs_node *node;
	uid_t ouid;
	gid_t ogid;
	int fmode, mode;

	KKASSERT(vn_islocked(vp));

	node = VP_TO_TMPFS_NODE(vp);

	/* Assign default values if they are unknown. */
	KKASSERT(uid != VNOVAL || gid != VNOVAL);
	if (uid == VNOVAL)
		uid = node->tn_uid;
	if (gid == VNOVAL)
		gid = node->tn_gid;
	KKASSERT(uid != VNOVAL && gid != VNOVAL);

	/* Disallow this operation if the file system is mounted read-only. */
	if (vp->v_mount->mnt_flag & MNT_RDONLY)
		return EROFS;

	/* Immutable or append-only files cannot be modified, either. */
	if (node->tn_flags & (IMMUTABLE | APPEND))
		return EPERM;

	fmode = FFLAGS(node->tn_flags);
	mode = 0;
	if (((fmode & (FREAD | FWRITE)) == 0) || (fmode & O_CREAT))
		return EINVAL;
	if (fmode & (FWRITE | O_TRUNC)) {
		if (vp->v_type == VDIR) {
			return EISDIR;
		}
		error = vn_writechk(vp, NULL);
		if (error)
			return (error);

		mode |= VWRITE;
	}
	if (fmode & FREAD)
		mode |= VREAD;
	if (mode) {
		error = VOP_ACCESS(vp, mode, cred);
		if (error)
			return (error);
	}

	/*
	 * To change the owner of a file, or change the group of a file to a
	 * group of which we are not a member, the caller must have
	 * privilege.
	 */
	if ((uid != node->tn_uid ||
	    (gid != node->tn_gid && !groupmember(gid, cred))) &&
	    (error = priv_check_cred(cred, PRIV_VFS_CHOWN, 0)))
		return (error);

	ogid = node->tn_gid;
	ouid = node->tn_uid;

	TMPFS_NODE_LOCK(node);
	node->tn_uid = uid;
	node->tn_gid = gid;

	node->tn_status |= TMPFS_NODE_CHANGED;

	if ((node->tn_mode & (S_ISUID | S_ISGID)) && (ouid != uid || ogid != gid)) {
		if (priv_check_cred(cred, PRIV_VFS_RETAINSUGID, 0))
			node->tn_mode &= ~(S_ISUID | S_ISGID);
	}
	TMPFS_NODE_UNLOCK(node);

	KKASSERT(vn_islocked(vp));

	return 0;
}

/* --------------------------------------------------------------------- */

/*
 * Change size of the given vnode.
 * Caller should execute tmpfs_update on vp after a successful execution.
 * The vnode must be locked on entry and remain locked on exit.
 */
int
tmpfs_chsize(struct vnode *vp, u_quad_t size, struct ucred *cred)
{
	int error;
	struct tmpfs_node *node;

	KKASSERT(vn_islocked(vp));

	node = VP_TO_TMPFS_NODE(vp);

	/* Decide whether this is a valid operation based on the file type. */
	error = 0;
	switch (vp->v_type) {
	case VDIR:
		return EISDIR;

	case VREG:
		if (vp->v_mount->mnt_flag & MNT_RDONLY)
			return EROFS;
		break;

	case VBLK:
		/* FALLTHROUGH */
	case VCHR:
		/* FALLTHROUGH */
	case VFIFO:
		/* Allow modifications of special files even if in the file
		 * system is mounted read-only (we are not modifying the
		 * files themselves, but the objects they represent). */
		return 0;

	default:
		/* Anything else is unsupported. */
		return EOPNOTSUPP;
	}

	/* Immutable or append-only files cannot be modified, either. */
	if (node->tn_flags & (IMMUTABLE | APPEND))
		return EPERM;

	error = tmpfs_truncate(vp, size);
	/* tmpfs_truncate will raise the NOTE_EXTEND and NOTE_ATTRIB kevents
	 * for us, as will update tn_status; no need to do that here. */

	KKASSERT(vn_islocked(vp));

	return error;
}

/* --------------------------------------------------------------------- */

/*
 * Change access and modification times of the given vnode.
 * Caller should execute tmpfs_update on vp after a successful execution.
 * The vnode must be locked on entry and remain locked on exit.
 */
int
tmpfs_chtimes(struct vnode *vp, struct timespec *atime, struct timespec *mtime,
	int vaflags, struct ucred *cred)
{
	int error;
	struct tmpfs_node *node;
	int fmode, mode;

	KKASSERT(vn_islocked(vp));

	node = VP_TO_TMPFS_NODE(vp);

	/* Disallow this operation if the file system is mounted read-only. */
	if (vp->v_mount->mnt_flag & MNT_RDONLY)
		return EROFS;

	/* Immutable or append-only files cannot be modified, either. */
	if (node->tn_flags & (IMMUTABLE | APPEND))
		return EPERM;

	/* Determine if the user have proper privilege to update time. */
	fmode = FFLAGS(node->tn_flags);
	mode = 0;
	if (((fmode & (FREAD | FWRITE)) == 0) || (fmode & O_CREAT))
		return EINVAL;
	if (fmode & (FWRITE | O_TRUNC)) {
		if (vp->v_type == VDIR) {
			return EISDIR;
		}
		error = vn_writechk(vp, NULL);
		if (error)
			return (error);

		mode |= VWRITE;
	}
	if (fmode & FREAD)
		mode |= VREAD;

	if (mode) {
		if (vaflags & VA_UTIMES_NULL) {
			error = VOP_ACCESS(vp, mode, cred);
			if (error)
				error = VOP_ACCESS(vp, VWRITE, cred);
		} else
			error = VOP_ACCESS(vp, mode, cred);
		if (error)
			return (error);
	}

	TMPFS_NODE_LOCK(node);
	if (atime->tv_sec != VNOVAL && atime->tv_nsec != VNOVAL)
		node->tn_status |= TMPFS_NODE_ACCESSED;

	if (mtime->tv_sec != VNOVAL && mtime->tv_nsec != VNOVAL)
		node->tn_status |= TMPFS_NODE_MODIFIED;

	TMPFS_NODE_UNLOCK(node);

	tmpfs_itimes(vp, atime, mtime);

	KKASSERT(vn_islocked(vp));

	return 0;
}

/* --------------------------------------------------------------------- */
/* Sync timestamps */
void
tmpfs_itimes(struct vnode *vp, const struct timespec *acc,
    const struct timespec *mod)
{
	struct tmpfs_node *node;
	struct timespec now;

	node = VP_TO_TMPFS_NODE(vp);

	if ((node->tn_status & (TMPFS_NODE_ACCESSED | TMPFS_NODE_MODIFIED |
	    TMPFS_NODE_CHANGED)) == 0)
		return;

	vfs_timestamp(&now);

	TMPFS_NODE_LOCK(node);
	if (node->tn_status & TMPFS_NODE_ACCESSED) {
		if (acc == NULL)
			 acc = &now;
		node->tn_atime = acc->tv_sec;
		node->tn_atimensec = acc->tv_nsec;
	}
	if (node->tn_status & TMPFS_NODE_MODIFIED) {
		if (mod == NULL)
			mod = &now;
		node->tn_mtime = mod->tv_sec;
		node->tn_mtimensec = mod->tv_nsec;
	}
	if (node->tn_status & TMPFS_NODE_CHANGED) {
		node->tn_ctime = now.tv_sec;
		node->tn_ctimensec = now.tv_nsec;
	}
	node->tn_status &=
	    ~(TMPFS_NODE_ACCESSED | TMPFS_NODE_MODIFIED | TMPFS_NODE_CHANGED);
	TMPFS_NODE_UNLOCK(node);
}

/* --------------------------------------------------------------------- */

void
tmpfs_update(struct vnode *vp)
{

	tmpfs_itimes(vp, NULL, NULL);
}

/* --------------------------------------------------------------------- */

int
tmpfs_truncate(struct vnode *vp, off_t length)
{
	int error;
	struct tmpfs_node *node;

	node = VP_TO_TMPFS_NODE(vp);

	if (length < 0) {
		error = EINVAL;
		goto out;
	}

	if (node->tn_size == length) {
		error = 0;
		goto out;
	}

	if (length > VFS_TO_TMPFS(vp->v_mount)->tm_maxfilesize)
		return (EFBIG);


	error = tmpfs_reg_resize(vp, length, 1);

	if (error == 0) {
		TMPFS_NODE_LOCK(node);
		node->tn_status |= TMPFS_NODE_CHANGED | TMPFS_NODE_MODIFIED;
		TMPFS_NODE_UNLOCK(node);
	}

out:
	tmpfs_update(vp);

	return error;
}

/* --------------------------------------------------------------------- */

static ino_t
tmpfs_fetch_ino(void)
{
	ino_t	ret;

	spin_lock_wr(&ino_lock);
	ret = t_ino++;
	spin_unlock_wr(&ino_lock);

	return ret;
}