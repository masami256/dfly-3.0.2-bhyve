# $FreeBSD: src/sys/modules/padlock/Makefile,v 1.3 2007/03/21 17:37:13 sam Exp $

KMOD=	padlock
SRCS=	padlock.c padlock_cipher.c padlock_hash.c padlock_rng.c rng_harvest_$(MACHINE_ARCH).S
SRCS   += device_if.h bus_if.h opt_bus.h cryptodev_if.h

.include <bsd.kmod.mk>
