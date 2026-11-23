LDADD = $(AX25_LIB) $(MATH_LIB) ../lib/libfpac.a 
sbin_PROGRAMS = calibrate aprslist mailbbs
calibrate_SOURCES = calibrate.c
aprslist_SOURCES = aprslist.c
mailbbs_SOURCES = mailbbs.c

# fpacmd5: run by the sysop on his own machine, only needs the MD5 of libfpac
bin_PROGRAMS = fpacmd5
fpacmd5_SOURCES = fpacmd5.c
fpacmd5_LDADD = ../lib/libfpac.a

AM_CPPFLAGS = -DAX25_SYSCONFDIR=\""$(sysconfdir)/ax25/"\" \
              -DAX25_LOCALSTATEDIR=\""$(localstatedir)/ax25/"\" \
	      -I$(srcdir)/../lib 

AX25_SYSCONFDIR=@sysconfdir@/ax25/
AX25_LOCALSTATEDIR=@localstatedir@/ax25/

