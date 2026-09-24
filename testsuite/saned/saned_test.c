/* sane - Scanner Access Now Easy.

   Copyright (C) 2026 SANE Project

   This file is part of the SANE package and is distributed under the
   terms of the GNU General Public License version 3 or later.

   saned tests. */

#define main saned_main
#include "../../frontend/saned.c"
#undef main

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

static int failures;

SANE_Status
sane_init (SANE_Word * version_code, SANE_Auth_Callback authorize)
{
  (void) authorize;
  if (version_code)
    *version_code = SANE_VERSION_CODE (V_MAJOR, V_MINOR, 0);
  return SANE_STATUS_GOOD;
}

void
sane_exit (void)
{
}

void
sane_set_auth_callback (SANE_Auth_Callback authorize)
{
  (void) authorize;
}

SANE_Status
sane_get_devices (const SANE_Device *** device_list, SANE_Bool local_only)
{
  (void) local_only;
  *device_list = NULL;
  return SANE_STATUS_GOOD;
}

SANE_Status
sane_open (SANE_String_Const name, SANE_Handle * handle)
{
  (void) name;
  (void) handle;
  return SANE_STATUS_UNSUPPORTED;
}

void
sane_close (SANE_Handle handle)
{
  (void) handle;
}

const SANE_Option_Descriptor *
sane_get_option_descriptor (SANE_Handle handle, SANE_Int option)
{
  (void) handle;
  (void) option;
  return NULL;
}

SANE_Status
sane_control_option (SANE_Handle handle, SANE_Int option,
		     SANE_Action action, void *value, SANE_Int * info)
{
  (void) handle;
  (void) option;
  (void) action;
  (void) value;
  (void) info;
  return SANE_STATUS_UNSUPPORTED;
}

SANE_Status
sane_get_parameters (SANE_Handle handle, SANE_Parameters * params)
{
  (void) handle;
  (void) params;
  return SANE_STATUS_UNSUPPORTED;
}

SANE_Status
sane_start (SANE_Handle handle)
{
  (void) handle;
  return SANE_STATUS_UNSUPPORTED;
}

SANE_Status
sane_read (SANE_Handle handle, SANE_Byte * buffer,
	   SANE_Int max_length, SANE_Int * length)
{
  (void) handle;
  (void) buffer;
  (void) max_length;
  *length = 0;
  return SANE_STATUS_EOF;
}

void
sane_cancel (SANE_Handle handle)
{
  (void) handle;
}

SANE_Status
sane_set_io_mode (SANE_Handle handle, SANE_Bool non_blocking)
{
  (void) handle;
  (void) non_blocking;
  return SANE_STATUS_UNSUPPORTED;
}

SANE_Status
sane_get_select_fd (SANE_Handle handle, SANE_Int * fd)
{
  (void) handle;
  *fd = -1;
  return SANE_STATUS_UNSUPPORTED;
}

SANE_String_Const
sane_strstatus (SANE_Status status)
{
  (void) status;
  return "";
}

static void
test_check_v4_in_range_case (const char *name, const char *peer_ip,
			     const char *base_ip, const char *netmask,
			     SANE_Bool expected)
{
  struct sockaddr_in sin;
  SANE_Bool ret;

  memset (&sin, 0, sizeof (sin));
  sin.sin_family = AF_INET;
  if (inet_pton (AF_INET, peer_ip, &sin.sin_addr) != 1)
    {
      fprintf (stderr, "%s: bad peer_ip %s\n", name, peer_ip);
      failures++;
      return;
    }

  ret = check_v4_in_range (&sin, (char *) base_ip, (char *) netmask);

  if (ret != expected)
    {
      fprintf (stderr, "%s: %s/%s contains %s: expected %d, got %d\n",
	       name, base_ip, netmask, peer_ip, expected, ret);
      failures++;
    }
}

