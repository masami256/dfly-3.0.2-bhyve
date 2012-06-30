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

#include <sys/types.h>
#include <sys/systm.h>

#include <machine/cpufunc.h>
#include <machine/md_var.h>
#include <machine/specialreg.h>

#include "x86.h"

#define	CPUID_VM_HIGH		0x40000000

static const char bhyve_id[12] = "BHyVE BHyVE ";

int
x86_emulate_cpuid(uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx,
	uint32_t vcpu_id)
{
	unsigned int 	func, regs[4];

	func = *eax;

	/*
	 * Requests for invalid CPUID levels should map to the highest
	 * available level instead.
	 */
	if (cpu_exthigh != 0 && *eax >= 0x80000000) {
		if (*eax > cpu_exthigh)
			*eax = cpu_exthigh;
	} else if (*eax >= 0x40000000) {
		if (*eax > CPUID_VM_HIGH)
			*eax = CPUID_VM_HIGH;
	} else if (*eax > cpu_high) {
		*eax = cpu_high;
	}

	/*
	 * In general the approach used for CPU topology is to
	 * advertise a flat topology where all CPUs are packages with
	 * no multi-core or SMT.
	 */
	switch (func) {
		case CPUID_0000_0000:
		case CPUID_0000_0002:
		case CPUID_0000_0003:
		case CPUID_0000_000A:
			cpuid_count(*eax, *ecx, regs);
			break;

		case CPUID_8000_0000:
		case CPUID_8000_0001:
		case CPUID_8000_0002:
		case CPUID_8000_0003:
		case CPUID_8000_0004:
		case CPUID_8000_0006:
		case CPUID_8000_0007:
		case CPUID_8000_0008:
			cpuid_count(*eax, *ecx, regs);
			break;

		case CPUID_0000_0001:
			do_cpuid(1, regs);

			/*
			 * Override the APIC ID only in ebx
			 */
			regs[1] &= ~(CPUID_LOCAL_APIC_ID);
			regs[1] |= (vcpu_id << CPUID_0000_0001_APICID_SHIFT);

			/*
			 * Don't expose VMX, SpeedStep or TME capability.
			 * Advertise x2APIC capability and Hypervisor guest.
			 */
			regs[2] &= ~(CPUID2_VMX | CPUID2_EST | CPUID2_TM2);
			regs[2] |= CPUID2_X2APIC | CPUID2_HV;

			/*
			 * Hide xsave/osxsave/avx until the FPU save/restore
			 * issues are resolved
			 */
			regs[2] &= ~(CPUID2_XSAVE | CPUID2_OSXSAVE |
				     CPUID2_AVX);

			/*
			 * Hide thermal monitoring
			 */
			regs[3] &= ~(CPUID_ACPI | CPUID_TM);
			
			/*
			 * Machine check handling is done in the host.
			 * Hide MTRR capability.
			 */
			regs[3] &= ~(CPUID_MCA | CPUID_MCE | CPUID_MTRR);

			/*
			 * Disable multi-core.
			 */
			regs[1] &= ~CPUID_HTT_CORES;
			regs[3] &= ~CPUID_HTT;
			break;

		case CPUID_0000_0004:
			do_cpuid(4, regs);

			/*
			 * Do not expose topology.
			 */
			regs[0] &= 0xffff8000;
			regs[0] |= 0x04008000;
			break;

		case CPUID_0000_0006:
			/*
			 * Handle the access, but report 0 for
			 * all options
			 */
			regs[0] = 0;
			regs[1] = 0;
			regs[2] = 0;
			regs[3] = 0;
			break;

		case CPUID_0000_000B:
			/*
			 * Processor topology enumeration
			 */
			regs[0] = 0;
			regs[1] = 0;
			regs[2] = *ecx & 0xff;
			regs[3] = vcpu_id;
			break;

		case 0x40000000:
			regs[0] = CPUID_VM_HIGH;
			bcopy(bhyve_id, &regs[1], 4);
			bcopy(bhyve_id, &regs[2], 4);
			bcopy(bhyve_id, &regs[3], 4);
			break;
		default:
			/* XXX: Leaf 5? */
			return (0);
	}

	*eax = regs[0];
	*ebx = regs[1];
	*ecx = regs[2];
	*edx = regs[3];
	return (1);
}
