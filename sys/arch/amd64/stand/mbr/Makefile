#	$OpenBSD: Makefile,v 1.10 2014/07/13 21:18:44 jasper Exp $
#

S=	${.CURDIR}/../../../..
SADIR=	${.CURDIR}/..

PROG=	mbr
SRCS=	mbr.S
AFLAGS+=-m32 -Wa,--32
AFLAGS+=-I${.CURDIR} -I${.CURDIR}/../../.. #-Wa,-a
AFLAGS+=-fno-pie
.if ${COMPILER_VERSION:L} == "clang"
AFLAGS+=-no-integrated-as
.endif
LD=ld
LDFLAGS=-nostdlib -Ttext 0 -x -N -s -Bstatic -e start -nopie
LDFLAGS+=-melf_i386 -L/usr/libdata

NOMAN=
#MAN+=	mbr.8

INSTALL_STRIP=

# Uncomment this to make mbr talk to a serial port.
#CPPFLAGS+=-DSERIAL=0

.depend mbr.o: assym.h

${PROG}: $(OBJS)
	$(LD) $(LDFLAGS) -o $(PROG) $(OBJS)
	@size $(PROG)
	@if [ -x ${.OBJDIR}/${PROG} ]; then \
		objcopy -O binary ${PROG} ${.OBJDIR}/.tmp;\
		mv -f ${.OBJDIR}/.tmp ${.OBJDIR}/${PROG}; \
		ls -l ${.OBJDIR}/${PROG}; \
	fi

.include <bsd.prog.mk>
