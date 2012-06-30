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
#include <sys/smp.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/pcpu.h>
#include <sys/proc.h>

#include <vm/vm.h>
#include <vm/pmap.h>

#include <machine/psl.h>
#include <machine/cpufunc.h>
#include <machine/md_var.h>
#include <machine/pmap.h>
#include <machine/segments.h>
#include <machine/specialreg.h>
#include <machine/vmparam.h>

#include <machine/vmm.h>
#include "vmm_lapic.h"
#include "vmm_msr.h"
#include "vmm_ktr.h"
#include "vmm_stat.h"

#include "vmx_msr.h"
#include "ept.h"
#include "vmx_cpufunc.h"
#include "vmx.h"
#include "x86.h"
#include "vmx_controls.h"

#define	CR4_VMXE	(1UL << 13)

#define	PINBASED_CTLS_ONE_SETTING					\
	(PINBASED_EXTINT_EXITING	|				\
	 PINBASED_NMI_EXITING		|				\
	 PINBASED_VIRTUAL_NMI)
#define	PINBASED_CTLS_ZERO_SETTING	0

#define PROCBASED_CTLS_WINDOW_SETTING					\
	(PROCBASED_INT_WINDOW_EXITING	|				\
	 PROCBASED_NMI_WINDOW_EXITING)

#define	PROCBASED_CTLS_ONE_SETTING 					\
	(PROCBASED_SECONDARY_CONTROLS	|				\
	 PROCBASED_IO_EXITING		|				\
	 PROCBASED_MSR_BITMAPS		|				\
	 PROCBASED_CTLS_WINDOW_SETTING)
#define	PROCBASED_CTLS_ZERO_SETTING	\
	(PROCBASED_CR3_LOAD_EXITING |	\
	PROCBASED_CR3_STORE_EXITING |	\
	PROCBASED_IO_BITMAPS)

#define	PROCBASED_CTLS2_ONE_SETTING	PROCBASED2_ENABLE_EPT
#define	PROCBASED_CTLS2_ZERO_SETTING	0

#define VM_EXIT_CTLS_ONE_SETTING_NO_PAT					\
	(VM_EXIT_HOST_LMA			|			\
	VM_EXIT_SAVE_EFER			|			\
	VM_EXIT_LOAD_EFER)

#define	VM_EXIT_CTLS_ONE_SETTING					\
	(VM_EXIT_CTLS_ONE_SETTING_NO_PAT       	|			\
	VM_EXIT_SAVE_PAT			|			\
	VM_EXIT_LOAD_PAT)
#define	VM_EXIT_CTLS_ZERO_SETTING	VM_EXIT_SAVE_DEBUG_CONTROLS

#define	VM_ENTRY_CTLS_ONE_SETTING_NO_PAT	VM_ENTRY_LOAD_EFER

#define	VM_ENTRY_CTLS_ONE_SETTING					\
	(VM_ENTRY_CTLS_ONE_SETTING_NO_PAT     	|			\
	VM_ENTRY_LOAD_PAT)
#define	VM_ENTRY_CTLS_ZERO_SETTING					\
	(VM_ENTRY_LOAD_DEBUG_CONTROLS		|			\
	VM_ENTRY_INTO_SMM			|			\
	VM_ENTRY_DEACTIVATE_DUAL_MONITOR)

#define	guest_msr_rw(vmx, msr) \
	msr_bitmap_change_access((vmx)->msr_bitmap, (msr), MSR_BITMAP_ACCESS_RW)

#define	HANDLED		1
#define	UNHANDLED	0

MALLOC_DEFINE(M_VMX, "vmx", "vmx");

extern  struct pcpu __pcpu[];

int vmxon_enabled[MAXCPU];
static char vmxon_region[MAXCPU][PAGE_SIZE] __aligned(PAGE_SIZE);

static uint32_t pinbased_ctls, procbased_ctls, procbased_ctls2;
static uint32_t exit_ctls, entry_ctls;

static uint64_t cr0_ones_mask, cr0_zeros_mask;
static uint64_t cr4_ones_mask, cr4_zeros_mask;

static volatile u_int nextvpid;

static int vmx_no_patmsr;

/*
 * Virtual NMI blocking conditions.
 *
 * Some processor implementations also require NMI to be blocked if
 * the STI_BLOCKING bit is set. It is possible to detect this at runtime
 * based on the (exit_reason,exit_qual) tuple being set to 
 * (EXIT_REASON_INVAL_VMCS, EXIT_QUAL_NMI_WHILE_STI_BLOCKING).
 *
 * We take the easy way out and also include STI_BLOCKING as one of the
 * gating items for vNMI injection.
 */
static uint64_t nmi_blocking_bits = VMCS_INTERRUPTIBILITY_MOVSS_BLOCKING |
				    VMCS_INTERRUPTIBILITY_NMI_BLOCKING |
				    VMCS_INTERRUPTIBILITY_STI_BLOCKING;

/*
 * Optional capabilities
 */
static int cap_halt_exit;
static int cap_pause_exit;
static int cap_unrestricted_guest;
static int cap_monitor_trap;
 
/* statistics */
static VMM_STAT_DEFINE(VCPU_MIGRATIONS, "vcpu migration across host cpus");
static VMM_STAT_DEFINE(VMEXIT_EXTINT, "vm exits due to external interrupt");

#ifdef KTR
static const char *
exit_reason_to_str(int reason)
{
	static char reasonbuf[32];

	switch (reason) {
	case EXIT_REASON_EXCEPTION:
		return "exception";
	case EXIT_REASON_EXT_INTR:
		return "extint";
	case EXIT_REASON_TRIPLE_FAULT:
		return "triplefault";
	case EXIT_REASON_INIT:
		return "init";
	case EXIT_REASON_SIPI:
		return "sipi";
	case EXIT_REASON_IO_SMI:
		return "iosmi";
	case EXIT_REASON_SMI:
		return "smi";
	case EXIT_REASON_INTR_WINDOW:
		return "intrwindow";
	case EXIT_REASON_NMI_WINDOW:
		return "nmiwindow";
	case EXIT_REASON_TASK_SWITCH:
		return "taskswitch";
	case EXIT_REASON_CPUID:
		return "cpuid";
	case EXIT_REASON_GETSEC:
		return "getsec";
	case EXIT_REASON_HLT:
		return "hlt";
	case EXIT_REASON_INVD:
		return "invd";
	case EXIT_REASON_INVLPG:
		return "invlpg";
	case EXIT_REASON_RDPMC:
		return "rdpmc";
	case EXIT_REASON_RDTSC:
		return "rdtsc";
	case EXIT_REASON_RSM:
		return "rsm";
	case EXIT_REASON_VMCALL:
		return "vmcall";
	case EXIT_REASON_VMCLEAR:
		return "vmclear";
	case EXIT_REASON_VMLAUNCH:
		return "vmlaunch";
	case EXIT_REASON_VMPTRLD:
		return "vmptrld";
	case EXIT_REASON_VMPTRST:
		return "vmptrst";
	case EXIT_REASON_VMREAD:
		return "vmread";
	case EXIT_REASON_VMRESUME:
		return "vmresume";
	case EXIT_REASON_VMWRITE:
		return "vmwrite";
	case EXIT_REASON_VMXOFF:
		return "vmxoff";
	case EXIT_REASON_VMXON:
		return "vmxon";
	case EXIT_REASON_CR_ACCESS:
		return "craccess";
	case EXIT_REASON_DR_ACCESS:
		return "draccess";
	case EXIT_REASON_INOUT:
		return "inout";
	case EXIT_REASON_RDMSR:
		return "rdmsr";
	case EXIT_REASON_WRMSR:
		return "wrmsr";
	case EXIT_REASON_INVAL_VMCS:
		return "invalvmcs";
	case EXIT_REASON_INVAL_MSR:
		return "invalmsr";
	case EXIT_REASON_MWAIT:
		return "mwait";
	case EXIT_REASON_MTF:
		return "mtf";
	case EXIT_REASON_MONITOR:
		return "monitor";
	case EXIT_REASON_PAUSE:
		return "pause";
	case EXIT_REASON_MCE:
		return "mce";
	case EXIT_REASON_TPR:
		return "tpr";
	case EXIT_REASON_APIC:
		return "apic";
	case EXIT_REASON_GDTR_IDTR:
		return "gdtridtr";
	case EXIT_REASON_LDTR_TR:
		return "ldtrtr";
	case EXIT_REASON_EPT_FAULT:
		return "eptfault";
	case EXIT_REASON_EPT_MISCONFIG:
		return "eptmisconfig";
	case EXIT_REASON_INVEPT:
		return "invept";
	case EXIT_REASON_RDTSCP:
		return "rdtscp";
	case EXIT_REASON_VMX_PREEMPT:
		return "vmxpreempt";
	case EXIT_REASON_INVVPID:
		return "invvpid";
	case EXIT_REASON_WBINVD:
		return "wbinvd";
	case EXIT_REASON_XSETBV:
		return "xsetbv";
	default:
		snprintf(reasonbuf, sizeof(reasonbuf), "%d", reason);
		return (reasonbuf);
	}
}

