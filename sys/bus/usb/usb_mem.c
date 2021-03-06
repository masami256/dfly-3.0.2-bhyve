/*
 * $NetBSD: usb_mem.c,v 1.26 2003/02/01 06:23:40 thorpej Exp $
 * $FreeBSD: src/sys/dev/usb/usb_mem.c,v 1.5 2003/10/04 22:13:21 joe Exp $
 */
/*
 * Copyright (c) 1998 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Lennart Augustsson (lennart@augustsson.net) at
 * Carlstedt Research & Technology.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *        This product includes software developed by the NetBSD
 *        Foundation, Inc. and its contributors.
 * 4. Neither the name of The NetBSD Foundation nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * USB DMA memory allocation.
 * We need to allocate a lot of small (many 8 byte, some larger)
 * memory blocks that can be used for DMA.  Using the bus_dma
 * routines directly would incur large overheads in space and time.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/kernel.h>
#include <sys/endian.h>
#include <sys/module.h>
#include <sys/bus.h>
#include <sys/queue.h>
#include <sys/sysctl.h>
#include <sys/ktr.h>

#include <machine/endian.h>

#ifdef DIAGNOSTIC
#include <sys/proc.h>
#endif
#include <sys/thread2.h>

#include <bus/usb/usb.h>
#include <bus/usb/usbdi.h>
#include <bus/usb/usbdivar.h>	/* just for usb_dma_t */
#include <bus/usb/usb_mem.h>

#define USBKTR_STRING   "ptr=%p bus=%p size=%zu align=%zu"
#define USBKTR_ARGS 	void  *p, usbd_bus_handle bus, size_t size, size_t align

#if !defined(KTR_USB_MEMORY)
#define KTR_USB_MEMORY	KTR_ALL
#endif
KTR_INFO_MASTER(usbmem);
KTR_INFO(KTR_USB_MEMORY, usbmem, alloc_full, 0, USBKTR_STRING, USBKTR_ARGS);
KTR_INFO(KTR_USB_MEMORY, usbmem, alloc_frag, 0, USBKTR_STRING, USBKTR_ARGS);
KTR_INFO(KTR_USB_MEMORY, usbmem, free_full, 0, USBKTR_STRING, USBKTR_ARGS);
KTR_INFO(KTR_USB_MEMORY, usbmem, free_frag, 0, USBKTR_STRING, USBKTR_ARGS);
KTR_INFO(KTR_USB_MEMORY, usbmem, blkalloc, 0, USBKTR_STRING, USBKTR_ARGS);
KTR_INFO(KTR_USB_MEMORY, usbmem, blkalloc2, 0, USBKTR_STRING, USBKTR_ARGS);
KTR_INFO(KTR_USB_MEMORY, usbmem, blkfree, 0, USBKTR_STRING, USBKTR_ARGS);

