##  Makefile.am -- an automake template for Makefile.in file
##  Copyright (C) 2019  Sane Developers.
##
##  This file is part of the "Sane" build infra-structure.  See
##  included LICENSE file for license information.

TEST_LDADD = ../minigtest/libminigtest.la

check_PROGRAMS = scanimage_unit_tests
TESTS = scanimage_unit_tests

AM_CPPFLAGS += -I. -I$(srcdir) -I$(top_builddir)/include -I$(top_srcdir)/include \
    -I$(top_srcdir)/testsuite

scanimage_unit_tests_SOURCES = scanimage_tests.cpp

scanimage_unit_tests_LDADD = $(TEST_LDADD)
