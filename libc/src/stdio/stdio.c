#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysbind.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "./impl.h"

static int _stdio_close(FILE *fp);
static size_t _stdio_read(FILE *fp, unsigned char *dst, size_t sz);
static size_t _stdio_write(FILE *fp, const unsigned char *src, size_t sz);
static off_t _stdio_seek(FILE *fp, off_t offset, int whence);

static FILE _stdin = {
    .close = NULL,
    .write = NULL,
    .seek = NULL,
    .read = _stdio_read,

    .fd = 0,
};

static char _stdout_buffer[BUFSIZ];
static FILE _stdout = {
    .close = NULL,
    .write = _stdio_write,
    .seek = NULL,
    .read = NULL,

    .fd = 1,

    .buffered = 1,
    .buf_len = 0,
    .buf_cap = BUFSIZ,
    .buffer = _stdout_buffer,
};

static char _stderr_buffer[BUFSIZ];
static FILE _stderr = {
    .close = NULL,
    .write = _stdio_write,
    .seek = NULL,
    .read = NULL,

    .fd = 2,

    .buffered = 1,
    .buf_len = 0,
    .buf_cap = BUFSIZ,
    .buffer = _stderr_buffer,
};

FILE *const stdin = &_stdin;
FILE *const stdout = &_stdout;
FILE *const stderr = &_stderr;

// TODO: lock
static FILE *ofl_head = NULL;

static FILE **ofl_lock() {
  // TODO: lock
  return &ofl_head;
}
static void ofl_unlock() {
  // TODO: unlock
}
static FILE *ofl_add(FILE *fp) {
  FILE **head = ofl_lock();
  fp->next = *head;
  if (*head) (*head)->prev = fp;
  *head = fp;
  ofl_unlock();
  return fp;
}

static void ofl_remove(FILE *fp) {
  FILE **head = ofl_lock();
  if (fp->prev) fp->prev->next = fp->next;
  if (fp->next) fp->next->prev = fp->prev;
  if (*head == fp) *head = fp->next;
  ofl_unlock();
}

static void stdio_exit(void) {
  fflush(stdout);
  fflush(stderr);
  // go through all the files and flush them
  for (FILE *f = ofl_head; f != NULL; f = f->next)
    fflush(f);
}

void stdio_init(void) { atexit(stdio_exit); }

static FILE *falloc();
static void ffree(FILE *);

FILE *falloc(void) {
  // allocate and zero out the file object
  FILE *fp = malloc(sizeof(*fp));
  memset(fp, 0, sizeof(*fp));

  fp->fd = -1;
  fp->lock = -1;

  fp->has_ungetc = 0;

  pthread_mutex_init(&fp->flock, NULL);

  return ofl_add(fp);
}

void ffree(FILE *fp) {
  // remove it from the open file list
  ofl_remove(fp);

  free(fp);
}


int fileno(FILE *stream) {
  if (stream == NULL) {
    return -1;
  }
  return stream->fd;
}

int fclose(FILE *fp) {
  if (fp->close != NULL) {
    fp->close(fp);
  }
  ffree(fp);
  return 0;
}

/* unlocked fwrite */
size_t _fwrite(const void *restrict src, size_t size, size_t nmemb, FILE *restrict f) {
  size_t len = size * nmemb;
  if (f->write != NULL) {
    size_t k = f->write(f, src, len);

    if (k != 0) {
      return k / size;
    }
  }
  return 0;
}
/* unlocked fread */
size_t _fread(void *restrict destv, size_t size, size_t nmemb, FILE *restrict f) {
  size_t len = size * nmemb;

  if (f->read != NULL) {
    size_t k = f->read(f, destv, len);

    if (k < 0) {
      f->eof = 1;
      return 0;
    }

    if (k != 0) {
      return k / size;
    }
  }
  return 0;
}



size_t fwrite(const void *restrict src, size_t size, size_t nmemb, FILE *restrict f) {
  FLOCK(f);
  size_t out = _fwrite(src, size, nmemb, f);
  FUNLOCK(f);
  return out;
}

size_t fread(void *restrict destv, size_t size, size_t nmemb, FILE *restrict f) {
  FLOCK(f);
  size_t out = _fread(destv, size, nmemb, f);
  FUNLOCK(f);
  return out;
}

static int _stdio_close(FILE *fp) {
  // close both ends of the file?
  errno_wrap(sysbind_close(fp->fd));

  return 0;
}

static size_t _stdio_read(FILE *fp, unsigned char *dst, size_t sz) {
  if (fp->fd == -1) {
    return 0;
  }
  long k = errno_wrap(sysbind_read(fp->fd, dst, sz));
  if (k < 0) return 0;
  return k;
}

