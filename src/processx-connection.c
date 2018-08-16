
#include "processx-connection.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef _WIN32
#include <sys/uio.h>
#include <poll.h>
#else
#include <io.h>
#endif

#include "processx.h"

#ifdef _WIN32
#include "win/processx-win.h"
#else
#include "unix/processx-unix.h"
#endif

/* Internal functions in this file */

static void processx__connection_find_chars(processx_connection_t *ccon,
					    ssize_t maxchars,
					    ssize_t maxbytes,
					    size_t *chars,
					    size_t *bytes);

static void processx__connection_find_lines(processx_connection_t *ccon,
					    ssize_t maxlines,
					    size_t *lines,
					    int *eof);

static void processx__connection_alloc(processx_connection_t *ccon);
static void processx__connection_realloc(processx_connection_t *ccon);
static ssize_t processx__connection_read(processx_connection_t *ccon);
static ssize_t processx__find_newline(processx_connection_t *ccon,
				      size_t start);
static ssize_t processx__connection_read_until_newline(processx_connection_t
						       *ccon);
static void processx__connection_xfinalizer(SEXP con);
static ssize_t processx__connection_to_utf8(processx_connection_t *ccon);
static void processx__connection_find_utf8_chars(processx_connection_t *ccon,
						 ssize_t maxchars,
						 ssize_t maxbytes,
						 size_t *chars,
						 size_t *bytes);

#ifdef _WIN32
#define PROCESSX_CHECK_VALID_CONN(x) do {				\
    if (!x) error("Invalid connection object");				\
    if (!(x)->handle.handle) {						\
      error("Invalid (uninitialized or closed?) connection object");	\
    }									\
  } while (0)
#else
#define PROCESSX_CHECK_VALID_CONN(x) do {				\
    if (!x) error("Invalid connection object");				\
    if ((x)->handle < 0) {                                              \
      error("Invalid (uninitialized or closed?) connection object");	\
}                                                                       \
  } while (0)
#endif

/* --------------------------------------------------------------------- */
/* API from R                                                            */
/* --------------------------------------------------------------------- */

SEXP processx_connection_create(SEXP handle, SEXP encoding) {
  processx_file_handle_t *os_handle = R_ExternalPtrAddr(handle);
  const char *c_encoding = CHAR(STRING_ELT(encoding, 0));
  SEXP result = R_NilValue;

  if (!os_handle) error("Cannot create connection, invalid handle");

  processx_c_connection_create(*os_handle, PROCESSX_FILE_TYPE_ASYNCPIPE,
			       c_encoding, &result);
  return result;
}

SEXP processx_connection_create_fd(SEXP handle, SEXP encoding, SEXP close) {
  int fd = INTEGER(handle)[0];
  const char *c_encoding = CHAR(STRING_ELT(encoding, 0));
  processx_file_handle_t os_handle;
  processx_connection_t *con;
  SEXP result = R_NilValue;

#ifdef _WIN32
  os_handle = (HANDLE) _get_osfhandle(fd);
#else
  os_handle = fd;
#endif

  con = processx_c_connection_create(os_handle, PROCESSX_FILE_TYPE_ASYNCPIPE,
				     c_encoding, &result);

  if (! LOGICAL(close)[0]) con->close_on_destroy = 0;

  return result;
}

SEXP processx_connection_create_file(SEXP filename, SEXP read, SEXP write) {
  const char *c_filename = CHAR(STRING_ELT(filename, 0));
  int c_read = LOGICAL(read)[0];
  int c_write = LOGICAL(write)[0];
  SEXP result = R_NilValue;
  processx_file_handle_t os_handle;

#ifdef _WIN32
  DWORD access = 0, create = 0;
  if (c_read) access |= GENERIC_READ;
  if (c_write) access |= GENERIC_WRITE;
  if (c_read) create |= OPEN_EXISTING;
  if (c_write) create |= CREATE_ALWAYS;
  os_handle = CreateFile(
    /* lpFilename = */ c_filename,
    /* dwDesiredAccess = */ access,
    /* dwShareMode = */ 0,
    /* lpSecurityAttributes = */ NULL,
    /* dwCreationDisposition = */ create,
    /* dwFlagsAndAttributes = */ FILE_ATTRIBUTE_NORMAL,
    /* hTemplateFile = */ NULL);
  if (os_handle == INVALID_HANDLE_VALUE) {
    PROCESSX_ERROR("Cannot open file", GetLastError());
  }

#else
  int flags = 0;
  if ( c_read && !c_write) flags |= O_RDONLY;
  if (!c_read &&  c_write) flags |= O_WRONLY | O_CREAT | O_TRUNC;
  if ( c_read &&  c_write) flags |= O_RDWR;
  os_handle = open(c_filename, flags, 0644);
  if (os_handle == -1) {
    error("Cannot open file `%s`: `%s`", c_filename, strerror(errno));
  }
#endif

  processx_c_connection_create(os_handle, PROCESSX_FILE_TYPE_FILE,
			       "", &result);

  return result;
}

SEXP processx_connection_read_chars(SEXP con, SEXP nchars) {

  processx_connection_t *ccon = R_ExternalPtrAddr(con);
  SEXP result;
  int cnchars = asInteger(nchars);
  size_t utf8_chars, utf8_bytes;

  processx__connection_find_chars(ccon, cnchars, -1, &utf8_chars,
				  &utf8_bytes);

  result = PROTECT(ScalarString(mkCharLenCE(ccon->utf8, (int) utf8_bytes,
					    CE_UTF8)));
  ccon->utf8_data_size -= utf8_bytes;
  memmove(ccon->utf8, ccon->utf8 + utf8_bytes, ccon->utf8_data_size);

  UNPROTECT(1);
  return result;
}

