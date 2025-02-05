#include <asm.h>
#include <console.h>
#include <cpu.h>
#include <lock.h>
#include <net/ipv4.h>
#include <printf.h>
#include <ck/string.h>
#include <syscall.h>
#include <time.h>
#include <types.h>
#include <cpu.h>
#include <vga.h>
#include "debug.h"
#include <thread.h>
#include <kshell.h>

void debug_dump_addr2line() {
#if CONFIG_X86
  off_t rbp = 0;
  asm volatile("mov %%rbp, %0\n\t" : "=r"(rbp));
  auto bt = debug::generate_backtrace(rbp);

  printf("addr2line -e build/chariot.elf ");
  for (auto pc : bt) {
    printf(" 0x%p", pc);
  }
  printf("\n");
#endif
}

extern volatile bool did_panic;
extern volatile long monitor_tid;

void debug_die(void) {
  // debug_dump_addr2line();


  if (cpu::in_thread()) {
    monitor_tid = curthd->tid;
    printf("panic in %d\n", monitor_tid);
  }

  __sync_synchronize();
  did_panic = true;
  if (cpu::in_thread()) {
    arch_enable_ints();
    kshell::run(RED "(panic)" RESET);
  }

  // arch_dump_backtrace();
  while (1) {
    arch_halt();
  }
}

// 'ntoa' conversion buffer size, this must be big enough to hold one converted
// numeric number including padded zeros (dynamically created on stack)
// default: 32 byte
#ifndef PRINTF_NTOA_BUFFER_SIZE
#define PRINTF_NTOA_BUFFER_SIZE 32U
#endif

// 'ftoa' conversion buffer size, this must be big enough to hold one converted
// float number including padded zeros (dynamically created on stack)
// default: 32 byte
#ifndef PRINTF_FTOA_BUFFER_SIZE
#define PRINTF_FTOA_BUFFER_SIZE 32U
#endif


#define PRINTF_DISABLE_SUPPORT_FLOAT
// support for the floating point type (%f)
// default: activated
#ifndef PRINTF_DISABLE_SUPPORT_FLOAT
#define PRINTF_SUPPORT_FLOAT
#endif

// support for exponential floating point notation (%e/%g)
// default: activated
#ifndef PRINTF_DISABLE_SUPPORT_EXPONENTIAL
#define PRINTF_SUPPORT_EXPONENTIAL
#endif

// define the default floating point precision
// default: 6 digits
#ifndef PRINTF_DEFAULT_FLOAT_PRECISION
#define PRINTF_DEFAULT_FLOAT_PRECISION 6U
#endif

// define the largest float suitable to print with %f
// default: 1e9
#ifndef PRINTF_MAX_FLOAT
#define PRINTF_MAX_FLOAT 1e9
#endif

// support for the long long types (%llu or %p)
// default: activated
#ifndef PRINTF_DISABLE_SUPPORT_LONG_LONG
#define PRINTF_SUPPORT_LONG_LONG
#endif

// support for the ptrdiff_t type (%t)
// ptrdiff_t is normally defined in <stddef.h> as long or long long type
// default: activated
#ifndef PRINTF_DISABLE_SUPPORT_PTRDIFF_T
#define PRINTF_SUPPORT_PTRDIFF_T
#endif

///////////////////////////////////////////////////////////////////////////////

// internal flag definitions
#define FLAGS_ZEROPAD (1U << 0U)
#define FLAGS_LEFT (1U << 1U)
#define FLAGS_PLUS (1U << 2U)
#define FLAGS_SPACE (1U << 3U)
#define FLAGS_HASH (1U << 4U)
#define FLAGS_UPPERCASE (1U << 5U)
#define FLAGS_CHAR (1U << 6U)
#define FLAGS_SHORT (1U << 7U)
#define FLAGS_LONG (1U << 8U)
#define FLAGS_LONG_LONG (1U << 9U)
#define FLAGS_PRECISION (1U << 10U)
#define FLAGS_ADAPT_EXP (1U << 11U)

// import float.h for DBL_MAX
#if defined(PRINTF_SUPPORT_FLOAT)
#include <float.h>
#endif

#define IO_PORT_PUTCHAR 0xfad



#define C_RED 91
#define C_GREEN 92
#define C_YELLOW 93
#define C_BLUE 94
#define C_MAGENTA 95
#define C_CYAN 96

#define C_RESET 0
#define C_GRAY 90

static int current_color = 0;
static void set_color(int code) {
  if (code != current_color) {
    printf("\x1b[%dm", code);
    current_color = code;
  }
}

static void set_color_for(char c) {
  if (c >= 'A' && c <= 'z') {
    set_color(C_YELLOW);
  } else if (c >= '!' && c <= '~') {
    set_color(C_CYAN);
  } else if (c == '\n' || c == '\r') {
    set_color(C_GREEN);
  } else if (c == '\a' || c == '\b' || c == 0x1b || c == '\f' || c == '\n' || c == '\r') {
    set_color(C_RED);
  } else if ((unsigned char)c == 0xFF) {
    set_color(C_MAGENTA);
  } else {
    set_color(C_GRAY);
  }
}


void putchar(char c) {
  console::putc(c, true /* debug */);
  // vga::putchar(c);
  // serial_send(SERIAL_PORT_A, c);
}
int puts(char *s) {
  int i;
  for (i = 0; s[i] != '\0'; i++)
    console::putc(s[i]);
  return i;
}

/*
const char *human_size(uint64_t bytes, char *buf) {
  const char *suffix[] = {"B", "KB", "MB", "GB", "TB", "PB", "EB"};
  char length = sizeof(suffix) / sizeof(suffix[0]);

  int i = 0;
  double dblBytes = bytes;

  if (bytes > 1024) {
    for (i = 0; (bytes / 1024) > 0 && i < length - 1; i++, bytes /= 1024)
      dblBytes = bytes / 1024.0;
  }

  sprintf(buf, "%.03lf %s", dblBytes, suffix[i]);
  return buf;
}
*/

