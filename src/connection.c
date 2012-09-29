#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>
#include <assert.h>
#include "connection.h"

static int 
make_socket_nonblock(int socket)
{
  int s = fcntl(socket, F_GETFL, 0);
  if (s < 0) return s;
  s |= O_NONBLOCK;
  return fcntl(socket, F_SETFL, s);
}

extern int
connection_make_nonblock(connection_t *connection)
{
    assert(connection);
    return make_socket_nonblock(connection->sock_fd);
}

connection_t*
connection_make(const char* host, int port)
{
  connection_t *connection = (connection_t*)malloc(sizeof(connection_t));
  assert(connection);
  memset(connection, 0, sizeof(connection_t));

  connection->port_no = port;
  connection->sock_fd = -1;
  connection->server = gethostbyname(host);
  if (!connection->server) {
    fprintf(stderr, "ERROR, no host: %s\n", host);
    goto cleanup;
  }

  connection->serv_addr.sin_family = AF_INET;
  bcopy((char*)connection->server->h_addr,
        (char*)&connection->serv_addr.sin_addr.s_addr,
        connection->server->h_length);
  connection->serv_addr.sin_port = htons(connection->port_no);

  connection->is_connected = 0;

  return connection;

cleanup:
  free(connection);
  return NULL;
}

int 
connection_connect(connection_t *connection)
{
  assert(connection->is_connected == 0);
  connection->sock_fd = socket(AF_INET, SOCK_STREAM, 0);
  int err = connect(connection->sock_fd, (struct sockaddr*)&connection->serv_addr, sizeof(connection->serv_addr));
  if (err == -1) {
      return err;
  }
  connection->is_connected = 1;
  return err;
}

int
connection_disconnect(connection_t *connection)
{
  int err = 0;
  if (connection->is_connected) {
    fprintf(stderr, "disconnect connection: %d\n", connection->sock_fd);
    err = shutdown(connection->sock_fd, 2);
    if (err == -1) {
      return err;
    }
    connection->sock_fd = -1;
    connection->is_connected = 0;
  }
  return err;
}

void
connection_free(connection_t *connection)
{
  connection_disconnect(connection);
  free (connection);
}

int
connection_write(connection_t* connection, const char* string, int len)
{
  assert(connection && connection->is_connected);
  return send(connection->sock_fd, string, len, 0);
}

int
connection_read(connection_t* connection, char* buffer, int len)
{
  assert(connection && connection->is_connected);
  return recv(connection->sock_fd, buffer, len, 0);
}
