LDADD = ../lib/libfpac.a $(AX25_LIB)
sbin_PROGRAMS = fpacrtd
fpacrtd_SOURCES = fpacrtd.c

AM_CPPFLAGS = -DAX25_SYSCONFDIR=\""$(sysconfdir)/ax25/"\" \
              -DAX25_LOCALSTATEDIR=\""$(localstatedir)/ax25/"\" \
              -I$(srcdir)/../lib -I$(top_builddir)
