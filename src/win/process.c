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
#include "../uv-common.h"
#include "internal.h"

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <windows.h>

#define UTF8_TO_UTF16(s, t)                               \
  size = uv_utf8_to_utf16(s, NULL, 0) * sizeof(wchar_t);  \
  t = (wchar_t*)malloc(size);                             \
  if (!t) {                                               \
    uv_fatal_error(ERROR_OUTOFMEMORY, "malloc");          \
  }                                                       \
  if (!uv_utf8_to_utf16(s, t, size / sizeof(wchar_t))) {  \
    uv_set_sys_error(GetLastError());                     \
    goto error;                                           \
  }


static const wchar_t DEFAULT_PATH[1] = L"";
static const wchar_t DEFAULT_PATH_EXT[20] = L".COM;.EXE;.BAT;.CMD";

static int uv_process_init(uv_process_t* handle) {
  handle->type = UV_PROCESS;
  handle->flags = 0;
  handle->error = uv_ok_;
  handle->exit_cb = NULL;
  handle->pid = 0;
  handle->exit_signal = 0;
  handle->wait_handle = INVALID_HANDLE_VALUE;
  handle->process_handle = INVALID_HANDLE_VALUE;
  handle->stdio_pipes[0].server_pipe = NULL;
  handle->stdio_pipes[0].child_pipe = INVALID_HANDLE_VALUE;
  handle->stdio_pipes[1].server_pipe = NULL;
  handle->stdio_pipes[1].child_pipe = INVALID_HANDLE_VALUE;
  handle->stdio_pipes[2].server_pipe = NULL;
  handle->stdio_pipes[2].child_pipe = INVALID_HANDLE_VALUE;

  uv_req_init((uv_req_t*)&handle->exit_req);
  handle->exit_req.type = UV_PROCESS_EXIT;
  handle->exit_req.data = handle;

  uv_counters()->handle_init++;
  uv_counters()->process_init++;

  uv_ref();

  return 0;
}


static struct watcher_status_struct {
  uv_async_t async_watcher;
  HANDLE lock;
  int num_active;
} watcher_status;


/*
 * Path search functions
 */

/*
 * Helper function for search_path
 */
