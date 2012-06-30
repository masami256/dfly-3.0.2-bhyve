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
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/bus.h>
#include <sys/pciio.h>
#include <sys/rman.h>
#include <sys/smp.h>

#include <dev/pci/pcivar.h>
#include <dev/pci/pcireg.h>

#include <machine/resource.h>

#include <machine/vmm.h>
#include <machine/vmm_dev.h>

#include "vmm_lapic.h"
#include "vmm_ktr.h"

#include "iommu.h"
#include "ppt.h"

#define	MAX_PPTDEVS	(sizeof(pptdevs) / sizeof(pptdevs[0]))
#define	MAX_MMIOSEGS	(PCIR_MAX_BAR_0 + 1)
#define	MAX_MSIMSGS	32

MALLOC_DEFINE(M_PPTMSIX, "pptmsix", "Passthru MSI-X resources");

struct pptintr_arg {				/* pptintr(pptintr_arg) */
	struct pptdev	*pptdev;
	int		vec;
	int 		vcpu;
};

static struct pptdev {
	device_t	dev;
	struct vm	*vm;			/* owner of this device */
	struct vm_memory_segment mmio[MAX_MMIOSEGS];
	struct {
		int	num_msgs;		/* guest state */
		int	vector;
		int	vcpu;

		int	startrid;		/* host state */
		struct resource *res[MAX_MSIMSGS];
		void	*cookie[MAX_MSIMSGS];
		struct pptintr_arg arg[MAX_MSIMSGS];
	} msi;

	struct {
		int num_msgs;
		int startrid;
		int msix_table_rid;
		struct resource *msix_table_res;
		struct resource **res;
		void **cookie;
		struct pptintr_arg *arg;
	} msix;
} pptdevs[32];

static int num_pptdevs;

static int
ppt_probe(device_t dev)
{
	int bus, slot, func;
	struct pci_devinfo *dinfo;

	dinfo = (struct pci_devinfo *)device_get_ivars(dev);

	bus = pci_get_bus(dev);
	slot = pci_get_slot(dev);
	func = pci_get_function(dev);

	/*
	 * To qualify as a pci passthrough device a device must:
	 * - be allowed by administrator to be used in this role
	 * - be an endpoint device
	 */
	if (vmm_is_pptdev(bus, slot, func) &&
	    (dinfo->cfg.hdrtype & PCIM_HDRTYPE) == PCIM_HDRTYPE_NORMAL)
		return (0);
	else
		return (ENXIO);
}

static int
ppt_attach(device_t dev)
{
	int n;

	if (num_pptdevs >= MAX_PPTDEVS) {
		printf("ppt_attach: maximum number of pci passthrough devices "
		       "exceeded\n");
		return (ENXIO);
	}

	n = num_pptdevs++;
	pptdevs[n].dev = dev;

	if (bootverbose)
		device_printf(dev, "attached\n");

	return (0);
}

static int
ppt_detach(device_t dev)
{
	/*
	 * XXX check whether there are any pci passthrough devices assigned
	 * to guests before we allow this driver to detach.
	 */

	return (0);
}

static device_method_t ppt_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		ppt_probe),
	DEVMETHOD(device_attach,	ppt_attach),
	DEVMETHOD(device_detach,	ppt_detach),
	{0, 0}
};

static devclass_t ppt_devclass;
DEFINE_CLASS_0(ppt, ppt_driver, ppt_methods, 0);
DRIVER_MODULE(ppt, pci, ppt_driver, ppt_devclass, NULL, NULL);

static struct pptdev *
ppt_find(int bus, int slot, int func)
{
	device_t dev;
	int i, b, s, f;

	for (i = 0; i < num_pptdevs; i++) {
		dev = pptdevs[i].dev;
		b = pci_get_bus(dev);
		s = pci_get_slot(dev);
		f = pci_get_function(dev);
		if (bus == b && slot == s && func == f)
			return (&pptdevs[i]);
	}
	return (NULL);
}

static void
ppt_unmap_mmio(struct vm *vm, struct pptdev *ppt)
{
	int i;
	struct vm_memory_segment *seg;

	for (i = 0; i < MAX_MMIOSEGS; i++) {
		seg = &ppt->mmio[i];
		if (seg->len == 0)
			continue;
		(void)vm_unmap_mmio(vm, seg->gpa, seg->len);
		bzero(seg, sizeof(struct vm_memory_segment));
	}
}

