/* { dg-require-effective-target vect_condition } */
/* { dg-require-effective-target vect_float } */

#include "tree-vect.h"

extern void abort (void) __attribute__ ((noreturn));

#define N 27

/* Condition reduction with different types.  */

int
condition_reduction (float *a, float min_v)
{
  int last = 0;

  for (int i = 0; i < N; i++)
    if (a[i] < min_v)
      last = i;

  return last;
}

int
main (void)
{
  float a[N] = {
  11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
  1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
  21, 22, 23, 24, 25, 26, 27
  };

  check_vect ();

  int ret = condition_reduction (a, 10);
  if (ret != 18)
    abort ();

  return 0;
}

/* { dg-final { scan-tree-dump-times "LOOP VECTORIZED" 2 "vect" } } */
/* { dg-final { scan-tree-dump-times "optimizing condition reduction with FOLD_EXTRACT_LAST" 4 "vect" { target vect_fold_extract_last } } } */
/* { dg-final { scan-tree-dump-times "condition expression based on integer induction." 2 "vect" { target { ! vect_fold_extract_last } } } } */

