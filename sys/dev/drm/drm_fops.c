/*-
 * Copyright 1999 Precision Insight, Inc., Cedar Park, Texas.
 * Copyright 2000 VA Linux Systems, Inc., Sunnyvale, California.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * VA LINUX SYSTEMS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Rickard E. (Rik) Faith <faith@valinux.com>
 *    Daryll Strauss <daryll@valinux.com>
 *    Gareth Hughes <gareth@valinux.com>
 *
 */

/** @file drm_fops.c
 * Support code for dealing with the file privates associated with each
 * open of the DRM device.
 */

#include "dev/drm/drmP.h"

struct drm_file *drm_find_file_by_proc(struct drm_device *dev, DRM_STRUCTPROC *p)
{
	uid_t uid = p->td_proc->p_ucred->cr_svuid;
	pid_t pid = p->td_proc->p_pid;
	struct drm_file *priv;

	TAILQ_FOREACH(priv, &dev->files, link)
		if (priv->pid == pid && priv->uid == uid)
			return priv;
	return NULL;
}

/* drm_open_helper is called whenever a process opens /dev/drm. */
int drm_open_helper(struct cdev *kdev, int flags, int fmt, DRM_STRUCTPROC *p,
		    struct drm_device *dev)
{
	struct drm_file *priv;
	int m = minor(kdev);
	int retcode;

	if (flags & O_EXCL)
		return EBUSY; /* No exclusive opens */
	dev->flags = flags;

	DRM_DEBUG("pid = %d, minor = %d\n", DRM_CURRENTPID, m);
        DRM_LOCK();
        priv = drm_find_file_by_proc(dev, p);
        if (priv) {
                priv->refs++;
        } else {
                priv = malloc(sizeof(*priv), DRM_MEM_FILES, M_NOWAIT | M_ZERO);
                if (priv == NULL) {
                        DRM_UNLOCK();
                        return ENOMEM;
                }
                priv->uid               = p->td_proc->p_ucred->cr_svuid;
                priv->pid               = p->td_proc->p_pid;
                priv->refs              = 1;
                priv->minor             = m;
                priv->ioctl_count       = 0;

                /* for compatibility root is always authenticated */
                priv->authenticated     = DRM_SUSER(p);

                if (dev->driver->open) {
                        /* shared code returns -errno */
                        retcode = -dev->driver->open(dev, priv);
                        if (retcode != 0) {
                                free(priv, DRM_MEM_FILES);
                                DRM_UNLOCK();
                                return retcode;
                        }
                }

                /* first opener automatically becomes master */
                priv->master = TAILQ_EMPTY(&dev->files);

                TAILQ_INSERT_TAIL(&dev->files, priv, link);
	}

	DRM_UNLOCK();
	kdev->si_drv1 = dev;
	return 0;
}


/* The drm_read and drm_poll are stubs to prevent spurious errors
 * on older X Servers (4.3.0 and earlier) */
int drm_read(struct dev_read_args *ap)
{
	return 0;
}

static int
drmfilt(struct knote *kn, long hint)
{
	return (0);
}

static void
drmfilt_detach(struct knote *kn) {}

static struct filterops drmfiltops =
        { FILTEROP_ISFD, NULL, drmfilt_detach, drmfilt };

int
drm_kqfilter(struct dev_kqfilter_args *ap)
{
	struct knote *kn = ap->a_kn;

	ap->a_result = 0;

	switch (kn->kn_filter) {
	case EVFILT_READ:
	case EVFILT_WRITE:
		kn->kn_fop = &drmfiltops;
		break;
	default:
		ap->a_result = EOPNOTSUPP;
		return (0);
	}

	return (0);
}