#ifdef SETJMP_TRACE
static const char *
vmx_setjmp_rc2str(int rc)
{
	switch (rc) {
	case VMX_RETURN_DIRECT:
		return "direct";
	case VMX_RETURN_LONGJMP:
		return "longjmp";
	case VMX_RETURN_VMRESUME:
		return "vmresume";
	case VMX_RETURN_VMLAUNCH:
		return "vmlaunch";
	default:
		return "unknown";
	}
}

#define	SETJMP_TRACE(vmx, vcpu, vmxctx, regname)			  \
	VMM_CTR1((vmx)->vm, (vcpu), "setjmp trace " #regname " 0x%016lx", \
		 (vmxctx)->regname)

static void
vmx_setjmp_trace(struct vmx *vmx, int vcpu, struct vmxctx *vmxctx, int rc)
{
	uint64_t host_rip, host_rsp;

	if (vmxctx != &vmx->ctx[vcpu])
		panic("vmx_setjmp_trace: invalid vmxctx %p; should be %p",
			vmxctx, &vmx->ctx[vcpu]);

	VMM_CTR1((vmx)->vm, (vcpu), "vmxctx = %p", vmxctx);
	VMM_CTR2((vmx)->vm, (vcpu), "setjmp return code %s(%d)",
		 vmx_setjmp_rc2str(rc), rc);

	host_rsp = host_rip = ~0;
	vmread(VMCS_HOST_RIP, &host_rip);
	vmread(VMCS_HOST_RSP, &host_rsp);
	VMM_CTR2((vmx)->vm, (vcpu), "vmcs host_rip 0x%016lx, host_rsp 0x%016lx",
		 host_rip, host_rsp);

	SETJMP_TRACE(vmx, vcpu, vmxctx, host_r15);
	SETJMP_TRACE(vmx, vcpu, vmxctx, host_r14);
	SETJMP_TRACE(vmx, vcpu, vmxctx, host_r13);
	SETJMP_TRACE(vmx, vcpu, vmxctx, host_r12);
	SETJMP_TRACE(vmx, vcpu, vmxctx, host_rbp);
	SETJMP_TRACE(vmx, vcpu, vmxctx, host_rsp);
	SETJMP_TRACE(vmx, vcpu, vmxctx, host_rbx);
	SETJMP_TRACE(vmx, vcpu, vmxctx, host_rip);

	SETJMP_TRACE(vmx, vcpu, vmxctx, guest_rdi);
	SETJMP_TRACE(vmx, vcpu, vmxctx, guest_rsi);
	SETJMP_TRACE(vmx, vcpu, vmxctx, guest_rdx);
	SETJMP_TRACE(vmx, vcpu, vmxctx, guest_rcx);
	SETJMP_TRACE(vmx, vcpu, vmxctx, guest_r8);
	SETJMP_TRACE(vmx, vcpu, vmxctx, guest_r9);
	SETJMP_TRACE(vmx, vcpu, vmxctx, guest_rax);
	SETJMP_TRACE(vmx, vcpu, vmxctx, guest_rbx);
	SETJMP_TRACE(vmx, vcpu, vmxctx, guest_rbp);
	SETJMP_TRACE(vmx, vcpu, vmxctx, guest_r10);
	SETJMP_TRACE(vmx, vcpu, vmxctx, guest_r11);
	SETJMP_TRACE(vmx, vcpu, vmxctx, guest_r12);
	SETJMP_TRACE(vmx, vcpu, vmxctx, guest_r13);
	SETJMP_TRACE(vmx, vcpu, vmxctx, guest_r14);
	SETJMP_TRACE(vmx, vcpu, vmxctx, guest_r15);
	SETJMP_TRACE(vmx, vcpu, vmxctx, guest_cr2);
}
#endif
#else
static void __inline
vmx_setjmp_trace(struct vmx *vmx, int vcpu, struct vmxctx *vmxctx, int rc)
{
	return;
}
#endif	/* KTR */

u_long
vmx_fix_cr0(u_long cr0)
{

	return ((cr0 | cr0_ones_mask) & ~cr0_zeros_mask);
}

u_long
vmx_fix_cr4(u_long cr4)
{

	return ((cr4 | cr4_ones_mask) & ~cr4_zeros_mask);
}

static void
msr_save_area_init(struct msr_entry *g_area, int *g_count)
{
	int cnt;

	static struct msr_entry guest_msrs[] = {
		{ MSR_KGSBASE, 0, 0 },
	};

	cnt = sizeof(guest_msrs) / sizeof(guest_msrs[0]);
	if (cnt > GUEST_MSR_MAX_ENTRIES)
		panic("guest msr save area overrun");
	bcopy(guest_msrs, g_area, sizeof(guest_msrs));
	*g_count = cnt;
}

static void
vmx_disable(void *arg __unused)
{
	struct invvpid_desc invvpid_desc = { 0 };
	struct invept_desc invept_desc = { 0 };

	if (vmxon_enabled[curcpu]) {
		/*
		 * See sections 25.3.3.3 and 25.3.3.4 in Intel Vol 3b.
		 *
		 * VMXON or VMXOFF are not required to invalidate any TLB
		 * caching structures. This prevents potential retention of
		 * cached information in the TLB between distinct VMX episodes.
		 */
		invvpid(INVVPID_TYPE_ALL_CONTEXTS, invvpid_desc);
		invept(INVEPT_TYPE_ALL_CONTEXTS, invept_desc);
		vmxoff();
	}
	load_cr4(rcr4() & ~CR4_VMXE);
}

static int
vmx_cleanup(void)
{

	smp_rendezvous(NULL, vmx_disable, NULL, NULL);

	return (0);
}

static void
vmx_enable(void *arg __unused)
{
	int error;

	load_cr4(rcr4() | CR4_VMXE);

	*(uint32_t *)vmxon_region[curcpu] = vmx_revision();
	error = vmxon(vmxon_region[curcpu]);
	if (error == 0)
		vmxon_enabled[curcpu] = 1;
}

