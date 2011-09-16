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
#include <errno.h>
#include <string.h>
#include "uv.h"
#include "internal.h"


static void uv_fs_event_init_handle(uv_loop_t* loop, uv_fs_event_t* handle,
    const char* filename, uv_fs_event_cb cb) {
  handle->type = UV_FS_EVENT;
  handle->loop = loop;
  handle->flags = 0;
  handle->cb = cb;
  handle->is_path_dir = 0;
  handle->dir_handle = INVALID_HANDLE_VALUE;
  handle->buffer = NULL;
  handle->req_pending = 0;
  
  uv_req_init(loop, (uv_req_t*)&handle->req);
  handle->req.type = UV_FS_EVENT_REQ;
  handle->req.data = (void*)handle;

  handle->filename = strdup(filename);
  if (!handle->filename) {
    uv_fatal_error(ERROR_OUTOFMEMORY, "malloc");
  }

  loop->counters.handle_init++;
  loop->counters.fs_event_init++;

  uv_ref(loop);
}


static void uv_fs_event_queue_readdirchanges(uv_loop_t* loop, uv_fs_event_t* handle) {
  assert(handle->dir_handle != INVALID_HANDLE_VALUE);
  assert(!handle->req_pending);

  memset(&(handle->req.overlapped), 0, sizeof(handle->req.overlapped));
  if (!ReadDirectoryChangesW(handle->dir_handle,
                              handle->buffer,
                              4096,
                              FALSE,
                              FILE_NOTIFY_CHANGE_FILE_NAME      |
                                FILE_NOTIFY_CHANGE_DIR_NAME     |
                                FILE_NOTIFY_CHANGE_ATTRIBUTES   |
                                FILE_NOTIFY_CHANGE_SIZE         |
                                FILE_NOTIFY_CHANGE_LAST_WRITE   |
                                FILE_NOTIFY_CHANGE_LAST_ACCESS  |
                                FILE_NOTIFY_CHANGE_CREATION     |
                                FILE_NOTIFY_CHANGE_SECURITY,
                              NULL,
                              &handle->req.overlapped,
                              NULL)) {
    /* Make this req pending reporting an error. */
    SET_REQ_ERROR(&handle->req, GetLastError());
    uv_insert_pending_req(loop, (uv_req_t*)&handle->req);
  }

  handle->req_pending = 1;
}


static int uv_split_path(const wchar_t* filename, wchar_t** dir, wchar_t** file) {
  int len = wcslen(filename);
  int i = len;
  while (i > 0 && filename[--i] != '\\' && filename[i] != '/');

  // $TODO: test with just 'file'
  if (i == 0) {
    *dir = (wchar_t*)malloc((MAX_PATH + 1) * sizeof(wchar_t));
    if (!*dir) {
      uv_fatal_error(ERROR_OUTOFMEMORY, "malloc");
    }

    if (!GetCurrentDirectoryW(MAX_PATH, *dir)) {
      free(*dir);
      *dir = NULL;
      return -1;
    }
    
    *file = wcsdup(filename);
  } else {
    *dir = (wchar_t*)malloc((i + 1) * sizeof(wchar_t));
    if (!*dir) {
      uv_fatal_error(ERROR_OUTOFMEMORY, "malloc");
    }
    wcsncpy(*dir, filename, i);
    (*dir)[i] = L'\0';

    *file = (wchar_t*)malloc((len - i) * sizeof(wchar_t));
    if (!*file) {
      uv_fatal_error(ERROR_OUTOFMEMORY, "malloc");
    }
    wcsncpy(*file, filename + i + 1, len - i - 1);
    (*file)[len - i - 1] = L'\0';
  }
}


