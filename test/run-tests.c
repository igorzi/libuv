/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <stdio.h>
#include <string.h>

#include "uv.h"
#include "runner.h"
#include "task.h"

/* Actual tests and helpers are defined in test-list.h */
#include "test-list.h"

/* The time in milliseconds after which a single test times out. */
#define TEST_TIMEOUT  5000

static int maybe_run_test(int argc, char **argv);


int main(int argc, char **argv) {
  platform_init(argc, argv);

  argv = uv_setup_args(argc, argv);

  switch (argc) {
  case 1: return run_tests(TEST_TIMEOUT, 0);
  case 2: return maybe_run_test(argc, argv);
  case 3: return run_test_part(argv[1], argv[2]);
  default:
    LOGF("Too many arguments.\n");
    return 1;
  }
}


static uv_pipe_t channel;
static uv_tcp_t tcp_server;
static uv_write_t conn_notify_req;
static int close_cb_called;
static int connection_accepted;

static uv_pipe_t stdin_pipe;
static uv_pipe_t stdout_pipe;
static int on_pipe_read_called;
static int after_write_called;
static int tcp_conn_read_cb_called;
static int tcp_conn_write_cb_called;

struct {
  uv_connect_t conn_req;
  uv_write_t tcp_write_req;
  uv_tcp_t conn;
} tcp_conn;

static void close_cb(uv_handle_t* handle) {
  close_cb_called++;
}


void conn_notify_write_cb(uv_write_t* req, int status) {
  uv_close((uv_handle_t*)&tcp_server, close_cb);
  uv_close((uv_handle_t*)&channel, close_cb);
}


void tcp_connection_write_cb(uv_write_t* req, int status) {
  ASSERT((uv_handle_t*)&tcp_conn.conn == (uv_handle_t*)req->handle);
  uv_close((uv_handle_t*)req->handle, close_cb);
  uv_close((uv_handle_t*)&channel, close_cb);
  uv_close((uv_handle_t*)&tcp_server, close_cb);
  tcp_conn_write_cb_called++;
}


void on_tcp_read(uv_stream_t* tcp, ssize_t nread, uv_buf_t buf) {
  uv_buf_t outbuf;
  int r;

  if (nread < 0) {
    if (uv_last_error(tcp->loop).code == UV_EOF) {
      free(buf.base);
      return;
    }

    printf("error recving on tcp connection: %s\n", 
      uv_strerror(uv_last_error(tcp->loop)));
    abort();
  }

  ASSERT(nread > 0);
  ASSERT(memcmp("world\n", buf.base, nread) == 0);
  on_pipe_read_called++;
  free(buf.base);

  /* Write to the socket */
  outbuf = uv_buf_init("hello again\n", 12);
  r = uv_write(&tcp_conn.tcp_write_req, tcp, &outbuf, 1, tcp_connection_write_cb);
  ASSERT(r == 0);

  tcp_conn_read_cb_called++;
}


static uv_buf_t on_read_alloc(uv_handle_t* handle,
    size_t suggested_size) {
  uv_buf_t buf;
  buf.base = (char*)malloc(suggested_size);
  buf.len = suggested_size;
  return buf;
}


static void connect_cb(uv_connect_t* req, int status) {
  int r;

  ASSERT(status == 0);
  r = uv_read_start(req->handle, on_read_alloc, on_tcp_read);
  ASSERT(r == 0);
}


static void ipc_on_connection(uv_stream_t* server, int status) {
  int r;
  uv_buf_t buf;

  if (!connection_accepted) {
    /*
     * Accept the connection and close it.  Also let the other
     * side know.
     */
    ASSERT(status == 0);
    ASSERT((uv_stream_t*)&tcp_server == server);

    r = uv_tcp_init(server->loop, &tcp_conn.conn);
    ASSERT(r == 0);

    r = uv_accept(server, (uv_stream_t*)&tcp_conn.conn);
    ASSERT(r == 0);

    uv_close((uv_handle_t*)&tcp_conn.conn, close_cb);

    buf = uv_buf_init("accepted_connection\n", 20);
    r = uv_write2(&conn_notify_req, (uv_stream_t*)&channel, &buf, 1,
      NULL, conn_notify_write_cb);
    ASSERT(r == 0);

    connection_accepted = 1;
  }
}