static int
vmx_init(void)
{
	int error;
	uint64_t fixed0, fixed1;
	uint32_t tmp;

	/* CPUID.1:ECX[bit 5] must be 1 for processor to support VMX */
	if (!(cpu_feature2 & CPUID2_VMX)) {
		printf("vmx_init: processor does not support VMX operation\n");
		return (ENXIO);
	}

	/* Check support for primary processor-based VM-execution controls */
	error = vmx_set_ctlreg(MSR_VMX_PROCBASED_CTLS,
			       MSR_VMX_TRUE_PROCBASED_CTLS,
			       PROCBASED_CTLS_ONE_SETTING,
			       PROCBASED_CTLS_ZERO_SETTING, &procbased_ctls);
	if (error) {
		printf("vmx_init: processor does not support desired primary "
		       "processor-based controls\n");
		return (error);
	}

	/* Clear the processor-based ctl bits that are set on demand */
	procbased_ctls &= ~PROCBASED_CTLS_WINDOW_SETTING;

	/* Check support for secondary processor-based VM-execution controls */
	error = vmx_set_ctlreg(MSR_VMX_PROCBASED_CTLS2,
			       MSR_VMX_PROCBASED_CTLS2,
			       PROCBASED_CTLS2_ONE_SETTING,
			       PROCBASED_CTLS2_ZERO_SETTING, &procbased_ctls2);
	if (error) {
		printf("vmx_init: processor does not support desired secondary "
		       "processor-based controls\n");
		return (error);
	}

	/* Check support for VPID */
	error = vmx_set_ctlreg(MSR_VMX_PROCBASED_CTLS2, MSR_VMX_PROCBASED_CTLS2,
			       PROCBASED2_ENABLE_VPID, 0, &tmp);
	if (error == 0)
		procbased_ctls2 |= PROCBASED2_ENABLE_VPID;

	/* Check support for pin-based VM-execution controls */
	error = vmx_set_ctlreg(MSR_VMX_PINBASED_CTLS,
			       MSR_VMX_TRUE_PINBASED_CTLS,
			       PINBASED_CTLS_ONE_SETTING,
			       PINBASED_CTLS_ZERO_SETTING, &pinbased_ctls);
	if (error) {
		printf("vmx_init: processor does not support desired "
		       "pin-based controls\n");
		return (error);
	}

	/* Check support for VM-exit controls */
	error = vmx_set_ctlreg(MSR_VMX_EXIT_CTLS, MSR_VMX_TRUE_EXIT_CTLS,
			       VM_EXIT_CTLS_ONE_SETTING,
			       VM_EXIT_CTLS_ZERO_SETTING,
			       &exit_ctls);
	if (error) {
		/* Try again without the PAT MSR bits */
		error = vmx_set_ctlreg(MSR_VMX_EXIT_CTLS,
				       MSR_VMX_TRUE_EXIT_CTLS,
				       VM_EXIT_CTLS_ONE_SETTING_NO_PAT,
				       VM_EXIT_CTLS_ZERO_SETTING,
				       &exit_ctls);
		if (error) {
			printf("vmx_init: processor does not support desired "
			       "exit controls\n");
			return (error);
		} else {
			if (bootverbose)
				printf("vmm: PAT MSR access not supported\n");
			guest_msr_valid(MSR_PAT);
			vmx_no_patmsr = 1;
		}
	}

	/* Check support for VM-entry controls */
	if (!vmx_no_patmsr) {
		error = vmx_set_ctlreg(MSR_VMX_ENTRY_CTLS,
				       MSR_VMX_TRUE_ENTRY_CTLS,
				       VM_ENTRY_CTLS_ONE_SETTING,
				       VM_ENTRY_CTLS_ZERO_SETTING,
				       &entry_ctls);
	} else {
		error = vmx_set_ctlreg(MSR_VMX_ENTRY_CTLS,
				       MSR_VMX_TRUE_ENTRY_CTLS,
				       VM_ENTRY_CTLS_ONE_SETTING_NO_PAT,
				       VM_ENTRY_CTLS_ZERO_SETTING,
				       &entry_ctls);
	}

	if (error) {
		printf("vmx_init: processor does not support desired "
		       "entry controls\n");
		       return (error);
	}

	/*
	 * Check support for optional features by testing them
	 * as individual bits
	 */
	cap_halt_exit = (vmx_set_ctlreg(MSR_VMX_PROCBASED_CTLS,
					MSR_VMX_TRUE_PROCBASED_CTLS,
					PROCBASED_HLT_EXITING, 0,
					&tmp) == 0);

	cap_monitor_trap = (vmx_set_ctlreg(MSR_VMX_PROCBASED_CTLS,
					MSR_VMX_PROCBASED_CTLS,
					PROCBASED_MTF, 0,
					&tmp) == 0);

	cap_pause_exit = (vmx_set_ctlreg(MSR_VMX_PROCBASED_CTLS,
					 MSR_VMX_TRUE_PROCBASED_CTLS,
					 PROCBASED_PAUSE_EXITING, 0,
					 &tmp) == 0);

	cap_unrestricted_guest = (vmx_set_ctlreg(MSR_VMX_PROCBASED_CTLS2,
					MSR_VMX_PROCBASED_CTLS2,
					PROCBASED2_UNRESTRICTED_GUEST, 0,
				        &tmp) == 0);

	/* Initialize EPT */
	error = ept_init();
	if (error) {
		printf("vmx_init: ept initialization failed (%d)\n", error);
		return (error);
	}

	/*
	 * Stash the cr0 and cr4 bits that must be fixed to 0 or 1
	 */
	fixed0 = rdmsr(MSR_VMX_CR0_FIXED0);
	fixed1 = rdmsr(MSR_VMX_CR0_FIXED1);
	cr0_ones_mask = fixed0 & fixed1;
	cr0_zeros_mask = ~fixed0 & ~fixed1;

	/*
	 * CR0_PE and CR0_PG can be set to zero in VMX non-root operation
	 * if unrestricted guest execution is allowed.
	 */
	if (cap_unrestricted_guest)
		cr0_ones_mask &= ~(CR0_PG | CR0_PE);

	/*
	 * Do not allow the guest to set CR0_NW or CR0_CD.
	 */
	cr0_zeros_mask |= (CR0_NW | CR0_CD);

	fixed0 = rdmsr(MSR_VMX_CR4_FIXED0);
	fixed1 = rdmsr(MSR_VMX_CR4_FIXED1);
	cr4_ones_mask = fixed0 & fixed1;
	cr4_zeros_mask = ~fixed0 & ~fixed1;

	/* enable VMX operation */
	smp_rendezvous(NULL, vmx_enable, NULL, NULL);

	return (0);
}

/*
 * If this processor does not support VPIDs then simply return 0.
 *
 * Otherwise generate the next value of VPID to use. Any value is alright
 * as long as it is non-zero.
 *
 * We always execute in VMX non-root context with EPT enabled. Thus all
 * combined mappings are tagged with the (EP4TA, VPID, PCID) tuple. This
 * in turn means that multiple VMs can share the same VPID as long as
 * they have distinct EPT page tables.
 *
 * XXX
 * We should optimize this so that it returns VPIDs that are not in
 * use. Then we will not unnecessarily invalidate mappings in
 * vmx_set_pcpu_defaults() just because two or more vcpus happen to
 * use the same 'vpid'.
 */
