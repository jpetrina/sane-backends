##  Makefile.am -- an automake template for Makefile.in file
##  Copyright (C) 2019  Sane Developers.
##
##  This file is part of the "Sane" build infra-structure.  See
##  included LICENSE file for license information.

check_PROGRAMS = coolscan3_policy_test
TESTS = $(check_PROGRAMS)

AM_CPPFLAGS = -I$(top_srcdir)/backend

coolscan3_policy_test_SOURCES = coolscan3_policy_test.c \
	../../backend/coolscan3-policy.c

if WITH_GENESYS_TESTS
SUBDIRS = genesys
endif
