/* PR tree-optimization/83369 - Missing diagnostics during inlining
   { dg-do compile }
   { dg-options "-O2 -Wall" } */

#ifndef __APPLE__
#include <string.h>
#else
/* OSX headers do not mark up the nonnull elements yet.  */
# include <stddef.h>
extern size_t strlen (const char *__s)
		      __attribute ((pure)) __attribute ((nonnull (1)));
extern void *memcpy (void *__restrict __dest, const void *__restrict __src,
		     size_t __n) __attribute ((nonnull (1, 2)));
#endif

char buf[100];

struct Test
{
  const char* s1;
  const char* s2;
};

__attribute ((nonnull (1, 2)))
inline char*
my_strcpy (char *restrict dst, const char *restrict src, size_t size)
{
  size_t len = strlen (src);        /* { dg-warning "argument 1 null where non-null expected" } */
  if (len < size)
    memcpy (dst, src, len + 1);     /* { dg-warning "argument 2 null where non-null expected" } */
  else
    {
      memcpy (dst, src, size - 1);  /* { dg-warning "argument 2 null where non-null expected" } */
      dst[size - 1] = '\0';
    }
  return dst;
}

void test (struct Test* test)
{
  if (test->s1)
    my_strcpy (buf, test->s1, sizeof buf);
  else if (test->s2)
    my_strcpy (buf, test->s2, sizeof buf);
  else
    my_strcpy (buf, test->s2, sizeof buf);
}

/* Verify that the inlining context is printed for -Wnonnull:
   { dg-message "function .my_strcpy..*inlined from .test." "" { target *-*-* } 0 }  */