SEXP processx_connection_read_lines(SEXP con, SEXP nlines) {

  processx_connection_t *ccon = R_ExternalPtrAddr(con);
  SEXP result;
  int cn = asInteger(nlines);
  ssize_t newline, eol = -1;
  size_t lines_read = 0, l;
  int eof = 0;
  int slashr;

  processx__connection_find_lines(ccon, cn, &lines_read, &eof);

  result = PROTECT(allocVector(STRSXP, lines_read + eof));
  for (l = 0, newline = -1; l < lines_read; l++) {
    eol = processx__find_newline(ccon, newline + 1);
    slashr = ccon->utf8[eol - 1] == '\r';
    SET_STRING_ELT(
      result, l,
      mkCharLenCE(ccon->utf8 + newline + 1,
		  (int) (eol - newline - 1 - slashr), CE_UTF8));
    newline = eol;
  }

  if (eof) {
    eol = ccon->utf8_data_size - 1;
    SET_STRING_ELT(
      result, l,
      mkCharLenCE(ccon->utf8 + newline + 1,
		  (int) (eol - newline), CE_UTF8));
  }

  if (eol >= 0) {
    ccon->utf8_data_size -= eol + 1;
    memmove(ccon->utf8, ccon->utf8 + eol + 1, ccon->utf8_data_size);
  }

  UNPROTECT(1);
  return result;
}

SEXP processx_connection_write_bytes(SEXP con, SEXP bytes) {
  processx_connection_t *ccon = R_ExternalPtrAddr(con);
  Rbyte *cbytes = RAW(bytes);
  size_t nbytes = LENGTH(bytes);
  SEXP result;

  ssize_t written = processx_c_connection_write_bytes(ccon, cbytes, nbytes);

  size_t left = nbytes - written;
  PROTECT(result = allocVector(RAWSXP, left));
  if (left > 0) memcpy(RAW(result), cbytes + written, left);

  UNPROTECT(1);
  return result;
}

SEXP processx_connection_is_eof(SEXP con) {
  processx_connection_t *ccon = R_ExternalPtrAddr(con);
  if (!ccon) error("Invalid connection object");
  return ScalarLogical(ccon->is_eof_);
}

SEXP processx_connection_close(SEXP con) {
  processx_connection_t *ccon = R_ExternalPtrAddr(con);
  if (!ccon) error("Invalid connection object");
  processx_c_connection_close(ccon);
  return R_NilValue;
}

SEXP processx_connection_is_closed(SEXP con) {
  processx_connection_t *ccon = R_ExternalPtrAddr(con);
  if (!ccon) error("Invalid connection object");
  return ScalarLogical(processx_c_connection_is_closed(ccon));
}

/* Poll connections and other pollable handles */
SEXP processx_connection_poll(SEXP pollables, SEXP timeout) {
  /* TODO: this is not used currently */
  error("Not implemented");
  return R_NilValue;
}

SEXP processx_connection_create_pipepair(SEXP encoding) {
  const char *c_encoding = CHAR(STRING_ELT(encoding, 0));
  SEXP result, con1, con2;

#ifdef _WIN32
  HANDLE h1, h2;
  processx__create_pipe(0, &h1, &h2);

#else
  int pipe[2], h1, h2;
  processx__make_socketpair(pipe);
  processx__nonblock_fcntl(pipe[0], 1);
  processx__nonblock_fcntl(pipe[1], 0);
  h1 = pipe[0];
  h2 = pipe[1];
#endif

  processx_c_connection_create(h1, PROCESSX_FILE_TYPE_ASYNCPIPE,
			       c_encoding, &con1);
  PROTECT(con1);
  processx_c_connection_create(h2, PROCESSX_FILE_TYPE_ASYNCPIPE,
			       c_encoding, &con2);
  PROTECT(con2);

  result = PROTECT(allocVector(VECSXP, 2));
  SET_VECTOR_ELT(result, 0, con1);
  SET_VECTOR_ELT(result, 1, con2);

  UNPROTECT(3);
  return result;
}

SEXP processx__connection_set_std(SEXP con, int which, int drop) {
  processx_connection_t *ccon = R_ExternalPtrAddr(con);
  if (!ccon) error("Invalid connection object");
  SEXP result = R_NilValue;

#ifdef _WIN32
  int fd, ret;
  if (!drop) {
    int saved = _dup(which);
    processx_file_handle_t os_handle;
    if (saved == -1) {
      PROCESSX_ERROR("Cannot save stdout/stderr for rerouting", GetLastError());
    }
    os_handle = (HANDLE) _get_osfhandle(saved) ;
    processx_c_connection_create(os_handle, PROCESSX_FILE_TYPE_PIPE,
				 "", &result);
  }
  fd = _open_osfhandle((intptr_t) ccon->handle.handle, 0);
  ret = _dup2(fd, which);
  if (ret) PROCESSX_ERROR("Cannot reroute stdout/stderr", GetLastError());

#else
  const char *what[] = { "stdin", "stdout", "stderr" };
  int ret;
  if (!drop) {
    processx_file_handle_t os_handle = dup(which);
    if (os_handle == -1) {
      error("Cannot save %s for rerouting: `%s`", what[which], strerror(errno));
    }
    processx_c_connection_create(os_handle, PROCESSX_FILE_TYPE_PIPE,
				 "", &result);
  }
  ret = dup2(ccon->handle, which);
  if (ret == -1) {
    error("Cannot reroute %s: `%s`", what[which], strerror(errno));
  }
#endif

  return result;
}

SEXP processx_connection_set_stdout(SEXP con, SEXP drop) {
  return processx__connection_set_std(con, 1, LOGICAL(drop)[0]);
}

SEXP processx_connection_set_stderr(SEXP con, SEXP drop) {
  return processx__connection_set_std(con, 2, LOGICAL(drop)[0]);
}

