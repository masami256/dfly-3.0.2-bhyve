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

#ifndef _VMM_DIST_CONV_H_
#define	_VMM_DIST_CONV_H_

#ifdef _KERNEL

#include <sys/thread2.h>

/*
 * Replace critical_enter and critical_exit that are FreeBSD's function name to
 * crit_enter and crit_exit.
 */
#define critical_enter	crit_enter
#define critical_exit	crit_exit

/*
 * Wrapper function for smp_rendezvous().
 */
static inline void 
smp_rendezvous(void (* setup_func)(void *),
               void (* action_func)(void *),
               void (* teardown_func)(void *),
               void *arg)
{
	/* TODO: need to know to pass mygd is correct or not. */
	globaldata_t mygd = mycpu;
	lwkt_send_ipiq3(mygd, (ipifunc3_t) action_func, arg, 0);

}


#endif	/* KERNEL */

#endif /* _VMM_DIST_CONV_H_ */
