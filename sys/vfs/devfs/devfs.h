/*
 * Copyright (c) 2009 The DragonFly Project.  All rights reserved.
 *
 * This code is derived from software contributed to The DragonFly Project
 * by Alex Hornung <ahornung@gmail.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of The DragonFly Project nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific, prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#ifndef _VFS_DEVFS_H_
#define	_VFS_DEVFS_H_

#if !defined(_KERNEL) && !defined(_KERNEL_STRUCTURES)
#error "This file should not be included by userland programs."
#endif

#ifndef _SYS_QUEUE_H_
#include <sys/queue.h>
#endif
#ifndef _SYS_LOCK_H_
#include <sys/lock.h>
#endif
#ifndef _SYS_CONF_H_
#include <sys/conf.h>
#endif
#ifndef _SYS_MSGPORT2_H_
//#include <sys/msgport2.h>
#endif
#ifndef _SYS_MSGPORT_H_
#include <sys/msgport.h>
#endif
#ifndef _SYS_DIRENT_H_
#include <sys/dirent.h>
#endif
#ifndef _SYS_DEVICE_H_
#include <sys/device.h>
#endif
#ifndef _SYS_UCRED_H_
#include <sys/ucred.h>
#endif


typedef enum {
	Proot,		/* the filesystem root */
	Plink,
	Preg,
	Pdir,
	Pdev
} devfs_nodetype;

/*
struct devfs_dev {
	cdev_t		d_devt;
	uint16_t	d_flags;
	uint16_t	d_class;

	//XXX: do I need a use count so I know when it is safe to
	//		unload the module or detach or whatever?
	TAILQ_ENTRY(devfs_dev) link;
};
*/

struct devfs_dirent {
	ino_t		d_ino;		/* file number of entry */
	uint16_t	d_namlen;	/* strlen(d_name) */
	uint8_t		d_type;		/* file type */
	char		*d_name;
};

struct devfs_node {
	cdev_t	d_dev;					/* device assoicated with this node */

	struct mount 		*mp;		/* mount point of this node */
	struct devfs_dirent	d_dir;		/* dirent data (name, inode, ...) */
	struct vnode 		*v_node;	/* assoicated vnode */
	struct devfs_node 	*parent;	/* parent of this node */
	devfs_nodetype		node_type;	/* devfs node type */

	u_int64_t	refs;		/* number of open references */
	size_t		nchildren;	/* number of children of a parent */
	u_int64_t	cookie_jar;	/* cookie pool for children */
	u_int64_t	cookie;		/* directory entry cookie for readdir */

	struct devfs_node 	*link_target;	/* target of this autolink-type node */
	size_t		nlinks;					/* number of links that point to this node */

	char		*symlink_name;		/* symlink name for readlink */
	size_t		symlink_namelen;	/* symlink name length for readlink */

	u_short		mode;		/* files access mode and type */
	uid_t		uid;		/* owner user id */
	gid_t		gid;		/* owner group id */
	u_long		flags;

	struct timespec	atime;	/* time of last access */
	struct timespec	mtime;	/* time of last modification */
	struct timespec	ctime;	/* time file changed */

	/* Other members */
	TAILQ_ENTRY(devfs_node) 	link;
	TAILQ_HEAD(, devfs_node) 	list;	/* linked list of children */
};

struct devfs_orphan {
	struct devfs_node 	*node;

	TAILQ_ENTRY(devfs_orphan) 	link;
};

struct devfs_mnt_data {
	TAILQ_HEAD(, devfs_orphan) 	orphan_list;

	struct devfs_node	*root_node;
	struct mount		*mp;

	uint32_t	mnt_type;
	size_t		leak_count;

	int		jailed;
	size_t	mntonnamelen;
	TAILQ_ENTRY(devfs_mnt_data)	link;
};

struct devfs_clone_handler {
	char 		name[128];
	u_char		namlen;
	d_clone_t 	*nhandler;