SEXP processx_connection_get_fileno(SEXP con) {
  processx_connection_t *ccon = R_ExternalPtrAddr(con);
  if (!ccon) error("Invalid connection object");
  int fd;

#ifdef _WIN32
  fd = _open_osfhandle((intptr_t) ccon->handle.handle, 0);
#else
  fd = ccon->handle;
#endif

  return ScalarInteger(fd);
}

#ifdef _WIN32

/*
 * Clear the HANDLE_FLAG_INHERIT flag from all HANDLEs that were inherited
 * the parent process. Don't check for errors - the stdio handles may not be
 * valid, or may be closed already. There is no guarantee that this function
 * does a perfect job.
 */

SEXP processx_connection_disable_inheritance() {
  HANDLE handle;
  STARTUPINFOW si;

  /* Make the windows stdio handles non-inheritable. */
  handle = GetStdHandle(STD_INPUT_HANDLE);
  if (handle != NULL && handle != INVALID_HANDLE_VALUE) {
    SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0);
  }

  handle = GetStdHandle(STD_OUTPUT_HANDLE);
  if (handle != NULL && handle != INVALID_HANDLE_VALUE) {
    SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0);
  }

  handle = GetStdHandle(STD_ERROR_HANDLE);
  if (handle != NULL && handle != INVALID_HANDLE_VALUE) {
    SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0);
  }

  /* Make inherited CRT FDs non-inheritable. */
  GetStartupInfoW(&si);
  if (processx__stdio_verify(si.lpReserved2, si.cbReserved2)) {
    processx__stdio_noinherit(si.lpReserved2);
  }

  return R_NilValue;
}

#else

SEXP processx_connection_disable_inheritance() {
  int fd;

  /* Set the CLOEXEC flag on all open descriptors. Unconditionally try the
   * first 16 file descriptors. After that, bail out after the first error.
   */
  for (fd = 0; ; fd++) {
    if (processx__cloexec_fcntl(fd, 1) && fd > 15) break;
  }

  return R_NilValue;
}

#endif

/* Api from C -----------------------------------------------------------*/

processx_connection_t *processx_c_connection_create(
  processx_file_handle_t os_handle,
  processx_file_type_t type,
  const char *encoding,
  SEXP *r_connection) {

  processx_connection_t *con;
  SEXP result, class;

  con = malloc(sizeof(processx_connection_t));
  if (!con) error("out of memory");

  con->type = type;
  con->is_closed_ = 0;
  con->is_eof_  = 0;
  con->is_eof_raw_ = 0;
  con->close_on_destroy = 1;
  con->iconv_ctx = 0;

  con->buffer = 0;
  con->buffer_allocated_size = 0;
  con->buffer_data_size = 0;

  con->utf8 = 0;
  con->utf8_allocated_size = 0;
  con->utf8_data_size = 0;

  con->encoding = 0;
  if (encoding && encoding[0]) {
    con->encoding = strdup(encoding);
    if (!con->encoding) {
      free(con);
      error("out of memory");
      return 0;			/* never reached */
    }
  }

#ifdef _WIN32
  con->handle.handle = os_handle;
  memset(&con->handle.overlapped, 0, sizeof(OVERLAPPED));
  con->handle.read_pending = FALSE;
#else
  con->handle = os_handle;
#endif

  if (r_connection) {
    result = PROTECT(R_MakeExternalPtr(con, R_NilValue, R_NilValue));
    R_RegisterCFinalizerEx(result, processx__connection_xfinalizer, 1);
    class = PROTECT(ScalarString(mkChar("processx_connection")));
    setAttrib(result, R_ClassSymbol, class);
    *r_connection = result;
    UNPROTECT(2);
  }

  return con;
}

/* Destroy */
void processx_c_connection_destroy(processx_connection_t *ccon) {

  if (!ccon) return;

  if (ccon->close_on_destroy) processx_c_connection_close(ccon);

  if (ccon->iconv_ctx) Riconv_close(ccon->iconv_ctx);

  if (ccon->buffer) free(ccon->buffer);
  if (ccon->utf8) free(ccon->utf8);
  if (ccon->encoding) free(ccon->encoding);

  free(ccon);
}

/* Read characters */
ssize_t processx_c_connection_read_chars(processx_connection_t *ccon,
					 void *buffer,
					 size_t nbyte) {
  size_t utf8_chars, utf8_bytes;

  if (nbyte < 4) {
    error("Buffer size must be at least 4 bytes, to allow multibyte "
	  "characters");
  }

  processx__connection_find_chars(ccon, -1, nbyte, &utf8_chars, &utf8_bytes);

  memcpy(buffer, ccon->utf8, utf8_bytes);
  ccon->utf8_data_size -= utf8_bytes;
  memmove(ccon->utf8, ccon->utf8 + utf8_bytes, ccon->utf8_data_size);

  return utf8_bytes;
}

/**
 * Read a single line, ending with \n
 *
 * The trailing \n character is not copied to the buffer.
 *
 * @param ccon Connection.
 * @param linep Must point to a buffer pointer. If must not be NULL. If
 *   the buffer pointer is NULL, it will be allocated. If it is not NULL,
 *   it might be reallocated using `realloc`, as needed.
 * @param linecapp Initial size of the buffer. It will be updated if the
 *   buffer is newly allocated or reallocated.
 * @return Number of characters read, not including the \n character.
 *   It returns -1 on EOF. If the connection is not at EOF yet, but there
 *   is nothing to read currently, it returns 0. If 0 is returned, `linep`
 *   and `linecapp` are not touched.
 *
 */
