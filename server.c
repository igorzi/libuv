
#include "../libuv/include/uv.h"
#include <io.h>
#include <stdio.h>
#include <string.h>

int server_id;
int accepted = 0;

uv_timer_t timer;
uv_tcp_t main_server;

char exe_path[1024];
size_t exe_path_size;

char message[] = "HTTP 1.1 200 OK\r\nContent-Length: 12\r\nConnection: close\r\n\r\nhello world\n";

#define CHECK(r) \
  if (!(r)) abort();

#define LOG(msg, ...) \
  printf("Server %d: " ## msg, GetCurrentThreadId(), __VA_ARGS__); \

void cl_close_cb(uv_handle_t* handle) {
  free(handle);
}

void cl_shutdown_cb(uv_shutdown_t* req, int status) {
  uv_close((uv_handle_t*) req->handle, cl_close_cb);
  free(req);
}

void cl_write_cb(uv_write_t* req, int status) {
  uv_close((uv_handle_t*) req->handle, cl_close_cb);
  free(req);
}

uv_buf_t cl_alloc_cb(uv_handle_t* handle, size_t suggested_size) {
  uv_buf_t buf;
  buf.base = (char*)malloc(4096);
  buf.len = 4096;
  return buf;
}

void cl_write(uv_tcp_t* handle) {
  int r;
  int volatile fubar;

  uv_buf_t buf = uv_buf_init(message, (sizeof message) - 1);
  uv_write_t* req = (uv_write_t*)malloc(sizeof *req);

  for (fubar = 50000; fubar > 0; fubar--) {}

  r = uv_write(req, (uv_stream_t*) handle, &buf, 1, cl_write_cb);
  if (r) {
    LOG("error");
    uv_close((uv_handle_t*) handle, cl_close_cb);
    free(req);
  }
  

  // Pretend our server is very busy:
  // Sleep(10);
}

void cl_read_cb(uv_stream_t* stream, ssize_t nread, uv_buf_t buf) {
  free(buf.base);

  if (nread > 0) {
    cl_write((uv_tcp_t*)stream);
  }
}

void connection_cb(uv_stream_t* server, int status) {
  int r;
  uv_tcp_t* client = (uv_tcp_t*) malloc(sizeof *client);

  CHECK(status == 0);

  //LOG("accepted\n");

  r = uv_tcp_init(server->loop, client);
  CHECK(r == 0);

  r = uv_accept(server, (uv_stream_t*) client);
  CHECK(r == 0);

  //accepted++;

  uv_read_start((uv_stream_t*) client, cl_alloc_cb, cl_read_cb);

  //LOG("accepted\n");
}


void timer_cb(uv_timer_t* timer, int status) {
  LOG("accepted %d connections\n", accepted);
}

DWORD WINAPI server_proc(LPVOID lpThreadParameter) {
  int r;
  uv_loop_t* loop = (uv_loop_t*)lpThreadParameter;
  uv_tcp_t server;

  r = uv_tcp_init(loop, &server);
  CHECK(r == 0);

  // Start listening now
  LOG("listen\n");
  r = uv_tcp_listen_import(&server, &main_server, 200, connection_cb);
  CHECK(r == 0);

  uv_run(loop);

  return 0;
}


int main(int argv, char** argc) {
  int i, r;
  int num_children = 1;
  uv_loop_t* default_loop;
  HANDLE threads[16];

  if (argv == 2) {
    num_children = strtol(argc[1], NULL, 10);
  }

  default_loop = uv_default_loop(num_children);

  r = uv_tcp_init(default_loop, &main_server);
  CHECK(r == 0);

  r = uv_tcp_bind(&main_server, uv_ip4_addr("0.0.0.0", 80));
  CHECK(r == 0);

  // Start listening now
  LOG("listen\n");
  r = uv_listen((uv_stream_t*) &main_server, 200, connection_cb);
  CHECK(r == 0);

  for (i = 0; i < num_children - 1; i++) {
    threads[i] = CreateThread(NULL, 0, server_proc, default_loop, 0, NULL);
  }

  uv_run(default_loop);

  //r = uv_timer_init(&timer);
  //CHECK(r == 0);
  //uv_timer_start(&timer, timer_cb, 1000, 1000);
}