// output function type
typedef void (*out_fct_type)(char character, void *buffer, size_t idx, size_t maxlen);

// wrapper (used as buffer) for output function type
typedef struct {
  void (*fct)(char character, void *arg);
  void *arg;
} out_fct_wrap_type;

// internal buffer output
static inline void _out_buffer(char character, void *buffer, size_t idx, size_t maxlen) {
  if (idx < maxlen) {
    ((char *)buffer)[idx] = character;
  }
}

// internal null output
static inline void _out_null(char character, void *buffer, size_t idx, size_t maxlen) {
  (void)character;
  (void)buffer;
  (void)idx;
  (void)maxlen;
}

// internal _putchar wrapper
static inline void _out_char(char character, void *buffer, size_t idx, size_t maxlen) {
  (void)buffer;
  (void)idx;
  (void)maxlen;
  if (character) {
    putchar(character);
  }
}

// internal output function wrapper
static inline void _out_fct(char character, void *buffer, size_t idx, size_t maxlen) {
  (void)idx;
  (void)maxlen;
  if (character) {
    // buffer is the output fct pointer
    ((out_fct_wrap_type *)buffer)->fct(character, ((out_fct_wrap_type *)buffer)->arg);
  }
}

// internal secure strlen
// \return The length of the string (excluding the terminating 0) limited by
// 'maxsize'
static inline unsigned int _strnlen_s(const char *str, size_t maxsize) {
  const char *s;
  for (s = str; *s && maxsize--; ++s)
    ;
  return (unsigned int)(s - str);
}

// internal test if char is a digit (0-9)
// \return true if char is a digit
static inline bool _is_digit(char ch) { return (ch >= '0') && (ch <= '9'); }

// internal ASCII string to unsigned int conversion
static unsigned int _atoi(const char **str) {
  unsigned int i = 0U;
  while (_is_digit(**str)) {
    i = i * 10U + (unsigned int)(*((*str)++) - '0');
  }
  return i;
}

// output the specified string in reverse, taking care of any zero-padding
static size_t _out_rev(
    out_fct_type out, char *buffer, size_t idx, size_t maxlen, const char *buf, size_t len, unsigned int width, unsigned int flags) {
  const size_t start_idx = idx;

  // pad spaces up to given width
  if (!(flags & FLAGS_LEFT) && !(flags & FLAGS_ZEROPAD)) {
    for (size_t i = len; i < width; i++) {
      out(' ', buffer, idx++, maxlen);
    }
  }

  // reverse string
  while (len) {
    out(buf[--len], buffer, idx++, maxlen);
  }

  // append pad spaces up to given width
  if (flags & FLAGS_LEFT) {
    while (idx - start_idx < width) {
      out(' ', buffer, idx++, maxlen);
    }
  }

  return idx;
}

// internal itoa format
static size_t _ntoa_format(out_fct_type out, char *buffer, size_t idx, size_t maxlen, char *buf, size_t len, bool negative,
    unsigned int base, unsigned int prec, unsigned int width, unsigned int flags) {
  // pad leading zeros
  if (!(flags & FLAGS_LEFT)) {
    if (width && (flags & FLAGS_ZEROPAD) && (negative || (flags & (FLAGS_PLUS | FLAGS_SPACE)))) {
      width--;
    }
    while ((len < prec) && (len < PRINTF_NTOA_BUFFER_SIZE)) {
      buf[len++] = '0';
    }
    while ((flags & FLAGS_ZEROPAD) && (len < width) && (len < PRINTF_NTOA_BUFFER_SIZE)) {
      buf[len++] = '0';
    }
  }

  // handle hash
  if (flags & FLAGS_HASH) {
    if (!(flags & FLAGS_PRECISION) && len && ((len == prec) || (len == width))) {
      len--;
      if (len && (base == 16U)) {
        len--;
      }
    }
    if ((base == 16U) && !(flags & FLAGS_UPPERCASE) && (len < PRINTF_NTOA_BUFFER_SIZE)) {
      buf[len++] = 'x';
    } else if ((base == 16U) && (flags & FLAGS_UPPERCASE) && (len < PRINTF_NTOA_BUFFER_SIZE)) {
      buf[len++] = 'X';
    } else if ((base == 2U) && (len < PRINTF_NTOA_BUFFER_SIZE)) {
      buf[len++] = 'b';
    }
    if (len < PRINTF_NTOA_BUFFER_SIZE) {
      buf[len++] = '0';
    }
  }

  if (len < PRINTF_NTOA_BUFFER_SIZE) {
    if (negative) {
      buf[len++] = '-';
    } else if (flags & FLAGS_PLUS) {
      buf[len++] = '+';  // ignore the space if the '+' exists
    } else if (flags & FLAGS_SPACE) {
      buf[len++] = ' ';
    }
  }

  return _out_rev(out, buffer, idx, maxlen, buf, len, width, flags);
}

// internal itoa for 'long' type
static size_t _ntoa_long(out_fct_type out, char *buffer, size_t idx, size_t maxlen, unsigned long value, bool negative, unsigned long base,
    unsigned int prec, unsigned int width, unsigned int flags) {
  char buf[PRINTF_NTOA_BUFFER_SIZE];
  size_t len = 0U;

  // no hash for 0 values
  if (!value) {
    flags &= ~FLAGS_HASH;
  }

  // write if precision != 0 and value is != 0
  if (!(flags & FLAGS_PRECISION) || value) {
    do {
      const char digit = (char)(value % base);
      buf[len++] = digit < 10 ? '0' + digit : (flags & FLAGS_UPPERCASE ? 'A' : 'a') + digit - 10;
      value /= base;
    } while (value && (len < PRINTF_NTOA_BUFFER_SIZE));
  }

  return _ntoa_format(out, buffer, idx, maxlen, buf, len, negative, (unsigned int)base, prec, width, flags);
}