ssize_t processx_c_connection_read_line(processx_connection_t *ccon,
					char **linep, size_t *linecapp) {

  int eof = 0;
  ssize_t newline;

  if (!linep) error("linep cannot be a null pointer");
  if (!linecapp) error("linecapp cannot be a null pointer");

  if (ccon->is_eof_) return -1;

  /* Read until a newline character shows up, or there is nothing more
     to read (at least for now). */
  newline = processx__connection_read_until_newline(ccon);

  /* If there is no newline at the end of the file, we still add the
     last line. */
  if (ccon->is_eof_raw_ && ccon->utf8_data_size != 0 &&
      ccon->buffer_data_size == 0 &&
      ccon->utf8[ccon->utf8_data_size - 1] != '\n') {
    eof = 1;
  }

  /* We cannot serve a line currently. Maybe later. */
  if (newline == -1 && ! eof) return 0;

  /* Newline will contain the end of the line now, even if EOF */
  if (newline == -1) newline = ccon->utf8_data_size;
  if (ccon->utf8[newline - 1] == '\r') newline--;

  if (! *linep) {
    *linep = malloc(newline + 1);
    *linecapp = newline + 1;
  } else if (*linecapp < newline + 1) {
    char *tmp = realloc(*linep, newline + 1);
    if (!tmp) error("out of memory");
    *linep = tmp;
    *linecapp = newline + 1;
  }

  memcpy(*linep, ccon->utf8, newline);
  (*linep)[newline] = '\0';

  if (!eof) {
    ccon->utf8_data_size -= (newline + 1);
    memmove(ccon->utf8, ccon->utf8 + newline + 1, ccon->utf8_data_size);
  } else {
    ccon->utf8_data_size = 0;
  }

  return newline;
}

/* Write bytes */
ssize_t processx_c_connection_write_bytes(
  processx_connection_t *ccon,
  const void *buffer,
  size_t nbytes) {

  PROCESSX_CHECK_VALID_CONN(ccon);

#ifdef _WIN32
  DWORD written;
  BOOL ret = WriteFile(
    /* hFile =                  */ ccon->handle.handle,
    /* lpBuffer =               */ buffer,
    /* nNumberOfBytesToWrite =  */ nbytes,
    /* lpNumberOfBytesWritten = */ &written,
    /* lpOverlapped =           */ NULL);
  if (!ret) PROCESSX_ERROR("Cannot write connection ", GetLastError());
  return (ssize_t) written;
#else
  ssize_t ret = write(ccon->handle, buffer, nbytes);
  if (ret == -1) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return 0;
    } else {
      error("Cannot write connection: %s at %s:%d", strerror(errno),
	    __FILE__, __LINE__);
    }
  }
  return ret;
#endif
}

/* Check if the connection has ended */
int processx_c_connection_is_eof(processx_connection_t *ccon) {
  return ccon->is_eof_;
}

/* Close */
void processx_c_connection_close(processx_connection_t *ccon) {
#ifdef _WIN32
  if (ccon->handle.handle) {
    CancelIo(ccon->handle.handle);
    CloseHandle(ccon->handle.handle);
  }
  ccon->handle.handle = 0;
  if (ccon->handle.overlapped.hEvent) {
    CloseHandle(ccon->handle.overlapped.hEvent);
  }
  ccon->handle.overlapped.hEvent = 0;
#else
  if (ccon->handle >= 0) close(ccon->handle);
  ccon->handle = -1;
#endif
  ccon->is_closed_ = 1;
}

int processx_c_connection_is_closed(processx_connection_t *ccon) {
  return ccon->is_closed_;
}

#ifdef _WIN32

int processx_c_connection_poll(processx_pollable_t pollables[],
			       size_t npollables, int timeout) {

  int hasdata = 0;
  size_t i, j = 0;
  int *ptr;
  int timeleft = timeout;
  HANDLE iocp = processx__get_default_iocp();
  DWORD bytes;
  OVERLAPPED *overlapped = 0;
  ULONG_PTR key;

  ptr = (int*) R_alloc(npollables, sizeof(int));

  for (i = 0; i < npollables; i++) {
    processx_pollable_t *el = pollables + i;
    processx_file_handle_t handle = 0;
    int again;
    el->event = el->poll_func(el->object, 0, &handle, &again);
    if (el->event == PXNOPIPE || el->event == PXCLOSED) {
      /* Do nothing */
    } else if (el->event == PXREADY) {
      hasdata++;
    } else if (el->event == PXSILENT && handle != 0) {
      ptr[j] = i;
      j++;
    } else {
      error("Cannnot poll pollable, not ready, and no handle");
    }
  }

  if (j == 0) return hasdata;

  if (hasdata) timeout = timeleft = 0;

  while (timeout < 0 || timeleft >= 0) {
    int poll_timeout;
    if (timeout < 0 || timeleft > PROCESSX_INTERRUPT_INTERVAL) {
      poll_timeout = PROCESSX_INTERRUPT_INTERVAL;
    } else {
      poll_timeout = timeleft;
    }

    GetQueuedCompletionStatus(
      /* CompletionPort  = */ iocp,
      /* lpNumberOfBytes = */ &bytes,
      /* lpCompletionKey = */ &key,
      /* lpOverlapped    = */ &overlapped,
      /* dwMilliseconds  = */ poll_timeout);

    if (overlapped) {
      /* data */
      processx_connection_t *con = (processx_connection_t*) key;
      int poll_idx = con->poll_idx;
      con->handle.read_pending = FALSE;
      con->buffer_data_size += bytes;
      if (con->buffer_data_size > 0) processx__connection_to_utf8(con);
      if (con->type == PROCESSX_FILE_TYPE_ASYNCFILE) {
	/* TODO: larget files */
	con->handle.overlapped.Offset += bytes;
      }

      if (!bytes) {
	con->is_eof_raw_ = 1;
	if (con->utf8_data_size == 0 && con->buffer_data_size == 0) {
	  con->is_eof_ = 1;
	}
      }

      if (poll_idx < npollables &&
	  pollables[poll_idx].object == con) {
	pollables[poll_idx].event = PXREADY;
	hasdata++;
	break;
      }

    } else if (GetLastError() != WAIT_TIMEOUT) {
      PROCESSX_ERROR("Cannot poll", GetLastError());
    }

    R_CheckUserInterrupt();
    timeleft -= PROCESSX_INTERRUPT_INTERVAL;
  }

  if (hasdata == 0) {
    for (i = 0; i < j; i++) pollables[ptr[i]].event = PXTIMEOUT;
  }

  return hasdata;
}