static void ipc_on_connection_tcp_conn(uv_stream_t* server, int status) {
  int r;
  uv_buf_t buf;
  uv_tcp_t* conn;

  ASSERT(status == 0);
  ASSERT((uv_stream_t*)&tcp_server == server);

  conn = malloc(sizeof(*conn));
  ASSERT(conn);

  r = uv_tcp_init(server->loop, conn);
  ASSERT(r == 0);

  r = uv_accept(server, (uv_stream_t*)conn);
  ASSERT(r == 0);

  /* Send the accepted connection to the other process */
  buf = uv_buf_init("hello\n", 6);
  r = uv_write2(&conn_notify_req, (uv_stream_t*)&channel, &buf, 1,
    (uv_stream_t*)conn, NULL);
  ASSERT(r == 0);

  r = uv_read_start((uv_stream_t*)conn, on_read_alloc, on_tcp_read);
  ASSERT(r == 0);

  uv_close((uv_handle_t*)conn, close_cb);
}


static int ipc_helper(int listen_after_write) {
  /*
   * This is launched from test-ipc.c. stdin is a duplex channel that we
   * over which a handle will be transmitted.
   */

  uv_write_t write_req;
  int r;
  uv_buf_t buf;

  r = uv_pipe_init(uv_default_loop(), &channel, 1);
  ASSERT(r == 0);

  uv_pipe_open(&channel, 0);

  ASSERT(uv_is_readable((uv_stream_t*) &channel));
  ASSERT(uv_is_writable((uv_stream_t*) &channel));

  r = uv_tcp_init(uv_default_loop(), &tcp_server);
  ASSERT(r == 0);

  r = uv_tcp_bind(&tcp_server, uv_ip4_addr("0.0.0.0", TEST_PORT));
  ASSERT(r == 0);

  if (!listen_after_write) {
    r = uv_listen((uv_stream_t*)&tcp_server, 12, ipc_on_connection);
    ASSERT(r == 0);
  }

  buf = uv_buf_init("hello\n", 6);
  r = uv_write2(&write_req, (uv_stream_t*)&channel, &buf, 1,
      (uv_stream_t*)&tcp_server, NULL);
  ASSERT(r == 0);

  if (listen_after_write) {
    r = uv_listen((uv_stream_t*)&tcp_server, 12, ipc_on_connection);
    ASSERT(r == 0);
  }

  r = uv_run(uv_default_loop());
  ASSERT(r == 0);

  ASSERT(connection_accepted == 1);
  ASSERT(close_cb_called == 3);

  return 0;
}


static int ipc_helper_tcp_connection() {
  /*
   * This is launched from test-ipc.c. stdin is a duplex channel that we
   * over which a handle will be transmitted.
   */

  int r;
  struct sockaddr_in addr;

  r = uv_pipe_init(uv_default_loop(), &channel, 1);
  ASSERT(r == 0);

  uv_pipe_open(&channel, 0);

  ASSERT(uv_is_readable((uv_stream_t*)&channel));
  ASSERT(uv_is_writable((uv_stream_t*)&channel));

  r = uv_tcp_init(uv_default_loop(), &tcp_server);
  ASSERT(r == 0);

  r = uv_tcp_bind(&tcp_server, uv_ip4_addr("0.0.0.0", TEST_PORT));
  ASSERT(r == 0);

  r = uv_listen((uv_stream_t*)&tcp_server, 12, ipc_on_connection_tcp_conn);
  ASSERT(r == 0);

  /* Make a connection to the server */
  r = uv_tcp_init(uv_default_loop(), &tcp_conn.conn);
  ASSERT(r == 0);

  addr = uv_ip4_addr("127.0.0.1", TEST_PORT);
  r = uv_tcp_connect(&tcp_conn.conn_req, (uv_tcp_t*)&tcp_conn.conn, addr, connect_cb);
  ASSERT(r == 0);

  r = uv_run(uv_default_loop());
  ASSERT(r == 0);

  ASSERT(tcp_conn_read_cb_called == 1);
  ASSERT(tcp_conn_write_cb_called == 1);
  ASSERT(close_cb_called == 4);

  return 0;
}


