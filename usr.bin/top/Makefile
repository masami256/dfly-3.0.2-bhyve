# $FreeBSD: src/usr.bin/top/Makefile,v 1.5.6.2 2002/08/11 17:09:54 dwmalone Exp $
# $DragonFly: src/usr.bin/top/Makefile,v 1.8 2007/08/27 16:50:59 pavalos Exp $
PROG=	top

TOPDIR=	${.CURDIR}/../../contrib/top
.PATH:	${TOPDIR}

CFLAGS+= -DHAVE_GETOPT -DHAVE_STRERROR -I${.CURDIR} -I${.OBJDIR} \
		 -I${TOPDIR} -I. -DORDER

.if ${MACHINE_ARCH} == "i386"
CFLAGS+=-D_ENABLEX32
.endif
.if ${MACHINE_ARCH} == "x86_64"
CFLAGS+=-D_ENABLEX64
.endif

SRCS=	commands.c getopt.c screen.c username.c version.c \
	color.c display.c hash.c m_dragonfly.c top.c utils.c

DPADD=  ${LIBTERMCAP} ${LIBM} ${LIBKVM} ${LIBKINFO}
LDADD=  -ltermcap -lm -lkvm -lkinfo
BINGRP= kmem
BINMODE=2555

WARNS?= 1

.include <bsd.prog.mk>
