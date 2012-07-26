/*-
 * Copyright (c) 2012 Masami Ichikawa <masami256@gmail.com>
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
 */

/*
 *  most functions came from FreeBSD
 *  http://fxr.watson.org/fxr/source/amd64/amd64/fpu.c
 */

#include <sys/cdefs.h>

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/objcache.h>

#include <machine/npx.h>
#include <machine_base/vmm/vmm_fpu.h>


#define DO_NOT_USE_FPU_FOR_TMP 1


typedef struct objcache *objcache_t;
#define uma_zone_t      objcache_t

MALLOC_DEFINE(M_FPU_SAVE_AREA, "fpu_save_area", "fpu_sava_area manager");

#define uma_zcreate(name, size, ctor, dtor, uminit, fini, align, flags) \
                        objcache_create_mbacked(M_FPU_SAVE_AREA, size,       \
	                        NULL, 0,                        \
                                NULL, NULL,               \
                                NULL)
#define uma_zalloc(zone, flags)                 \
                   objcache_get(zone, flags)

#define uma_zfree(zone, item)                   \
                   objcache_put(zone, item)

#define cpu_max_ext_state_size sizeof(union savefpu)

int use_xsave;			/* non-static for cpu_switch.S */
uint64_t xsave_mask;		/* the same */
static union savefpu *fpu_initialstate;

static uma_zone_t fpu_save_area_zone;
union savefpu *
fpu_save_area_alloc(void)
{
	if (fpu_save_area_zone == NULL) {
		fpu_save_area_zone = uma_zcreate("FPU_save_area",
				         cpu_max_ext_state_size,
				         NULL, NULL, NULL, NULL,
				         XSAVE_AREA_ALIGN - 1, 0);
	}

	return (uma_zalloc(fpu_save_area_zone, 0));
}

void
fpu_save_area_free(union savefpu *fsa)
{

	uma_zfree(fpu_save_area_zone, fsa);
}

void
fpu_save_area_reset(union savefpu *fsa)
{

	bcopy(fpu_initialstate, fsa, cpu_max_ext_state_size);
}

#ifndef DO_NOT_USE_FPU_FOR_TMP
/*
 * Free coprocessor (if we have it).
 */
void
fpuexit(struct thread *td)
{
         crit_enter();
         if (curthread == CPU_GET(fpcurthread)) {
                 stop_emulating();
                 fxsave(PCPU_GET(curpcb)->pcb_save);
                 start_emulating();
                 PCPU_SET(fpcurthread, 0);
         }
         crit_exit();
}
#else
void
fpuexit(struct thread *td) 
{
	panic("It hasn't been implemented yet.");
}

#endif


#define	fxrstor(addr)		__asm __volatile("fxrstor %0" : : "m" (*(addr)))
#define	fxsave(addr)		__asm __volatile("fxsave %0" : "=m" (*(addr)))

static __inline void
xrstor(char *addr, uint64_t mask)
{
	uint32_t low, hi;

	low = mask;
	hi = mask >> 32;
	/* xrstor (%rdi) */
	__asm __volatile(".byte	0x0f,0xae,0x2f" : :
			    "a" (low), "d" (hi), "D" (addr));
}

static __inline void
xsave(char *addr, uint64_t mask)
{
	uint32_t low, hi;

	low = mask;
	hi = mask >> 32;
	/* xsave (%rdi) */
	__asm __volatile(".byte	0x0f,0xae,0x27" : :
			    "a" (low), "d" (hi), "D" (addr) : "memory");
}

static __inline void
xsetbv(uint32_t reg, uint64_t val)
{
	uint32_t low, hi;

	low = val;
	hi = val >> 32;
	__asm __volatile(".byte 0x0f,0x01,0xd1" : :
			    "c" (reg), "a" (low), "d" (hi));
}

void
fpurestore(void *addr)
{

	if (use_xsave)
		xrstor((char *)addr, xsave_mask);
	else
		fxrstor((char *)addr);
}

void
fpusave(void *addr)
{

	if (use_xsave)
		xsave((char *)addr, xsave_mask);
	else
		fxsave((char *)addr);
}
