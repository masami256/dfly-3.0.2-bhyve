# $Whistle: Makefile,v 1.5 1999/01/24 06:48:37 archie Exp $
# $FreeBSD: src/sys/modules/netgraph/Makefile,v 1.8.2.8 2002/08/20 23:48:15 archie Exp $

SUBDIR=	UI \
	async \
	atmllc \
	bpf \
	bridge \
	cisco \
	deflate \
	echo \
	etf \
	ether \
	frame_relay \
	hole \
	hub \
	iface \
	ksocket \
	l2tp \
	lmi \
	mppc \
	netgraph \
	one2many \
	ppp \
	pppoe \
	pptpgre \
	rfc1490 \
	socket \
	tcpmss \
	tee \
	vjc
#	eiface \
#	fec \
#	netgraph \
#	socket \
#	sync_ar \
#	sync_sr \
#	tty \

.if !defined(NO_CRYPT) && exists(${.CURDIR}/../crypto/rc4/rc4.c)
#SUBDIR+=	mppc
.endif

.include <bsd.subdir.mk>
