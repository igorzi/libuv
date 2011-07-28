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

#include <assert.h>
#include <stdlib.h>
#include <windows.h>


static const wchar_t DEFAULT_PATH[1] = L"";
static const wchar_t DEFAULT_PATH_EXT[20] = L".COM;.EXE;.BAT;.CMD";


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
      malloc(sizeof(wchar_t) * (cwd_len + 1 + dir_len + 1 + name_len + 1 + ext_len + 1));

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

  DWORD attrs = GetFileAttributesW(result);

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
  wchar_t* result = NULL;

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
  wchar_t *file_name_start;
  for (file_name_start = (wchar_t*)file + file_len;
       file_name_start > file
           && file_name_start[-1] != L'\\'
           && file_name_start[-1] != L'/'
           && file_name_start[-1] != L':';
       file_name_start--);

  int file_has_dir = file_name_start != file;

  /* Check if the filename includes an extension */
  wchar_t *dot = wcschr(file_name_start, L'.');
  int name_has_ext = (dot != NULL && dot[1] != L'\0');

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


/* Returns the size in words of `src` converted
 * from UTF-8 to wchar_t, or (size_t)-1 on error.
 *
 * Note that the returned value is in words. Multiply
 * it with sizeof(wchar_t) to obtain the byte length.
 *
 * Also note that the returned value does not include
 * space for the trailing nul byte.
 */
static size_t utf8_to_wchar_len(const char* src) {
  size_t size;
  int error;

  if ((error = mbstowcs_s(&size, NULL, 0, src, 0)) != 0) {
    assert(0);
    return (size_t) -1;
  }
  else {
    assert(size != 0);
    return size - 1; /* Subtract nul byte. */
  }
}


/* Convert UTF-8 string to wchar_t. The returned buffer
 * is null terminated and should be freed by the caller.
 * Returns NULL on error (bad input, out of memory).
 */
static wchar_t* utf8_to_wchar(const char* src) {
  wchar_t* dst;
  size_t size;
  int error;

  if ((size = utf8_to_wchar_len(src)) == (size_t) -1) {
    assert(0);
    return NULL;
  }

  if ((dst = malloc((size + 1) * sizeof(wchar_t))) == NULL) {
    assert(0);
    return NULL;
  }

  if ((error = mbstowcs_s(NULL, dst, size + 1, src, size)) != 0) {
    assert(0);
    free(dst);
    return NULL;
  }

  return dst;
}


static wchar_t* make_program_args(char* const* args) {
  wchar_t* dst;
  wchar_t* end;
  wchar_t* ptr;
  size_t size;
  size_t len;
  int error;
  int i;

  dst = NULL;

  /* Combine arguments into a single wchar_t string. Calculate length first. */
  size = 0;
  for (i = 0; args[i] != NULL; i++) {
    if ((len = utf8_to_wchar_len(args[i])) == (size_t) -1) {
      assert(0);
      goto err;
    }
    else {
      size += len;
    }
  }

  /* Assume all characters need escaping. */
  size *= 2;

  /* Arguments are separated with a space. */
  if (i > 0) {
    size += i - 1;
  }

  if ((dst = malloc((size + 1) * sizeof(wchar_t))) == NULL) {
    goto err;
  }

  ptr = dst;
  end = dst + size;

  for (i = 0; args[i] != NULL; i++) {
    assert(ptr + size < end);
    if ((error = mbstowcs_s(&len, ptr, size + 1, args[i], size)) != 0) {
      assert(0);
      goto err;
    }
    else {
      size -= len;
      ptr += len;
    }
  }

  return dst;

err:
  free(dst);
  return NULL;
}


int uv_spawn(uv_process_t* process, uv_process_options_t options) {
  memset(process, 0, sizeof *process);

  InitializeCriticalSection(&process->info_lock_);
  process->kill_me_ = 0;
  process->did_start_ = 0;
  process->exit_signal_ = 0;

  process->application_ = utf8_to_wchar(options.file);
  process->arguments_ = make_program_args(options.args);
  process->cwd_ = utf8_to_wchar(options.cwd);

  if (options.stdin_stream) {
    uv_pipe_init(options.stdin_stream);
  }
  if (options.stdout_stream) {
    uv_pipe_init(options.stdout_stream);
  }
  if (options.stderr_stream) {
    uv_pipe_init(options.stderr_stream);
  }

  return 0;

err:
  free(process->application_);
  free(process->arguments_);
  free(process->cwd_);
  return -1;
}


int uv_process_kill(uv_process_t* process, int signum) {
  int rv = 0;

  EnterCriticalSection(&process->info_lock_);

  process->exit_signal_ = signum;

  if (process->did_start_) {
    /* On windows killed processes normally return 1 */
    if (!TerminateProcess(process->process_handle_, 1))
      rv = -1;
  } else {
    process->kill_me_ = 1;
  }

  LeaveCriticalSection(&process->info_lock_);

  return rv;
}