// internal itoa for 'long long' type
#if defined(PRINTF_SUPPORT_LONG_LONG)
static size_t _ntoa_long_long(out_fct_type out, char *buffer, size_t idx, size_t maxlen, unsigned long long value, bool negative,
    unsigned long long base, unsigned int prec, unsigned int width, unsigned int flags) {
  char buf[PRINTF_NTOA_BUFFER_SIZE];
  size_t len = 0U;

  // no hash for 0 values
  if (!value) {
    flags &= ~FLAGS_HASH;
  }

  // write if precision != 0 and value is != 0
  if (!(flags & FLAGS_PRECISION) || value) {
    do {
      const char digit = (char)(value % base);
      buf[len++] = digit < 10 ? '0' + digit : (flags & FLAGS_UPPERCASE ? 'A' : 'a') + digit - 10;
      value /= base;
    } while (value && (len < PRINTF_NTOA_BUFFER_SIZE));
  }

  return _ntoa_format(out, buffer, idx, maxlen, buf, len, negative, (unsigned int)base, prec, width, flags);
}
#endif  // PRINTF_SUPPORT_LONG_LONG

#if defined(PRINTF_SUPPORT_FLOAT)

#if defined(PRINTF_SUPPORT_EXPONENTIAL)
// forward declaration so that _ftoa can switch to exp notation for values >
// PRINTF_MAX_FLOAT
static size_t _etoa(
    out_fct_type out, char *buffer, size_t idx, size_t maxlen, double value, unsigned int prec, unsigned int width, unsigned int flags);
#endif

// internal ftoa for fixed decimal floating point
static size_t _ftoa(
    out_fct_type out, char *buffer, size_t idx, size_t maxlen, double value, unsigned int prec, unsigned int width, unsigned int flags) {
  char buf[PRINTF_FTOA_BUFFER_SIZE];
  size_t len = 0U;
  double diff = 0.0;

  // powers of 10
  static const double pow10[] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000};

  // test for special values
  if (value != value) return _out_rev(out, buffer, idx, maxlen, "nan", 3, width, flags);
  if (value < -DBL_MAX) return _out_rev(out, buffer, idx, maxlen, "fni-", 4, width, flags);
  if (value > DBL_MAX)
    return _out_rev(out, buffer, idx, maxlen, (flags & FLAGS_PLUS) ? "fni+" : "fni", (flags & FLAGS_PLUS) ? 4U : 3U, width, flags);

  // test for very large values
  // standard printf behavior is to print EVERY whole number digit -- which
  // could be 100s of characters overflowing your buffers == bad
  if ((value > PRINTF_MAX_FLOAT) || (value < -PRINTF_MAX_FLOAT)) {
#if defined(PRINTF_SUPPORT_EXPONENTIAL)
    return _etoa(out, buffer, idx, maxlen, value, prec, width, flags);
#else
    return 0U;
#endif
  }

  // test for negative
  bool negative = false;
  if (value < 0) {
    negative = true;
    value = 0 - value;
  }

  // set default precision, if not set explicitly
  if (!(flags & FLAGS_PRECISION)) {
    prec = PRINTF_DEFAULT_FLOAT_PRECISION;
  }
  // limit precision to 9, cause a prec >= 10 can lead to overflow errors
  while ((len < PRINTF_FTOA_BUFFER_SIZE) && (prec > 9U)) {
    buf[len++] = '0';
    prec--;
  }

  int whole = (int)value;
  double tmp = (value - whole) * pow10[prec];
  unsigned long frac = (unsigned long)tmp;
  diff = tmp - frac;

  if (diff > 0.5) {
    ++frac;
    // handle rollover, e.g. case 0.99 with prec 1 is 1.0
    if (frac >= pow10[prec]) {
      frac = 0;
      ++whole;
    }
  } else if (diff < 0.5) {
  } else if ((frac == 0U) || (frac & 1U)) {
    // if halfway, round up if odd OR if last digit is 0
    ++frac;
  }

  if (prec == 0U) {
    diff = value - (double)whole;
    if ((!(diff < 0.5) || (diff > 0.5)) && (whole & 1)) {
      // exactly 0.5 and ODD, then round up
      // 1.5 -> 2, but 2.5 -> 2
      ++whole;
    }
  } else {
    unsigned int count = prec;
    // now do fractional part, as an unsigned number
    while (len < PRINTF_FTOA_BUFFER_SIZE) {
      --count;
      buf[len++] = (char)(48U + (frac % 10U));
      if (!(frac /= 10U)) {
        break;
      }
    }
    // add extra 0s
    while ((len < PRINTF_FTOA_BUFFER_SIZE) && (count-- > 0U)) {
      buf[len++] = '0';
    }
    if (len < PRINTF_FTOA_BUFFER_SIZE) {
      // add decimal
      buf[len++] = '.';
    }
  }

  // do whole part, number is reversed
  while (len < PRINTF_FTOA_BUFFER_SIZE) {
    buf[len++] = (char)(48 + (whole % 10));
    if (!(whole /= 10)) {
      break;
    }
  }

  // pad leading zeros
  if (!(flags & FLAGS_LEFT) && (flags & FLAGS_ZEROPAD)) {
    if (width && (negative || (flags & (FLAGS_PLUS | FLAGS_SPACE)))) {
      width--;
    }
    while ((len < width) && (len < PRINTF_FTOA_BUFFER_SIZE)) {
      buf[len++] = '0';
    }
  }

  if (len < PRINTF_FTOA_BUFFER_SIZE) {
    if (negative) {
      buf[len++] = '-';
    } else if (flags & FLAGS_PLUS) {
      buf[len++] = '+';  // ignore the space if the '+' exists
    } else if (flags & FLAGS_SPACE) {
      buf[len++] = ' ';
    }
  }

  return _out_rev(out, buffer, idx, maxlen, buf, len, width, flags);
}