	TAILQ_ENTRY(devfs_clone_handler) 	link;
};


struct devfs_alias {
	char	name[PATH_MAX + 1];
	cdev_t	dev_target;

	TAILQ_ENTRY(devfs_alias)	link;
};


typedef struct devfs_msg {
	struct lwkt_msg hdr;
	__uint32_t		id;

	union {
		struct {
			cdev_t	dev;
			uid_t	uid;
			gid_t	gid;
			int		perms;
		} __m_dev;
		struct {
			struct devfs_mnt_data *mnt;
		} __m_mnt;
		struct {
			char		*name;
			d_clone_t 	*nhandler;
		} __m_chandler;
		struct {
			void *load;
		} __m_gen;
		struct {
			void *resp;
		} __m_resp;
		struct {
			udev_t	udev;
		} __m_udev;
		struct {
			cdev_t	cdev;
		} __m_cdev;
		struct {
			char *name;
		} __m_name;
		struct {
			char *basename;
			u_char unit;
			struct vnode *vp;
		} __m_clone;
		struct {
			struct devfs_node *node;
		} __m_node;
		struct {
			char *name;
			char *target;
			struct mount *mp;
		} __m_link;
		struct {
			struct dev_ops *ops;
			int minor;
		} __m_ops;
		struct {
			char *name;
			uint32_t flag;
		} __m_flags;
	} __m_u;

#define mdv_chandler	__m_u.__m_chandler
#define mdv_mnt	__m_u.__m_mnt.mnt
#define mdv_load	__m_u.__m_gen.load
#define mdv_response	__m_u.__m_resp.resp
#define mdv_dev	__m_u.__m_dev
#define mdv_link	__m_u.__m_link
#define mdv_udev	__m_u.__m_udev.udev
#define mdv_cdev	__m_u.__m_cdev.cdev
#define mdv_name	__m_u.__m_name.name
#define mdv_clone	__m_u.__m_clone
#define mdv_node	__m_u.__m_node.node
#define mdv_ops	__m_u.__m_ops
#define mdv_flags	__m_u.__m_flags

} *devfs_msg_t;

typedef struct devfs_core_args {
    thread_t     td;
} *devfs_core_args_t;


TAILQ_HEAD(devfs_node_head, devfs_node);
TAILQ_HEAD(devfs_dev_head, cdev);
TAILQ_HEAD(devfs_mnt_head, devfs_mnt_data);
TAILQ_HEAD(devfs_chandler_head, devfs_clone_handler);
TAILQ_HEAD(devfs_alias_head, devfs_alias);

typedef void (devfs_scan_t)(cdev_t);


#define DEVFS_NODE(x)			((struct devfs_node *)((x)->v_data))
#define DEVFS_MNTDATA(x)		((struct devfs_mnt_data *)((x)->mnt_data))
#define DEVFS_ORPHANLIST(x)		(&(DEVFS_MNTDATA(x)->orphan_list))
#define DEVFS_DENODE_HEAD(x) 	(&((x)->list))
#define DEVFS_ISDIGIT(x)		((x >= '0') && (x <= '9'))

#define DEVFS_DEFAULT_MODE	((VREAD|VWRITE|VEXEC) | ((VREAD|VEXEC)>>3) | ((VREAD|VEXEC)>>6)); /* -rwxr-xr-x */
#define DEVFS_DEFAULT_UID	0	/* root */
#define DEVFS_DEFAULT_GID	5 	/* operator */

//#define DEVFS_DEFAULT_FLAGS 0

/*
 * debug levels
 */
#define DEVFS_DEBUG_SHOW		0x00
#define DEVFS_DEBUG_WARNING		0x01
#define DEVFS_DEBUG_INFO		0x02
#define DEVFS_DEBUG_DEBUG		0x03

/*
 * Message ids
 */
