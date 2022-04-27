#ifndef HEADER_Macros
#define HEADER_Macros

#include <assert.h> // IWYU pragma: keep
#include <math.h> // IWYU pragma: keep

#if !defined(HAVE_ISNAN) && !defined(__SUPPORT_SNAN__)
/* isnan(x) is technically not equivalent to (x != x) as the latter raises
   FE_INVALID on a "signaling" NaN but isnan(x) raise no exceptions on a sNaN.
   (TS 18661-1 / C2X standard)
   htop doesn't use any signaling NaN. */
#define isnan(x)                       ((x) != (x))
#endif

#ifndef HAVE_FP_QUIET_COMPARE

/* These fallback expressions are slower than built-ins from the compiler. */
#ifndef isunordered
#ifdef __SUPPORT_SNAN__
/* isunordered should raise FE_INVALID when either argument is a "signaling"
   NaN. The (a != b) addresses the "isunordered(NAN, SNAN)" case. */
#define isunordered(a, b)              ((a) != (b) && ((a) != (a) || (b) != (b)))
#else
#define isunordered(a, b)              ((a) != (a) || (b) != (b))
#endif
#endif
#ifndef isgreater
#define isgreater(a, b)                (!isunordered(a, b) && (a) > (b))
#endif
#ifndef isgreaterequal
#define isgreaterequal(a, b)           (!isunordered(a, b) && (a) >= (b))
#endif
#ifndef isless
#define isless(a, b)                   isgreater(b, a)
#endif
#ifndef islessequal
#define islessequal(a, b)              isgreaterequal(b, a)
#endif

#endif

#ifndef MINIMUM
#define MINIMUM(a, b)                  ((a) < (b) ? (a) : (b))
#endif

#ifndef MAXIMUM
#define MAXIMUM(a, b)                  ((a) > (b) ? (a) : (b))
#endif

#ifndef CLAMP
#define CLAMP(x, low, high)            (assert((low) <= (high)), ((x) > (high)) ? (high) : MAXIMUM(x, low))
#endif

#ifndef ARRAYSIZE
#define ARRAYSIZE(x)                   (sizeof(x) / sizeof((x)[0]))
#endif

#ifndef SPACESHIP_NUMBER
#define SPACESHIP_NUMBER(a, b)         (((a) > (b)) - ((a) < (b)))
#endif

#ifndef SPACESHIP_NULLSTR
#define SPACESHIP_NULLSTR(a, b)        strcmp((a) ? (a) : "", (b) ? (b) : "")
#endif

#ifndef SPACESHIP_DEFAULTSTR
#define SPACESHIP_DEFAULTSTR(a, b, s)  strcmp((a) ? (a) : (s), (b) ? (b) : (s))
#endif

#ifdef  __GNUC__  // defined by GCC and Clang

#define ATTR_FORMAT(type, index, check) __attribute__((format (type, index, check)))
#define ATTR_NONNULL                    __attribute__((nonnull))
#define ATTR_NORETURN                   __attribute__((noreturn))
#define ATTR_UNUSED                     __attribute__((unused))
#define ATTR_MALLOC                     __attribute__((malloc))

#else /* __GNUC__ */

#define ATTR_FORMAT(type, index, check)
#define ATTR_NONNULL
#define ATTR_NORETURN
#define ATTR_UNUSED
#define ATTR_MALLOC

#endif /* __GNUC__ */

#ifdef HAVE_ATTR_ALLOC_SIZE

#define ATTR_ALLOC_SIZE1(a)             __attribute__((alloc_size (a)))
#define ATTR_ALLOC_SIZE2(a, b)          __attribute__((alloc_size (a, b)))

#else

#define ATTR_ALLOC_SIZE1(a)
#define ATTR_ALLOC_SIZE2(a, b)

#endif /* HAVE_ATTR_ALLOC_SIZE */

// ignore casts discarding const specifier, e.g.
//     const char []     ->  char * / void *
//     const char *[2]'  ->  char *const *
#if defined(__clang__)
#define IGNORE_WCASTQUAL_BEGIN  _Pragma("clang diagnostic push") \
                                _Pragma("clang diagnostic ignored \"-Wcast-qual\"")
#define IGNORE_WCASTQUAL_END    _Pragma("clang diagnostic pop")
#elif defined(__GNUC__)
#define IGNORE_WCASTQUAL_BEGIN  _Pragma("GCC diagnostic push") \
                                _Pragma("GCC diagnostic ignored \"-Wcast-qual\"")
#define IGNORE_WCASTQUAL_END    _Pragma("GCC diagnostic pop")
#else
#define IGNORE_WCASTQUAL_BEGIN
#define IGNORE_WCASTQUAL_END
#endif

/* This subtraction is used by Linux / NetBSD / OpenBSD for calculation of CPU usage items. */
static inline unsigned long long saturatingSub(unsigned long long a, unsigned long long b) {
   return a > b ? a - b : 0;
}

#endif
