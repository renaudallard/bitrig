# $OpenBSD: Makefile,v 1.8 2014/12/03 01:06:22 schwarze Exp $

REGRESS_TARGETS = one two multi B c c_man e f h w z ignore
LINT_TARGETS	= B h w

# Postprocessing to remove "character backspace" sequences
# unless they are followed by the same character again.
# This removes underlining as well, so we mustn't use it.
# Cannot use /g because matches progress backwards.

z.out_ascii: z.in
	${NROFF} ${NOPTS} -Tascii ${.ALLSRC} | \
		perl -pe 'while (s/(.)\010(?!\1)//) {}' \
		> ${.TARGET}

.include <bsd.regress.mk>
