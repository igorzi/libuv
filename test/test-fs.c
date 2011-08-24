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

#include "uv.h"
#include "task.h"

#include <fcntl.h>


static char exepath[1024];
static size_t exepath_size = sizeof(exepath);

static uv_fs_req_t open_req;
static uv_fs_req_t read_req;
static uv_fs_req_t close_req;

static char buf[128];

void read_cb(uv_fs_req_t* req) {
  ASSERT(req == &read_req);
  ASSERT(req->fs_type == UV_FS_READ);
  ASSERT(req->result != -1);
}

void open_cb(uv_fs_req_t* req) {
  ASSERT(req == &open_req);
  ASSERT(req->fs_type == UV_FS_OPEN);
  ASSERT(req->result != -1);

  uv_fs_read(&read_req, req->result, buf, sizeof(buf), -1, );
}


TEST_IMPL(fs_async) {
  uv_init();

  int r = uv_exepath(exepath, &exepath_size);
  ASSERT(r == 0);

  uv_fs_open(&open_req, exepath, _O_RDONLY, 0, open_cb);

  uv_run();
  return 0;
}