static void _stdio_flush_buffer(FILE *fp) {
  if (fp->buffered) {
    if (fp->buffer != NULL && fp->buf_len > 0) {
      errno_wrap(sysbind_write(fp->fd, fp->buffer, fp->buf_len));
      fp->buf_len = 0;
      // NULL out the buffer
      memset(fp->buffer, 0x00, fp->buf_cap);
    }
  }
}

static size_t _stdio_write(FILE *fp, const unsigned char *src, size_t sz) {
  if (fp->fd == -1) {
    return 0;
  }

  if (fp->buffered && fp->buffer != NULL) {
    /*
    if (fp->bufrole != BUFROLE_WRITE) {
            _stdio_flush_buffer(fp);
    }
    */
    fp->bufrole = BUFROLE_WRITE;

    /**
     * copy from the src into the buffer, flushing on \n or when it is full
     */
    for (size_t i = 0; i < sz; i++) {
      // Assume that the buffer has space avail.
      unsigned char c = src[i];

      fp->buffer[fp->buf_len++] = c;

      if (c == '\n' || /* is full */ fp->buf_len == fp->buf_cap) {
        _stdio_flush_buffer(fp);
      }
    }
    return sz;
  }

  long k = errno_wrap(sysbind_write(fp->fd, (void *)src, sz));
  if (k < 0) return 0;
  return k;
}

static off_t _stdio_seek(FILE *fp, off_t offset, int whence) { return errno_wrap(sysbind_lseek(fp->fd, offset, whence)); }



static int string_to_mode(const char *mode) {
  if (mode == NULL) return -1;
#define DEF_MODE(s, m) \
  if (strcmp(mode, s) == 0) return m

  DEF_MODE("r", O_RDONLY);
  DEF_MODE("rb", O_RDONLY);

  DEF_MODE("w", O_WRONLY | O_CREAT | O_TRUNC);
  DEF_MODE("wb", O_WRONLY | O_CREAT | O_TRUNC);

  DEF_MODE("a", O_WRONLY | O_CREAT | O_APPEND);
  DEF_MODE("ab", O_WRONLY | O_CREAT | O_APPEND);

  DEF_MODE("r+", O_RDWR);
  DEF_MODE("rb+", O_RDWR);

  DEF_MODE("w+", O_RDWR | O_CREAT | O_TRUNC);
  DEF_MODE("wb+", O_RDWR | O_CREAT | O_TRUNC);

  DEF_MODE("a+", O_RDWR | O_CREAT | O_APPEND);
  DEF_MODE("ab+", O_RDWR | O_CREAT | O_APPEND);

  return -1;
#undef DEF_MODE
}


FILE *fdopen(int fd, const char *string_mode) {
  int mode = string_to_mode(string_mode);
  if (mode == -1) {
    errno = EINVAL;
    return NULL;
  }

  FILE *fp = falloc();

  fp->fd = fd;


  fp->pos = ftell(fp);

  fp->close = _stdio_close;
  fp->read = _stdio_read;
  fp->write = _stdio_write;
  fp->seek = _stdio_seek;

  fp->buffer = fp->default_buffer;
  fp->buffered = 1;
  fp->bufrole = BUFROLE_NONE;
  fp->buf_cap = BUFSIZ;
  fp->buf_len = 0;

  return fp;
}

FILE *fopen(const char *path, const char *string_mode) {
  int mode = string_to_mode(string_mode);
  if (mode == -1) {
    errno = EINVAL;
    return NULL;
  }
  int fd = open(path, mode);

  if (fd < 0) return NULL;

  return fdopen(fd, string_mode);
}

int fflush(FILE *fp) {
  _stdio_flush_buffer(fp);
  return 0;
}

int fputc(int c, FILE *stream) {
  char data[] = {c};
  fwrite(data, 1, 1, stream);
  return c;
}

int putc(int c, FILE *stream) __attribute__((weak, alias("fputc")));

int fgetc(FILE *stream) {
  if (stream->has_ungetc) {
    stream->has_ungetc = 0;
    return stream->ungetc;
  }
  char buf[1];
  int r;
  r = fread(buf, 1, 1, stream);
  if (r <= 0) {
    stream->eof = 1;
    return EOF;
  }
  return (unsigned char)buf[0];
}

int feof(FILE *stream) { return stream->eof; }

int ferror(FILE *s) { return 0; }
void clearerr(FILE *s) {}

int getc(FILE *stream) __attribute__((weak, alias("fgetc")));

int getchar(void) { return fgetc(stdin); }

