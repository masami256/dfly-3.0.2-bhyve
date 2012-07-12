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

#include <sys/cdefs.h>

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/objcache.h>

#include <machine/npx.h>
#include <machine/vmm_fpu.h>

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