#define DEVFS_TERMINATE_CORE	0x01
#define DEVFS_DEVICE_CREATE		0x02
#define DEVFS_DEVICE_DESTROY 	0x03
#define DEVFS_MOUNT_ADD			0x04
#define DEVFS_MOUNT_DEL			0x05
#define DEVFS_CREATE_ALL_DEV	0x06
#define DEVFS_DESTROY_SUBNAMES	0x07
#define DEVFS_DESTROY_DEV_BY_OPS 0x08
#define DEVFS_CHANDLER_ADD		0x09
#define DEVFS_CHANDLER_DEL		0x0A
#define DEVFS_FIND_DEVICE_BY_UDEV 0x0B
#define DEVFS_FIND_DEVICE_BY_NAME 0x0C
#define DEVFS_MAKE_ALIAS		0x0D
#define DEVFS_APPLY_RULES		0x0F
#define	DEVFS_RESET_RULES		0x10
#define DEVFS_SCAN_CALLBACK		0x11
#define DEVFS_CLR_SUBNAMES_FLAG	0x12
#define DEVFS_DESTROY_SUBNAMES_WO_FLAG	0x13
#define DEVFS_SYNC				0x99

/*
 * Node flags
 */
#define DEVFS_NODE_LINKED		0x01	/* Linked into topology */
#define	DEVFS_USER_CREATED		0x02	/* Node was user-created */
#define DEVFS_ORPHANED			0x04	/* on orphan list */
#define DEVFS_CLONED			0x08	/* Created by cloning code */
#define DEVFS_HIDDEN			0x10	/* Makes node inaccessible, apart from already allocated vnodes*/
#define DEVFS_INVISIBLE			0x20	/* Makes node invisible in a readdir() */
#define	DEVFS_PTY			0x40	/* PTY device */
#define DEVFS_DESTROYED			0x80	/* Sanity check */


/*
 * Clone helper stuff
 */
#define DEVFS_UNIT_HSIZE        64	/* power of 2 */
#define DEVFS_UNIT_HMASK        (DEVFS_UNIT_HSIZE - 1)
#define DEVFS_CLONE_HASHLIST(name)	devfs_ ## name ## _clone_hashlist
#define DEVFS_DECLARE_CLONE_HASHLIST(name)	struct devfs_unit_hash* DEVFS_CLONE_HASHLIST(name) [DEVFS_UNIT_HSIZE]


#define DEVFS_BITMAP_INITIAL_SIZE 1
#define DEVFS_CLONE_BITMAP(name)	devfs_ ## name ## _clone_bitmap
#define DEVFS_DECLARE_CLONE_BITMAP(name)	struct devfs_bitmap DEVFS_CLONE_BITMAP(name)
#define devfs_clone_bitmap_put	devfs_clone_bitmap_rst

struct devfs_bitmap {
	int	chunks;
	unsigned long *bitmap;
};

struct devfs_unit_hash {
        struct devfs_unit_hash  *next;
        int     unit_no;

		cdev_t	dev;
};

struct devfs_clone_helper {
	DEVFS_DECLARE_CLONE_HASHLIST(generic);
	DEVFS_DECLARE_CLONE_BITMAP(generic);
};

#define DEVFS_CLONE_HELPER(name)	devfs_ ## name ## _clone_helper
#define DEVFS_DECLARE_CLONE_HELPER(name)	static struct devfs_clone_helper DEVFS_CLONE_HELPER(name)


void devfs_clone_bitmap_init(struct devfs_bitmap *);
void devfs_clone_bitmap_uninit(struct devfs_bitmap *);
void devfs_clone_bitmap_resize(struct devfs_bitmap *, int);
int devfs_clone_bitmap_fff(struct devfs_bitmap *);
void devfs_clone_bitmap_set(struct devfs_bitmap *, int);
void devfs_clone_bitmap_rst(struct devfs_bitmap *, int);
int devfs_clone_bitmap_get(struct devfs_bitmap *, int);
int devfs_clone_bitmap_chk(struct devfs_bitmap *, int);