void on_pipe_read(uv_stream_t* tcp, ssize_t nread, uv_buf_t buf) {
  ASSERT(nread > 0);
  ASSERT(memcmp("hello world\n", buf.base, nread) == 0);
  on_pipe_read_called++;

  free(buf.base);

  uv_close((uv_handle_t*)&stdin_pipe, close_cb);
  uv_close((uv_handle_t*)&stdout_pipe, close_cb);
}


static void after_pipe_write(uv_write_t* req, int status) {
  ASSERT(status == 0);
  after_write_called++;
}


static int stdio_over_pipes_helper() {
  /* Write several buffers to test that the write order is preserved. */
  char* buffers[] = {
    "he",
    "ll",
    "o ",
    "wo",
    "rl",
    "d",
    "\n"
  };

  uv_write_t write_req[ARRAY_SIZE(buffers)];
  uv_buf_t buf[ARRAY_SIZE(buffers)];
  int r, i;
  uv_loop_t* loop = uv_default_loop();

  ASSERT(UV_NAMED_PIPE == uv_guess_handle(0));
  ASSERT(UV_NAMED_PIPE == uv_guess_handle(1));

  r = uv_pipe_init(loop, &stdin_pipe, 0);
  ASSERT(r == 0);
  r = uv_pipe_init(loop, &stdout_pipe, 0);
  ASSERT(r == 0);

  uv_pipe_open(&stdin_pipe, 0);
  uv_pipe_open(&stdout_pipe, 1);

  /* Unref both stdio handles to make sure that all writes complete. */
  uv_unref(loop);
  uv_unref(loop);

  for (i = 0; i < ARRAY_SIZE(buffers); i++) {
    buf[i] = uv_buf_init((char*)buffers[i], strlen(buffers[i]));
  }

  for (i = 0; i < ARRAY_SIZE(buffers); i++) {
    r = uv_write(&write_req[i], (uv_stream_t*)&stdout_pipe, &buf[i], 1,
      after_pipe_write);
    ASSERT(r == 0);
  }

  uv_run(loop);

  ASSERT(after_write_called == 7);
  ASSERT(on_pipe_read_called == 0);
  ASSERT(close_cb_called == 0);

  uv_ref(loop);
  uv_ref(loop);

  r = uv_read_start((uv_stream_t*)&stdin_pipe, on_read_alloc,
    on_pipe_read);
  ASSERT(r == 0);

  uv_run(loop);

  ASSERT(after_write_called == 7);
  ASSERT(on_pipe_read_called == 1);
  ASSERT(close_cb_called == 2);

  return 0;
}


static int maybe_run_test(int argc, char **argv) {
  if (strcmp(argv[1], "--list") == 0) {
    print_tests(stdout);
    return 0;
  }

  if (strcmp(argv[1], "ipc_helper_listen_before_write") == 0) {
    return ipc_helper(0);
  }

  if (strcmp(argv[1], "ipc_helper_listen_after_write") == 0) {
    return ipc_helper(1);
  }

  if (strcmp(argv[1], "ipc_send_recv_helper") == 0) {
    int ipc_send_recv_helper(void); /* See test-ipc-send-recv.c */
    return ipc_send_recv_helper();
  }

  if (strcmp(argv[1], "ipc_helper_tcp_connection") == 0) {
    return ipc_helper_tcp_connection();
  }

  if (strcmp(argv[1], "stdio_over_pipes_helper") == 0) {
    return stdio_over_pipes_helper();
  }

  if (strcmp(argv[1], "spawn_helper1") == 0) {
    return 1;
  }

  if (strcmp(argv[1], "spawn_helper2") == 0) {
    printf("hello world\n");
    return 1;
  }

  if (strcmp(argv[1], "spawn_helper3") == 0) {
    char buffer[256];
    fgets(buffer, sizeof(buffer) - 1, stdin);
    buffer[sizeof(buffer) - 1] = '\0';
    fputs(buffer, stdout);
    return 1;
  }

  if (strcmp(argv[1], "spawn_helper4") == 0) {
    /* Never surrender, never return! */
    while (1) uv_sleep(10000);
  }

  return run_test(argv[1], TEST_TIMEOUT, 0);
}
