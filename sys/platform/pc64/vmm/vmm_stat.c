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
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/smp.h>

#include <machine/vmm.h>
#include "vmm_stat.h"

static int vstnum;
static struct vmm_stat_type *vsttab[MAX_VMM_STAT_TYPES];

static MALLOC_DEFINE(M_VMM_STAT, "vmm stat", "vmm stat");

void
vmm_stat_init(void *arg)
{
	struct vmm_stat_type *vst = arg;

	/* We require all stats to identify themselves with a description */
	if (vst->desc == NULL)
		return;

	if (vstnum >= MAX_VMM_STAT_TYPES) {
		printf("Cannot accomodate vmm stat type \"%s\"!\n", vst->desc);
		return;
	}

	vst->index = vstnum;
	vsttab[vstnum++] = vst;
}

int
vmm_stat_copy(struct vm *vm, int vcpu, int *num_stats, uint64_t *buf)
{
	int i;
	uint64_t *stats;

	if (vcpu < 0 || vcpu >= VM_MAXCPU)
		return (EINVAL);
		
	stats = vcpu_stats(vm, vcpu);
	for (i = 0; i < vstnum; i++)
		buf[i] = stats[i];
	*num_stats = vstnum;
	return (0);
}

void *
vmm_stat_alloc(void)
{
	u_long size;
	
	size = vstnum * sizeof(uint64_t);

	return (malloc(size, M_VMM_STAT, M_ZERO | M_WAITOK));
}

void
vmm_stat_free(void *vp)
{
	free(vp, M_VMM_STAT);
}

const char *
vmm_stat_desc(int index)
{

	if (index >= 0 && index < vstnum)
		return (vsttab[index]->desc);
	else
		return (NULL);
}
