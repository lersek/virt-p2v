/* virt-p2v
 * Copyright (C) 2009-2019 Red Hat Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/* This file handles running L<nbdkit(1)>. */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <netdb.h>
#include <errno.h>
#include <error.h>
#include <libintl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <assert.h>

#include "p2v.h"

/* How long to wait for the NBD server to start (seconds). */
#define WAIT_NBD_TIMEOUT 10

/* The local port that the NBD server listens on (incremented for
 * each server which is started).
 */
static int nbd_local_port;

/* Supported server types. */
enum nbd_server {
  /* 0 is reserved for "end of list" */
  NBDKIT = 1,
};

/* We use this standard list of nbd server types. Must match the documentation
 * in virt-p2v(1).
 */
static const enum nbd_server standard_servers[] =
  { NBDKIT, 0 };

/* After testing the list of servers passed by the user, this is
 * server we decide to use.
 */
static enum nbd_server use_server;

static pid_t start_nbdkit (const char *device, int *fds, size_t nr_fds);
static int open_listening_socket (int **fds, size_t *nr_fds);
static int bind_tcpip_socket (const char *port, int **fds, size_t *nr_fds);
static int connect_to_nbdkit (int dest_port);

static char *nbd_error;

static void set_nbd_error (const char *fs, ...)
  __attribute__((format(printf,1,2)));

static void
set_nbd_error (const char *fs, ...)
{
  va_list args;
  char *msg;
  int len;

  va_start (args, fs);
  len = vasprintf (&msg, fs, args);
  va_end (args);

  if (len < 0)
    error (EXIT_FAILURE, errno,
           "vasprintf (original error format string: %s)", fs);

  free (nbd_error);
  nbd_error = msg;
}

const char *
get_nbd_error (void)
{
  return nbd_error;
}

/**
 * Test the built-in default list to see which servers are actually installed
 * and appear to be working.
 *
 * Set the C<use_server> global accordingly.
 */
void
test_nbd_servers (void)
{
  size_t i;
  int r;
  const enum nbd_server *servers;

  /* Initialize nbd_local_port. */
  if (is_iso_environment)
    /* The p2v ISO should allow us to open up just about any port, so
     * we can fix a port number in that case.  Using a predictable
     * port number in this case should avoid rare errors if the port
     * colides with another (ie. it'll either always fail or never
     * fail).
     */
    nbd_local_port = 50123;
  else
    /* When testing on the local machine, choose a random port. */
    nbd_local_port = 50000 + (random () % 10000);

  servers = standard_servers;

  use_server = 0;

  for (i = 0; servers[i] != 0; ++i) {
#if DEBUG_STDERR
    fprintf (stderr, "checking for nbdkit ...\n");
#endif

    switch (servers[i]) {
    case NBDKIT: /* with socket activation */
      r = system ("nbdkit file --version"
#ifndef DEBUG_STDERR
                  " >/dev/null 2>&1"
#endif
                  );
      if (r == 0) {
        use_server = servers[i];
        goto finish;
      }
      break;

    default:
      abort ();
    }
  }

 finish:
  if (use_server == 0) {
    fprintf (stderr,
             _("%s: no working NBD server was found, cannot continue.\n"),
             g_get_prgname ());
    exit (EXIT_FAILURE);
  }

#if DEBUG_STDERR
  fprintf (stderr, "picked nbdkit\n");
#endif
}

/**
 * Start the NBD server.
 *
 * We previously tested all NBD servers (see C<test_nbd_servers>) and
 * hopefully found one which will work.
 *
 * Returns the process ID (E<gt> 0) or C<0> if there is an error.
 */
pid_t
start_nbd_server (int *port, const char *device)
{
  int *fds = NULL;
  size_t i, nr_fds;
  pid_t pid;

  switch (use_server) {
  case NBDKIT:                  /* nbdkit with socket activation */
    *port = open_listening_socket (&fds, &nr_fds);
    if (*port == -1) return -1;
    pid = start_nbdkit (device, fds, nr_fds);
    for (i = 0; i < nr_fds; ++i)
      close (fds[i]);
    free (fds);
    return pid;
  }

  abort ();
}

#define FIRST_SOCKET_ACTIVATION_FD 3

/**
 * Set up file descriptors and environment variables for
 * socket activation.
 *
 * Note this function runs in the child between fork and exec.
 */