#else

static int processx__poll_decode(short code) {
  if (code & POLLNVAL) return PXCLOSED;
  if (code & POLLIN || code & POLLHUP) return PXREADY;
  return PXSILENT;
}

/* Poll connections and other pollable handles */
int processx_c_connection_poll(processx_pollable_t pollables[],
			       size_t npollables, int timeout) {

  int hasdata = 0;
  size_t i, j = 0;
  struct pollfd *fds;
  int *ptr;
  int ret;

  if (npollables == 0) return 0;

  /* Need to allocate this, because we need to put in the fds, maybe */
  ptr = (int*) R_alloc(npollables, sizeof(int));
  fds = (struct pollfd*) R_alloc(npollables, sizeof(struct pollfd));

  /* Need to call the poll method for every pollable */
  for (i = 0; i < npollables; i++) {
    processx_pollable_t *el = pollables + i;
    processx_file_handle_t handle;
    int again;
    el->event = el->poll_func(el->object, 0, &handle, &again);
    if (el->event == PXNOPIPE || el->event == PXCLOSED) {
      /* Do nothing */
    } else if (el->event == PXREADY) {
      hasdata++;
    } else if (el->event == PXSILENT && handle >= 0) {
      fds[j].fd = handle;
      fds[j].events = POLLIN;
      fds[j].revents = 0;
      ptr[j] = (int) i;
      j++;
    } else {
      error("Cannot poll pollable: not ready and no fd");
    }
  }

  /* j contains the number of fds to poll now */

  /* Nothing to poll */
  if (j == 0) return hasdata;

  /* If we already have some data, then we don't wait any more,
     just check if other connections are ready */
  ret = processx__interruptible_poll(fds, (nfds_t) j,
				     hasdata > 0 ? 0 : timeout);

  if (ret == -1) {
    error("Processx poll error: %s", strerror(errno));

  } else if (ret == 0) {
    if (hasdata == 0) {
      for (i = 0; i < j; i++) pollables[ptr[i]].event = PXTIMEOUT;
    }

  } else {
    for (i = 0; i < j; i++) {
      pollables[ptr[i]].event = processx__poll_decode(fds[i].revents);
      hasdata += (pollables[ptr[i]].event == PXREADY);
    }
  }

  return hasdata;
}

#endif

#ifdef _WIN32

void processx__connection_start_read(processx_connection_t *ccon) {
  DWORD bytes_read;
  BOOLEAN res;
  size_t todo;

  if (! ccon->handle.handle) return;

  if (ccon->handle.read_pending) return;

  if (! ccon->handle.overlapped.hEvent &&
      (ccon->type == PROCESSX_FILE_TYPE_ASYNCFILE ||
       ccon->type == PROCESSX_FILE_TYPE_ASYNCPIPE)) {
    ccon->handle.overlapped.hEvent = CreateEvent(
      /* lpEventAttributes = */ NULL,
      /* bManualReset = */      FALSE,
      /* bInitialState = */     FALSE,
      /* lpName = */            NULL);

    if (ccon->handle.overlapped.hEvent == NULL) {
      PROCESSX_ERROR("Cannot read from connection", GetLastError());
    }

    HANDLE iocp = processx__get_default_iocp();
    HANDLE res = CreateIoCompletionPort(
      /* FileHandle =  */                ccon->handle.handle,
      /* ExistingCompletionPort = */     iocp,
      /* CompletionKey = */              (ULONG_PTR) ccon,
      /* NumberOfConcurrentThreads = */  0);

    if (!res) PROCESSX_ERROR("cannot add file to IOCP", GetLastError());
  }

  if (!ccon->buffer) processx__connection_alloc(ccon);

  todo = ccon->buffer_allocated_size - ccon->buffer_data_size;

  /* These need to be set to zero for non-file handles */
  if (ccon->type != PROCESSX_FILE_TYPE_ASYNCFILE) {
     ccon->handle.overlapped.Offset = 0;
     ccon->handle.overlapped.OffsetHigh = 0;
  }
  res = ReadFile(
    /* hfile = */                ccon->handle.handle,
    /* lpBuffer = */             ccon->buffer + ccon->buffer_data_size,
    /* nNumberOfBytesToRead = */ todo,
    /* lpNumberOfBytesRead = */  &bytes_read,
    /* lpOverlapped = */         &ccon->handle.overlapped);

  if (!res) {
    DWORD err = GetLastError();
    if (err == ERROR_BROKEN_PIPE || err == ERROR_HANDLE_EOF) {
      ccon->is_eof_raw_ = 1;
      if (ccon->utf8_data_size == 0 && ccon->buffer_data_size == 0) {
        ccon->is_eof_ = 1;
      }
      if (ccon->buffer_data_size) processx__connection_to_utf8(ccon);
    } else if (err == ERROR_IO_PENDING) {
      ccon->handle.read_pending = TRUE;
    } else {
      ccon->handle.read_pending = FALSE;
      PROCESSX_ERROR("reading from connection", err);
    }
  } else {
    /* Returned synchronously, but the event will be still signalled,
       so we just drop the sync data for now. */
    ccon->handle.read_pending  = TRUE;
  }
}