void devfs_clone_helper_init(struct devfs_clone_helper *);
void devfs_clone_helper_uninit(struct devfs_clone_helper *);
int devfs_clone_helper_insert(struct devfs_clone_helper *, cdev_t);
int devfs_clone_helper_remove(struct devfs_clone_helper *, int);


/*
 * Prototypes
 */
int devfs_debug(int level, char *fmt, ...);
int devfs_allocv(struct vnode **, struct devfs_node *);
struct devfs_node *devfs_allocp(devfs_nodetype, char *, struct devfs_node *, struct mount *, cdev_t);
int devfs_allocvp(struct mount *, struct vnode **, devfs_nodetype, char *, struct devfs_node *, cdev_t);

int devfs_freep(struct devfs_node *);
int devfs_reaperp(struct devfs_node *);

int devfs_unlinkp(struct devfs_node *);

void devfs_tracer_add_orphan(struct devfs_node *);
void devfs_tracer_del_orphan(struct devfs_node *);
size_t devfs_tracer_orphan_count(struct mount *, int);

int devfs_set_perms(struct devfs_node *, uid_t, gid_t, u_short, u_long);
int devfs_gc(struct devfs_node *);

int devfs_create_dev(cdev_t, uid_t, gid_t, int);
int devfs_destroy_dev(cdev_t);

devfs_msg_t devfs_msg_send_sync(uint32_t, devfs_msg_t);
__uint32_t devfs_msg_send(uint32_t, devfs_msg_t);
__uint32_t devfs_msg_send_dev(uint32_t, cdev_t dev, uid_t, gid_t, int);
__uint32_t devfs_msg_send_mount(uint32_t, struct devfs_mnt_data *);
__uint32_t devfs_msg_send_ops(uint32_t, struct dev_ops *, int);
__uint32_t devfs_msg_send_chandler(uint32_t, char *, d_clone_t);
__uint32_t devfs_msg_send_generic(uint32_t, void *);
__uint32_t devfs_msg_send_name(uint32_t, char *);
__uint32_t devfs_msg_send_link(uint32_t, char *, char *, struct mount *);

devfs_msg_t devfs_msg_get(void);
int devfs_msg_put(devfs_msg_t);

int devfs_mount_add(struct devfs_mnt_data *);
int devfs_mount_del(struct devfs_mnt_data *);

int devfs_create_all_dev(struct devfs_node *);

struct devfs_node *devfs_resolve_or_create_path(struct devfs_node *, char *, int);
int devfs_resolve_name_path(char *, char *, char **, char **);
struct devfs_node *devfs_create_device_node(struct devfs_node *, cdev_t, char *, char *, ...);

int devfs_destroy_device_node(struct devfs_node *, cdev_t);
int devfs_destroy_subnames(char *);
int devfs_destroy_dev_by_ops(struct dev_ops *, int);
struct devfs_node *devfs_find_device_node(struct devfs_node *, cdev_t);
struct devfs_node *devfs_find_device_node_by_name(struct devfs_node *, char *);

cdev_t devfs_new_cdev(struct dev_ops *, int);

cdev_t devfs_find_device_by_name(const char *, ...);
cdev_t devfs_find_device_by_udev(udev_t);

int devfs_clone_handler_add(char *, d_clone_t *);
int devfs_clone_handler_del(char *);
int devfs_clone(char *, size_t *, cdev_t *, int, struct ucred *);

int devfs_link_dev(cdev_t);

int devfs_make_alias(char *, cdev_t);

int devfs_alias_create(char *name_orig, struct devfs_node *target);

int devfs_apply_rules(char *);
int devfs_reset_rules(char *);

int devfs_scan_callback(devfs_scan_t *);
int devfs_node_to_path(struct devfs_node *, char *);

int devfs_clr_subnames_flag(char *, uint32_t);
int devfs_destroy_subnames_without_flag(char *, uint32_t);

void devfs_config(void *);
#endif /* _VFS_DEVFS_H_ */