static inline void
socket_activation (int *fds, size_t nr_fds)
{
  size_t i;
  char nr_fds_str[16];
  char pid_str[16];

  if (fds == NULL) return;

  for (i = 0; i < nr_fds; ++i) {
    int fd = FIRST_SOCKET_ACTIVATION_FD + i;
    if (fds[i] != fd) {
      dup2 (fds[i], fd);
      close (fds[i]);
    }
  }

  snprintf (nr_fds_str, sizeof nr_fds_str, "%zu", nr_fds);
  setenv ("LISTEN_FDS", nr_fds_str, 1);
  snprintf (pid_str, sizeof pid_str, "%d", (int) getpid ());
  setenv ("LISTEN_PID", pid_str, 1);
}

/**
 * Start a local L<nbdkit(1)> process using the
 * L<nbdkit-file-plugin(1)>.
 *
 * C<fds> and C<nr_fds> will contain the locally pre-opened file descriptors
 * for this.
 *
 * Returns the process ID (E<gt> 0) or C<0> if there is an error.
 */
static pid_t
start_nbdkit (const char *device, int *fds, size_t nr_fds)
{
  pid_t pid;
  CLEANUP_FREE char *file_str = NULL;

#if DEBUG_STDERR
  fprintf (stderr, "starting nbdkit for %s using socket activation\n", device);
#endif

  if (asprintf (&file_str, "file=%s", device) == -1)
    error (EXIT_FAILURE, errno, "asprintf");

  pid = fork ();
  if (pid == -1) {
    set_nbd_error ("fork: %m");
    return 0;
  }

  if (pid == 0) {               /* Child. */
    close (0);
    if (open ("/dev/null", O_RDONLY) == -1) {
      perror ("open: /dev/null");
      _exit (EXIT_FAILURE);
    }

    socket_activation (fds, nr_fds);

    execlp ("nbdkit",
            "nbdkit",
            "-r",             /* readonly (vital!) */
            "-f",             /* don't fork */
            "file",           /* file plugin */
            file_str,         /* a device like file=/dev/sda */
            NULL);
    perror ("nbdkit");
    _exit (EXIT_FAILURE);
  }

  /* Parent. */
  return pid;
}

/**
 * This is used when we are starting an NBD server which supports
 * socket activation.  We can open a listening socket on an unused
 * local port and return it.
 *
 * Returns the port number on success or C<-1> on error.
 *
 * The file descriptor(s) bound are returned in the array *fds, *nr_fds.
 * The caller must free the array.
 */
static int
open_listening_socket (int **fds, size_t *nr_fds)
{
  int port;
  char port_str[16];

  /* This just ensures we don't try the port we previously bound to. */
  port = nbd_local_port;

  /* Search for a free port. */
  for (; port < 60000; ++port) {
    snprintf (port_str, sizeof port_str, "%d", port);
    if (bind_tcpip_socket (port_str, fds, nr_fds) == 0) {
      /* See above. */
      nbd_local_port = port + 1;
      return port;
    }
  }

  set_nbd_error ("cannot find a free local port");
  return -1;
}

static int
bind_tcpip_socket (const char *port, int **fds_rtn, size_t *nr_fds_rtn)
{
  struct addrinfo *ai = NULL;
  struct addrinfo hints;
  struct addrinfo *a;
  int err;
  int *fds = NULL;
  size_t nr_fds;
  int addr_in_use = 0;

  memset (&hints, 0, sizeof hints);
  hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG;
  hints.ai_socktype = SOCK_STREAM;

  err = getaddrinfo ("localhost", port, &hints, &ai);
  if (err != 0) {
#if DEBUG_STDERR
    fprintf (stderr, "%s: getaddrinfo: localhost: %s: %s", g_get_prgname (),
             port, gai_strerror (err));
#endif
    return -1;
  }

  nr_fds = 0;

  for (a = ai; a != NULL; a = a->ai_next) {
    int sock, opt;

    sock = socket (a->ai_family, a->ai_socktype, a->ai_protocol);
    if (sock == -1)
      error (EXIT_FAILURE, errno, "socket");

    opt = 1;
    if (setsockopt (sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt) == -1)
      perror ("setsockopt: SO_REUSEADDR");

#ifdef IPV6_V6ONLY
    if (a->ai_family == PF_INET6) {
      if (setsockopt (sock, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof opt) == -1)
        perror ("setsockopt: IPv6 only");
    }
#endif

    if (bind (sock, a->ai_addr, a->ai_addrlen) == -1) {
      if (errno == EADDRINUSE) {
        addr_in_use = 1;
        close (sock);
        continue;
      }
      perror ("bind");
      close (sock);
      continue;
    }

    if (listen (sock, SOMAXCONN) == -1) {
      perror ("listen");
      close (sock);
      continue;
    }

    nr_fds++;
    fds = realloc (fds, sizeof (int) * nr_fds);
    if (!fds)
      error (EXIT_FAILURE, errno, "realloc");
    fds[nr_fds-1] = sock;
  }

  freeaddrinfo (ai);

  if (nr_fds == 0 && addr_in_use) {
#if DEBUG_STDERR
    fprintf (stderr, "%s: unable to bind to localhost:%s: %s\n",
             g_get_prgname (), port, strerror (EADDRINUSE));
#endif
    return -1;
  }

#if DEBUG_STDERR
  fprintf (stderr, "%s: bound to localhost:%s (%zu socket(s))\n",
           g_get_prgname (), port, nr_fds);
#endif

  *fds_rtn = fds;
  *nr_fds_rtn = nr_fds;
  return 0;
}

