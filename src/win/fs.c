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

#include <assert.h>
#include <malloc.h>

#include "uv.h"
#include "internal.h"
#include <io.h>
#include <fcntl.h>

#define UV_FS_FREE_ARG0          0x0001
#define UV_FS_FREE_ARG1          0x0002
#define UV_FS_FREE_ARG2          0x0004
#define UV_FS_FREE_ARG3          0x0008

#define SET_REQ_ERRNO(req)             \
        if (req->result == -1) {  \
          req->errorno = errno;   \
        }

#define STRDUP_ARG(req, i)                         \
        req->arg##i## = (void*)strdup((const char*)req->arg##i##);                                \
        if (!req->arg##i##) {                                       \
          uv_fatal_error(ERROR_OUTOFMEMORY, "malloc");    \
        }                                                 \
        req->flags |= UV_FS_FREE_ARG##i##;


#define WRAP_REQ_ARGS1(req, a0)              \
        req->arg0 = (void*)a0;

#define WRAP_REQ_ARGS2(req, a0, a1)        \
        WRAP_REQ_ARGS1(req, a0)               \
        req->arg1 = (void*)a1;

#define WRAP_REQ_ARGS3(req, a0, a1, a2)   \
        WRAP_REQ_ARGS2(req, a0, a1)                    \
        req->arg2 = (void*)a2;

#define WRAP_REQ_ARGS4(req, a0, a1, a2, a3)   \
        WRAP_REQ_ARGS3(req, a0, a1, a2)                    \
        req->arg3 = (void*)a3;

#define QUEUE_FS_TP_JOB(req)                                                 \
    if (!QueueUserWorkItem(&fs_thread_proc, req, WT_EXECUTELONGFUNCTION)) {   \
      /* TODO: set error */                                                   \
      uv_insert_pending_req((uv_req_t*)req);                                  \
    }                                                                         \
    uv_ref();


void uv_fs_init() {
  _fmode = _O_BINARY;
}

static void uv_fs_req_sync_init(uv_fs_req_t* req, uv_fs_type fs_type) {
  uv_req_init((uv_req_t*) req);
  req->fs_type = fs_type;
}


static void uv_fs_req_async_init(uv_fs_req_t* req, uv_fs_type fs_type, uv_tp_cb cb) {
  uv_req_init((uv_req_t*) req);
  req->fs_type = fs_type;
  req->cb = cb;
  memset(&req->overlapped, 0, sizeof(req->overlapped));
}

// $TODO: figure out inline or whether we need these wrappers at all
__inline void fs__open(uv_fs_req_t* req, const char* path, int flags, int mode) {
  req->result = _open(path, flags, mode);
  SET_REQ_ERRNO(req);
}


__inline void fs__close(uv_fs_req_t* req, uv_native_file_type file) {
  req->result = _close(file);
  SET_REQ_ERRNO(req);
}


__inline void fs__read(uv_fs_req_t* req, uv_native_file_type file, void *buf, size_t length, off_t offset) {
  if (offset != -1) {
    _lseek(file, offset, SEEK_SET);
  }

  req->result = _read(file, buf, length);
  SET_REQ_ERRNO(req);
}


static DWORD WINAPI fs_thread_proc(void* parameter) {
  uv_fs_req_t* req = (uv_fs_req_t*)parameter;

  assert(req != NULL);

  switch (req->fs_type) {
    case UV_FS_OPEN:
      fs__open(req, (const char*)req->arg0, (int)req->arg1, (int)req->arg2);
      break;
    case UV_FS_CLOSE:
      fs__close(req, (uv_native_file_type)req->arg0);
      break;
    case UV_FS_READ:
      fs__read(req, (uv_native_file_type)req->arg0, req->arg1, (size_t)req->arg2, (off_t)req->arg3);
      break;
  }

  /* Free stashed arguments; we no longer need them. */
  if (req->flags & UV_FS_FREE_ARG0) {
    free(req->arg0);
    req->arg0 = NULL;
  }

  if (req->flags & UV_FS_FREE_ARG1) {
    free(req->arg1);
    req->arg1 = NULL;
  }

  if (req->flags & UV_FS_FREE_ARG2) {
    free(req->arg2);
    req->arg2 = NULL;
  }

  if (req->flags & UV_FS_FREE_ARG3) {
    free(req->arg3);
    req->arg3 = NULL;
  }

  /* post getaddrinfo completed */
  POST_COMPLETION_FOR_REQ(req);

  return 0;
}


void uv_fs_open(uv_fs_req_t* req, const char* path, int flags, int mode, uv_tp_cb cb) {
  if (cb) {
    uv_fs_req_async_init(req, UV_FS_OPEN, cb);
    WRAP_REQ_ARGS3(req, path, flags, mode);
    STRDUP_ARG(req, 0);
    QUEUE_FS_TP_JOB(req);
  } else {
    uv_fs_req_sync_init(req, UV_FS_OPEN);
    fs__open(req, path, flags, mode);
  }
}


void uv_fs_close(uv_fs_req_t* req, uv_native_file_type file, uv_tp_cb cb) {
  if (cb) {
    uv_fs_req_async_init(req, UV_FS_CLOSE, cb);
    WRAP_REQ_ARGS1(req, file);
    QUEUE_FS_TP_JOB(req);
  } else {
    uv_fs_req_sync_init(req, UV_FS_CLOSE);
    fs__close(req, file);
  }
}


void uv_fs_read(uv_fs_req_t* req, uv_native_file_type file, void *buf, size_t length, off_t offset, uv_tp_cb cb) {
  if (cb) {
    uv_fs_req_async_init(req, UV_FS_READ, cb);
    WRAP_REQ_ARGS4(req, file, buf, length, offset);
    QUEUE_FS_TP_JOB(req);
  } else {
    uv_fs_req_sync_init(req, UV_FS_READ);
    fs__read(req, file, buf, length, offset);
  }
}


void uv_process_fs_req(uv_fs_req_t* req) {
  assert(req->cb);
  req->cb(req);
}