static uint16_t
vmx_vpid(void)
{
	uint16_t vpid = 0;

	if ((procbased_ctls2 & PROCBASED2_ENABLE_VPID) != 0) {
		do {
			vpid = atomic_fetchadd_int(&nextvpid, 1);
		} while (vpid == 0);
	}

	return (vpid);
}

static int
vmx_setup_cr0_shadow(struct vmcs *vmcs)
{
	int error;
	uint64_t mask, shadow;

	mask = cr0_ones_mask | cr0_zeros_mask;
	error = vmcs_setreg(vmcs, VMCS_IDENT(VMCS_CR0_MASK), mask);
	if (error)
		return (error);

	shadow = cr0_ones_mask;
	error = vmcs_setreg(vmcs, VMCS_IDENT(VMCS_CR0_SHADOW), shadow);
	if (error)
		return (error);

	return (0);
}

static void *
vmx_vminit(struct vm *vm)
{
	uint16_t vpid;
	int i, error, guest_msr_count;
	struct vmx *vmx;

	vmx = malloc(sizeof(struct vmx), M_VMX, M_WAITOK | M_ZERO);
	if ((uintptr_t)vmx & PAGE_MASK) {
		panic("malloc of struct vmx not aligned on %d byte boundary",
		      PAGE_SIZE);
	}
	vmx->vm = vm;

	/*
	 * Clean up EPTP-tagged guest physical and combined mappings
	 *
	 * VMX transitions are not required to invalidate any guest physical
	 * mappings. So, it may be possible for stale guest physical mappings
	 * to be present in the processor TLBs.
	 *
	 * Combined mappings for this EP4TA are also invalidated for all VPIDs.
	 */
	ept_invalidate_mappings(vtophys(vmx->pml4ept));

	msr_bitmap_initialize(vmx->msr_bitmap);

	/*
	 * It is safe to allow direct access to MSR_GSBASE and MSR_FSBASE.
	 * The guest FSBASE and GSBASE are saved and restored during
	 * vm-exit and vm-entry respectively. The host FSBASE and GSBASE are
	 * always restored from the vmcs host state area on vm-exit.
	 *
	 * Guest KGSBASE is saved and restored in the guest MSR save area.
	 * Host KGSBASE is restored before returning to userland from the pcb.
	 * There will be a window of time when we are executing in the host
	 * kernel context with a value of KGSBASE from the guest. This is ok
	 * because the value of KGSBASE is inconsequential in kernel context.
	 *
	 * MSR_EFER is saved and restored in the guest VMCS area on a
	 * VM exit and entry respectively. It is also restored from the
	 * host VMCS area on a VM exit.
	 */
	if (guest_msr_rw(vmx, MSR_GSBASE) ||
	    guest_msr_rw(vmx, MSR_FSBASE) ||
	    guest_msr_rw(vmx, MSR_KGSBASE) ||
	    guest_msr_rw(vmx, MSR_EFER))
		panic("vmx_vminit: error setting guest msr access");

	/*
	 * MSR_PAT is saved and restored in the guest VMCS are on a VM exit
	 * and entry respectively. It is also restored from the host VMCS
	 * area on a VM exit. However, if running on a system with no
	 * MSR_PAT save/restore support, leave access disabled so accesses
	 * will be trapped.
	 */
	if (!vmx_no_patmsr && guest_msr_rw(vmx, MSR_PAT))
		panic("vmx_vminit: error setting guest pat msr access");

	for (i = 0; i < VM_MAXCPU; i++) {
		vmx->vmcs[i].identifier = vmx_revision();
		error = vmclear(&vmx->vmcs[i]);
		if (error != 0) {
			panic("vmx_vminit: vmclear error %d on vcpu %d\n",
			      error, i);
		}

		vpid = vmx_vpid();

		error = vmcs_set_defaults(&vmx->vmcs[i],
					  (u_long)vmx_longjmp,
					  (u_long)&vmx->ctx[i],
					  vtophys(vmx->pml4ept),
					  pinbased_ctls,
					  procbased_ctls,
					  procbased_ctls2,
					  exit_ctls, entry_ctls,
					  vtophys(vmx->msr_bitmap),
					  vpid);

		if (error != 0)
			panic("vmx_vminit: vmcs_set_defaults error %d", error);

		vmx->cap[i].set = 0;
		vmx->cap[i].proc_ctls = procbased_ctls;

		vmx->state[i].request_nmi = 0;
		vmx->state[i].lastcpu = -1;
		vmx->state[i].vpid = vpid;

		msr_save_area_init(vmx->guest_msrs[i], &guest_msr_count);

		error = vmcs_set_msr_save(&vmx->vmcs[i],
					  vtophys(vmx->guest_msrs[i]),
					  guest_msr_count);
		if (error != 0)
			panic("vmcs_set_msr_save error %d", error);

		error = vmx_setup_cr0_shadow(&vmx->vmcs[i]);
	}

	return (vmx);
}

static int
vmx_handle_cpuid(int vcpu, struct vmxctx *vmxctx)
{
	int handled, func;
	
	func = vmxctx->guest_rax;

	handled = x86_emulate_cpuid((uint32_t*)(&vmxctx->guest_rax),
	    (uint32_t*)(&vmxctx->guest_rbx), (uint32_t*)(&vmxctx->guest_rcx),
	    (uint32_t*)(&vmxctx->guest_rdx), vcpu);
#if 0
	printf("%s: func %x rax %lx rbx %lx rcx %lx rdx %lx handled %d\n",
		__func__, func, vmxctx->guest_rax, vmxctx->guest_rbx,
		vmxctx->guest_rcx, vmxctx->guest_rdx, handled);
#endif

	return (handled);
}

static __inline void
vmx_run_trace(struct vmx *vmx, int vcpu)
{
#ifdef KTR
	VMM_CTR1(vmx->vm, vcpu, "Resume execution at 0x%0lx", vmcs_guest_rip());
#endif
}

static __inline void
vmx_exit_trace(struct vmx *vmx, int vcpu, uint64_t rip, uint32_t exit_reason,
	       int handled, int astpending)
{
#ifdef KTR
	VMM_CTR3(vmx->vm, vcpu, "%s %s vmexit at 0x%0lx",
		 handled ? "handled" : "unhandled",
		 exit_reason_to_str(exit_reason), rip);

	if (astpending)
		VMM_CTR0(vmx->vm, vcpu, "astpending");
#endif
}

static int
vmx_set_pcpu_defaults(struct vmx *vmx, int vcpu)
{
	int error, lastcpu;
	struct vmxstate *vmxstate;
	struct invvpid_desc invvpid_desc = { 0 };

	vmxstate = &vmx->state[vcpu];
	lastcpu = vmxstate->lastcpu;
	vmxstate->lastcpu = curcpu;

	if (lastcpu == curcpu) {
		error = 0;
		goto done;
	}

	vmm_stat_incr(vmx->vm, vcpu, VCPU_MIGRATIONS, 1);

	error = vmwrite(VMCS_HOST_TR_BASE, (u_long)PCPU_GET(tssp));
	if (error != 0)
		goto done;

	error = vmwrite(VMCS_HOST_GDTR_BASE, (u_long)&gdt[NGDT * curcpu]);
	if (error != 0)
		goto done;

	error = vmwrite(VMCS_HOST_GS_BASE, (u_long)&__pcpu[curcpu]);
	if (error != 0)
		goto done;

	/*
	 * If we are using VPIDs then invalidate all mappings tagged with 'vpid'
	 *
	 * We do this because this vcpu was executing on a different host
	 * cpu when it last ran. We do not track whether it invalidated
	 * mappings associated with its 'vpid' during that run. So we must
	 * assume that the mappings associated with 'vpid' on 'curcpu' are
	 * stale and invalidate them.
	 *
	 * Note that we incur this penalty only when the scheduler chooses to
	 * move the thread associated with this vcpu between host cpus.
	 *
	 * Note also that this will invalidate mappings tagged with 'vpid'
	 * for "all" EP4TAs.
	 */
	if (vmxstate->vpid != 0) {
		invvpid_desc.vpid = vmxstate->vpid;
		invvpid(INVVPID_TYPE_SINGLE_CONTEXT, invvpid_desc);
	}
done:
	return (error);
}