static void
ppt_teardown_msi(struct pptdev *ppt)
{
	int i, rid;
	void *cookie;
	struct resource *res;

	if (ppt->msi.num_msgs == 0)
		return;

	for (i = 0; i < ppt->msi.num_msgs; i++) {
		rid = ppt->msi.startrid + i;
		res = ppt->msi.res[i];
		cookie = ppt->msi.cookie[i];

		if (cookie != NULL)
			bus_teardown_intr(ppt->dev, res, cookie);

		if (res != NULL)
			bus_release_resource(ppt->dev, SYS_RES_IRQ, rid, res);
		
		ppt->msi.res[i] = NULL;
		ppt->msi.cookie[i] = NULL;
	}

	if (ppt->msi.startrid == 1)
		pci_release_msi(ppt->dev);

	ppt->msi.num_msgs = 0;
}

static void 
ppt_teardown_msix_intr(struct pptdev *ppt, int idx)
{
	int rid;
	struct resource *res;
	void *cookie;

	rid = ppt->msix.startrid + idx;
	res = ppt->msix.res[idx];
	cookie = ppt->msix.cookie[idx];

	if (cookie != NULL) 
		bus_teardown_intr(ppt->dev, res, cookie);

	if (res != NULL) 
		bus_release_resource(ppt->dev, SYS_RES_IRQ, rid, res);

	ppt->msix.res[idx] = NULL;
	ppt->msix.cookie[idx] = NULL;
}

static void 
ppt_teardown_msix(struct pptdev *ppt)
{
	int i, error;

	if (ppt->msix.num_msgs == 0) 
		return;

	for (i = 0; i < ppt->msix.num_msgs; i++) 
		ppt_teardown_msix_intr(ppt, i);

	if (ppt->msix.msix_table_res) {
		bus_release_resource(ppt->dev, SYS_RES_MEMORY, 
				     ppt->msix.msix_table_rid,
				     ppt->msix.msix_table_res);
		ppt->msix.msix_table_res = NULL;
		ppt->msix.msix_table_rid = 0;
	}

	free(ppt->msix.res, M_PPTMSIX);
	free(ppt->msix.cookie, M_PPTMSIX);
	free(ppt->msix.arg, M_PPTMSIX);

	error = pci_release_msi(ppt->dev);
	if (error) 
		printf("ppt_teardown_msix: Failed to release MSI-X resources (error %i)\n", error);

	ppt->msix.num_msgs = 0;
}

int
ppt_assign_device(struct vm *vm, int bus, int slot, int func)
{
	struct pptdev *ppt;

	ppt = ppt_find(bus, slot, func);
	if (ppt != NULL) {
		/*
		 * If this device is owned by a different VM then we
		 * cannot change its owner.
		 */
		if (ppt->vm != NULL && ppt->vm != vm)
			return (EBUSY);

		ppt->vm = vm;
		iommu_add_device(vm_iommu_domain(vm), bus, slot, func);
		return (0);
	}
	return (ENOENT);
}

int
ppt_unassign_device(struct vm *vm, int bus, int slot, int func)
{
	struct pptdev *ppt;

	ppt = ppt_find(bus, slot, func);
	if (ppt != NULL) {
		/*
		 * If this device is not owned by this 'vm' then bail out.
		 */
		if (ppt->vm != vm)
			return (EBUSY);
		ppt_unmap_mmio(vm, ppt);
		ppt_teardown_msi(ppt);
		ppt_teardown_msix(ppt);
		iommu_remove_device(vm_iommu_domain(vm), bus, slot, func);
		ppt->vm = NULL;
		return (0);
	}
	return (ENOENT);
}

int
ppt_unassign_all(struct vm *vm)
{
	int i, bus, slot, func;
	device_t dev;

	for (i = 0; i < num_pptdevs; i++) {
		if (pptdevs[i].vm == vm) {
			dev = pptdevs[i].dev;
			bus = pci_get_bus(dev);
			slot = pci_get_slot(dev);
			func = pci_get_function(dev);
			ppt_unassign_device(vm, bus, slot, func);
		}
	}

	return (0);
}

int
ppt_map_mmio(struct vm *vm, int bus, int slot, int func,
	     vm_paddr_t gpa, size_t len, vm_paddr_t hpa)
{
	int i, error;
	struct vm_memory_segment *seg;
	struct pptdev *ppt;

	ppt = ppt_find(bus, slot, func);
	if (ppt != NULL) {
		if (ppt->vm != vm)
			return (EBUSY);

		for (i = 0; i < MAX_MMIOSEGS; i++) {
			seg = &ppt->mmio[i];
			if (seg->len == 0) {
				error = vm_map_mmio(vm, gpa, len, hpa);
				if (error == 0) {
					seg->gpa = gpa;
					seg->len = len;
					seg->hpa = hpa;
				}
				return (error);
			}
		}
		return (ENOSPC);
	}
	return (ENOENT);
}

