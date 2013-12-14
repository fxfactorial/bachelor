#define _POSIX_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include <openssl/x509.h>
#include <openssl/ssl.h>

#include "qa/qa.h"

#define SOCKET_PROTOCOL 0
#define INVALID_SOCKET  (-1)

/**
 * \brief Converts a uri into a tuple {host, service}.
 *
 * Parses an input string containing a host and (maybe) a service/port.
 * Valid options are:
 *  - service://hostname
 *  - hostname
 *  - hostname:port
 * The resulting tuple will be stored in params \ref host and \ref service.
 * \note \ref uri might be modified during parsing.
 *
 * \param[in]  uri     The input uri.
 * \param[out] host    Place where to store parsed host.
 * \param[out] service Place where to store parsed service.
 *
 * \return 0 if the string was not parsable, 1 otherwise.

 */
int host_port(char *uri, char **host, char **service)
{
  char* c;

  if (!(c = strchr(uri, ':'))) {
    *host = uri;
    *service = NULL;
  } else {
    *c = '\0';
    if (c[1] == '/' && c[2] == '/') {
      *service = uri;
      *host = c+3;
    } else {
      *service = c+1;
      *host = uri;
    }
  }

  return 1;
}

int init_client(const char *host, const char *port)
{
  struct addrinfo hints;
  struct addrinfo *result, *rp;
  int s;
  int i;


  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = 0;
  hints.ai_protocol = 0;

  if ((i=getaddrinfo(host, port, NULL, &result))) {
    BIO_printf(bio_err, "Error: %s\n", gai_strerror(i));
    return -1;
  }

  for (rp=result; rp; rp = rp->ai_next)  {
    s = socket(rp->ai_family,
               rp->ai_socktype,
               rp->ai_protocol);
    if (s == INVALID_SOCKET) continue;

    if (rp->ai_protocol == SOCK_STREAM) {
       i = 0;
       i = setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (char*) &i, sizeof(i));
       if (i < 0) exit(EXIT_FAILURE);
    }

    if (connect(s, rp->ai_addr, rp->ai_addrlen) != -1) break;
  }
  if (!rp) return -1;

  return s;
}

/**
 * \brief Connection informations.
 */
typedef struct qa_connection {
  int socket;   /**< socket file descriptor. */
  SSL* ssl;     /**< ssl handler for this connection. */
  SSL_CTX* ctx; /**< ssl context used in this connection. */
} qa_connection_t;


/**
 * \brief Destructor for a \ref qa_connection.
 *
 * Closes the socket, shuts down the connection, and frees all memory used for
 * holding the connection.
 * \note Input might be partial (ex. a socket exists, but not no ssl session).
 *
 * \param c   The connection to be freed.
 */
static void qa_connection_free(struct qa_connection* c)
{
  if (!c) return;

  if (c->socket != -1)
    close(c->socket);
  if (c->ssl) {
    SSL_shutdown(c->ssl);
    SSL_free(c->ssl);
  }
  if (c->ctx)
    SSL_CTX_free(c->ctx);

  free(c);
}


static int verify_callback(int ok, X509_STORE_CTX* ctx)
{
  return ok;
}


/**
 * \brief Set up a new ssl connection.
 *
 * Create a new \ref qa_connection, turning on OpenSSL (if not yet started), and
 * opening a socket with the target server over ssl.
 *
 * \param[in] conf  Configuration holding informations about the target.
 * \return          The new connection.
 */
struct qa_connection* qa_connection_new(char* address)
{
  struct qa_connection* c;
  char *host, *port;
  int err;

  /* parse input address */
  if (!host_port(address, &host, &port)) goto error;

  c = malloc(sizeof(struct qa_connection));
  if (!c) goto error;
  /* set up context, and protocol versions */
  c->ctx = SSL_CTX_new(SSLv23_client_method());
  if (!c->ctx) goto error;
  /* create the ssl session, disabling certificate verification */
  SSL_CTX_set_verify(c->ctx, SSL_VERIFY_NONE, verify_callback);
  c->ssl = SSL_new(c->ctx);
  if (!c->ssl) goto error;
  /* open the socket over ssl */
  c->socket = init_client(host, port);
  if (c->socket == -1)                goto error;
  if (!SSL_set_fd(c->ssl, c->socket)) goto error;
  if (SSL_connect(c->ssl) != 1)       goto error;

  SSL_set_connect_state(c->ssl);
  return c;

 error:
  /* XXX. add checks for errno, and the ssl error stack (ssl_get_error) */
  qa_connection_free(c);
  return NULL;
}

/**
 * \brief Fetches the certificate opening a tcp connection to the given address.
 *
 * Attempts to open a new tcp connection to the address `address`, and return
 * the X509 certificate presented by the server.
 *
 * \param address[in] the uri to which handshake a ssl connection.
 * \return a valid pointer to the X509 certificate if the handshake succeeded,
 *         NULL otherwise.
 */
X509* get_remote_cert(char *address)
{
  X509 *crt;
  qa_connection_t *c;

  c = qa_connection_new(address);
  if (!c) return NULL;

  crt = SSL_get_peer_certificate(c->ssl);
  qa_connection_free(c);
  return crt;
}
