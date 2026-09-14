##  Makefile.am -- an automake template for Makefile.in file
##  Copyright (C) 2009 Chris Bagwell and Sane Developers.
##
##  This file is part of the "Sane" build infra-structure.  See
##  included LICENSE file for license information.

SUBDIRS = backend sanei tools

check_PROGRAMS = scanimage_cancel_test
TESTS = $(check_PROGRAMS)

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

SCANIMAGE = ../frontend/scanimage$(EXEEXT)
TESTFILE  = $(srcdir)/testfile.pnm
OUTFILE   = outfile.pnm
DEVICE    = test
OPTIONS   = --mode Color --depth 16 --test-picture "Color pattern" --resolution 50 -y 20 -x 20 > $(OUTFILE)

EXTRA_DIST = README testfile.pnm
CLEANFILES = $(OUTFILE)

all: help

help:
	@echo "Use 'make test' to run the tests."

test: test.local

test.local:
	@echo "**** Testing $(SCANIMAGE) with device $(DEVICE)"
	@if echo "---> Trying flatbed scanner" && \
	  $(SCANIMAGE) -d $(DEVICE) -T && \
	  echo "<--- Flatbed scanner succeeded" && \
	  echo "---> Trying three pass flatbed scanner" && \
	  $(SCANIMAGE) -d $(DEVICE) --mode Color --three-pass=yes   -T && \
	  echo "<--- Three pass scanner succeeded" && \
	  echo "---> Trying hand scanner" && \
	  $(SCANIMAGE) -d $(DEVICE) --hand-scanner=yes -T && \
	  echo "<--- Hand scanner succeeded" && \
	  echo "---> Checking 16 bit color mode" && \
	  $(SCANIMAGE) -d $(DEVICE) $(OPTIONS) && \
	  cmp -s $(TESTFILE) $(OUTFILE) && \
	  echo "<--- 16 bit color mode succeeded" && \
	  rm $(OUTFILE) ; \
	then echo ; echo ; echo "**** All tests passed" ; \
	else echo ; echo; \
	echo "**** Something failed (maybe test backend not enabled by configure?)";\
	exit 1; \
	fi