#if defined(PRINTF_SUPPORT_EXPONENTIAL)
// internal ftoa variant for exponential floating-point type, contributed by
// Martijn Jasperse <m.jasperse@gmail.com>
static size_t _etoa(
    out_fct_type out, char *buffer, size_t idx, size_t maxlen, double value, unsigned int prec, unsigned int width, unsigned int flags) {
  // check for NaN and special values
  if ((value != value) || (value > DBL_MAX) || (value < -DBL_MAX)) {
    return _ftoa(out, buffer, idx, maxlen, value, prec, width, flags);
  }

  // determine the sign
  const bool negative = value < 0;
  if (negative) {
    value = -value;
  }

  // default precision
  if (!(flags & FLAGS_PRECISION)) {
    prec = PRINTF_DEFAULT_FLOAT_PRECISION;
  }

  // determine the decimal exponent
  // based on the algorithm by David Gay (https://www.ampl.com/netlib/fp/dtoa.c)
  union {
    uint64_t U;
    double F;
  } conv;

  conv.F = value;
  int exp2 = (int)((conv.U >> 52U) & 0x07FFU) - 1023;           // effectively log2
  conv.U = (conv.U & ((1ULL << 52U) - 1U)) | (1023ULL << 52U);  // drop the exponent so conv.F is now in [1,2)
  // now approximate log10 from the log2 integer part and an expansion of ln
  // around 1.5
  int expval = (int)(0.1760912590558 + exp2 * 0.301029995663981 + (conv.F - 1.5) * 0.289529654602168);
  // now we want to compute 10^expval but we want to be sure it won't overflow
  exp2 = (int)(expval * 3.321928094887362 + 0.5);
  const double z = expval * 2.302585092994046 - exp2 * 0.6931471805599453;
  const double z2 = z * z;
  conv.U = (uint64_t)(exp2 + 1023) << 52U;
  // compute exp(z) using continued fractions, see
  // https://en.wikipedia.org/wiki/Exponential_function#Continued_fractions_for_ex
  conv.F *= 1 + 2 * z / (2 - z + (z2 / (6 + (z2 / (10 + z2 / 14)))));
  // correct for rounding errors
  if (value < conv.F) {
    expval--;
    conv.F /= 10;
  }

  // the exponent format is "%+03d" and largest value is "307", so set aside 4-5
  // characters
  unsigned int minwidth = ((expval < 100) && (expval > -100)) ? 4U : 5U;

  // in "%g" mode, "prec" is the number of *significant figures* not decimals
  if (flags & FLAGS_ADAPT_EXP) {
    // do we want to fall-back to "%f" mode?
    if ((value >= 1e-4) && (value < 1e6)) {
      if ((int)prec > expval) {
        prec = (unsigned)((int)prec - expval - 1);
      } else {
        prec = 0;
      }
      flags |= FLAGS_PRECISION;  // make sure _ftoa respects precision
      // no characters in exponent
      minwidth = 0U;
      expval = 0;
    } else {
      // we use one sigfig for the whole part
      if ((prec > 0) && (flags & FLAGS_PRECISION)) {
        --prec;
      }
    }
  }

  // will everything fit?
  unsigned int fwidth = width;
  if (width > minwidth) {
    // we didn't fall-back so subtract the characters required for the exponent
    fwidth -= minwidth;
  } else {
    // not enough characters, so go back to default sizing
    fwidth = 0U;
  }
  if ((flags & FLAGS_LEFT) && minwidth) {
    // if we're padding on the right, DON'T pad the floating part
    fwidth = 0U;
  }

  // rescale the float value
  if (expval) {
    value /= conv.F;
  }

  // output the floating part
  const size_t start_idx = idx;
  idx = _ftoa(out, buffer, idx, maxlen, negative ? -value : value, prec, fwidth, flags & ~FLAGS_ADAPT_EXP);

  // output the exponent part
  if (minwidth) {
    // output the exponential symbol
    out((flags & FLAGS_UPPERCASE) ? 'E' : 'e', buffer, idx++, maxlen);
    // output the exponent value
    idx =
        _ntoa_long(out, buffer, idx, maxlen, (expval < 0) ? -expval : expval, expval < 0, 10, 0, minwidth - 1, FLAGS_ZEROPAD | FLAGS_PLUS);
    // might need to right-pad spaces
    if (flags & FLAGS_LEFT) {
      while (idx - start_idx < width)
        out(' ', buffer, idx++, maxlen);
    }
  }
  return idx;
}
#endif  // PRINTF_SUPPORT_EXPONENTIAL
#endif  // PRINTF_SUPPORT_FLOAT