static void 
vm_exit_update_rip(struct vm_exit *vmexit)
{
	int error;

	error = vmwrite(VMCS_GUEST_RIP, vmexit->rip + vmexit->inst_length);
	if (error)
		panic("vmx_run: error %d writing to VMCS_GUEST_RIP", error);
}

/*
 * We depend on 'procbased_ctls' to have the Interrupt Window Exiting bit set.
 */
CTASSERT((PROCBASED_CTLS_ONE_SETTING & PROCBASED_INT_WINDOW_EXITING) != 0);

static void __inline
vmx_set_int_window_exiting(struct vmx *vmx, int vcpu)
{
	int error;

	vmx->cap[vcpu].proc_ctls |= PROCBASED_INT_WINDOW_EXITING;

	error = vmwrite(VMCS_PRI_PROC_BASED_CTLS, vmx->cap[vcpu].proc_ctls);
	if (error)
		panic("vmx_set_int_window_exiting: vmwrite error %d", error);
}

static void __inline
vmx_clear_int_window_exiting(struct vmx *vmx, int vcpu)
{
	int error;

	vmx->cap[vcpu].proc_ctls &= ~PROCBASED_INT_WINDOW_EXITING;

	error = vmwrite(VMCS_PRI_PROC_BASED_CTLS, vmx->cap[vcpu].proc_ctls);
	if (error)
		panic("vmx_clear_int_window_exiting: vmwrite error %d", error);
}

static void __inline
vmx_set_nmi_window_exiting(struct vmx *vmx, int vcpu)
{
	int error;

	vmx->cap[vcpu].proc_ctls |= PROCBASED_NMI_WINDOW_EXITING;

	error = vmwrite(VMCS_PRI_PROC_BASED_CTLS, vmx->cap[vcpu].proc_ctls);
	if (error)
		panic("vmx_set_nmi_window_exiting: vmwrite error %d", error);
}

static void __inline
vmx_clear_nmi_window_exiting(struct vmx *vmx, int vcpu)
{
	int error;

	vmx->cap[vcpu].proc_ctls &= ~PROCBASED_NMI_WINDOW_EXITING;

	error = vmwrite(VMCS_PRI_PROC_BASED_CTLS, vmx->cap[vcpu].proc_ctls);
	if (error)
		panic("vmx_clear_nmi_window_exiting: vmwrite error %d", error);
}

static int
vmx_inject_nmi(struct vmx *vmx, int vcpu)
{
	int error;
	uint64_t info, interruptibility;

	/* Bail out if no NMI requested */
	if (vmx->state[vcpu].request_nmi == 0)
		return (0);

	error = vmread(VMCS_GUEST_INTERRUPTIBILITY, &interruptibility);
	if (error) {
		panic("vmx_inject_nmi: vmread(interruptibility) %d",
			error);
	}
	if (interruptibility & nmi_blocking_bits)
		goto nmiblocked;

	/*
	 * Inject the virtual NMI. The vector must be the NMI IDT entry
	 * or the VMCS entry check will fail.
	 */
	info = VMCS_INTERRUPTION_INFO_NMI | VMCS_INTERRUPTION_INFO_VALID;
	info |= IDT_NMI;

	error = vmwrite(VMCS_ENTRY_INTR_INFO, info);
	if (error)
		panic("vmx_inject_nmi: vmwrite(intrinfo) %d", error);

	VMM_CTR0(vmx->vm, vcpu, "Injecting vNMI");

	/* Clear the request */
	vmx->state[vcpu].request_nmi = 0;
	return (1);

nmiblocked:
	/*
	 * Set the NMI Window Exiting execution control so we can inject
	 * the virtual NMI as soon as blocking condition goes away.
	 */
	vmx_set_nmi_window_exiting(vmx, vcpu);

	VMM_CTR0(vmx->vm, vcpu, "Enabling NMI window exiting");
	return (1);
}

static void
vmx_inject_interrupts(struct vmx *vmx, int vcpu)
{
	int error, vector;
	uint64_t info, rflags, interruptibility;

	const int HWINTR_BLOCKED = VMCS_INTERRUPTIBILITY_STI_BLOCKING |
				   VMCS_INTERRUPTIBILITY_MOVSS_BLOCKING;

#if 1
	/*
	 * XXX
	 * If an event is being injected from userland then just return.
	 * For e.g. we may inject a breakpoint exception to cause the
	 * guest to enter the debugger so we can inspect its state.
	 */
	error = vmread(VMCS_ENTRY_INTR_INFO, &info);
	if (error)
		panic("vmx_inject_interrupts: vmread(intrinfo) %d", error);
	if (info & VMCS_INTERRUPTION_INFO_VALID)
		return;
#endif
	/*
	 * NMI injection has priority so deal with those first
	 */
	if (vmx_inject_nmi(vmx, vcpu))
		return;

	/* Ask the local apic for a vector to inject */
	vector = lapic_pending_intr(vmx->vm, vcpu);
	if (vector < 0)
		return;

	if (vector < 32 || vector > 255)
		panic("vmx_inject_interrupts: invalid vector %d\n", vector);

	/* Check RFLAGS.IF and the interruptibility state of the guest */
	error = vmread(VMCS_GUEST_RFLAGS, &rflags);
	if (error)
		panic("vmx_inject_interrupts: vmread(rflags) %d", error);

	if ((rflags & PSL_I) == 0)
		goto cantinject;

	error = vmread(VMCS_GUEST_INTERRUPTIBILITY, &interruptibility);
	if (error) {
		panic("vmx_inject_interrupts: vmread(interruptibility) %d",
			error);
	}
	if (interruptibility & HWINTR_BLOCKED)
		goto cantinject;

	/* Inject the interrupt */
	info = VMCS_INTERRUPTION_INFO_HW_INTR | VMCS_INTERRUPTION_INFO_VALID;
	info |= vector;
	error = vmwrite(VMCS_ENTRY_INTR_INFO, info);
	if (error)
		panic("vmx_inject_interrupts: vmwrite(intrinfo) %d", error);

	/* Update the Local APIC ISR */
	lapic_intr_accepted(vmx->vm, vcpu, vector);

	VMM_CTR1(vmx->vm, vcpu, "Injecting hwintr at vector %d", vector);

	return;

cantinject:
	/*
	 * Set the Interrupt Window Exiting execution control so we can inject
	 * the interrupt as soon as blocking condition goes away.
	 */
	vmx_set_int_window_exiting(vmx, vcpu);

	VMM_CTR0(vmx->vm, vcpu, "Enabling interrupt window exiting");
}