int ungetc(int c, FILE *stream) {
  if (stream->has_ungetc) fprintf(stderr, "File has ungetc already\n");
  stream->has_ungetc = 1;
  stream->ungetc = c;
  return EOF;
}

char *fgets(char *s, int size, FILE *stream) {
  int c;
  char *out = s;
  while ((c = fgetc(stream)) > 0) {
    *s++ = c;
    size--;
    if (size == 0) {
      return out;
    }
    *s = '\0';
    if (c == '\n') {
      return out;
    }
  }
  if (c == EOF) {
    stream->eof = 1;
    if (out == s) {
      return NULL;
    } else {
      return out;
    }
  }
  return NULL;
}

int putchar(int c) { return fputc(c, stdout); }

int fseek(FILE *stream, long offset, int whence) { return lseek(stream->fd, offset, whence); }

long ftell(FILE *stream) { return lseek(stream->fd, 0, SEEK_CUR); }

void rewind(FILE *stream) { fseek(stream, 0, SEEK_SET); }

int fputs(const char *s, FILE *stream) {
  int len = strlen(s);
  int r = fwrite(s, len, 1, stream);
  return r >= 0 ? 0 : EOF;
}

int puts(const char *s) {
  int r = -(fputs(s, stdout) < 0 || fputc('\n', stdout) < 0);
  return r;
}

int remove(const char *pathname) { return -1; }

void perror(const char *msg) {
  // store errno in case something else in this function fiddles with it
  int e = errno;

  printf("%s: %s\n", msg, strerror(e));
}

void setbuf(FILE *stream, char *buf) { setvbuf(stream, buf, buf ? _IOFBF : _IONBF, BUFSIZ); }
void setbuffer(FILE *stream, char *buf, size_t size) {}
void setlinebuf(FILE *stream) {}

int setvbuf(FILE *restrict f, char *restrict buf, int type, size_t size) {
  // f->lbf = EOF;

  if (type == _IONBF) {
    f->buf_cap = 0;
  } else {
    if (buf && size >= UNGET) {
      f->buffer = (void *)(buf + UNGET);
      f->buf_cap = size - UNGET;
    }
    // if (type == _IOLBF && f->buf_cap) f->lbf = '\n';
  }

  f->flags |= F_SVB;

  return 0;
}

FILE *tmpfile(void) { return NULL; }



int rename(const char *old_filename, const char *new_filename) {
  errno = -ENOTIMPL;
  return -1;
}



// TODO:
int fscanf(FILE *stream, const char *format, ...) { return -1; }




#define MIN_LINE_SIZE 4
#define DEFAULT_LINE_SIZE 128

ssize_t __getdelim(char **bufptr, size_t *n, int delim, FILE *fp) {
  char *buf;
  char *ptr;
  size_t newsize, numbytes;
  int pos;
  int ch;
  int cont;

  if (fp == NULL || bufptr == NULL || n == NULL) {
    errno = EINVAL;
    return -1;
  }

  buf = *bufptr;
  if (buf == NULL || *n < MIN_LINE_SIZE) {
    buf = (char *)realloc(*bufptr, DEFAULT_LINE_SIZE);
    if (buf == NULL) {
      return -1;
    }
    *bufptr = buf;
    *n = DEFAULT_LINE_SIZE;
  }

  // CHECK_INIT (_REENT, fp);

  // _newlib_flockfile_start (fp);

  numbytes = *n;
  ptr = buf;

  cont = 1;

  while (cont) {
    /* fill buffer - leaving room for nul-terminator */
    while (--numbytes > 0) {
      if ((ch = getc(fp)) == EOF) {
        cont = 0;
        break;
      } else {
        *ptr++ = ch;
        if (ch == delim) {
          cont = 0;
          break;
        }
      }
    }

    if (cont) {
      /* Buffer is too small so reallocate a larger buffer.  */
      pos = ptr - buf;
      newsize = (*n << 1);
      buf = realloc(buf, newsize);
      if (buf == NULL) {
        cont = 0;
        break;
      }

      /* After reallocating, continue in new buffer */
      *bufptr = buf;
      *n = newsize;
      ptr = buf + pos;
      numbytes = newsize - pos;
    }
  }

  // _newlib_flockfile_end (fp);

  /* if no input data, return failure */
  if (ptr == buf) return -1;

  /* otherwise, nul-terminate and return number of bytes read */
  *ptr = '\0';
  return (ssize_t)(ptr - buf);
}

ssize_t getdelim(char **lineptr, size_t *n, int delim, FILE *stream) { return __getdelim(lineptr, n, delim, stream); }

ssize_t getline(char **lineptr, size_t *n, FILE *stream) { return getdelim(lineptr, n, '\n', stream); }