static void
test_check_v4_in_range (void)
{
  test_check_v4_in_range_case ("v4 /24 in", "192.168.1.77",
			       "192.168.1.0", "24", SANE_TRUE);
  test_check_v4_in_range_case ("v4 /24 out", "192.168.2.77",
			       "192.168.1.0", "24", SANE_FALSE);
  test_check_v4_in_range_case ("v4 /25 in", "10.0.0.127",
			       "10.0.0.0", "25", SANE_TRUE);
  test_check_v4_in_range_case ("v4 /25 out", "10.0.0.128",
			       "10.0.0.0", "25", SANE_FALSE);
  test_check_v4_in_range_case ("v4 network addr", "10.0.0.0",
			       "10.0.0.0", "24", SANE_TRUE);
  test_check_v4_in_range_case ("v4 /0 in", "1.2.3.4",
			       "0.0.0.0", "0", SANE_TRUE);
  test_check_v4_in_range_case ("v4 /32 in", "10.0.0.1",
			       "10.0.0.1", "32", SANE_TRUE);
  test_check_v4_in_range_case ("v4 /32 out", "10.0.0.2",
			       "10.0.0.1", "32", SANE_FALSE);
  test_check_v4_in_range_case ("v4 alpha netmask", "10.0.0.1",
			       "10.0.0.0", "abc", SANE_FALSE);
  test_check_v4_in_range_case ("v4 netmask 33", "10.0.0.1",
			       "10.0.0.0", "33", SANE_FALSE);
  test_check_v4_in_range_case ("v4 netmask -1", "10.0.0.1",
			       "10.0.0.0", "-1", SANE_FALSE);
  test_check_v4_in_range_case ("v4 netmask 8x", "10.0.0.1",
			       "10.0.0.0", "8x", SANE_FALSE);
}

#ifdef ENABLE_IPV6
static void
test_check_v6_in_range_case (const char *name, const char *peer_ip,
			     const char *base_ip, const char *netmask,
			     SANE_Bool expected)
{
  struct sockaddr_in6 sin6;
  SANE_Bool ret;

  memset (&sin6, 0, sizeof (sin6));
  sin6.sin6_family = AF_INET6;
  if (inet_pton (AF_INET6, peer_ip, &sin6.sin6_addr) != 1)
    {
      fprintf (stderr, "%s: bad peer_ip %s\n", name, peer_ip);
      failures++;
      return;
    }

  ret = check_v6_in_range (&sin6, (char *) base_ip, (char *) netmask);

  if (ret != expected)
    {
      fprintf (stderr, "%s: %s/%s contains %s: expected %d, got %d\n",
	       name, base_ip, netmask, peer_ip, expected, ret);
      failures++;
    }
}

static void
test_check_v6_in_range (void)
{
  test_check_v6_in_range_case ("v6 /64 in", "2001:db8:0:0:77::",
			       "2001:db8::", "64", SANE_TRUE);
  test_check_v6_in_range_case ("v6 /64 out", "2001:db9::77",
			       "2001:db8::", "64", SANE_FALSE);
  test_check_v6_in_range_case ("v6 /17 in", "2001:7fff::1",
			       "2001::", "17", SANE_TRUE);
  test_check_v6_in_range_case ("v6 /17 out", "2001:8000::1",
			       "2001::", "17", SANE_FALSE);
  test_check_v6_in_range_case ("v6 /0 in", "2001:db8::1",
			       "::", "0", SANE_TRUE);
  test_check_v6_in_range_case ("v6 /128 in", "2001:db8::1",
			       "2001:db8::1", "128", SANE_TRUE);
  test_check_v6_in_range_case ("v6 /128 out", "2001:db8::2",
			       "2001:db8::1", "128", SANE_FALSE);
  test_check_v6_in_range_case ("v6 netmask 129", "2001:db8::1",
			       "2001:db8::", "129", SANE_FALSE);
  test_check_v6_in_range_case ("v6 empty netmask", "2001:db8::1",
			       "2001:db8::", "", SANE_FALSE);
  test_check_v6_in_range_case ("v6 netmask 64x", "2001:db8::1",
			       "2001:db8::", "64x", SANE_FALSE);
}
#endif /* ENABLE_IPV6 */

int
main (void)
{
  debug = DBG_ERR;
  log_to_syslog = SANE_FALSE;
  prog_name = "saned_test";
  alarm (60);

  test_check_v4_in_range ();
#ifdef ENABLE_IPV6
  test_check_v6_in_range ();
#endif

  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
