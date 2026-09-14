/* scanimage_cancel_test -- regression tests for scanimage cancellation.
   Copyright (C) 2026 Jefferson Kline

   This file is part of the SANE package.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "../include/sane/config.h"

#if defined(HAVE_FORK) && defined(HAVE_POLL) && defined(HAVE_POLL_H)

/* Exercise the actual private handler without changing the frontend API.
 * Only sane_cancel is replaced; no backend is initialized or device opened.
 */
#define main scanimage_main
#define sane_cancel mock_sane_cancel
#include "../frontend/scanimage.c"
#undef sane_cancel
#undef main

#include <poll.h>
#include <sys/wait.h>

static int notification_fd;

void
mock_sane_cancel (SANE_Handle handle)
{
  char event = 'C';

  (void) handle;
  /* A backend may initiate cancellation without completing the operation. */
  if (write (notification_fd, &event, 1) != 1)
    _exit (99);
}

static int
wait_for_event (int fd, char expected)
{
  struct pollfd descriptor = { fd, POLLIN, 0 };
  char event;
  int ready;

  do
    ready = poll (&descriptor, 1, 5000);
  while (ready < 0 && errno == EINTR);

  return ready > 0 && read (fd, &event, 1) == 1 && event == expected;
}

static int
check_abort (int first_signal, int second_signal)
{
  int descriptors[2];
  int status;
  int failed = 1;
  pid_t child, waited;

  if (pipe (descriptors) < 0)
    {
      perror ("pipe");
      return 1;
    }
  child = fork ();
  if (child < 0)
    {
      perror ("fork");
      close (descriptors[0]);
      close (descriptors[1]);
      return 1;
    }
  if (child == 0)
    {
      close (descriptors[0]);
      notification_fd = descriptors[1];
      device = &notification_fd;
      prog_name = "scanimage-cancel-test";
      signal (SIGALRM, SIG_DFL);
      alarm (10);
      if (signal (SIGINT, sighandler) == SIG_ERR
          || signal (SIGTERM, sighandler) == SIG_ERR)
        _exit (98);
      if (write (notification_fd, "R", 1) != 1)
        _exit (99);
      for (;;)
        pause ();
    }

  close (descriptors[1]);
  /* Synchronize on handler installation and the first cancellation request,
   * rather than relying on sleeps or racing two queued signals.
   */
  if (!wait_for_event (descriptors[0], 'R')
      || kill (child, first_signal) < 0
      || !wait_for_event (descriptors[0], 'C')
      || kill (child, second_signal) < 0)
    {
      fprintf (stderr, "signals %d/%d: cancellation handshake failed\n",
               first_signal, second_signal);
      goto cleanup;
    }

  do
    waited = waitpid (child, &status, 0);
  while (waited < 0 && errno == EINTR);
  if (waited != child)
    {
      perror ("waitpid");
      goto cleanup;
    }
  child = -1;
  if (!WIFEXITED (status) || WEXITSTATUS (status) != SANE_STATUS_CANCELLED)
    fprintf (stderr, "signals %d/%d: exit %d, expected cancellation (%d)\n",
             first_signal, second_signal,
             WIFEXITED (status) ? WEXITSTATUS (status) : -1,
             SANE_STATUS_CANCELLED);
  else
    {
      printf ("PASS: signals %d/%d report cancellation\n",
              first_signal, second_signal);
      failed = 0;
    }

cleanup:
  if (child > 0)
    {
      kill (child, SIGKILL);
      while (waitpid (child, &status, 0) < 0 && errno == EINTR)
        ;
    }
  close (descriptors[0]);
  return failed;
}

int
main (void)
{
  int failures = 0;

  failures += check_abort (SIGINT, SIGINT);
  failures += check_abort (SIGTERM, SIGTERM);
  failures += check_abort (SIGINT, SIGTERM);
  failures += check_abort (SIGTERM, SIGINT);
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}

#else

int
main (void)
{
  return 77; /* Automake skip: this test needs POSIX processes and poll. */
}

#endif
