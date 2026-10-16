#ifndef SERVER_H
#define SERVER_H

#define SERVER_MAX_REQUEST 8192
#define SERVER_CLIENT_TIMEOUT_SEC 5

/*
 * Handle one HTTP request on client_fd (read, parse, respond).
 * The caller owns client_fd and must close it afterwards.
 * Safe to call from multiple worker threads.
 */
void server_handle_client(int client_fd);

#endif