#endif

/* Poll a connection
 *
 * Checks if there is anything in the buffer. If yes, it returns
 * PXREADY. Otherwise it returns the handle.
 *
 * We can read immediately (without an actual device read), potentially:
 * 1. if the connection is already closed, we return PXCLOSED
 * 2. if the connection is already EOF, we return PXREADY
 * 3. if there is data in the UTF8 buffer, we return PXREADY
 * 4. if there is data in the raw buffer, and the raw file was EOF, we
 *    return PXREADY, because we can surely return something, even if the
 *    raw buffer has incomplete UTF8 characters.
 * 5. otherwise, if there is something in the raw buffer, we try
 *    to convert it to UTF8.
 */

#define PROCESSX__I_POLL_FUNC_CONNECTION_READY do {			\
  if (!ccon) return PXNOPIPE;						\
  if (ccon->is_closed_) return PXCLOSED;				\
  if (ccon->is_eof_) return PXREADY;					\
  if (ccon->utf8_data_size > 0) return PXREADY;				\
  if (ccon->buffer_data_size > 0 && ccon->is_eof_raw_) return PXREADY;	\
  if (ccon->buffer_data_size > 0) {					\
    processx__connection_to_utf8(ccon);					\
    if (ccon->utf8_data_size > 0) return PXREADY;			\
  } } while (0)

int processx_i_poll_func_connection(
  void * object,
  int status,
  processx_file_handle_t *handle,
  int *again) {

  processx_connection_t *ccon = (processx_connection_t*) object;

  PROCESSX__I_POLL_FUNC_CONNECTION_READY;

#ifdef _WIN32
  processx__connection_start_read(ccon);
  /* Starting to read may actually get some data, or an EOF, so check again */
  PROCESSX__I_POLL_FUNC_CONNECTION_READY;
  if (handle) *handle = ccon->handle.overlapped.hEvent;
#else
  if (handle) *handle = ccon->handle;
#endif

  if (again) *again = 0;

  return PXSILENT;
}

int processx_c_pollable_from_connection(
  processx_pollable_t *pollable,
  processx_connection_t *ccon) {

  pollable->poll_func = processx_i_poll_func_connection;
  pollable->object = ccon;
  pollable->free = 0;
  return 0;
}

processx_file_handle_t processx_c_connection_fileno(
  const processx_connection_t *con) {
#ifdef _WIN32
  return con->handle.handle;
#else
  return con->handle;
#endif
}

/* --------------------------------------------------------------------- */
/* Internals                                                             */
/* --------------------------------------------------------------------- */

/**
 * Work out how many UTF-8 characters we can read
 *
 * It might try to read more data, but it does not modify the buffer
 * otherwise.
 *
 * @param ccon Connection.
 * @param maxchars Maximum number of characters to find.
 * @param maxbytes Maximum number of bytes to check while searching.
 * @param chars Number of characters found is stored here.
 * @param bytes Number of bytes the `chars` characters span.
 *
 */

static void processx__connection_find_chars(processx_connection_t *ccon,
					    ssize_t maxchars,
					    ssize_t maxbytes,
					    size_t *chars,
					    size_t *bytes) {

  int should_read_more;

  PROCESSX_CHECK_VALID_CONN(ccon);

  should_read_more = ! ccon->is_eof_ && ccon->utf8_data_size == 0;
  if (should_read_more) processx__connection_read(ccon);

  if (ccon->utf8_data_size == 0 || maxchars == 0) { *bytes = 0; return; }

  /* At at most cnchars characters from the UTF8 buffer */
  processx__connection_find_utf8_chars(ccon, maxchars, maxbytes, chars,
				       bytes);
}

/**
 * Find one or more lines in the buffer
 *
 * Since the buffer is UTF-8 encoded, `\n` is assumed as end-of-line
 * character.
 *
 * @param ccon Connection.
 * @param maxlines Maximum number of lines to find.
 * @param lines Number of lines found is stored here.
 * @param eof If the end of the file is reached, and there is no `\n`
 *   at the end of the file, this is set to 1.
 *
 */

static void processx__connection_find_lines(processx_connection_t *ccon,
					    ssize_t maxlines,
					    size_t *lines,
					    int *eof ) {

  ssize_t newline;

  *eof = 0;

  if (maxlines < 0) maxlines = 1000;

  PROCESSX_CHECK_VALID_CONN(ccon);

  /* Read until a newline character shows up, or there is nothing more
     to read (at least for now). */
  newline = processx__connection_read_until_newline(ccon);

  /* Count the number of lines we got. */
  while (newline != -1 && *lines < maxlines) {
    (*lines) ++;
    newline = processx__find_newline(ccon, /* start = */ newline + 1);
  }

  /* If there is no newline at the end of the file, we still add the
     last line. */
  if (ccon->is_eof_raw_ && ccon->utf8_data_size != 0 &&
      ccon->buffer_data_size == 0 &&
      ccon->utf8[ccon->utf8_data_size - 1] != '\n') {
    *eof = 1;
  }

}

static void processx__connection_xfinalizer(SEXP con) {
  processx_connection_t *ccon = R_ExternalPtrAddr(con);
  processx_c_connection_destroy(ccon);
}

static ssize_t processx__find_newline(processx_connection_t *ccon,
				     size_t start) {

  if (ccon->utf8_data_size == 0) return -1;
  const char *ret = ccon->utf8 + start;
  const char *end = ccon->utf8 + ccon->utf8_data_size;

  while (ret < end && *ret != '\n') ret++;

  if (ret < end) return ret - ccon->utf8; else return -1;
}