int uv_fs_event_init(uv_loop_t* loop, uv_fs_event_t* handle,
    const char* filename, uv_fs_event_cb cb) {
  int name_size;
  DWORD attr, last_error;
  wchar_t* dir = NULL, *dir_to_watch, *filenamew;

  uv_fs_event_init_handle(loop, handle, filename, cb);

  /* Convert name to UTF16. */
  name_size = uv_utf8_to_utf16(filename, NULL, 0) * sizeof(wchar_t);
  filenamew = (wchar_t*)malloc(name_size);
  if (!filenamew) {
    uv_fatal_error(ERROR_OUTOFMEMORY, "malloc");
  }

  if (!uv_utf8_to_utf16(filename, filenamew, 
      name_size / sizeof(wchar_t))) {
    uv_set_sys_error(loop, GetLastError());
    return -1;
  }

  /* Determine whether filename is a file or a directory. */
  attr = GetFileAttributesW(filenamew);
  if (attr == INVALID_FILE_ATTRIBUTES) {
    last_error = GetLastError();
    goto error;
  }

  handle->is_path_dir = attr & FILE_ATTRIBUTE_DIRECTORY;

  if (handle->is_path_dir) {
    dir_to_watch = filenamew;
  } else {
    if (uv_split_path(filenamew, &dir, &handle->filew) != 0) {
      last_error = GetLastError();
      goto error;
    }

    dir_to_watch = dir;
  }

  /* filename points to a directory. */
  handle->dir_handle = CreateFileW(dir_to_watch,
                                   FILE_LIST_DIRECTORY,
                                   FILE_SHARE_READ | FILE_SHARE_DELETE |
                                     FILE_SHARE_WRITE,
                                   NULL,
                                   OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS |
                                     FILE_FLAG_OVERLAPPED,
                                   NULL);

  if (dir) {
    free(dir);
    dir = NULL;
  }

  if (handle->dir_handle == INVALID_HANDLE_VALUE) {
    last_error = GetLastError();
    goto error;
  }

  if (CreateIoCompletionPort(handle->dir_handle,
                              loop->iocp,
                              (ULONG_PTR)handle,
                              0) == NULL) {
    return -1;
  }

  handle->buffer = _aligned_malloc(4096, sizeof(DWORD));
  if (!handle->buffer) {
    uv_fatal_error(ERROR_OUTOFMEMORY, "malloc");
  }

  memset(&(handle->req.overlapped), 0, sizeof(handle->req.overlapped));

  if (!ReadDirectoryChangesW(handle->dir_handle,
                             handle->buffer,
                             4096,
                             FALSE,
                             FILE_NOTIFY_CHANGE_FILE_NAME      |
                               FILE_NOTIFY_CHANGE_DIR_NAME     |
                               FILE_NOTIFY_CHANGE_ATTRIBUTES   |
                               FILE_NOTIFY_CHANGE_SIZE         |
                               FILE_NOTIFY_CHANGE_LAST_WRITE   |
                               FILE_NOTIFY_CHANGE_LAST_ACCESS  |
                               FILE_NOTIFY_CHANGE_CREATION     |
                               FILE_NOTIFY_CHANGE_SECURITY,
                             NULL,
                             &handle->req.overlapped,
                             NULL)) {
    last_error = GetLastError();
    goto error;
  }

  handle->req_pending = 1;
  return 0;

error:
  if (handle->filename) {
    free(handle->filename);
    handle->filename = NULL;
  }

  if (handle->filew) {
    free(handle->filew);
    handle->filew = NULL;
  }

  if (handle->dir_handle != INVALID_HANDLE_VALUE) {
    CloseHandle(handle->dir_handle);
    handle->dir_handle = INVALID_HANDLE_VALUE;
  }

  if (handle->buffer) {
    _aligned_free(handle->buffer);
    handle->buffer = NULL;
  }

  uv_set_sys_error(loop, last_error);
  return -1;
}


void uv_process_fs_event_req(uv_loop_t* loop, uv_req_t* req,
    uv_fs_event_t* handle) {
  FILE_NOTIFY_INFORMATION* file_info;
  DWORD offset = 0;

  assert(handle->req_pending);
  handle->req_pending = 0;

  if (REQ_SUCCESS(req) && req->overlapped.InternalHigh > 0) {
    do {
      file_info = (FILE_NOTIFY_INFORMATION*)((char*)handle->buffer + offset);

      if (handle->is_path_dir || _wcsnicmp(handle->filew, file_info->FileName,
        file_info->FileNameLength / sizeof(wchar_t)) == 0) {
        switch (file_info->Action) {
          case FILE_ACTION_ADDED:
          case FILE_ACTION_REMOVED:
          case FILE_ACTION_RENAMED_NEW_NAME:
            handle->cb(handle, NULL, UV_RENAME, 0);
            break;

          case FILE_ACTION_MODIFIED:
            handle->cb(handle, NULL, UV_CHANGE, 0);
            break;
        }
      }

      offset = file_info->NextEntryOffset;
    } while(offset);
  } else {
    // $TODO: set error if InternalHigh == 0
    loop->last_error = GET_REQ_UV_ERROR(req);
    handle->cb(handle, NULL, -1, -1);
  }

  if (!(handle->flags & UV_HANDLE_CLOSING)) {
    uv_fs_event_queue_readdirchanges(loop, handle);
  } else {
    uv_want_endgame(loop, (uv_handle_t*)handle);
  }
}


void uv_fs_event_close(uv_loop_t* loop, uv_fs_event_t* handle) {
  if (handle->dir_handle != INVALID_HANDLE_VALUE) {
    CloseHandle(handle->dir_handle);
  }

  if (handle->filename) {
    free(handle->filename);
    handle->filename = NULL;
  }

  if (!handle->req_pending) {
    uv_want_endgame(loop, (uv_handle_t*)handle);
  }
}


void uv_fs_event_endgame(uv_loop_t* loop, uv_fs_event_t* handle) {
  if (handle->flags & UV_HANDLE_CLOSING &&
      !handle->req_pending) {
    assert(!(handle->flags & UV_HANDLE_CLOSED));
    handle->flags |= UV_HANDLE_CLOSED;

    if (handle->buffer) {
      _aligned_free(handle->buffer);
      handle->buffer = NULL;
    }

    if (handle->filew) {
      free(handle->filew);
      handle->filew = NULL;
    }

    if (handle->close_cb) {
      handle->close_cb((uv_handle_t*)handle);
    }

    uv_unref(loop);
  }
}