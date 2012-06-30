/*-
 * Copyright (c) 2011 NetApp, Inc.
 * All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY NETAPP, INC ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL NETAPP, INC OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/queue.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/malloc.h>
#include <sys/conf.h>
#include <sys/sysctl.h>
#include <sys/libkern.h>
#include <sys/ioccom.h>
#include <sys/mman.h>
#include <sys/uio.h>

#include <vm/vm.h>
#include <vm/pmap.h>

#include <machine/pmap.h>
#include <machine/vmparam.h>

#include <machine/vmm.h>
#include "vmm_lapic.h"
#include "vmm_stat.h"
#include "io/ppt.h"
#include <machine/vmm_dev.h>

struct vmmdev_softc {
	struct vm	*vm;		/* vm instance cookie */
	struct cdev	*cdev;
	SLIST_ENTRY(vmmdev_softc) link;
};
static SLIST_HEAD(, vmmdev_softc) head;

static struct mtx vmmdev_mtx;

static MALLOC_DEFINE(M_VMMDEV, "vmmdev", "vmmdev");

SYSCTL_DECL(_hw_vmm);

static struct vmmdev_softc *
vmmdev_lookup(const char *name)
{
	struct vmmdev_softc *sc;

#ifdef notyet	/* XXX kernel is not compiled with invariants */
	mtx_assert(&vmmdev_mtx, MA_OWNED);
#endif

	SLIST_FOREACH(sc, &head, link) {
		if (strcmp(name, vm_name(sc->vm)) == 0)
			break;
	}

	return (sc);
}

static struct vmmdev_softc *
vmmdev_lookup2(struct cdev *cdev)
{
	struct vmmdev_softc *sc;

#ifdef notyet	/* XXX kernel is not compiled with invariants */
	mtx_assert(&vmmdev_mtx, MA_OWNED);
#endif

	SLIST_FOREACH(sc, &head, link) {
		if (sc->cdev == cdev)
			break;
	}

	return (sc);
}

static int
vmmdev_rw(struct cdev *cdev, struct uio *uio, int flags)
{
	int error, off, c;
	vm_paddr_t hpa, gpa;
	struct vmmdev_softc *sc;

	static char zerobuf[PAGE_SIZE];

	error = 0;
	mtx_lock(&vmmdev_mtx);
	sc = vmmdev_lookup2(cdev);

	while (uio->uio_resid > 0 && error == 0) {
		gpa = uio->uio_offset;
		off = gpa & PAGE_MASK;
		c = min(uio->uio_resid, PAGE_SIZE - off);

		/*
		 * The VM has a hole in its physical memory map. If we want to
		 * use 'dd' to inspect memory beyond the hole we need to
		 * provide bogus data for memory that lies in the hole.
		 *
		 * Since this device does not support lseek(2), dd(1) will
		 * read(2) blocks of data to simulate the lseek(2).
		 */
		hpa = vm_gpa2hpa(sc->vm, gpa, c);
		if (hpa == (vm_paddr_t)-1) {
			if (uio->uio_rw == UIO_READ)
				error = uiomove(zerobuf, c, uio);
			else
				error = EFAULT;
		} else
			error = uiomove((void *)PHYS_TO_DMAP(hpa), c, uio);
	}

	mtx_unlock(&vmmdev_mtx);
	return (error);
}

static int
vmmdev_ioctl(struct cdev *cdev, u_long cmd, caddr_t data, int fflag,
	     struct thread *td)
{
	int error, vcpu;
	struct vmmdev_softc *sc;
	struct vm_memory_segment *seg;
	struct vm_register *vmreg;
	struct vm_seg_desc* vmsegdesc;
	struct vm_pin *vmpin;
	struct vm_run *vmrun;
	struct vm_event *vmevent;
	struct vm_lapic_irq *vmirq;
	struct vm_capability *vmcap;
	struct vm_pptdev *pptdev;
	struct vm_pptdev_mmio *pptmmio;
	struct vm_pptdev_msi *pptmsi;
	struct vm_pptdev_msix *pptmsix;
	struct vm_nmi *vmnmi;
	struct vm_stats *vmstats;
	struct vm_stat_desc *statdesc;