static ssize_t processx__connection_read_until_newline
  (processx_connection_t *ccon) {

  char *ptr, *end;

  /* Make sure we try to have something, unless EOF */
  if (ccon->utf8_data_size == 0) processx__connection_read(ccon);
  if (ccon->utf8_data_size == 0) return -1;

  /* We have sg in the utf8 at this point */

  ptr = ccon->utf8;
  end = ccon->utf8 + ccon->utf8_data_size;
  while (1) {
    ssize_t new_bytes;
    while (ptr < end && *ptr != '\n') ptr++;

    /* Have we found a newline? */
    if (ptr < end) return ptr - ccon->utf8;

    /* No newline, but EOF? */
    if (ccon->is_eof_) return -1;

    /* Maybe we can read more, but might need a bigger utf8.
     * The 8 bytes is definitely more than what we need for a UTF8
     * character, and this makes sure that we don't stop just because
     * no more UTF8 characters fit in the UTF8 buffer. */
    if (ccon->utf8_data_size >= ccon->utf8_allocated_size - 8) {
      size_t ptrnum = ptr - ccon->utf8;
      size_t endnum = end - ccon->utf8;
      processx__connection_realloc(ccon);
      ptr = ccon->utf8 + ptrnum;
      end = ccon->utf8 + endnum;
    }
    new_bytes = processx__connection_read(ccon);

    /* If we cannot read now, then we give up */
    if (new_bytes == 0) return -1;
  }
}

/* Allocate buffer for reading */

static void processx__connection_alloc(processx_connection_t *ccon) {
  ccon->buffer = malloc(64 * 1024);
  if (!ccon->buffer) error("Cannot allocate memory for processx buffer");
  ccon->buffer_allocated_size = 64 * 1024;
  ccon->buffer_data_size = 0;

  ccon->utf8 = malloc(64 * 1024);
  if (!ccon->utf8) {
    free(ccon->buffer);
    error("Cannot allocate memory for processx buffer");
  }
  ccon->utf8_allocated_size = 64 * 1024;
  ccon->utf8_data_size = 0;
}

/* We only really need to re-alloc the UTF8 buffer, because the
   other buffer is transient, even if there are no newline characters. */

static void processx__connection_realloc(processx_connection_t *ccon) {
  size_t new_size = (size_t) (ccon->utf8_allocated_size * 1.2);
  void *nb;
  if (new_size == ccon->utf8_allocated_size) new_size = 2 * new_size;
  nb = realloc(ccon->utf8, new_size);
  if (!nb) error("Cannot allocate memory for processx line");
  ccon->utf8 = nb;
  ccon->utf8_allocated_size = new_size;
}

/* Read as much as we can. This is the only function that explicitly
   works with the raw buffer. It is also the only function that actually
   reads from the data source.

   When this is called, the UTF8 buffer is probably empty, but the raw
   buffer might not be. */

#ifdef _WIN32

static ssize_t processx__connection_read(processx_connection_t *ccon) {
  DWORD todo, bytes_read = 0;

  /* Nothing to read, nothing to convert to UTF8 */
  if (ccon->is_eof_raw_ && ccon->buffer_data_size == 0) {
    if (ccon->utf8_data_size == 0) ccon->is_eof_ = 1;
    return 0;
  }

  if (!ccon->buffer) processx__connection_alloc(ccon);

  /* If cannot read anything more, then try to convert to UTF8 */
  todo = ccon->buffer_allocated_size - ccon->buffer_data_size;
  if (todo == 0) return processx__connection_to_utf8(ccon);

  /* Otherwise we read. If there is no read pending, we start one. */
  processx__connection_start_read(ccon);

  /* A read might be pending at this point. See if it has finished. */
  if (ccon->handle.read_pending) {
    HANDLE iocp = processx__get_default_iocp();
    ULONG_PTR key;
    DWORD bytes;
    OVERLAPPED *overlapped = 0;

    while (1) {
      GetQueuedCompletionStatus(
        /* CompletionPort  = */ iocp,
	/* lpNumberOfBytes = */ &bytes,
	/* lpCompletionKey = */ &key,
	/* lpOverlapped    = */ &overlapped,
	/* dwMilliseconds  = */ 0);

      if (overlapped) {
	processx_connection_t *con = (processx_connection_t *) key;
	con->handle.read_pending = FALSE;
	con->buffer_data_size += bytes;
	if (con->buffer && con->buffer_data_size > 0) {
	  bytes = processx__connection_to_utf8(con);
	}
	if (con->type == PROCESSX_FILE_TYPE_ASYNCFILE) {
	  /* TODO: large files */
	  con->handle.overlapped.Offset += bytes;
	}
	if (!bytes) {
	  con->is_eof_raw_ = 1;
	  if (con->utf8_data_size == 0 && con->buffer_data_size == 0) {
	    con->is_eof_ = 1;
	  }
	}

	if (con == ccon) {
	  bytes_read = bytes;
	  break;
	}

      } else if (GetLastError() != WAIT_TIMEOUT) {
	PROCESSX_ERROR("Read error", GetLastError());

      } else {
	break;
      }
    }
  }

  return bytes_read;
}

#else