static int
pptintr(void *arg)
{
	int vec;
	struct pptdev *ppt;
	struct pptintr_arg *pptarg;
	
	pptarg = arg;
	ppt = pptarg->pptdev;
	vec = pptarg->vec;

	if (ppt->vm != NULL)
		(void) lapic_set_intr(ppt->vm, pptarg->vcpu, vec);
	else {
		/*
		 * XXX
		 * This is not expected to happen - panic?
		 */
	}

	/*
	 * For legacy interrupts give other filters a chance in case
	 * the interrupt was not generated by the passthrough device.
	 */
	if (ppt->msi.startrid == 0)
		return (FILTER_STRAY);
	else
		return (FILTER_HANDLED);
}

/*
 * XXX
 * When we try to free the MSI resource the kernel will bind the thread to
 * the host cpu was originally handling the MSI. The function freeing the
 * MSI vector (apic_free_vector()) will panic the kernel if the thread
 * is already bound to a cpu.
 * 
 * So, we temporarily unbind the vcpu thread before freeing the MSI resource.
 */
static void
PPT_TEARDOWN_MSI(struct vm *vm, int vcpu, struct pptdev *ppt)
{
	int pincpu = -1;

	vm_get_pinning(vm, vcpu, &pincpu);

	if (pincpu >= 0)
		vm_set_pinning(vm, vcpu, -1);

	ppt_teardown_msi(ppt);

	if (pincpu >= 0)
		vm_set_pinning(vm, vcpu, pincpu);
}

int
ppt_setup_msi(struct vm *vm, int vcpu, int bus, int slot, int func,
	      int destcpu, int vector, int numvec)
{
	int i, rid, flags;
	int msi_count, startrid, error, tmp;
	struct pptdev *ppt;

	if ((destcpu >= VM_MAXCPU || destcpu < 0) ||
	    (vector < 0 || vector > 255) ||
	    (numvec < 0 || numvec > MAX_MSIMSGS))
		return (EINVAL);

	ppt = ppt_find(bus, slot, func);
	if (ppt == NULL)
		return (ENOENT);
	if (ppt->vm != vm)		/* Make sure we own this device */
		return (EBUSY);

	/* Free any allocated resources */
	PPT_TEARDOWN_MSI(vm, vcpu, ppt);

	if (numvec == 0)		/* nothing more to do */
		return (0);

	flags = RF_ACTIVE;
	msi_count = pci_msi_count(ppt->dev);
	if (msi_count == 0) {
		startrid = 0;		/* legacy interrupt */
		msi_count = 1;
		flags |= RF_SHAREABLE;
	} else
		startrid = 1;		/* MSI */

	/*
	 * The device must be capable of supporting the number of vectors
	 * the guest wants to allocate.
	 */
	if (numvec > msi_count)
		return (EINVAL);

	/*
	 * Make sure that we can allocate all the MSI vectors that are needed
	 * by the guest.
	 */
	if (startrid == 1) {
		tmp = numvec;
		error = pci_alloc_msi(ppt->dev, &tmp);
		if (error)
			return (error);
		else if (tmp != numvec) {
			pci_release_msi(ppt->dev);
			return (ENOSPC);
		} else {
			/* success */
		}
	}
	
	ppt->msi.vector = vector;
	ppt->msi.vcpu = destcpu;
	ppt->msi.startrid = startrid;

	/*
	 * Allocate the irq resource and attach it to the interrupt handler.
	 */
	for (i = 0; i < numvec; i++) {
		ppt->msi.num_msgs = i + 1;
		ppt->msi.cookie[i] = NULL;

		rid = startrid + i;
		ppt->msi.res[i] = bus_alloc_resource_any(ppt->dev, SYS_RES_IRQ,
							 &rid, flags);
		if (ppt->msi.res[i] == NULL)
			break;

		ppt->msi.arg[i].pptdev = ppt;
		ppt->msi.arg[i].vec = vector + i;

		error = bus_setup_intr(ppt->dev, ppt->msi.res[i],
				       INTR_TYPE_NET | INTR_MPSAFE,
				       pptintr, NULL, &ppt->msi.arg[i],
				       &ppt->msi.cookie[i]);
		if (error != 0)
			break;
	}
	
	if (i < numvec) {
		PPT_TEARDOWN_MSI(vm, vcpu, ppt);
		return (ENXIO);
	}

	return (0);
}