/**
 * Wait for a local NBD server to start and be listening for
 * connections.
 */
int
wait_for_nbd_server_to_start (int port)
{
  int sockfd = -1;
  int result = -1;
  time_t start_t, now_t;
  struct timespec half_sec = { .tv_sec = 0, .tv_nsec = 500000000 };
  struct timeval timeout = { .tv_usec = 0 };
  char magic[8]; /* NBDMAGIC */
  size_t bytes_read = 0;
  ssize_t recvd;

  time (&start_t);

  for (;;) {
    time (&now_t);

    if (now_t - start_t >= WAIT_NBD_TIMEOUT) {
      set_nbd_error ("timed out waiting for NBD server to start");
      goto cleanup;
    }

    sockfd = connect_to_nbdkit (port);
    if (sockfd >= 0)
      break;

    nanosleep (&half_sec, NULL);
  }

  time (&now_t);
  timeout.tv_sec = (start_t + WAIT_NBD_TIMEOUT) - now_t;
  if (setsockopt (sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) == -1) {
    set_nbd_error ("waiting for NBD server to start: "
                   "setsockopt(SO_RCVTIMEO): %m");
    goto cleanup;
  }

  do {
    recvd = recv (sockfd, magic, sizeof magic - bytes_read, 0);

    if (recvd == -1) {
      set_nbd_error ("waiting for NBD server to start: recv: %m");
      goto cleanup;
    }

    bytes_read += recvd;
  } while (bytes_read < sizeof magic);

  if (memcmp (magic, "NBDMAGIC", sizeof magic) != 0) {
    set_nbd_error ("waiting for NBD server to start: "
                   "'NBDMAGIC' was not received from NBD server");
    goto cleanup;
  }

  result = 0;
 cleanup:
  if (sockfd >= 0)
    close (sockfd);

  return result;
}

/**
 * Connect to C<localhost:dest_port>, resolving the address using
 * L<getaddrinfo(3)>.
 *
 * This may involve multiple connections - to IPv4 and IPv6 for
 * instance.
 */
static int
connect_to_nbdkit (int dest_port)
{
  struct addrinfo hints;
  struct addrinfo *results, *rp;
  char dest_port_str[16];
  int r, sockfd = -1;

  snprintf (dest_port_str, sizeof dest_port_str, "%d", dest_port);

  memset (&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;     /* allow IPv4 or IPv6 */
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICSERV; /* numeric dest port number */
  hints.ai_protocol = 0;           /* any protocol */

  r = getaddrinfo ("localhost", dest_port_str, &hints, &results);
  if (r != 0) {
    set_nbd_error ("getaddrinfo: localhost/%s: %s",
                   dest_port_str, gai_strerror (r));
    return -1;
  }

  for (rp = results; rp != NULL; rp = rp->ai_next) {
    sockfd = socket (rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (sockfd == -1)
      continue;

    /* Connect. */
    if (connect (sockfd, rp->ai_addr, rp->ai_addrlen) == -1) {
      set_nbd_error ("waiting for NBD server to start: "
                     "connect to localhost/%s: %m", dest_port_str);
      close (sockfd);
      sockfd = -1;
      continue;
    }

    break;
  }

  freeaddrinfo (results);
  return sockfd;
}
