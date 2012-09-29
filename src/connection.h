#ifndef __CONNECTION_H__
#define __CONNECTION_H__

#include <netinet/in.h>

struct hostent;
typedef struct connection_s {
  int sock_fd;
  int port_no;
  int is_connected;
  struct sockaddr_in serv_addr;
  struct hostent *server;
} connection_t;

extern connection_t* 
connection_make(const char* host, int port);

extern void
connection_free(connection_t *connection);

extern int 
connection_connect(connection_t *connection);

extern int
connection_disconnect(connection_t *connection);

extern int 
connection_write(connection_t *connection, const char* string, int len);

extern int
connection_read(connection_t *connection, char* buffer, int len);

extern int
connection_make_nonblock(connection_t *connection);

#endif