	mtx_lock(&vmmdev_mtx);
	sc = vmmdev_lookup2(cdev);
	if (sc == NULL) {
		mtx_unlock(&vmmdev_mtx);
		return (ENXIO);
	}

	/*
	 * Some VMM ioctls can operate only on vcpus that are not running.
	 */
	switch (cmd) {
	case VM_RUN:
	case VM_SET_PINNING:
	case VM_GET_REGISTER:
	case VM_SET_REGISTER:
	case VM_GET_SEGMENT_DESCRIPTOR:
	case VM_SET_SEGMENT_DESCRIPTOR:
	case VM_INJECT_EVENT:
	case VM_GET_CAPABILITY:
	case VM_SET_CAPABILITY:
	case VM_PPTDEV_MSI:
		/*
		 * XXX fragile, handle with care
		 * Assumes that the first field of the ioctl data is the vcpu.
		 */
		vcpu = *(int *)data;
		if (vcpu < 0 || vcpu >= VM_MAXCPU) {
			error = EINVAL;
			goto done;
		}

		if (vcpu_is_running(sc->vm, vcpu, NULL)) {
			error = EBUSY;
			goto done;
		}
		break;
	default:
		break;
	}

	switch(cmd) {
	case VM_RUN:
		vmrun = (struct vm_run *)data;

		vm_set_run_state(sc->vm, vmrun->cpuid, VCPU_RUNNING);
		mtx_unlock(&vmmdev_mtx);

		error = vm_run(sc->vm, vmrun);

		mtx_lock(&vmmdev_mtx);
		vm_set_run_state(sc->vm, vmrun->cpuid, VCPU_STOPPED);
		break;
	case VM_STAT_DESC: {
		const char *desc;
		statdesc = (struct vm_stat_desc *)data;
		desc = vmm_stat_desc(statdesc->index);
		if (desc != NULL) {
			error = 0;
			strlcpy(statdesc->desc, desc, sizeof(statdesc->desc));
		} else
			error = EINVAL;
		break;
	}
	case VM_STATS: {
		CTASSERT(MAX_VM_STATS >= MAX_VMM_STAT_TYPES);
		vmstats = (struct vm_stats *)data;
		getmicrotime(&vmstats->tv);
		error = vmm_stat_copy(sc->vm, vmstats->cpuid,
				      &vmstats->num_entries, vmstats->statbuf);
		break;
	}
	case VM_PPTDEV_MSI:
		pptmsi = (struct vm_pptdev_msi *)data;
		error = ppt_setup_msi(sc->vm, pptmsi->vcpu,
				      pptmsi->bus, pptmsi->slot, pptmsi->func,
				      pptmsi->destcpu, pptmsi->vector,
				      pptmsi->numvec);
		break;
	case VM_PPTDEV_MSIX:
		pptmsix = (struct vm_pptdev_msix *)data;
		error = ppt_setup_msix(sc->vm, pptmsix->vcpu,
				       pptmsix->bus, pptmsix->slot, 
				       pptmsix->func, pptmsix->idx,
				       pptmsix->msg, pptmsix->vector_control,
				       pptmsix->addr);
		break;
	case VM_MAP_PPTDEV_MMIO:
		pptmmio = (struct vm_pptdev_mmio *)data;
		error = ppt_map_mmio(sc->vm, pptmmio->bus, pptmmio->slot,
				     pptmmio->func, pptmmio->gpa, pptmmio->len,
				     pptmmio->hpa);
		break;
	case VM_BIND_PPTDEV:
		pptdev = (struct vm_pptdev *)data;
		error = ppt_assign_device(sc->vm, pptdev->bus, pptdev->slot,
					  pptdev->func);
		break;
	case VM_UNBIND_PPTDEV:
		pptdev = (struct vm_pptdev *)data;
		error = ppt_unassign_device(sc->vm, pptdev->bus, pptdev->slot,
					    pptdev->func);
		break;
	case VM_INJECT_EVENT:
		vmevent = (struct vm_event *)data;
		error = vm_inject_event(sc->vm, vmevent->cpuid, vmevent->type,
					vmevent->vector,
					vmevent->error_code,
					vmevent->error_code_valid);
		break;
	case VM_INJECT_NMI:
		vmnmi = (struct vm_nmi *)data;
		error = vm_inject_nmi(sc->vm, vmnmi->cpuid);
		break;
	case VM_LAPIC_IRQ:
		vmirq = (struct vm_lapic_irq *)data;
		error = lapic_set_intr(sc->vm, vmirq->cpuid, vmirq->vector);
		break;
	case VM_SET_PINNING:
		vmpin = (struct vm_pin *)data;
		error = vm_set_pinning(sc->vm, vmpin->vm_cpuid,
				       vmpin->host_cpuid);
		break;
	case VM_GET_PINNING:
		vmpin = (struct vm_pin *)data;
		error = vm_get_pinning(sc->vm, vmpin->vm_cpuid,
				       &vmpin->host_cpuid);
		break;
	case VM_MAP_MEMORY:
		seg = (struct vm_memory_segment *)data;
		error = vm_malloc(sc->vm, seg->gpa, seg->len, &seg->hpa);
		break;
	case VM_GET_MEMORY_SEG:
		seg = (struct vm_memory_segment *)data;
		seg->hpa = seg->len = 0;
		(void)vm_gpabase2memseg(sc->vm, seg->gpa, seg);
		error = 0;
		break;
	case VM_GET_REGISTER:
		vmreg = (struct vm_register *)data;
		error = vm_get_register(sc->vm, vmreg->cpuid, vmreg->regnum,
					&vmreg->regval);
		break;
	case VM_SET_REGISTER:
		vmreg = (struct vm_register *)data;
		error = vm_set_register(sc->vm, vmreg->cpuid, vmreg->regnum,
					vmreg->regval);
		break;
	case VM_SET_SEGMENT_DESCRIPTOR:
		vmsegdesc = (struct vm_seg_desc *)data;
		error = vm_set_seg_desc(sc->vm, vmsegdesc->cpuid,
					vmsegdesc->regnum,
					&vmsegdesc->desc);
		break;
	case VM_GET_SEGMENT_DESCRIPTOR:
		vmsegdesc = (struct vm_seg_desc *)data;
		error = vm_get_seg_desc(sc->vm, vmsegdesc->cpuid,
					vmsegdesc->regnum,
					&vmsegdesc->desc);
		break;
	case VM_GET_CAPABILITY:
		vmcap = (struct vm_capability *)data;
		error = vm_get_capability(sc->vm, vmcap->cpuid,
					  vmcap->captype,
					  &vmcap->capval);
		break;
	case VM_SET_CAPABILITY:
		vmcap = (struct vm_capability *)data;
		error = vm_set_capability(sc->vm, vmcap->cpuid,
					  vmcap->captype,
					  vmcap->capval);
		break;
	default:
		error = ENOTTY;
		break;
	}
done:
	mtx_unlock(&vmmdev_mtx);