static int
vmx_emulate_cr_access(struct vmx *vmx, int vcpu, uint64_t exitqual)
{
	int error;
	uint64_t regval;
	const struct vmxctx *vmxctx;

	/* We only handle mov to %cr0 at this time */
	if ((exitqual & 0xff) != 0x00)
		return (UNHANDLED);

	vmxctx = &vmx->ctx[vcpu];

	/*
	 * We must use vmwrite() directly here because vmcs_setreg() will
	 * call vmclear(vmcs) as a side-effect which we certainly don't want.
	 */
	switch ((exitqual >> 8) & 0xf) {
	case 0:
		regval = vmxctx->guest_rax;
		break;
	case 1:
		regval = vmxctx->guest_rcx;
		break;
	case 2:
		regval = vmxctx->guest_rdx;
		break;
	case 3:
		regval = vmxctx->guest_rbx;
		break;
	case 4:
		error = vmread(VMCS_GUEST_RSP, &regval);
		if (error) {
			panic("vmx_emulate_cr_access: "
			      "error %d reading guest rsp", error);
		}
		break;
	case 5:
		regval = vmxctx->guest_rbp;
		break;
	case 6:
		regval = vmxctx->guest_rsi;
		break;
	case 7:
		regval = vmxctx->guest_rdi;
		break;
	case 8:
		regval = vmxctx->guest_r8;
		break;
	case 9:
		regval = vmxctx->guest_r9;
		break;
	case 10:
		regval = vmxctx->guest_r10;
		break;
	case 11:
		regval = vmxctx->guest_r11;
		break;
	case 12:
		regval = vmxctx->guest_r12;
		break;
	case 13:
		regval = vmxctx->guest_r13;
		break;
	case 14:
		regval = vmxctx->guest_r14;
		break;
	case 15:
		regval = vmxctx->guest_r15;
		break;
	}

	regval |= cr0_ones_mask;
	regval &= ~cr0_zeros_mask;
	error = vmwrite(VMCS_GUEST_CR0, regval);
	if (error)
		panic("vmx_emulate_cr_access: error %d writing cr0", error);

	return (HANDLED);
}

static int
vmx_exit_process(struct vmx *vmx, int vcpu, struct vm_exit *vmexit)
{
	int handled;
	struct vmcs *vmcs;
	struct vmxctx *vmxctx;
	uint32_t eax, ecx, edx;
	uint64_t qual;

	handled = 0;
	vmcs = &vmx->vmcs[vcpu];
	vmxctx = &vmx->ctx[vcpu];
	qual = vmexit->u.vmx.exit_qualification;
	vmexit->exitcode = VM_EXITCODE_BOGUS;

	switch (vmexit->u.vmx.exit_reason) {
	case EXIT_REASON_CR_ACCESS:
		handled = vmx_emulate_cr_access(vmx, vcpu, qual);
		break;
	case EXIT_REASON_RDMSR:
		ecx = vmxctx->guest_rcx;
		handled = emulate_rdmsr(vmx->vm, vcpu, ecx);
		if (!handled) {
			vmexit->exitcode = VM_EXITCODE_RDMSR;
			vmexit->u.msr.code = ecx;
		}
		break;
	case EXIT_REASON_WRMSR:
		eax = vmxctx->guest_rax;
		ecx = vmxctx->guest_rcx;
		edx = vmxctx->guest_rdx;
		handled = emulate_wrmsr(vmx->vm, vcpu, ecx,
					(uint64_t)edx << 32 | eax);
		if (!handled) {
			vmexit->exitcode = VM_EXITCODE_WRMSR;
			vmexit->u.msr.code = ecx;
			vmexit->u.msr.wval = (uint64_t)edx << 32 | eax;
		}
		break;
	case EXIT_REASON_HLT:
		vmexit->exitcode = VM_EXITCODE_HLT;
		break;
	case EXIT_REASON_MTF:
		vmexit->exitcode = VM_EXITCODE_MTRAP;
		break;
	case EXIT_REASON_PAUSE:
		vmexit->exitcode = VM_EXITCODE_PAUSE;
		break;
	case EXIT_REASON_INTR_WINDOW:
		vmx_clear_int_window_exiting(vmx, vcpu);
		VMM_CTR0(vmx->vm, vcpu, "Disabling interrupt window exiting");
		/* FALLTHRU */
	case EXIT_REASON_EXT_INTR:
		/*
		 * External interrupts serve only to cause VM exits and allow
		 * the host interrupt handler to run.
		 *
		 * If this external interrupt triggers a virtual interrupt
		 * to a VM, then that state will be recorded by the
		 * host interrupt handler in the VM's softc. We will inject
		 * this virtual interrupt during the subsequent VM enter.
		 */

		/*
		 * This is special. We want to treat this as an 'handled'
		 * VM-exit but not increment the instruction pointer.
		 */
		vmm_stat_incr(vmx->vm, vcpu, VMEXIT_EXTINT, 1);
		return (1);
	case EXIT_REASON_NMI_WINDOW:
		/* Exit to allow the pending virtual NMI to be injected */
		vmx_clear_nmi_window_exiting(vmx, vcpu);
		VMM_CTR0(vmx->vm, vcpu, "Disabling NMI window exiting");
		return (1);
	case EXIT_REASON_INOUT:
		vmexit->exitcode = VM_EXITCODE_INOUT;
		vmexit->u.inout.bytes = (qual & 0x7) + 1;
		vmexit->u.inout.in = (qual & 0x8) ? 1 : 0;
		vmexit->u.inout.string = (qual & 0x10) ? 1 : 0;
		vmexit->u.inout.rep = (qual & 0x20) ? 1 : 0;
		vmexit->u.inout.port = (uint16_t)(qual >> 16);
		vmexit->u.inout.eax = (uint32_t)(vmxctx->guest_rax);
		break;
	case EXIT_REASON_CPUID:
		handled = vmx_handle_cpuid(vcpu, vmxctx);
		break;
	case EXIT_REASON_EPT_FAULT:
		vmexit->exitcode = VM_EXITCODE_PAGING;
		vmexit->u.paging.cr3 = vmcs_guest_cr3();
		break;
	default:
		break;
	}

	if (handled) {
		/*
		 * It is possible that control is returned to userland
		 * even though we were able to handle the VM exit in the
		 * kernel (for e.g. 'astpending' is set in the run loop).
		 *
		 * In such a case we want to make sure that the userland
		 * restarts guest execution at the instruction *after*
		 * the one we just processed. Therefore we update the
		 * guest rip in the VMCS and in 'vmexit'.
		 */
		vm_exit_update_rip(vmexit);
		vmexit->rip += vmexit->inst_length;
		vmexit->inst_length = 0;
	} else {
		if (vmexit->exitcode == VM_EXITCODE_BOGUS) {
			/*
			 * If this VM exit was not claimed by anybody then
			 * treat it as a generic VMX exit.
			 */
			vmexit->exitcode = VM_EXITCODE_VMX;
			vmexit->u.vmx.error = 0;
		} else {
			/*
			 * The exitcode and collateral have been populated.
			 * The VM exit will be processed further in userland.
			 */
		}
	}
	return (handled);
}