static wchar_t* search_path_join_test(const wchar_t* dir,
                                      int dir_len,
                                      const wchar_t* name,
                                      int name_len,
                                      const wchar_t* ext,
                                      int ext_len,
                                      const wchar_t* cwd,
                                      int cwd_len) {
  wchar_t *result, *result_pos;
  DWORD attrs;

  if (dir_len >= 1 && (dir[0] == L'/' || dir[0] == L'\\')) {
    /* It's a full path without drive letter, use cwd's drive letter only */
    cwd_len = 2;
  } else if (dir_len >= 2 && dir[1] == L':' &&
      (dir_len < 3 || (dir[2] != L'/' && dir[2] != L'\\'))) {
    /* It's a relative path with drive letter (ext.g. D:../some/file)
     * Replace drive letter in dir by full cwd if it points to the same drive,
     * otherwise use the dir only.
     */
    if (cwd_len < 2 || _wcsnicmp(cwd, dir, 2) != 0) {
      cwd_len = 0;
    } else {
      dir += 2;
      dir_len -= 2;
    }
  } else if (dir_len > 2 && dir[1] == L':') {
    /* It's an absolute path with drive letter
     * Don't use the cwd at all
     */
    cwd_len = 0;
  }

  /* Allocate buffer for output */
  result = result_pos =
      (wchar_t*)malloc(sizeof(wchar_t) * (cwd_len + 1 + dir_len + 1 + name_len + 1 + ext_len + 1));

  /* Copy cwd */
  wcsncpy(result_pos, cwd, cwd_len);
  result_pos += cwd_len;

  /* Add a path separator if cwd didn't end with one */
  if (cwd_len && wcsrchr(L"\\/:", result_pos[-1]) == NULL) {
    result_pos[0] = L'\\';
    result_pos++;
  }

  /* Copy dir */
  wcsncpy(result_pos, dir, dir_len);
  result_pos += dir_len;

  /* Add a separator if the dir didn't end with one */
  if (dir_len && wcsrchr(L"\\/:", result_pos[-1]) == NULL) {
    result_pos[0] = L'\\';
    result_pos++;
  }

  /* Copy filename */
  wcsncpy(result_pos, name, name_len);
  result_pos += name_len;

  /* Copy extension */
  if (ext_len) {
    result_pos[0] = L'.';
    result_pos++;
    wcsncpy(result_pos, ext, ext_len);
    result_pos += ext_len;
  }

  /* Null terminator */
  result_pos[0] = L'\0';

  attrs = GetFileAttributesW(result);

  if (attrs != INVALID_FILE_ATTRIBUTES &&
     !(attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
    return result;
  }

  free(result);
  return NULL;
}


/*
 * Helper function for search_path
 */
static wchar_t* path_search_walk_ext(const wchar_t *dir,
                                     int dir_len,
                                     const wchar_t *name,
                                     int name_len,
                                     wchar_t *cwd,
                                     int cwd_len,
                                     const wchar_t *path_ext,
                                     int name_has_ext) {
  wchar_t* result = NULL;

  const wchar_t *ext_start,
              *ext_end = path_ext;

  /* If the name itself has a nonemtpy extension, try this extension first */
  if (name_has_ext) {
    result = search_path_join_test(dir, dir_len,
                                   name, name_len,
                                   L"", 0,
                                   cwd, cwd_len);
  }

  /* Add path_ext extensions and try to find a name that matches */
  while (result == NULL) {
    if (*ext_end == L'\0') {
      break;
    }

    /* Skip the separator that ext_end now points to */
    if (ext_end != path_ext) {
      ext_end++;
    }

    /* Find the next dot in path_ext */
    ext_start = wcschr(ext_end, L'.');
    if (ext_start == NULL) {
      break;
    }

    /* Skip the dot */
    ext_start++;

    /* Slice until we found a ; or alternatively a \0 */
    ext_end = wcschr(ext_start, L';');
    if (ext_end == NULL) {
       ext_end = wcschr(ext_start, '\0');
    }

    result = search_path_join_test(dir, dir_len,
                                   name, name_len,
                                   ext_start, (ext_end - ext_start),
                                   cwd, cwd_len);
  }

  return result;
}


/*
 * search_path searches the system path for an executable filename -
 * the windows API doesn't provide this as a standalone function nor as an
 * option to CreateProcess.
 *
 * It tries to return an absolute filename.
 *
 * Furthermore, it tries to follow the semantics that cmd.exe uses as closely
 * as possible:
 *
 * - Do not search the path if the filename already contains a path (either
 *   relative or absolute).
 *     (but do use path_ext)
 *
 * - If there's really only a filename, check the current directory for file,
 *   then search all path directories.
 *
 * - If filename specifies has *any* extension, search for the file with the
 *   specified extension first.
 *     (not necessary an executable one or one that appears in path_ext;
 *      *but* no extension or just a dot is *not* allowed)
 *
 * - If the literal filename is not found in a directory, try *appending*
 *   (not replacing) extensions from path_ext in the specified order.
 *     (an extension consisting of just a dot *may* appear in path_ext;
 *      unlike what happens if the specified filename ends with a dot,
 *      if path_ext specifies a single dot cmd.exe *does* look for an
 *      extension-less file)
 *
 * - The path variable may contain relative paths; relative paths are relative
 *   to the cwd.
 *
 * - Directories in path may or may not end with a trailing backslash.
 *
 * - Extensions path_ext portions must always start with a dot.
 *
 * - CMD does not trim leading/trailing whitespace from path/pathex entries
 *   nor from the environment variables as a whole.
 *
 * - When cmd.exe cannot read a directory, it wil just skip it and go on
 *   searching. However, unlike posix-y systems, it will happily try to run a
 *   file that is not readable/executable; if the spawn fails it will not
 *   continue searching.
 *
 * TODO: correctly interpret UNC paths
 * TODO: check with cmd what should happen when a pathext entry does not start
 *       with a dot
 */
static wchar_t* search_path(const wchar_t *file,
                            wchar_t *cwd,
                            const wchar_t *path,
                            const wchar_t *path_ext) {
  int file_has_dir;
  wchar_t* result = NULL;
  wchar_t *file_name_start;
  wchar_t *dot;
  int name_has_ext;

  int file_len = wcslen(file);
  int cwd_len = wcslen(cwd);

  /* If the caller supplies an empty filename,
   * we're not gonna return c:\windows\.exe -- GFY!
   */
  if (file_len == 0
      || (file_len == 1 && file[0] == L'.')) {
    return NULL;
  }

  /* Find the start of the filename so we can split the directory from the name */
  for (file_name_start = (wchar_t*)file + file_len;
       file_name_start > file
           && file_name_start[-1] != L'\\'
           && file_name_start[-1] != L'/'
           && file_name_start[-1] != L':';
       file_name_start--);

  file_has_dir = file_name_start != file;

  /* Check if the filename includes an extension */
  dot = wcschr(file_name_start, L'.');
  name_has_ext = (dot != NULL && dot[1] != L'\0');

  if (file_has_dir) {
    /* The file has a path inside, don't use path (but do use path_ex) */
    result = path_search_walk_ext(
        file, file_name_start - file,
        file_name_start, file_len - (file_name_start - file),
        cwd, cwd_len,
        path_ext, name_has_ext);

  } else {
    const wchar_t *dir_start,
                *dir_end = path;

    /* The file is really only a name; look in cwd first, then scan path */
    result = path_search_walk_ext(L"", 0,
                                  file, file_len,
                                  cwd, cwd_len,
                                  path_ext, name_has_ext);

    while (result == NULL) {
      if (*dir_end == L'\0') {
        break;
      }

      /* Skip the separator that dir_end now points to */
      if (dir_end != path) {
        dir_end++;
      }

      /* Next slice starts just after where the previous one ended */
      dir_start = dir_end;

      /* Slice until the next ; or \0 is found */
      dir_end = wcschr(dir_start, L';');
      if (dir_end == NULL) {
        dir_end = wcschr(dir_start, L'\0');
      }

      /* If the slice is zero-length, don't bother */
      if (dir_end - dir_start == 0) {
        continue;
      }

      result = path_search_walk_ext(dir_start, dir_end - dir_start,
                                    file, file_len,
                                    cwd, cwd_len,
                                    path_ext, name_has_ext);
    }
  }

  return result;
}


static wchar_t* make_program_args(char* const* args) {
  wchar_t* dst;
  wchar_t* ptr;
  char* const* arg = args;
  size_t size = 0;
  size_t len;
  int i = 0;

  dst = NULL;

  while (*arg) {
    size += (uv_utf8_to_utf16(*arg, NULL, 0) * sizeof(wchar_t));
    i++;
    arg++;
  }

  /* Arguments are separated with a space. */
  if (i > 0) {
    size += i - 1;
  }

  dst = (wchar_t*)malloc(size);
  if (!dst) {
    // TODO: FATAL
  }

  ptr = dst;

  arg = args;
  while (*arg) {
    len = uv_utf8_to_utf16(*arg, ptr, (size_t)(size - (ptr - dst)));
    if (!len) {
      free(dst);
      return NULL;
    }

    arg++;
    ptr += len;
    *(ptr - 1) = L' ';
  }

  *(ptr - 1) = L'\0';

  return dst;
}

/*
  * The way windows takes environment variables is different than what C does;
  * Windows wants a contiguous block of null-terminated strings, terminated
  * with an additional null.
  * Get a pointer to the pathext and path environment variables as well,
  * because search_path needs it. These are just pointers into env_win.
  */
wchar_t* make_program_env(char* const* envBlock, const wchar_t **path,  const wchar_t **path_ext) {
  char* const* env = envBlock;
  int env_win_len = 1 * sizeof(wchar_t); // room for closing null
  int len;
  wchar_t* env_win, *env_win_pos;

  while (*env) {
    env_win_len += (uv_utf8_to_utf16(*env, NULL, 0) * sizeof(wchar_t));
    env++;
  }

  env_win = (wchar_t*)malloc(env_win_len);
  if (!env_win) {
    // TODO: FATAL
  }

  env_win_pos = env_win;

  env = envBlock;
  while (*env) {
    len = uv_utf8_to_utf16(*env, env_win_pos, (size_t)(env_win_len - (env_win_pos - env_win)));
    if (!len) {
      free(env_win);
      return NULL;
    }

    // Try to get a pointer to PATH and PATHEXT
    if (_wcsnicmp(L"PATH=", env_win_pos, 5) == 0) {
      *path = env_win_pos + 5;
    }
    if (_wcsnicmp(L"PATHEXT=", env_win_pos, 8) == 0) {
      *path_ext = env_win_pos + 8;
    }

    env++;
    env_win_pos += len;
  }

  *env_win_pos = L'\0';
  return env_win;
}


static void CALLBACK watch_wait_callback(void* data, BOOLEAN didTimeout) {
  uv_process_t* process = (uv_process_t*)data;
  
  assert(didTimeout == FALSE);
  assert(process);
  
  memset(&process->exit_req.overlapped, 0, sizeof(process->exit_req.overlapped));

  /* Post completed */
  if (!PostQueuedCompletionStatus(LOOP->iocp,
                                0,
                                0,
                                &process->exit_req.overlapped)) {
    uv_fatal_error(GetLastError(), "PostQueuedCompletionStatus");
  }
}


void uv_process_proc_exit(uv_process_t* handle, uv_req_t* req) {
  int i;
  DWORD exit_code;

  /* Close stdio handles. */
  for (i = 0; i < COUNTOF(handle->stdio_pipes); i++) {
    if (handle->stdio_pipes[i].server_pipe != NULL) {
      close_pipe(handle->stdio_pipes[i].server_pipe, NULL, NULL);
      handle->stdio_pipes[i].server_pipe = NULL;
    }

    if (handle->stdio_pipes[i].child_pipe != INVALID_HANDLE_VALUE) {
      CloseHandle(handle->stdio_pipes[i].child_pipe);
      handle->stdio_pipes[i].child_pipe = INVALID_HANDLE_VALUE;
    }
  }

  /* Unregister from process notification. */
  UnregisterWait(handle->wait_handle);

  /* Get the exit code. */
  if (!GetExitCodeProcess(handle->process_handle, &exit_code)) {
    // TODO: fatal error
  }

  /* Clean-up the process handle. */
  CloseHandle(handle->process_handle);
  handle->process_handle = INVALID_HANDLE_VALUE;

  /* Fire the exit callback. */
  handle->exit_cb(handle, exit_code, handle->exit_signal);
}


int uv_spawn(uv_process_t* process, uv_process_options_t options) {
  int size;
  wchar_t* application_path;
  char name[64];
  BOOL success;
  wchar_t* application, *arguments, *env, *cwd;
  const wchar_t* path = NULL;
  const wchar_t* path_ext = NULL;

  SECURITY_ATTRIBUTES sa; 
  STARTUPINFOW startup;
  PROCESS_INFORMATION info;
 
  memset(process, 0, sizeof(uv_process_t)); 
  uv_process_init(process);

  process->exit_cb = options.exit_cb;

  UTF8_TO_UTF16(options.file, application);

  if (options.cwd) {
    UTF8_TO_UTF16(options.cwd, cwd);
  } else {
    size  = GetCurrentDirectoryW(0, NULL) * sizeof(wchar_t);
    if (!size) {
      // TODO: error
    } else {
      cwd = (wchar_t*)malloc(size);
      GetCurrentDirectoryW(size, cwd);
    }
  }

  if (options.args) {
    arguments = make_program_args(options.args);
  } else {
    arguments = NULL;
  }

  if (options.env) {
    env = make_program_env(options.env, &path, &path_ext);
  } else {
    env = NULL;
  }

  application_path = search_path(application, 
                                 cwd,
                                 path ? path : DEFAULT_PATH,
                                 path_ext ? path_ext : DEFAULT_PATH_EXT);

  if (!application_path) {
    goto error;
  }

  sa.nLength = sizeof(SECURITY_ATTRIBUTES); 
  sa.bInheritHandle = TRUE; 
  sa.lpSecurityDescriptor = NULL; 

  /* Create pipes */
  if (options.stdin_stream) {
    // TODO: check for errors
    uv_stdio_pipe_server(options.stdin_stream, PIPE_ACCESS_OUTBOUND, name, sizeof(name));
    process->stdio_pipes[0].server_pipe = options.stdin_stream;

    /* Create client pipe handle */
    process->stdio_pipes[0].child_pipe = CreateFileA(name,
                                                     GENERIC_READ,
                                                     0,
                                                     &sa,
                                                     OPEN_EXISTING,
                                                     0,
                                                     NULL);

    // TODO: check for errors
  }
  if (options.stdout_stream) {
    uv_stdio_pipe_server(options.stdout_stream, PIPE_ACCESS_INBOUND, name, sizeof(name));
    process->stdio_pipes[1].server_pipe = options.stdout_stream;

    /* Create client pipe handle */
    process->stdio_pipes[1].child_pipe = CreateFileA(name,
                                                     GENERIC_WRITE,
                                                     0,
                                                     &sa,
                                                     OPEN_EXISTING,
                                                     0,
                                                     NULL);

    // TODO: check for errors
  }
  if (options.stderr_stream) {
    uv_stdio_pipe_server(options.stderr_stream, PIPE_ACCESS_INBOUND, name, sizeof(name));
    process->stdio_pipes[2].server_pipe = options.stderr_stream;

    /* Create client pipe handle */
    process->stdio_pipes[2].child_pipe = CreateFileA(name,
                                                     GENERIC_WRITE,
                                                     0,
                                                     &sa,
                                                     OPEN_EXISTING,
                                                     0,
                                                     NULL);
  }

  startup.cb = sizeof(startup);
  startup.lpReserved = NULL;
  startup.lpDesktop = NULL;
  startup.lpTitle = NULL;
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.cbReserved2 = 0;
  startup.lpReserved2 = NULL;
  startup.hStdInput = process->stdio_pipes[0].child_pipe;
  startup.hStdOutput = process->stdio_pipes[1].child_pipe;
  startup.hStdError = process->stdio_pipes[2].child_pipe;

  success = CreateProcessW(
    application_path,
    arguments,
    NULL,
    NULL,
    1,
    CREATE_UNICODE_ENVIRONMENT,
    env,
    cwd,
    &startup,
    &info
  );

  if (!success) {
    goto error;
  }

  process->process_handle = info.hProcess;
  process->pid = info.dwProcessId;
  
  /* Get a notification when the child process exits. */
  RegisterWaitForSingleObject(&process->wait_handle, process->process_handle,
      watch_wait_callback, (void*)process, INFINITE,
      WT_EXECUTEINWAITTHREAD | WT_EXECUTEONLYONCE);

  // TODO: handle error

  CloseHandle(info.hThread);

  return 0;

error:
  // TODO: free these unconditionally
  free(application);
  free(arguments);
  free(cwd);
  free(env);
  return -1;
}


int uv_process_kill(uv_process_t* process, int signum) {
  process->exit_signal = signum;

  /* On windows killed processes normally return 1 */
  if (process->process_handle != INVALID_HANDLE_VALUE &&
      TerminateProcess(process->process_handle, 1)) {
      return 0;
  }

  return -1;
}
