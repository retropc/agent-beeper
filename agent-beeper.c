#define _POSIX_C_SOURCE 199309L
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include "connections.h"

static int create_listener(const char *path) {
  struct sockaddr_un local = { 0 };

  int fd = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK, 0);
  if (fd == -1) {
    perror("socket");
    return -1;
  }

  if (fchmod(fd, 0700) == -1) {
    perror("fchmod");
    close(fd);
    return -1;
  }

  local.sun_family = AF_UNIX;
  strncpy(local.sun_path, path, sizeof(local.sun_path) - 1);
  local.sun_path[sizeof(local.sun_path) - 1] = '\0';

  if (strcmp(local.sun_path, path)) {
    fprintf(stderr, "path too long");
    return -1;
  }

  unlink(local.sun_path);

  if (bind(fd, (struct sockaddr *)&local, strlen(local.sun_path) + sizeof(local.sun_family)) == -1) {
    perror("bind");
    close(fd);
    return -1;
  }

  if (listen(fd, 5) == -1) {
    perror("listen");
    close(fd);
    return -1;
  }

  return fd;
}

static int exec_beep(void) {
  int devnull = open("/dev/null", O_RDWR|O_CLOEXEC);
  if (devnull == -1) {
    perror("open");
    return 1;
  }

  int r = dup2(devnull, 0);
  close(devnull);
  if (r == -1) {
    perror("dup2");
    return 1;
  }

  execlp("beep", "beep", "-f", "800", "-d", "50", "-r", "3", NULL);
//  execlp("sleep", "sleep", "50", NULL);
  perror("execlp");
  return 1;
}

static int beep(void) {
  pid_t p = fork();
  if (p == -1) {
    perror("fork");
    return 0;
  }
  if (p != 0) {
    /* parent */
    return 1;
  }

  /* child */
  _exit(exec_beep());
}

static int connect_agent(const char *path, int *fd_out) {
  struct sockaddr_un local = { 0 };

  int fd = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK, 0);
  if (fd == -1) {
    perror("socket");
    return -1;
  }

  local.sun_family = AF_UNIX;
  strncpy(local.sun_path, path, sizeof(local.sun_path) - 1);
  local.sun_path[sizeof(local.sun_path) - 1] = '\0';

  if (connect(fd, (struct sockaddr *)&local, strlen(local.sun_path) + sizeof(local.sun_family)) == -1) {
    if (errno != EINPROGRESS) {
      perror("connect");
      close(fd);
      return -1;
    }
    *fd_out = fd;
    return 0;
  }

  *fd_out = fd;
  return 1;
}

static int pump(int src, int dst) {
  char buf[8192];
  ssize_t r = recv(src, buf, sizeof(buf), 0);
  if (r == -1) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return 1;
    }
    perror("recv");
    return -1;
  }
  if (r == 0) {
    fprintf(stderr, "recv: EOF\n");
    return 0;
  }

  ssize_t r2 = send(dst, buf, r, MSG_NOSIGNAL);
  if (r2 == -1) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return 1;
    }
    perror("write");
    return -1;
  }
  if (r2 == 0) {
    fprintf(stderr, "send: EOF\n");
    return 0;
  }
  if (r != r2) {
    fprintf(stderr, "send: short write\n");
    return -1;
  }

  return 1;
}

static void close_connection(struct connection *c) {
  close(c->agent.fd);
  close(c->socket.fd);
  free_connection(c);
}

static void finish_agent_connection(struct connection *c) {
  int error;
  socklen_t len = sizeof(error);
  if (getsockopt(c->agent.fd, SOL_SOCKET, SO_ERROR, &error, &len) < -1) {
    perror("getsockopt");
    close_connection(c);
    return;
  }

  if (error != 0) {
    errno = error;
    perror("connect");
    close_connection(c);
    return;
  }

  c->connected = 1;
  c->agent.epoll.events = EPOLLIN;
  if (epoll_ctl(c->epfd, EPOLL_CTL_MOD, c->agent.fd, &c->agent.epoll) == -1) {
    perror("epoll_ctl");
    close_connection(c);
    return;
  }
  if (epoll_ctl(c->epfd, EPOLL_CTL_ADD, c->socket.fd, &c->socket.epoll) == -1) {
    perror("epoll_ctl");
    close_connection(c);
    return;
  }
}