static int
vmx_run(void *arg, int vcpu, register_t rip, struct vm_exit *vmexit)
{
	int error, vie, rc, handled, astpending;
	uint32_t exit_reason;
	struct vmx *vmx;
	struct vmxctx *vmxctx;
	struct vmcs *vmcs;
	
	vmx = arg;
	vmcs = &vmx->vmcs[vcpu];
	vmxctx = &vmx->ctx[vcpu];
	vmxctx->launched = 0;

	/*
	 * XXX Can we avoid doing this every time we do a vm run?
	 */
	VMPTRLD(vmcs);

	/*
	 * XXX
	 * We do this every time because we may setup the virtual machine
	 * from a different process than the one that actually runs it.
	 *
	 * If the life of a virtual machine was spent entirely in the context
	 * of a single process we could do this once in vmcs_set_defaults().
	 */
	if ((error = vmwrite(VMCS_HOST_CR3, rcr3())) != 0)
		panic("vmx_run: error %d writing to VMCS_HOST_CR3", error);

	if ((error = vmwrite(VMCS_GUEST_RIP, rip)) != 0)
		panic("vmx_run: error %d writing to VMCS_GUEST_RIP", error);

	if ((error = vmx_set_pcpu_defaults(vmx, vcpu)) != 0)
		panic("vmx_run: error %d setting up pcpu defaults", error);

	do {
		lapic_timer_tick(vmx->vm, vcpu);
		vmx_inject_interrupts(vmx, vcpu);
		vmx_run_trace(vmx, vcpu);
		rc = vmx_setjmp(vmxctx);
#ifdef SETJMP_TRACE
		vmx_setjmp_trace(vmx, vcpu, vmxctx, rc);
#endif
		switch (rc) {
		case VMX_RETURN_DIRECT:
			if (vmxctx->launched == 0) {
				vmxctx->launched = 1;
				vmx_launch(vmxctx);
			} else
				vmx_resume(vmxctx);
			panic("vmx_launch/resume should not return");
			break;
		case VMX_RETURN_LONGJMP:
			break;			/* vm exit */
		case VMX_RETURN_VMRESUME:
			vie = vmcs_instruction_error();
			if (vmxctx->launch_error == VM_FAIL_INVALID ||
			    vie != VMRESUME_WITH_NON_LAUNCHED_VMCS) {
				printf("vmresume error %d vmcs inst error %d\n",
					vmxctx->launch_error, vie);
				goto err_exit;
			}
			vmx_launch(vmxctx);	/* try to launch the guest */
			panic("vmx_launch should not return");
			break;
		case VMX_RETURN_VMLAUNCH:
			vie = vmcs_instruction_error();
#if 1
			printf("vmlaunch error %d vmcs inst error %d\n",
				vmxctx->launch_error, vie);
#endif
			goto err_exit;
		default:
			panic("vmx_setjmp returned %d", rc);
		}
		
		/*
		 * XXX locking?
		 * See comments in exception.S about checking for ASTs
		 * atomically while interrupts are disabled. But it is
		 * not clear that they apply in our case.
		 */
		astpending = curthread->td_flags & TDF_ASTPENDING;

		/* enable interrupts */
		enable_intr();

		/* collect some basic information for VM exit processing */
		vmexit->rip = rip = vmcs_guest_rip();
		vmexit->inst_length = vmexit_instruction_length();
		vmexit->u.vmx.exit_reason = exit_reason = vmcs_exit_reason();
		vmexit->u.vmx.exit_qualification = vmcs_exit_qualification();

		handled = vmx_exit_process(vmx, vcpu, vmexit);

		vmx_exit_trace(vmx, vcpu, rip, exit_reason, handled,
			       astpending);
	} while (handled && !astpending);

	/*
	 * If a VM exit has been handled then the exitcode must be BOGUS
	 * If a VM exit is not handled then the exitcode must not be BOGUS
	 */
	if ((handled && vmexit->exitcode != VM_EXITCODE_BOGUS) ||
	    (!handled && vmexit->exitcode == VM_EXITCODE_BOGUS)) {
		panic("Mismatch between handled (%d) and exitcode (%d)",
		      handled, vmexit->exitcode);
	}

	VMM_CTR1(vmx->vm, vcpu, "goto userland: exitcode %d",vmexit->exitcode);

	/*
	 * XXX
	 * We need to do this to ensure that any VMCS state cached by the
	 * processor is flushed to memory. We need to do this in case the
	 * VM moves to a different cpu the next time it runs.
	 *
	 * Can we avoid doing this?
	 */
	VMCLEAR(vmcs);
	return (0);

err_exit:
	vmexit->exitcode = VM_EXITCODE_VMX;
	vmexit->u.vmx.exit_reason = (uint32_t)-1;
	vmexit->u.vmx.exit_qualification = (uint32_t)-1;
	vmexit->u.vmx.error = vie;
	VMCLEAR(vmcs);
	return (ENOEXEC);
}

static void
vmx_vmcleanup(void *arg)
{
	int error;
	struct vmx *vmx = arg;

	/*
	 * XXXSMP we also need to clear the VMCS active on the other vcpus.
	 */
	error = vmclear(&vmx->vmcs[0]);
	if (error != 0)
		panic("vmx_vmcleanup: vmclear error %d on vcpu 0", error);

	ept_vmcleanup(vmx);
	free(vmx, M_VMX);

	return;
}

static register_t *
vmxctx_regptr(struct vmxctx *vmxctx, int reg)
{

	switch (reg) {
	case VM_REG_GUEST_RAX:
		return (&vmxctx->guest_rax);
	case VM_REG_GUEST_RBX:
		return (&vmxctx->guest_rbx);
	case VM_REG_GUEST_RCX:
		return (&vmxctx->guest_rcx);
	case VM_REG_GUEST_RDX:
		return (&vmxctx->guest_rdx);
	case VM_REG_GUEST_RSI:
		return (&vmxctx->guest_rsi);
	case VM_REG_GUEST_RDI:
		return (&vmxctx->guest_rdi);
	case VM_REG_GUEST_RBP:
		return (&vmxctx->guest_rbp);
	case VM_REG_GUEST_R8:
		return (&vmxctx->guest_r8);
	case VM_REG_GUEST_R9:
		return (&vmxctx->guest_r9);
	case VM_REG_GUEST_R10:
		return (&vmxctx->guest_r10);
	case VM_REG_GUEST_R11:
		return (&vmxctx->guest_r11);
	case VM_REG_GUEST_R12:
		return (&vmxctx->guest_r12);
	case VM_REG_GUEST_R13:
		return (&vmxctx->guest_r13);
	case VM_REG_GUEST_R14:
		return (&vmxctx->guest_r14);
	case VM_REG_GUEST_R15:
		return (&vmxctx->guest_r15);
	default:
		break;
	}
	return (NULL);
}

static int
vmxctx_getreg(struct vmxctx *vmxctx, int reg, uint64_t *retval)
{
	register_t *regp;

	if ((regp = vmxctx_regptr(vmxctx, reg)) != NULL) {
		*retval = *regp;
		return (0);
	} else
		return (EINVAL);
}

static int
vmxctx_setreg(struct vmxctx *vmxctx, int reg, uint64_t val)
{
	register_t *regp;

	if ((regp = vmxctx_regptr(vmxctx, reg)) != NULL) {
		*regp = val;
		return (0);
	} else
		return (EINVAL);
}

static int
vmx_getreg(void *arg, int vcpu, int reg, uint64_t *retval)
{
	struct vmx *vmx = arg;

	if (vmxctx_getreg(&vmx->ctx[vcpu], reg, retval) == 0)
		return (0);

	/*
	 * If the vcpu is running then don't mess with the VMCS.
	 *
	 * vmcs_getreg will VMCLEAR the vmcs when it is done which will cause
	 * the subsequent vmlaunch/vmresume to fail.
	 */
	if (vcpu_is_running(vmx->vm, vcpu, NULL))
		panic("vmx_getreg: %s%d is running", vm_name(vmx->vm), vcpu);

	return (vmcs_getreg(&vmx->vmcs[vcpu], reg, retval));
}

