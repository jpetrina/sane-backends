##  Makefile.am -- an automake template for Makefile.in file
##  Copyright (C) 2019  Sane Developers.
##
##  This file is part of the "Sane" build infra-structure.  See
##  included LICENSE file for license information.

TEST_LDADD = \
  ../../../sanei/libsanei.la \
  ../../../sanei/sanei_tcp.lo \
  ../../../sanei/sanei_usb.lo \
  ../../../sanei/sanei_init_debug.lo \
  ../../../lib/liblib.la \
  ../../../backend/libepsonds.la \
  ../../../backend/sane_strstatus.lo \
  $(JPEG_LIBS) $(USB_LIBS) $(SSL_LIBS) $(MATH_LIB) $(RESMGR_LIBS) $(SOCKET_LIBS) $(XML_LIBS)

AM_CFLAGS += -I. -I$(srcdir) -I$(top_builddir)/include -I$(top_srcdir)/include $(USB_CFLAGS) \
    -DBACKEND_NAME=epsonds -fsanitize=address,fuzzer
AM_LDFLAGS+=-fsanitize=address

bin_PROGRAMS = fuzz
fuzz_SOURCES = fuzz.c
fuzz_LDADD = $(TEST_LDADD)
fuzz_CFLAGS = $(AM_CFLAGS)
fuzz_LDFLAGS = $(AM_LD)