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

#ifndef	_VMX_CPUFUNC_H_
#define	_VMX_CPUFUNC_H_

#include <sys/thread2.h>

struct vmcs;

/*
 * Section 5.2 "Conventions" from Intel Architecture Manual 2B.
 *
 *			error
 * VMsucceed		  0
 * VMFailInvalid	  1
 * VMFailValid		  2	see also VMCS VM-Instruction Error Field
 */
#define	VM_SUCCESS		0
#define	VM_FAIL_INVALID		1
#define	VM_FAIL_VALID		2
#define	VMX_SET_ERROR_CODE(varname)					\
	do {								\
	__asm __volatile("	jnc 1f;"				\
			 "	mov $1, %0;"	/* CF: error = 1 */	\
			 "	jmp 3f;"				\
			 "1:	jnz 2f;"				\
			 "	mov $2, %0;"	/* ZF: error = 2 */	\
			 "	jmp 3f;"				\
			 "2:	mov $0, %0;"				\
			 "3:	nop"					\
			 :"=r" (varname));				\
	} while (0)

/* returns 0 on success and non-zero on failure */
static inline int
vmxon(char *region)
{
	int error;
	uint64_t addr;

	addr = vtophys(region);
	__asm __volatile("vmxon %0" : : "m" (*(uint64_t *)&addr) : "memory");
	VMX_SET_ERROR_CODE(error);
	return (error);
}

/* returns 0 on success and non-zero on failure */
static inline int
vmclear(struct vmcs *vmcs)
{
	int error;
	uint64_t addr;

	addr = vtophys(vmcs);
	__asm __volatile("vmclear %0" : : "m" (*(uint64_t *)&addr) : "memory");
	VMX_SET_ERROR_CODE(error);
	return (error);
}

static inline void
vmxoff(void)
{
	__asm __volatile("vmxoff");
}

static inline void
vmptrst(uint64_t *addr)
{
	__asm __volatile("vmptrst %0" : : "m" (*addr) : "memory");
}

static inline int
vmptrld(struct vmcs *vmcs)
{
	int error;
	uint64_t addr;

	addr = vtophys(vmcs);
	__asm __volatile("vmptrld %0" : : "m" (*(uint64_t *)&addr) : "memory");
	VMX_SET_ERROR_CODE(error);
	return (error);
}

static inline int
vmwrite(uint64_t reg, uint64_t val)
{
	int error;

	__asm __volatile("vmwrite %0, %1" : : "r" (val), "r" (reg) : "memory");

	VMX_SET_ERROR_CODE(error);

	return (error);
}

static inline int
vmread(uint64_t r, uint64_t *addr)
{
	int error;

	__asm __volatile("vmread %0, %1" : : "r" (r), "m" (*addr) : "memory");

	VMX_SET_ERROR_CODE(error);

	return (error);
}

static inline void 
VMCLEAR(struct vmcs *vmcs)
{
	int err;

	err = vmclear(vmcs);
	if (err != 0)
		panic("%s: vmclear(%p) error %d", __func__, vmcs, err);

	crit_exit();
}

static inline void 
VMPTRLD(struct vmcs *vmcs)
{
	int err;

	crit_enter();

	err = vmptrld(vmcs);
	if (err != 0)
		panic("%s: vmptrld(%p) error %d", __func__, vmcs, err);
}

#define	INVVPID_TYPE_ADDRESS		0UL
#define	INVVPID_TYPE_SINGLE_CONTEXT	1UL
#define	INVVPID_TYPE_ALL_CONTEXTS	2UL

struct invvpid_desc {
	uint16_t	vpid;
	uint16_t	_res1;
	uint32_t	_res2;
	uint64_t	linear_addr;
};
CTASSERT(sizeof(struct invvpid_desc) == 16);

static inline void 
invvpid(uint64_t type, struct invvpid_desc desc)
{
	int error;

	__asm __volatile("invvpid %0, %1" :: "m" (desc), "r" (type) : "memory");

	VMX_SET_ERROR_CODE(error);
	if (error)
		panic("invvpid error %d", error);
}

#define	INVEPT_TYPE_SINGLE_CONTEXT	1UL
#define	INVEPT_TYPE_ALL_CONTEXTS	2UL
struct invept_desc {
	uint64_t	eptp;
	uint64_t	_res;
};
CTASSERT(sizeof(struct invept_desc) == 16);

static inline void 
invept(uint64_t type, struct invept_desc desc)
{
	int error;

	__asm __volatile("invept %0, %1" :: "m" (desc), "r" (type) : "memory");

	VMX_SET_ERROR_CODE(error);
	if (error)
		panic("invept error %d", error);
}
#endif
