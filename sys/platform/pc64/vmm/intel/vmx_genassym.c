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
#include <sys/malloc.h>
#include <sys/assym.h>

#include <vm/vm.h>
#include <vm/pmap.h>

#include <machine/pmap.h>

#include <machine/vmm.h>
#include "vmx.h"
#include "vmx_cpufunc.h"

ASSYM(VMXCTX_TMPSTKTOP, offsetof(struct vmxctx, tmpstktop));
ASSYM(VMXCTX_GUEST_RDI, offsetof(struct vmxctx, guest_rdi));
ASSYM(VMXCTX_GUEST_RSI, offsetof(struct vmxctx, guest_rsi));
ASSYM(VMXCTX_GUEST_RDX, offsetof(struct vmxctx, guest_rdx));
ASSYM(VMXCTX_GUEST_RCX, offsetof(struct vmxctx, guest_rcx));
ASSYM(VMXCTX_GUEST_R8, offsetof(struct vmxctx, guest_r8));
ASSYM(VMXCTX_GUEST_R9, offsetof(struct vmxctx, guest_r9));
ASSYM(VMXCTX_GUEST_RAX, offsetof(struct vmxctx, guest_rax));
ASSYM(VMXCTX_GUEST_RBX, offsetof(struct vmxctx, guest_rbx));
ASSYM(VMXCTX_GUEST_RBP, offsetof(struct vmxctx, guest_rbp));
ASSYM(VMXCTX_GUEST_R10, offsetof(struct vmxctx, guest_r10));
ASSYM(VMXCTX_GUEST_R11, offsetof(struct vmxctx, guest_r11));
ASSYM(VMXCTX_GUEST_R12, offsetof(struct vmxctx, guest_r12));
ASSYM(VMXCTX_GUEST_R13, offsetof(struct vmxctx, guest_r13));
ASSYM(VMXCTX_GUEST_R14, offsetof(struct vmxctx, guest_r14));
ASSYM(VMXCTX_GUEST_R15, offsetof(struct vmxctx, guest_r15));
ASSYM(VMXCTX_GUEST_CR2, offsetof(struct vmxctx, guest_cr2));

ASSYM(VMXCTX_HOST_R15, offsetof(struct vmxctx, host_r15));
ASSYM(VMXCTX_HOST_R14, offsetof(struct vmxctx, host_r14));
ASSYM(VMXCTX_HOST_R13, offsetof(struct vmxctx, host_r13));
ASSYM(VMXCTX_HOST_R12, offsetof(struct vmxctx, host_r12));
ASSYM(VMXCTX_HOST_RBP, offsetof(struct vmxctx, host_rbp));
ASSYM(VMXCTX_HOST_RSP, offsetof(struct vmxctx, host_rsp));
ASSYM(VMXCTX_HOST_RBX, offsetof(struct vmxctx, host_rbx));
ASSYM(VMXCTX_HOST_RIP, offsetof(struct vmxctx, host_rip));

ASSYM(VMXCTX_LAUNCH_ERROR, offsetof(struct vmxctx, launch_error));

ASSYM(VM_SUCCESS,	VM_SUCCESS);
ASSYM(VM_FAIL_INVALID,	VM_FAIL_INVALID);
ASSYM(VM_FAIL_VALID,	VM_FAIL_VALID);

ASSYM(VMX_RETURN_DIRECT,	VMX_RETURN_DIRECT);
ASSYM(VMX_RETURN_LONGJMP,	VMX_RETURN_LONGJMP);
ASSYM(VMX_RETURN_VMRESUME,	VMX_RETURN_VMRESUME);
ASSYM(VMX_RETURN_VMLAUNCH,	VMX_RETURN_VMLAUNCH);
