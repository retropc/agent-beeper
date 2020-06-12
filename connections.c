#include "config.h"
#include "connections.h"

static struct connection *freelist;
static int inited;
static struct connection conns[MAX_CONNECTIONS];

struct connection *new_connection(void) {
  struct connection *c;

  if (!inited) {
    inited = 1;
    for(int i=0;i<MAX_CONNECTIONS;i++) {
      conns[i].next_free = freelist;
      freelist = &conns[i];
    }
  }

  c = freelist;
  if (c) {
    freelist = c->next_free;
  }

  return c;
}

void free_connection(struct connection *c) {
  c->next_free = freelist;
  freelist = c;
}
