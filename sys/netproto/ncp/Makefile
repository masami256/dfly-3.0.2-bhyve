#	$FreeBSD: src/sys/modules/ncp/Makefile,v 1.3 1999/12/12 21:15:51 peter Exp $
#	$DragonFly: src/sys/netproto/ncp/Makefile,v 1.4 2005/09/11 10:52:56 swildner Exp $

.PATH:	${.CURDIR}/../../netncp

KMOD=	ncp

# Build with IPX support (1|0)
NCP_IPX?=	1

# Build with INET support (1|0)
NCP_INET?=	1

SRCS=	ncp_conn.c ncp_sock.c ncp_ncp.c ncp_subr.c ncp_crypt.c ncp_mod.c \
	ncp_rq.c ncp_login.c ncp_nls.c opt_inet.h opt_ipx.h opt_ncp.h

.if defined(NCPBURST)
SRCS+=	ncp_burst.c
CFLAGS+= -DNCPBURST
.endif

opt_inet.h:
	touch ${.TARGET}
.if ${NCP_INET} > 0
	echo "#define INET 1" > ${.TARGET}
.endif

opt_ipx.h:
	touch ${.TARGET}
.if ${NCP_IPX} > 0
	echo "#define IPX 1" > ${.TARGET}
.endif

deinstall:
	rm -f ${DESTDIR}/modules/ncp.ko

.include <bsd.kmod.mk>