	return (error);
}

static int
vmmdev_mmap(struct cdev *cdev, vm_ooffset_t offset, vm_paddr_t *paddr,
    int nprot, vm_memattr_t *memattr)
{
	int error;
	struct vmmdev_softc *sc;

	error = -1;
	mtx_lock(&vmmdev_mtx);

	sc = vmmdev_lookup2(cdev);
	if (sc != NULL && (nprot & PROT_EXEC) == 0) {
		*paddr = vm_gpa2hpa(sc->vm, (vm_paddr_t)offset, PAGE_SIZE);
		if (*paddr != (vm_paddr_t)-1)
			error = 0;
	}

	mtx_unlock(&vmmdev_mtx);

	return (error);
}

static void
vmmdev_destroy(struct vmmdev_softc *sc)
{

#ifdef notyet	/* XXX kernel is not compiled with invariants */
	mtx_assert(&vmmdev_mtx, MA_OWNED);
#endif

	/*
	 * XXX must stop virtual machine instances that may be still
	 * running and cleanup their state.
	 */
	SLIST_REMOVE(&head, sc, vmmdev_softc, link);
	destroy_dev(sc->cdev);
	vm_destroy(sc->vm);
	free(sc, M_VMMDEV);
}

static int
sysctl_vmm_destroy(SYSCTL_HANDLER_ARGS)
{
	int error;
	char buf[VM_MAX_NAMELEN];
	struct vmmdev_softc *sc;

	strlcpy(buf, "beavis", sizeof(buf));
	error = sysctl_handle_string(oidp, buf, sizeof(buf), req);
	if (error != 0 || req->newptr == NULL)
		return (error);

	mtx_lock(&vmmdev_mtx);
	sc = vmmdev_lookup(buf);
	if (sc == NULL) {
		mtx_unlock(&vmmdev_mtx);
		return (EINVAL);
	}
	vmmdev_destroy(sc);
	mtx_unlock(&vmmdev_mtx);
	return (0);
}
SYSCTL_PROC(_hw_vmm, OID_AUTO, destroy, CTLTYPE_STRING | CTLFLAG_RW,
	    NULL, 0, sysctl_vmm_destroy, "A", NULL);