// internal vsnprintf
static int _vsnprintf(out_fct_type out, char *buffer, const size_t maxlen, const char *format, va_list va) {
  unsigned int flags, width, precision, n;
  size_t idx = 0U;

  if (!buffer) {
    // use null output function
    out = _out_null;
  }

  while (*format) {
    // format specifier?  %[flags][width][.precision][length]
    if (*format != '%') {
      // no
      out(*format, buffer, idx++, maxlen);
      format++;
      continue;
    } else {
      // yes, evaluate it
      format++;
    }

    // evaluate flags
    flags = 0U;
    do {
      switch (*format) {
        case '0':
          flags |= FLAGS_ZEROPAD;
          format++;
          n = 1U;
          break;
        case '-':
          flags |= FLAGS_LEFT;
          format++;
          n = 1U;
          break;
        case '+':
          flags |= FLAGS_PLUS;
          format++;
          n = 1U;
          break;
        case ' ':
          flags |= FLAGS_SPACE;
          format++;
          n = 1U;
          break;
        case '#':
          flags |= FLAGS_HASH;
          format++;
          n = 1U;
          break;
        default:
          n = 0U;
          break;
      }
    } while (n);

    // evaluate width field
    width = 0U;
    if (_is_digit(*format)) {
      width = _atoi(&format);
    } else if (*format == '*') {
      const int w = va_arg(va, int);
      if (w < 0) {
        flags |= FLAGS_LEFT;  // reverse padding
        width = (unsigned int)-w;
      } else {
        width = (unsigned int)w;
      }
      format++;
    }

    // evaluate precision field
    precision = 0U;
    if (*format == '.') {
      flags |= FLAGS_PRECISION;
      format++;
      if (_is_digit(*format)) {
        precision = _atoi(&format);
      } else if (*format == '*') {
        const int prec = (int)va_arg(va, int);
        precision = prec > 0 ? (unsigned int)prec : 0U;
        format++;
      }
    }

    // evaluate length field
    switch (*format) {
      case 'l':
        flags |= FLAGS_LONG;
        format++;
        if (*format == 'l') {
          flags |= FLAGS_LONG_LONG;
          format++;
        }
        break;
      case 'h':
        flags |= FLAGS_SHORT;
        format++;
        if (*format == 'h') {
          flags |= FLAGS_CHAR;
          format++;
        }
        break;
#if defined(PRINTF_SUPPORT_PTRDIFF_T)
      case 't':
        flags |= (sizeof(ptrdiff_t) == sizeof(long) ? FLAGS_LONG : FLAGS_LONG_LONG);
        format++;
        break;
#endif
      case 'z':
        flags |= (sizeof(size_t) == sizeof(long) ? FLAGS_LONG : FLAGS_LONG_LONG);
        format++;
        break;
      default:
        break;
    }

    // evaluate specifier
    switch (*format) {
      case 'd':
      case 'i':
      case 'u':
      case 'x':
      case 'X':
      case 'o':
      case 'b': {
        // set the base
        unsigned int base;
        if (*format == 'x' || *format == 'X') {
          base = 16U;
        } else if (*format == 'o') {
          base = 8U;
        } else if (*format == 'b') {
          base = 2U;
        } else {
          base = 10U;
          flags &= ~FLAGS_HASH;  // no hash for dec format
        }
        // uppercase
        if (*format == 'X') {
          flags |= FLAGS_UPPERCASE;
        }

        // no plus or space flag for u, x, X, o, b
        if ((*format != 'i') && (*format != 'd')) {
          flags &= ~(FLAGS_PLUS | FLAGS_SPACE);
        }

        // ignore '0' flag when precision is given
        if (flags & FLAGS_PRECISION) {
          flags &= ~FLAGS_ZEROPAD;
        }

        // convert the integer
        if ((*format == 'i') || (*format == 'd')) {
          // signed
          if (flags & FLAGS_LONG_LONG) {
#if defined(PRINTF_SUPPORT_LONG_LONG)
            const long long value = va_arg(va, long long);
            idx = _ntoa_long_long(
                out, buffer, idx, maxlen, (unsigned long long)(value > 0 ? value : 0 - value), value < 0, base, precision, width, flags);
#endif
          } else if (flags & FLAGS_LONG) {
            const long value = va_arg(va, long);
            idx = _ntoa_long(
                out, buffer, idx, maxlen, (unsigned long)(value > 0 ? value : 0 - value), value < 0, base, precision, width, flags);
          } else {
            const int value = (flags & FLAGS_CHAR)    ? (char)va_arg(va, int)
                              : (flags & FLAGS_SHORT) ? (short int)va_arg(va, int)
                                                      : va_arg(va, int);
            idx = _ntoa_long(
                out, buffer, idx, maxlen, (unsigned int)(value > 0 ? value : 0 - value), value < 0, base, precision, width, flags);
          }
        } else {
          // unsigned
          if (flags & FLAGS_LONG_LONG) {
#if defined(PRINTF_SUPPORT_LONG_LONG)
            idx = _ntoa_long_long(out, buffer, idx, maxlen, va_arg(va, unsigned long long), false, base, precision, width, flags);
#endif
          } else if (flags & FLAGS_LONG) {
            idx = _ntoa_long(out, buffer, idx, maxlen, va_arg(va, unsigned long), false, base, precision, width, flags);
          } else {
            const unsigned int value = (flags & FLAGS_CHAR)    ? (unsigned char)va_arg(va, unsigned int)
                                       : (flags & FLAGS_SHORT) ? (unsigned short int)va_arg(va, unsigned int)
                                                               : va_arg(va, unsigned int);
            idx = _ntoa_long(out, buffer, idx, maxlen, value, false, base, precision, width, flags);
          }
        }
        format++;
        break;
      }
#if defined(PRINTF_SUPPORT_FLOAT)
      case 'f':
      case 'F':
        if (*format == 'F') flags |= FLAGS_UPPERCASE;
        idx = _ftoa(out, buffer, idx, maxlen, va_arg(va, double), precision, width, flags);
        format++;
        break;
#if defined(PRINTF_SUPPORT_EXPONENTIAL)
      case 'e':
      case 'E':
      case 'g':
      case 'G':
        if ((*format == 'g') || (*format == 'G')) flags |= FLAGS_ADAPT_EXP;
        if ((*format == 'E') || (*format == 'G')) flags |= FLAGS_UPPERCASE;
        idx = _etoa(out, buffer, idx, maxlen, va_arg(va, double), precision, width, flags);
        format++;
        break;
#endif  // PRINTF_SUPPORT_EXPONENTIAL
#endif  // PRINTF_SUPPORT_FLOAT
      case 'c': {
        unsigned int l = 1U;
        // pre padding
        if (!(flags & FLAGS_LEFT)) {
          while (l++ < width) {
            out(' ', buffer, idx++, maxlen);
          }
        }
        // char output
        out((char)va_arg(va, int), buffer, idx++, maxlen);
        // post padding
        if (flags & FLAGS_LEFT) {
          while (l++ < width) {
            out(' ', buffer, idx++, maxlen);
          }
        }
        format++;
        break;
      }

      case 's': {
        const char *p = va_arg(va, char *);
        unsigned int l = _strnlen_s(p, precision ? precision : (size_t)-1);
        // pre padding
        if (flags & FLAGS_PRECISION) {
          l = (l < precision ? l : precision);
        }
        if (!(flags & FLAGS_LEFT)) {
          while (l++ < width) {
            out(' ', buffer, idx++, maxlen);
          }
        }
        // string output
        while ((*p != 0) && (!(flags & FLAGS_PRECISION) || precision--)) {
          out(*(p++), buffer, idx++, maxlen);
        }
        // post padding
        if (flags & FLAGS_LEFT) {
          while (l++ < width) {
            out(' ', buffer, idx++, maxlen);
          }
        }
        format++;
        break;
      }


      // nonstandard "Mac address" formatter
      case 'A': {
        uint8_t *addr = (uint8_t *)va_arg(va, void *);
        for (int i = 0; i < 6; i++) {
          idx = _ntoa_long(out, buffer, idx, maxlen, addr[i], false, 16, 2, 2, 0);
          if (i != 5) out(':', buffer, idx++, maxlen);
        }
        format++;
        break;
      }


      // ip address formatter
      case 'I': {
        uint32_t addr = (uint32_t)va_arg(va, long long);
        char buf[32];
        net::ipv4::format_ip(addr, buf);
        for (int i = 0; buf[i]; i++) {
          out(buf[i], buffer, idx++, maxlen);
        }
        format++;
        break;
      }


      case 'p': {
        width = sizeof(void *) * 2U;
        flags |= FLAGS_ZEROPAD;  // | FLAGS_UPPERCASE;
#if defined(PRINTF_SUPPORT_LONG_LONG)
        const bool is_ll = sizeof(uintptr_t) == sizeof(long long);
        if (is_ll) {
          idx = _ntoa_long_long(out, buffer, idx, maxlen, (uintptr_t)va_arg(va, void *), false, 16U, precision, width, flags);
        } else {
#endif
          idx = _ntoa_long(out, buffer, idx, maxlen, (unsigned long)((uintptr_t)va_arg(va, void *)), false, 16U, precision, width, flags);
#if defined(PRINTF_SUPPORT_LONG_LONG)
        }
#endif
        format++;
        break;
      }


      case 'P': {
        width = sizeof(void *) * 2U;
        flags |= FLAGS_ZEROPAD;  // | FLAGS_UPPERCASE;
        unsigned long val = (unsigned long)((uintptr_t)va_arg(va, void *));

        idx = _ntoa_long(out, buffer, idx, maxlen, val >> 32, false, 16U, precision, width / 2, flags);

        out('`', buffer, idx++, maxlen);

        idx = _ntoa_long(out, buffer, idx, maxlen, val & 0xFFFF'FFFF, false, 16U, precision, width / 2, flags);

        /*
idx = _ntoa_long(out, buffer, idx, maxlen, (val >> 12) & 0xF'FFFF,
 false, 16U, precision, (width / 2) - 3, flags);

out(',', buffer, idx++, maxlen);
idx = _ntoa_long(out, buffer, idx, maxlen, val & 0xFFF,
 false, 16U, precision, 3, flags);
                                                                         */
        format++;
        break;
      }

      case '%':
        out('%', buffer, idx++, maxlen);
        format++;
        break;

      default:
        out(*format, buffer, idx++, maxlen);
        format++;
        break;
    }
  }

  // termination
  out((char)0, buffer, idx < maxlen ? idx : maxlen - 1U, maxlen);

  // return written chars without terminating \0
  return (int)idx;
}

///////////////////////////////////////////////////////////////////////////////


// TODO
static int loglevel = 100;
static int do_printf(const char *format, va_list va) {
  bool valid_loglevel = true;

  if (format[0] == '\001') {
    const char *prefix = NULL;
    int val = format[1] - '0';

    if (val <= loglevel) {
      switch (val) {
        case 0:
#ifndef CONFIG_LOG_ERROR
          return 0;
#endif
          prefix = RED "[ERR]";
          break;
        case 1:
#ifndef CONFIG_LOG_WARN
          return 0;
#endif
          prefix = YEL "[WRN]";
          break;
        case 2:
#ifndef CONFIG_LOG_INFO
          return 0;
#endif
          prefix = GRN ">";
          break;
        case 3:
#ifndef CONFIG_LOG_DEBUG
          return 0;
#endif
          prefix = MAG ">";
          break;
      }
      if (prefix != NULL) {
        printf_nolock("%s%s ", prefix, RESET);
      }
    } else {
      valid_loglevel = false;
    }
    format += 2;
  }

  if (!valid_loglevel) {
    return 0;
  }

  char buffer[1];
  return _vsnprintf(_out_char, buffer, (size_t)-1, format, va);
}


void printf_nolock(const char *format, ...) {
  va_list va;
  va_start(va, format);
  do_printf(format, va);
  va_end(va);
}


static spinlock printk_lock;
int printf(const char *format, ...) {
  printk_lock.lock();
  va_list va;
  va_start(va, format);
  const int ret = do_printf(format, va);
  va_end(va);
  printk_lock.unlock();
  return ret;
}

int pprintf(const char *format, ...) {
  printk_lock.lock();
  if (cpu::in_thread()) {
    int color = curthd->tid % 5 + 31;
    printf_nolock("\e[0;%dm(c:%d p:%d t:%d) %s:\e[0m ", color, cpu::current().id, curthd->pid, curthd->tid, curproc->name.get());
  }
  va_list va;
  va_start(va, format);
  const int ret = do_printf(format, va);
  va_end(va);
  printk_lock.unlock();
  return ret;
}


int sprintf(char *buffer, const char *format, ...) {
  va_list va;
  va_start(va, format);
  const int ret = _vsnprintf(_out_buffer, buffer, (size_t)-1, format, va);
  va_end(va);
  return ret;
}

int snprintf(char *buffer, size_t count, const char *format, ...) {
  va_list va;
  va_start(va, format);
  const int ret = _vsnprintf(_out_buffer, buffer, count, format, va);
  va_end(va);
  return ret;
}

int vprintf(const char *format, va_list va) {
  char buffer[1];
  return _vsnprintf(_out_char, buffer, (size_t)-1, format, va);
}

int vsnprintf_(char *buffer, size_t count, const char *format, va_list va) { return _vsnprintf(_out_buffer, buffer, count, format, va); }

int fctprintf(void (*out)(char character, void *arg), void *arg, const char *format, ...) {
  va_list va;
  va_start(va, format);
  const out_fct_wrap_type out_fct_wrap = {out, arg};
  const int ret = _vsnprintf(_out_fct, (char *)(uintptr_t)&out_fct_wrap, (size_t)-1, format, va);
  va_end(va);
  return ret;
}

// typedef void (*out_fct_type)(char character, void* buffer, size_t idx,
// size_t maxlen);

static void string_out_fct(char c, void *buf, size_t idx, size_t maxlen) {
  auto *s = (ck::string *)buf;
  s->push(c);
}

template <>
ck::string ck::basic_string<char>::format(const char *fmt, ...) {
  ck::string dst;

  va_list va;
  va_start(va, fmt);
  _vsnprintf(string_out_fct, (char *)&dst, (size_t)-1, fmt, va);
  va_end(va);

  return dst;
}

static bool isspace(char c) { return c == ' ' || c == '\t' || c == '\n'; }

static int isxdigit(int c) { return (((c >= '0') && (c <= '9')) || ((c >= 'a') && (c <= 'f')) || ((c >= 'A') && (c <= 'F'))); }

static int isdigit(int c) { return ((c >= '0') && (c <= '9')); }
int islower(int c) { return ((c >= 'a') && (c <= 'z')); }
int toupper(int c) {
  if (islower(c)) {
    return c & ~0x20;
  } else {
    return c;
  }
}

/**
 * simple_strtoul - convert a string to an unsigned long
 * @cp: The start of the string
 * @endp: A pointer to the end of the parsed string will be placed here
 * @base: The number base to use
 */
unsigned long simple_strtoul(const char *cp, char **endp, unsigned int base) {
  unsigned long result = 0, value;

  if (!base) {
    base = 10;
    if (*cp == '0') {
      base = 8;
      cp++;
      if ((*cp == 'x') && isxdigit(cp[1])) {
        cp++;
        base = 16;
      }
    }
  }
  while (isxdigit(*cp) && (value = isdigit(*cp) ? *cp - '0' : toupper(*cp) - 'A' + 10) < base) {
    result = result * base + value;
    cp++;
  }
  if (endp) *endp = (char *)cp;
  return result;
}

/**
 * simple_strtol - convert a string to a signed long
 * @cp: The start of the string
 * @endp: A pointer to the end of the parsed string will be placed here
 * @base: The number base to use
 */
long simple_strtol(const char *cp, char **endp, unsigned int base) {
  if (*cp == '-') return -simple_strtoul(cp + 1, endp, base);
  return simple_strtoul(cp, endp, base);
}

/**
 * simple_strtoull - convert a string to an unsigned long long
 * @cp: The start of the string
 * @endp: A pointer to the end of the parsed string will be placed here
 * @base: The number base to use
 */
unsigned long long simple_strtoull(const char *cp, char **endp, unsigned int base) {
  unsigned long long result = 0, value;

  if (!base) {
    base = 10;
    if (*cp == '0') {
      base = 8;
      cp++;
      if ((*cp == 'x') && isxdigit(cp[1])) {
        cp++;
        base = 16;
      }
    }
  }
  while (isxdigit(*cp) && (value = isdigit(*cp) ? *cp - '0' : (islower(*cp) ? toupper(*cp) : *cp) - 'A' + 10) < base) {
    result = result * base + value;
    cp++;
  }
  if (endp) *endp = (char *)cp;
  return result;
}

/**
 * simple_strtoll - convert a string to a signed long long
 * @cp: The start of the string
 * @endp: A pointer to the end of the parsed string will be placed here
 * @base: The number base to use
 */
long long simple_strtoll(const char *cp, char **endp, unsigned int base) {
  if (*cp == '-') return -simple_strtoull(cp + 1, endp, base);
  return simple_strtoull(cp, endp, base);
}

static int skip_atoi(const char **s) {
  int i = 0;

  while (isdigit(**s))
    i = i * 10 + *((*s)++) - '0';
  return i;
}

/**
 * vsscanf - Unformat a buffer into a list of arguments
 * @buf:	input buffer
 * @fmt:	format of buffer
 * @args:	arguments
 */
int vsscanf(const char *buf, const char *fmt, va_list args) {
  const char *str = buf;
  char *next;
  char digit;
  int num = 0;
  int qualifier;
  int base;
  int field_width;
  int is_sign = 0;

  while (*fmt && *str) {
    /* skip any white space in format */
    /* white space in format matchs any amount of
     * white space, including none, in the input.
     */
    if (isspace(*fmt)) {
      while (isspace(*fmt))
        ++fmt;
      while (isspace(*str))
        ++str;
    }

    /* anything that is not a conversion must match exactly */
    if (*fmt != '%' && *fmt) {
      if (*fmt++ != *str++) break;
      continue;
    }

    if (!*fmt) break;
    ++fmt;

    /* skip this conversion.
     * advance both strings to next white space
     */
    if (*fmt == '*') {
      while (!isspace(*fmt) && *fmt)
        fmt++;
      while (!isspace(*str) && *str)
        str++;
      continue;
    }

    /* get field width */
    field_width = -1;
    if (isdigit(*fmt)) field_width = skip_atoi(&fmt);

    /* get conversion qualifier */
    qualifier = -1;
    if (*fmt == 'h' || *fmt == 'l' || *fmt == 'L' || *fmt == 'Z' || *fmt == 'z') {
      qualifier = *fmt;
      fmt++;
    }
    base = 10;
    is_sign = 0;

    if (!*fmt || !*str) break;

    switch (*fmt++) {
      case 'c': {
        char *s = (char *)va_arg(args, char *);
        if (field_width == -1) field_width = 1;
        do {
          *s++ = *str++;
        } while (--field_width > 0 && *str);
        num++;
      }
        continue;
      case 's': {
        char *s = (char *)va_arg(args, char *);
        if (field_width == -1) field_width = sizeof(int);
        /* first, skip leading white space in buffer */
        while (isspace(*str))
          str++;

        /* now copy until next white space */
        while (*str && !isspace(*str) && field_width--) {
          *s++ = *str++;
        }
        *s = '\0';
        num++;
      }
        continue;
      case 'n':
        /* return number of characters read so far */
        {
          int *i = (int *)va_arg(args, int *);
          *i = str - buf;
        }
        continue;
      case 'o':
        base = 8;
        break;
      case 'x':
      case 'X':
        base = 16;
        break;
      case 'i':
        base = 0;
      case 'd':
        is_sign = 1;
      case 'u':
        break;
      case '%':
        /* looking for '%' in str */
        if (*str++ != '%') return num;
        continue;
      default:
        /* invalid format; stop here */
        return num;
    }

    /* have some sort of integer conversion.
     * first, skip white space in buffer.
     */
    while (isspace(*str))
      str++;

    digit = *str;
    if (is_sign && digit == '-') digit = *(str + 1);

    if (!digit || (base == 16 && !isxdigit(digit)) || (base == 10 && !isdigit(digit)) || (base == 8 && (!isdigit(digit) || digit > '7')) ||
        (base == 0 && !isdigit(digit)))
      break;

    switch (qualifier) {
      case 'h':
        if (is_sign) {
          short *s = (short *)va_arg(args, short *);
          *s = (short)simple_strtol(str, &next, base);
        } else {
          unsigned short *s = (unsigned short *)va_arg(args, unsigned short *);
          *s = (unsigned short)simple_strtoul(str, &next, base);
        }
        break;
      case 'l':
        if (is_sign) {
          long *l = (long *)va_arg(args, long *);
          *l = simple_strtol(str, &next, base);
        } else {
          unsigned long *l = (unsigned long *)va_arg(args, unsigned long *);
          *l = simple_strtoul(str, &next, base);
        }
        break;
      case 'L':
        if (is_sign) {
          long long *l = (long long *)va_arg(args, long long *);
          *l = simple_strtoll(str, &next, base);
        } else {
          unsigned long long *l = (unsigned long long *)va_arg(args, unsigned long long *);
          *l = simple_strtoull(str, &next, base);
        }
        break;
      case 'Z':
      case 'z': {
        size_t *s = (size_t *)va_arg(args, size_t *);
        *s = (size_t)simple_strtoul(str, &next, base);
      } break;
      default:
        if (is_sign) {
          int *i = (int *)va_arg(args, int *);
          *i = (int)simple_strtol(str, &next, base);
        } else {
          unsigned int *i = (unsigned int *)va_arg(args, unsigned int *);
          *i = (unsigned int)simple_strtoul(str, &next, base);
        }
        break;
    }
    num++;

    if (!next) break;
    str = next;
  }
  return num;
}

/**
 * sscanf - Unformat a buffer into a list of arguments
 * @buf:	input buffer
 * @fmt:	formatting of buffer
 * @...:	resulting arguments
 */
int sscanf(const char *buf, const char *fmt, ...) {
  va_list args;
  int i;

  va_start(args, fmt);
  i = vsscanf(buf, fmt, args);
  va_end(args);
  return i;
}

template <>
int ck::string::scan(const char *fmt, ...) {
  va_list args;
  int i;

  va_start(args, fmt);
  i = vsscanf(this->get(), fmt, args);
  va_end(args);
  return i;
}




extern "C" int vfctprintf(void (*out)(char character, void *arg), void *arg, const char *format, va_list va);


static inline void scribe_draw_text_callback(char c, void *arg, size_t idx, size_t maxlen) {
  auto &f = *(ck::string *)arg;
  f += c;
}


template <>
void ck::basic_string<char>::appendf(const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  _vsnprintf(scribe_draw_text_callback, (char *)this, (size_t)-1, fmt, va);
  va_end(va);
}