static ssize_t processx__connection_read(processx_connection_t *ccon) {
  ssize_t todo, bytes_read;

  /* Nothing to read, nothing to convert to UTF8 */
  if (ccon->is_eof_raw_ && ccon->buffer_data_size == 0) {
    if (ccon->utf8_data_size == 0) ccon->is_eof_ = 1;
    return 0;
  }

  if (!ccon->buffer) processx__connection_alloc(ccon);

  /* If cannot read anything more, then try to convert to UTF8 */
  todo = ccon->buffer_allocated_size - ccon->buffer_data_size;
  if (todo == 0) return processx__connection_to_utf8(ccon);

  /* Otherwise we read */
  bytes_read = read(ccon->handle, ccon->buffer + ccon->buffer_data_size, todo);

  if (bytes_read == 0) {
    /* EOF */
    ccon->is_eof_raw_ = 1;
    if (ccon->utf8_data_size == 0 && ccon->buffer_data_size == 0) {
      ccon->is_eof_ = 1;
    }

  } else if (bytes_read == -1 && errno == EAGAIN) {
    /* There is still data to read, potentially */
    bytes_read = 0;

  } else if (bytes_read == -1) {
    /* Proper error  */
    error("Cannot read from processx connection: %s", strerror(errno));
  }

  ccon->buffer_data_size += bytes_read;

  /* If there is anything to convert to UTF8, try converting */
  if (ccon->buffer_data_size > 0) {
    bytes_read = processx__connection_to_utf8(ccon);
  } else {
    bytes_read = 0;
  }

  return bytes_read;
}
#endif

static ssize_t processx__connection_to_utf8(processx_connection_t *ccon) {

  const char *inbuf, *inbufold;
  char *outbuf, *outbufold;
  size_t inbytesleft = ccon->buffer_data_size;
  size_t outbytesleft = ccon->utf8_allocated_size - ccon->utf8_data_size;
  size_t r, indone = 0, outdone = 0;
  int moved = 0;
  const char *emptystr = "";
  const char *encoding = ccon->encoding ? ccon->encoding : emptystr;

  inbuf = inbufold = ccon->buffer;
  outbuf = outbufold = ccon->utf8 + ccon->utf8_data_size;

  /* If we this is the first time we are here. */
  if (! ccon->iconv_ctx) ccon->iconv_ctx = Riconv_open("UTF-8", encoding);

  /* If nothing to do, or no space to do more, just return */
  if (inbytesleft == 0 || outbytesleft == 0) return 0;

  while (!moved) {
    r = Riconv(ccon->iconv_ctx, &inbuf, &inbytesleft, &outbuf,
	       &outbytesleft);
    moved = 1;

    if (r == (size_t) -1) {
      /* Error */
      if (errno == E2BIG) {
	/* Output buffer is full, that's fine, we'll try later.
	   Just use what we have done so far. */

      } else if (errno == EILSEQ) {
	/* Invalid characters in encoding, *inbuf points to the beginning
	   of the invalid sequence. We can just try to remove this, and
	   convert again? */
	inbuf++; inbytesleft--;
	if (inbytesleft > 0) moved = 0;

      } else if (errno == EINVAL) {
	/* Does not end with a complete multi-byte character */
	/* This is fine, we'll handle it later, unless we are at the end */
	if (ccon->is_eof_raw_) {
	  warning("Invalid multi-byte character at end of stream ignored");
	  inbuf += inbytesleft; inbytesleft = 0;
	}
      }
    }
  }

  /* We converted 'r' bytes, update the buffer structure accordingly */
  indone = inbuf - inbufold;
  outdone = outbuf - outbufold;
  if (outdone > 0 || indone > 0) {
    ccon->buffer_data_size -= indone;
    memmove(ccon->buffer, ccon->buffer + indone, ccon->buffer_data_size);
    ccon->utf8_data_size += outdone;
  }

  return outdone;
}

/* Try to get at max 'max' UTF8 characters from the buffer. Return the
 * number of characters found, and also the corresponding number of
 * bytes. */

/* Number of additional bytes */
static const unsigned char processx__utf8_length[] = {
  2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
  2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
  3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,
  4,4,4,4,4,4,4,4,5,5,5,5,6,6,6,6 };

static void processx__connection_find_utf8_chars(processx_connection_t *ccon,
						 ssize_t maxchars,
						 ssize_t maxbytes,
						 size_t *chars,
						 size_t *bytes) {

  char *ptr = ccon->utf8;
  char *end = ccon->utf8 + ccon->utf8_data_size;
  size_t length = ccon->utf8_data_size;
  *chars = *bytes = 0;

  while (maxchars != 0 && maxbytes != 0 && ptr < end) {
    int clen, c = (unsigned char) *ptr;

    /* ASCII byte */
    if (c < 128) {
      (*chars) ++; (*bytes) ++; ptr++; length--;
      if (maxchars > 0) maxchars--;
      if (maxbytes > 0) maxbytes--;
      continue;
    }

    /* Catch some errors */
    if (c <  0xc0) goto invalid;
    if (c >= 0xfe) goto invalid;

    clen = processx__utf8_length[c & 0x3f];
    if (length < clen) goto invalid;
    if (maxbytes > 0 && clen > maxbytes) break;
    (*chars) ++; (*bytes) += clen; ptr += clen; length -= clen;
    if (maxchars > 0) maxchars--;
    if (maxbytes > 0) maxbytes -= clen;
  }

  return;

 invalid:
  error("Invalid UTF-8 string, internal error");
}

#ifndef _WIN32

int processx__interruptible_poll(struct pollfd fds[],
				 nfds_t nfds, int timeout) {
  int ret = 0;
  int timeleft = timeout;

  while (timeout < 0 || timeleft > PROCESSX_INTERRUPT_INTERVAL) {
    do {
      ret = poll(fds, nfds, PROCESSX_INTERRUPT_INTERVAL);
    } while (ret == -1 && errno == EINTR);

    /* If not a timeout, then return */
    if (ret != 0) return ret;

    R_CheckUserInterrupt();
    timeleft -= PROCESSX_INTERRUPT_INTERVAL;
  }

  /* Maybe we are not done, and there is a little left from the timeout */
  if (timeleft >= 0) {
    do {
      ret = poll(fds, nfds, timeleft);
    } while (ret == -1 && errno == EINTR);
  }

  return ret;
}

#endif

#undef PROCESSX_CHECK_VALID_CONN