int
ppt_setup_msix(struct vm *vm, int vcpu, int bus, int slot, int func,
	       int idx, uint32_t msg, uint32_t vector_control, uint64_t addr)
{
	struct pptdev *ppt;
	struct pci_devinfo *dinfo;
	int numvec, vector_count, rid, error;
	size_t res_size, cookie_size, arg_size;

	ppt = ppt_find(bus, slot, func);
	if (ppt == NULL)
		return (ENOENT);
	if (ppt->vm != vm)		/* Make sure we own this device */
		return (EBUSY);

	dinfo = device_get_ivars(ppt->dev);
	if (!dinfo) 
		return (ENXIO);

	/* 
	 * First-time configuration:
	 * 	Allocate the MSI-X table
	 *	Allocate the IRQ resources
	 *	Set up some variables in ppt->msix
	 */
	if (!ppt->msix.msix_table_res) {
		ppt->msix.res = NULL;
		ppt->msix.cookie = NULL;
		ppt->msix.arg = NULL;

		rid = dinfo->cfg.msix.msix_table_bar;
		ppt->msix.msix_table_res = bus_alloc_resource_any(ppt->dev, SYS_RES_MEMORY,
								  &rid, RF_ACTIVE);
		if (ppt->msix.msix_table_res == NULL) 
			return (ENOSPC);

		ppt->msix.msix_table_rid = rid;

		vector_count = numvec = pci_msix_count(ppt->dev);

		error = pci_alloc_msix(ppt->dev, &numvec);
		if (error) 
			return (error);
		else if (vector_count != numvec) {
			pci_release_msi(ppt->dev);
			return (ENOSPC);
		} 

		ppt->msix.num_msgs = numvec;

		ppt->msix.startrid = 1;

		res_size = numvec * sizeof(ppt->msix.res[0]);
		cookie_size = numvec * sizeof(ppt->msix.cookie[0]);
		arg_size = numvec * sizeof(ppt->msix.arg[0]);

		ppt->msix.res = malloc(res_size, M_PPTMSIX, M_WAITOK);
		ppt->msix.cookie = malloc(cookie_size, M_PPTMSIX, M_WAITOK);
		ppt->msix.arg = malloc(arg_size, M_PPTMSIX, M_WAITOK);
		if (ppt->msix.res == NULL || ppt->msix.cookie == NULL || 
		    ppt->msix.arg == NULL) {
			ppt_teardown_msix(ppt);
			return (ENOSPC);
		}
		bzero(ppt->msix.res, res_size);
		bzero(ppt->msix.cookie, cookie_size);
		bzero(ppt->msix.arg, arg_size);
	}

	if ((vector_control & PCIM_MSIX_VCTRL_MASK) == 0) {
		/* Tear down the IRQ if it's already set up */
		ppt_teardown_msix_intr(ppt, idx);

		/* Allocate the IRQ resource */
		ppt->msix.cookie[idx] = NULL;
		rid = ppt->msix.startrid + idx;
		ppt->msix.res[idx] = bus_alloc_resource_any(ppt->dev, SYS_RES_IRQ,
							    &rid, RF_ACTIVE);
		if (ppt->msix.res[idx] == NULL)
			return (ENXIO);
	
		ppt->msix.arg[idx].pptdev = ppt;
		ppt->msix.arg[idx].vec = msg;
		ppt->msix.arg[idx].vcpu = (addr >> 12) & 0xFF;
	
		/* Setup the MSI-X interrupt */
		error = bus_setup_intr(ppt->dev, ppt->msix.res[idx],
				       INTR_TYPE_NET | INTR_MPSAFE,
				       pptintr, NULL, &ppt->msix.arg[idx],
				       &ppt->msix.cookie[idx]);
	
		if (error != 0) {
			bus_teardown_intr(ppt->dev, ppt->msix.res[idx], ppt->msix.cookie[idx]);
			bus_release_resource(ppt->dev, SYS_RES_IRQ, rid, ppt->msix.res[idx]);
			ppt->msix.cookie[idx] = NULL;
			ppt->msix.res[idx] = NULL;
			return (ENXIO);
		}
	} else {
		/* Masked, tear it down if it's already been set up */
		ppt_teardown_msix_intr(ppt, idx);
	}

	return (0);
}

