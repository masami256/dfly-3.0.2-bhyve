# $FreeBSD: src/sys/modules/sound/driver/maestro/Makefile,v 1.4 2005/01/26 16:29:07 imp Exp $
# $DragonFly: src/sys/dev/sound/driver/maestro/Makefile,v 1.3 2007/01/04 21:47:01 corecode Exp $

.PATH: ${.CURDIR}/../../../../dev/sound/pci

KMOD=	snd_maestro
SRCS=	device_if.h bus_if.h pci_if.h
SRCS+=	maestro.c

WERROR=

.include <bsd.kmod.mk>
