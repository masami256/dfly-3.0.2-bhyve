# $FreeBSD: src/usr.bin/opiepasswd/Makefile,v 1.4.2.4 2002/07/15 14:17:10 des Exp $
# $DragonFly: src/usr.bin/opiepasswd/Makefile,v 1.4 2007/08/27 16:50:57 pavalos Exp $
#
OPIE_DIST?=	${.CURDIR}/../../contrib/opie

PROG=	opiepasswd

CFLAGS+=-I${.CURDIR}/../../lib/libopie
CFLAGS+=-I${OPIE_DIST}
CFLAGS+=-DINSECURE_OVERRIDE
CSTD?=	gnu89
WARNS?=	1

DPADD=	${LIBOPIE} ${LIBMD}
LDADD=	-lopie -lmd

BINOWN=	root
BINMODE=4555
.if !defined(NOFSCHG)
INSTALLFLAGS=-fschg
.endif

.PATH:	${OPIE_DIST}

.include <bsd.prog.mk>
