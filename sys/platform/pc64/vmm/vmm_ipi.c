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
/*__FBSDID("$FreeBSD$");*/

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/bus.h>

#include <machine/intr_machdep.h>
#include <machine/segments.h>
#include <machine/md_var.h>
#include <machine/smp.h>

#include <machine_base/apic/apicvar.h>
#include <machine/vmm.h>
#include "vmm_ipi.h"

extern inthand_t IDTVEC(rsvd), IDTVEC(justreturn);

/* 
 * DragonFly BSD dosen't have IPI_AST, APIC_IPI_INTS and
 * APIC_SPURIOUS_INT so define them here.
 */
#define IPI_AST 0
#define APIC_IPI_INTS 243
#define APIC_SPURIOUS_INT 255

/*
 * The default is to use the IPI_AST to interrupt a vcpu.
 */
static int ipinum[MAXCPU] = { IPI_AST };

CTASSERT(APIC_SPURIOUS_INT == 255);

static void
vmm_ipi_init2(int cpu)
{
	int idx;
	uintptr_t func;
	struct gate_descriptor *ip;

	/*
	 * Search backwards from the highest IDT vector available for use
	 * as our IPI vector. We install the 'justreturn' handler at that
	 * vector and use it to interrupt the vcpus.
	 *
	 * We do this because the IPI_AST is heavyweight and saves all
	 * registers in the trapframe. This is overkill for our use case
	 * which is simply to EOI the interrupt and return.
	 */
	idx = APIC_SPURIOUS_INT;
	while (--idx >= APIC_IPI_INTS) {
		ip = &idt_arr[cpu][idx];
		func = ((long)ip->gd_hioffset << 16 | ip->gd_looffset);
		if (func == (uintptr_t)&IDTVEC(rsvd)) {
			ipinum[cpu] = idx;
			setidt(ipinum[cpu], IDTVEC(justreturn), SDT_SYSIGT,
			       SEL_KPL, 0, cpu);
			break;
		}
	}
	
	if (ipinum[cpu] != IPI_AST && bootverbose) {
		kprintf("vmm_ipi_init: installing ipi handler to interrupt "
		       "vcpus at vector %d\n", ipinum[cpu]);
	}
}

void
vmm_ipi_init(void)
{
	int cpu;

	/* ncpus come from sys/ststm.h */
	for (cpu = 0; cpu < ncpus; cpu++) {
		vmm_ipi_init2(cpu);
	}
}

void
vmm_ipi_cleanup(void)
{
	int cpu;
	
	for (cpu = 0; cpu < ncpus; cpu++) {
		if (ipinum[cpu] != IPI_AST)
			setidt(ipinum[cpu], IDTVEC(rsvd), SDT_SYSIGT, SEL_KPL, 0, cpu);
	}
}

void
vm_interrupt_hostcpu(struct vm *vm, int vcpu)
{
	int hostcpu;

	if (vcpu_is_running(vm, vcpu, &hostcpu) && hostcpu != mycpuid)
		ipi_cpu(hostcpu, ipinum);
}
