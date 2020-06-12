#ifndef CONNECTIONS_H
#define CONNECTIONS_H

#include <sys/epoll.h>

#define TYPE_SOCKET 0
#define TYPE_AGENT 1

struct connection;

struct socket_connection {
  int type;
  int fd;
  struct connection *conn;
  struct epoll_event epoll;
};

struct connection {
  struct socket_connection socket, agent;
  int connected;
  int epfd;
  struct connection *next_free;
};

struct connection *new_connection(void);
void free_connection(struct connection *conn);

#endif