static int
vmx_setreg(void *arg, int vcpu, int reg, uint64_t val)
{
	int error;
	uint64_t ctls;
	struct vmx *vmx = arg;

	/*
	 * XXX Allow caller to set contents of the guest registers saved in
	 * the 'vmxctx' even though the vcpu might be running. We need this
	 * specifically to support the rdmsr emulation that will set the
	 * %eax and %edx registers during vm exit processing.
	 */
	if (vmxctx_setreg(&vmx->ctx[vcpu], reg, val) == 0)
		return (0);

	/*
	 * If the vcpu is running then don't mess with the VMCS.
	 *
	 * vmcs_setreg will VMCLEAR the vmcs when it is done which will cause
	 * the subsequent vmlaunch/vmresume to fail.
	 */
	if (vcpu_is_running(vmx->vm, vcpu, NULL))
		panic("vmx_setreg: %s%d is running", vm_name(vmx->vm), vcpu);

	error = vmcs_setreg(&vmx->vmcs[vcpu], reg, val);

	if (error == 0) {
		/*
		 * If the "load EFER" VM-entry control is 1 then the
		 * value of EFER.LMA must be identical to "IA-32e mode guest"
		 * bit in the VM-entry control.
		 */
		if ((entry_ctls & VM_ENTRY_LOAD_EFER) != 0 &&
		    (reg == VM_REG_GUEST_EFER)) {
			vmcs_getreg(&vmx->vmcs[vcpu],
				    VMCS_IDENT(VMCS_ENTRY_CTLS), &ctls);
			if (val & EFER_LMA)
				ctls |= VM_ENTRY_GUEST_LMA;
			else
				ctls &= ~VM_ENTRY_GUEST_LMA;
			vmcs_setreg(&vmx->vmcs[vcpu],
				    VMCS_IDENT(VMCS_ENTRY_CTLS), ctls);
		}
	}

	return (error);
}

static int
vmx_getdesc(void *arg, int vcpu, int reg, struct seg_desc *desc)
{
	struct vmx *vmx = arg;

	return (vmcs_getdesc(&vmx->vmcs[vcpu], reg, desc));
}

static int
vmx_setdesc(void *arg, int vcpu, int reg, struct seg_desc *desc)
{
	struct vmx *vmx = arg;

	return (vmcs_setdesc(&vmx->vmcs[vcpu], reg, desc));
}

static int
vmx_inject(void *arg, int vcpu, int type, int vector, uint32_t code,
	   int code_valid)
{
	int error;
	uint32_t info;
	struct vmx *vmx = arg;
	struct vmcs *vmcs = &vmx->vmcs[vcpu];

	static uint32_t type_map[VM_EVENT_MAX] = {
		0x1,		/* VM_EVENT_NONE */
		0x0,		/* VM_HW_INTR */
		0x2,		/* VM_NMI */
		0x3,		/* VM_HW_EXCEPTION */
		0x4,		/* VM_SW_INTR */
		0x5,		/* VM_PRIV_SW_EXCEPTION */
		0x6,		/* VM_SW_EXCEPTION */
	};

	info = vector | (type_map[type] << 8) | (code_valid ? 1 << 11 : 0);
	info |= VMCS_INTERRUPTION_INFO_VALID;
	error = vmcs_setreg(vmcs, VMCS_IDENT(VMCS_ENTRY_INTR_INFO), info);
	if (error != 0)
		return (error);

	if (code_valid) {
		error = vmcs_setreg(vmcs,
				    VMCS_IDENT(VMCS_ENTRY_EXCEPTION_ERROR),
				    code);
	}
	return (error);
}

static int
vmx_nmi(void *arg, int vcpu)
{
	struct vmx *vmx = arg;

	atomic_set_int(&vmx->state[vcpu].request_nmi, 1);

	return (0);
}

static int
vmx_getcap(void *arg, int vcpu, int type, int *retval)
{
	struct vmx *vmx = arg;
	int vcap;
	int ret;

	ret = ENOENT;

	vcap = vmx->cap[vcpu].set;

	switch (type) {
	case VM_CAP_HALT_EXIT:
		if (cap_halt_exit)
			ret = 0;
		break;
	case VM_CAP_PAUSE_EXIT:
		if (cap_pause_exit)
			ret = 0;
		break;
	case VM_CAP_MTRAP_EXIT:
		if (cap_monitor_trap)
			ret = 0;
		break;
	case VM_CAP_UNRESTRICTED_GUEST:
		if (cap_unrestricted_guest)
			ret = 0;
		break;
	default:
		break;
	}

	if (ret == 0)
		*retval = (vcap & (1 << type)) ? 1 : 0;

	return (ret);
}

static int
vmx_setcap(void *arg, int vcpu, int type, int val)
{
	struct vmx *vmx = arg;
	struct vmcs *vmcs = &vmx->vmcs[vcpu];
	uint32_t baseval;
	uint32_t *pptr;
	int error;
	int flag;
	int reg;
	int retval;

	retval = ENOENT;
	pptr = NULL;

	switch (type) {
	case VM_CAP_HALT_EXIT:
		if (cap_halt_exit) {
			retval = 0;
			pptr = &vmx->cap[vcpu].proc_ctls;
			baseval = *pptr;
			flag = PROCBASED_HLT_EXITING;
			reg = VMCS_PRI_PROC_BASED_CTLS;
		}
		break;
	case VM_CAP_MTRAP_EXIT:
		if (cap_monitor_trap) {
			retval = 0;
			pptr = &vmx->cap[vcpu].proc_ctls;
			baseval = *pptr;
			flag = PROCBASED_MTF;
			reg = VMCS_PRI_PROC_BASED_CTLS;
		}
		break;
	case VM_CAP_PAUSE_EXIT:
		if (cap_pause_exit) {
			retval = 0;
			pptr = &vmx->cap[vcpu].proc_ctls;
			baseval = *pptr;
			flag = PROCBASED_PAUSE_EXITING;
			reg = VMCS_PRI_PROC_BASED_CTLS;
		}
		break;
	case VM_CAP_UNRESTRICTED_GUEST:
		if (cap_unrestricted_guest) {
			retval = 0;
			baseval = procbased_ctls2;
			flag = PROCBASED2_UNRESTRICTED_GUEST;
			reg = VMCS_SEC_PROC_BASED_CTLS;
		}
		break;
	default:
		break;
	}

	if (retval == 0) {
		if (val) {
			baseval |= flag;
		} else {
			baseval &= ~flag;
		}
		VMPTRLD(vmcs);
		error = vmwrite(reg, baseval);
		VMCLEAR(vmcs);

		if (error) {
			retval = error;
		} else {
			/*
			 * Update optional stored flags, and record
			 * setting
			 */
			if (pptr != NULL) {
				*pptr = baseval;
			}

			if (val) {
				vmx->cap[vcpu].set |= (1 << type);
			} else {
				vmx->cap[vcpu].set &= ~(1 << type);
			}
		}
	}

        return (retval);
}

struct vmm_ops vmm_ops_intel = {
	vmx_init,
	vmx_cleanup,
	vmx_vminit,
	vmx_run,
	vmx_vmcleanup,
	ept_vmmmap,
	vmx_getreg,
	vmx_setreg,
	vmx_getdesc,
	vmx_setdesc,
	vmx_inject,
	vmx_nmi,
	vmx_getcap,
	vmx_setcap
};
