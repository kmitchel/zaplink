#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

/* Run the HTTP server until shutdown. Returns zero after an orderly shutdown
 * and nonzero when listener setup or the event loop fails. */
int start_http_server(int port);

#endif
