# $DragonFly: src/usr.bin/cpio/Makefile,v 1.2 2008/07/26 16:25:31 pavalos Exp $

PROG=	bsdcpio
SRCS=	cmdline.c cpio.c err.c line_reader.c matching.c pathmatch.c
DPADD=	${LIBARCHIVE} ${LIBBZ2} ${LIBLZMA} ${LIBMD} ${LIBZ}
LDADD=	-larchive -lbz2 -llzma -lmd -lz

CONTRIBDIR=	${.CURDIR}/../../contrib/libarchive/cpio
.PATH: ${CONTRIBDIR} ${CONTRIBDIR}/../libarchive_fe
LIBARCHIVE_VERSION!=	cat ${CONTRIBDIR}/../build/version
VERSION!=	echo $$((${LIBARCHIVE_VERSION} / 1000000)).$$((${LIBARCHIVE_VERSION} / 1000 % 1000)).$$((${LIBARCHIVE_VERSION} % 1000))
CFLAGS+=	-I${.CURDIR}/../../lib/libarchive -I${CONTRIBDIR} -I${CONTRIBDIR}/../libarchive_fe
CFLAGS+=	-DPLATFORM_CONFIG_H=\"config.h\"
CFLAGS+=	-DBSDCPIO_VERSION_STRING=\"${VERSION}\"

SYMLINKS=	${BINDIR}/bsdcpio ${BINDIR}/cpio
MLINKS=	bsdcpio.1 cpio.1

.include <bsd.prog.mk>
