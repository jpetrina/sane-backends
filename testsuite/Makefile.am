##  Makefile.am -- an automake template for Makefile.in file
##  Copyright (C) 2009 Chris Bagwell and Sane Developers.
##
##  This file is part of the "Sane" build infra-structure.  See
##  included LICENSE file for license information.

SUBDIRS = backend sanei tools saned

check_PROGRAMS = scanimage_cancel_test
TESTS = $(check_PROGRAMS) scanimage.sh

AM_CPPFLAGS += -I. -I$(srcdir) -I$(top_builddir)/include -I$(top_srcdir)/include

# Keep helper objects separate from the frontend build.
scanimage_cancel_test_CPPFLAGS = $(AM_CPPFLAGS)
scanimage_cancel_test_SOURCES = scanimage_cancel_test.c \
    ../frontend/sicc.c ../frontend/stiff.c ../frontend/jpegtopdf.c
scanimage_cancel_test_LDADD = ../backend/libsane.la ../sanei/libsanei.la \
    ../lib/liblib.la $(PNG_LIBS) $(JPEG_LIBS)

if preloadable_backends_enabled
if WITH_GENESYS_TESTS
scanimage_cancel_test_LDADD += -lstdc++
endif
endif

EXTRA_DIST = README testfile.pnm scanimage.sh

all: help

help:
	@echo "Use 'make test' to run the tests."

test: test.local
	cd saned && $(MAKE) test

test.local:
	@$(srcdir)/scanimage.sh --installed