#define logmemory(name, ptr, bus, size, align)		\
	KTR_LOG(usbmem_ ## name, ptr, bus, size, align);

#ifdef USB_DEBUG
#define DPRINTF(x)	if (usbdebug) kprintf x
#define DPRINTFN(n,x)	if (usbdebug>(n)) kprintf x
extern int usbdebug;
#else
#define DPRINTF(x)
#define DPRINTFN(n,x)
#endif

#define USB_MEM_SMALL 64
#define USB_MEM_CHUNKS (PAGE_SIZE / USB_MEM_SMALL)
#define USB_MEM_BLOCK (USB_MEM_SMALL * USB_MEM_CHUNKS)

/* This struct is overlayed on free fragments. */
struct usb_frag_dma {
	usb_dma_block_t *block;
	u_int offs;
	LIST_ENTRY(usb_frag_dma) next;
};

static bus_dmamap_callback_t usbmem_callback;
static usbd_status	usb_block_allocmem(bus_dma_tag_t, size_t, size_t,
					   usb_dma_block_t **);
static void		usb_block_freemem(usb_dma_block_t *);

static LIST_HEAD(, usb_dma_block) usb_blk_freelist =
	LIST_HEAD_INITIALIZER(usb_blk_freelist);
static int usb_blk_nfree = 0;
/* XXX should have different free list for different tags (for speed) */
static LIST_HEAD(, usb_frag_dma) usb_frag_freelist =
	LIST_HEAD_INITIALIZER(usb_frag_freelist);

static void
usbmem_callback(void *arg, bus_dma_segment_t *segs, int nseg, int error)
{
	int i;
        usb_dma_block_t *p = arg;

	if (error == EFBIG) {
		kprintf("usb: mapping to large\n");
		return;
	}

	p->nsegs = nseg;
	for (i = 0; i < nseg && i < sizeof p->segs / sizeof *p->segs; i++)
		p->segs[i] = segs[i];
}

static usbd_status
usb_block_allocmem(bus_dma_tag_t tag, size_t size, size_t align,
		   usb_dma_block_t **dmap)
{
        usb_dma_block_t *p;

	DPRINTFN(5, ("usb_block_allocmem: size=%lu align=%lu\n",
		     (u_long)size, (u_long)align));

	crit_enter();
	/* First check the free list. */
	for (p = LIST_FIRST(&usb_blk_freelist); p; p = LIST_NEXT(p, next)) {
		if (p->tag == tag && p->size >= size && p->align >= align) {
			LIST_REMOVE(p, next);
			usb_blk_nfree--;
			crit_exit();
			*dmap = p;
			DPRINTFN(6,("usb_block_allocmem: free list size=%lu\n",
				    (u_long)p->size));
			logmemory(blkalloc2, p, NULL, size, align);
			return (USBD_NORMAL_COMPLETION);
		}
	}
	crit_exit();

	DPRINTFN(6, ("usb_block_allocmem: no free\n"));
	p = kmalloc(sizeof *p, M_USB, M_INTWAIT);
	logmemory(blkalloc, p, NULL, size, align);

	if (bus_dma_tag_create(tag, align, 0,
	    BUS_SPACE_MAXADDR_32BIT, BUS_SPACE_MAXADDR, NULL, NULL,
	    size, NELEM(p->segs), size, BUS_DMA_ALLOCNOW, &p->tag) == ENOMEM) {
		goto free;
	}

	p->size = size;
	p->align = align;
	if (bus_dmamem_alloc(p->tag, &p->kaddr,
	    BUS_DMA_NOWAIT|BUS_DMA_COHERENT, &p->map))
		goto tagfree;

	if (bus_dmamap_load(p->tag, p->map, p->kaddr, p->size,
	    usbmem_callback, p, 0))
		goto memfree;

	/* XXX - override the tag, ok since we never free it */
	p->tag = tag;
	*dmap = p;
	return (USBD_NORMAL_COMPLETION);

	/*
	 * XXX - do we need to _unload? is the order of _free and _destroy
	 * correct?
	 */
memfree:
	bus_dmamem_free(p->tag, p->kaddr, p->map);
tagfree:
	bus_dma_tag_destroy(p->tag);
free:
	kfree(p, M_USB);
	return (USBD_NOMEM);
}

/*
 * Do not free the memory unconditionally since we might be called
 * from an interrupt context and that is BAD.
 * XXX when should we really free?
 */
static void
usb_block_freemem(usb_dma_block_t *p)
{
	DPRINTFN(6, ("usb_block_freemem: size=%lu\n", (u_long)p->size));
	logmemory(blkfree, p, NULL, p->size, p->align);
	crit_enter();
	LIST_INSERT_HEAD(&usb_blk_freelist, p, next);
	usb_blk_nfree++;
	crit_exit();
}

usbd_status
usb_allocmem(usbd_bus_handle bus, size_t size, size_t align, usb_dma_t *p)
{
	bus_dma_tag_t tag = bus->dmatag;
	usbd_status err;
	struct usb_frag_dma *f;
	usb_dma_block_t *b;
	int i;

	/* compat w/ Net/OpenBSD */
	if (align == 0)
		align = 1;

	/* If the request is large then just use a full block. */
	if (size > USB_MEM_SMALL || align > USB_MEM_SMALL) {
		DPRINTFN(1, ("usb_allocmem: large alloc %d\n", (int)size));
		size = (size + USB_MEM_BLOCK - 1) & ~(USB_MEM_BLOCK - 1);
		err = usb_block_allocmem(tag, size, align, &p->block);
		if (!err) {
			p->block->fullblock = 1;
			p->offs = 0;
			p->len = size;
			logmemory(alloc_full, p, NULL, size, align);
		}
		return (err);
	}

	crit_enter();
	/* Check for free fragments. */
	for (f = LIST_FIRST(&usb_frag_freelist); f; f = LIST_NEXT(f, next))
		if (f->block->tag == tag)
			break;
	if (f == NULL) {
		DPRINTFN(1, ("usb_allocmem: adding fragments\n"));
		err = usb_block_allocmem(tag, USB_MEM_BLOCK, USB_MEM_SMALL,&b);
		if (err) {
			crit_exit();
			return (err);
		}
		b->fullblock = 0;
		/* XXX - override the tag, ok since we never free it */
		b->tag = tag;
		KASSERT(sizeof *f <= USB_MEM_SMALL, ("USB_MEM_SMALL(%d) is too small for struct usb_frag_dma(%zd)\n",
		    USB_MEM_SMALL, sizeof *f));
		for (i = 0; i < USB_MEM_BLOCK; i += USB_MEM_SMALL) {
			f = (struct usb_frag_dma *)((char *)b->kaddr + i);
			f->block = b;
			f->offs = i;
			LIST_INSERT_HEAD(&usb_frag_freelist, f, next);
		}
		f = LIST_FIRST(&usb_frag_freelist);
	}
	p->block = f->block;
	p->offs = f->offs;
	p->len = USB_MEM_SMALL;
	LIST_REMOVE(f, next);
	crit_exit();
	logmemory(alloc_frag, p, NULL, size, align);
	DPRINTFN(5, ("usb_allocmem: use frag=%p size=%d\n", f, (int)size));
	return (USBD_NORMAL_COMPLETION);
}

void
usb_freemem(usbd_bus_handle bus, usb_dma_t *p)
{
	struct usb_frag_dma *f;

	if (p->block->fullblock) {
		DPRINTFN(1, ("usb_freemem: large free\n"));
		usb_block_freemem(p->block);
		logmemory(free_full, p, bus, (size_t)0, (size_t)0);
		return;
	}
	logmemory(free_frag, p, bus, (size_t)0, (size_t)0);
	f = KERNADDR(p, 0);
	f->block = p->block;
	f->offs = p->offs;
	crit_enter();
	LIST_INSERT_HEAD(&usb_frag_freelist, f, next);
	crit_exit();
	DPRINTFN(5, ("usb_freemem: frag=%p\n", f));
}