static struct cdevsw vmmdevsw = {
	.d_name		= "vmmdev",
	.d_version	= D_VERSION,
	.d_ioctl	= vmmdev_ioctl,
	.d_mmap		= vmmdev_mmap,
	.d_read		= vmmdev_rw,
	.d_write	= vmmdev_rw,
};

static int
sysctl_vmm_create(SYSCTL_HANDLER_ARGS)
{
	int error;
	struct vm *vm;
	struct vmmdev_softc *sc;
	char buf[VM_MAX_NAMELEN];

	strlcpy(buf, "beavis", sizeof(buf));
	error = sysctl_handle_string(oidp, buf, sizeof(buf), req);
	if (error != 0 || req->newptr == NULL)
		return (error);

	mtx_lock(&vmmdev_mtx);

	sc = vmmdev_lookup(buf);
	if (sc != NULL) {
		mtx_unlock(&vmmdev_mtx);
		return (EEXIST);
	}

	vm = vm_create(buf);
	if (vm == NULL) {
		mtx_unlock(&vmmdev_mtx);
		return (EINVAL);
	}

	sc = malloc(sizeof(struct vmmdev_softc), M_VMMDEV, M_WAITOK | M_ZERO);
	sc->vm = vm;
	sc->cdev = make_dev(&vmmdevsw, 0, UID_ROOT, GID_WHEEL, 0600,
			    "vmm/%s", buf);
	sc->cdev->si_drv1 = sc;
	SLIST_INSERT_HEAD(&head, sc, link);

	mtx_unlock(&vmmdev_mtx);
	return (0);
}
SYSCTL_PROC(_hw_vmm, OID_AUTO, create, CTLTYPE_STRING | CTLFLAG_RW,
	    NULL, 0, sysctl_vmm_create, "A", NULL);

void
vmmdev_init(void)
{
	mtx_init(&vmmdev_mtx, "vmm device mutex", NULL, MTX_DEF);
}

void
vmmdev_cleanup(void)
{
	struct vmmdev_softc *sc, *sc2;

	mtx_lock(&vmmdev_mtx);

	SLIST_FOREACH_SAFE(sc, &head, link, sc2)
		vmmdev_destroy(sc);

	mtx_unlock(&vmmdev_mtx);
}