static void handle_socket(struct socket_connection *sc) {
  struct connection *c = sc->conn;

  int fd = sc->fd;
  int other_fd;
  if (sc->type == TYPE_AGENT) {
    if (!c->connected) {
      finish_agent_connection(c);
      return;
    }
    other_fd = c->socket.fd;
  } else {
    other_fd = c->agent.fd;
  }

  int r = pump(fd, other_fd);
  if (r <= 0) {
    close_connection(c);
    return;
  }
}

static void handle_listener(int s, int epfd, const char *agent_path) {
  int s2 = accept4(s, NULL, NULL, SOCK_NONBLOCK|SOCK_CLOEXEC);
  if (s2 == -1) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return;
    }
    perror("accept");
    return;
  }

  struct connection *c = new_connection();
  if (!c) {
    fprintf(stderr, "too many connections\n");
    close(s2);
    return;
  }

  int s3;
  int r = connect_agent(agent_path, &s3);
  if (r == -1) {
    close(s2);
    free_connection(c);
    return;
  }

  c->epfd = epfd;

  c->socket.type = TYPE_SOCKET;
  c->socket.fd = s2;
  c->socket.conn = c;
  c->socket.epoll.data.ptr = &c->socket;
  c->socket.epoll.events = EPOLLIN;

  c->agent.type = TYPE_AGENT;
  c->agent.fd = s3;
  c->agent.conn = c;
  c->agent.epoll.data.ptr = &c->agent;

  if (r == 0) {
    /* in progress */
    c->connected = 0;
    c->agent.epoll.events = EPOLLOUT;
  } else {
    /* connected */
    c->connected = 1;
    c->agent.epoll.events = EPOLLIN;

    if (epoll_ctl(epfd, EPOLL_CTL_ADD, c->socket.fd, &c->socket.epoll) == -1) {
      perror("epoll_ctl");
      close_connection(c);
      return;
    }
  }

  if (epoll_ctl(epfd, EPOLL_CTL_ADD, c->agent.fd, &c->agent.epoll) == -1) {
    perror("epoll_ctl");
    close_connection(c);
    return;
  }

  if (!beep()) {
    close_connection(c);
    return;
  }
}

int main(int argc, char *argv[]) {
  if (argc < 3) {
    fprintf(stderr, "args: %s [listen path] [agent path]\n", argv[0]);
    return 1;
  }

  const char *listen_path = argv[1];
  const char *agent_path = argv[2];

  if (signal(SIGCHLD, SIG_IGN) == SIG_ERR) {
    perror("signal");
    return 1;
  }

  int epfd = epoll_create1(EPOLL_CLOEXEC);
  if (epfd == -1) {
    perror("epoll_create1");
    return 1;
  }

  int s = create_listener(listen_path);
  if (s == -1) {
    return 1;
  }

  struct epoll_event le;
  le.events = EPOLLIN;
  le.data.ptr = NULL;
  if (epoll_ctl(epfd, EPOLL_CTL_ADD, s, &le) == -1) {
    perror("epoll_ctl");
    return 1;
  }

  for (;;) {
    struct epoll_event events[50];
    int count = epoll_wait(epfd, events, 50, -1);
    if (count == -1) {
      perror("epoll_wait");
      break;
    }

    for(int i=0;i<count;i++) {
      struct epoll_event *e = &events[i];
      if (e->data.ptr == NULL) {
        handle_listener(s, epfd, agent_path);
      } else {
        handle_socket((struct socket_connection *)e->data.ptr);
      }
    }
  }

  return 1;
}
