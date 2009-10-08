/*
 * mtx.c
 * Copyright (C) 2007, Tomasz Koziara (t.koziara AT gmail.com)
 * --------------------------------------------------------------
 * general sparse or dense matrix
 */

/* This file is part of Solfec.
 * Solfec is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * Solfec is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Solfec. If not, see <http://www.gnu.org/licenses/>. */

#include <stdlib.h>
#include <string.h>
#include "lap.h"
#include "bla.h"
#include "mem.h"
#include "alg.h"
#include "err.h"
#include "mtx.h"
#include "ext/csparse.h"

/* macros */
#define KIND(a) ((a)->kind)
#define MXTRANS(a) ((a)->flags & MXTRANS)
#define MXSTATIC(a) ((a)->flags & MXSTATIC)
#define MXDSUBLK(a) ((a)->flags & MXDSUBLK)
#define TEMPORARY(a) (MXTRANS(a)||MXDSUBLK(a))
#define MXIFAC(a) ((a)->flags & MXIFAC)
#define MXIFAC_WORK1(a) ((a)->x + (a)->nzmax)
#define MXIFAC_WORK2(a) ((a)->x + (a)->nzmax + (a)->n)

/* types */
typedef int (*qcmp_t) (const void*, const void*);

/* structures */
struct eigpair
{
  double value,
        *vector;
  int shift,
      length;
};

/* qsort compatible comparison */
static int eigpaircmp (struct eigpair *a, struct eigpair *b)
{
  if (a->value < b->value) return -1;
  else return 0;
}

/* copy transpose(a) into 'b' */
static void dense_transpose_copy (MX *a, MX *b)
{
  double *ax = a->x,
	 *bx = b->x;
  int m = a->m,
      n = a->n,
      i, j;

  for (i = 0; i < m; i ++)
    for (j = 0; j < n; j ++, bx ++)
      *bx = ax [i+j*m];
}

/* copy transpose(a) into 'b' */
static void bd_transpose_copy (MX *a, MX *b)
{
  double *ax, *bx;
  int n = a->n,
     *p = a->p,
     *i = a->i,
      j, k, l, m;

  for (j = 0, m = i[j+1]-i[j]; j < n; j ++) /* loop over blocks of size 'm' */
    for (k = 0, ax = &a->x[p[j]], bx = &b->x[p[j]]; k < m; k ++) /* loop over rows of 'j'th block */
      for (l = 0; l < m; l ++, bx ++) /* loop over columns of 'j'th block */
	*bx = ax [k+l*m];
}

/* copy transpose(a) into 'b' */
#define csc_transpose_copy(a, b) ERRMEM (cs_transpose_ext (a, b)) /* workspace memory error possible */

/* compare structures of diagonal block matrices */
inline static int bdseq (int n, int *pa, int *ia, int *pb, int *ib)
{
  for (; n >= 0; n --)
    if (pa[n] != pb[n] ||
	ia[n] != ib[n]) return 0;

  return 1;
}

/* prepare the structure of 'b' according to the specification */
static int prepare (MX *b, unsigned short kind, int nzmax, int m, int n, int *p, int *i)
{
  if (KIND (b) == kind)
  {
    switch (kind)
    {
      case MXDENSE:
#if DEBUG
	if (b->nzmax >= nzmax) /* dense matrix can be downscaled and resized */
	{
#endif
	  b->nzmax = nzmax;
	  b->m = m;
	  b->n = n;
#if DEBUG
	}
	else return 0;
#endif
      break;
#if DEBUG  /* block diagonal tests are performed only in debug mode */
      case MXBD:
	if (b->nzmax == nzmax &&
	    b->m == m &&
	    b->n == n) /* structure of diagonal block matrix must much exactly */
	{ if (! bdseq (n, b->p, b->i, p, i)) return 0; }
      break;
#endif
      case MXCSC: /* compressed column matrix can change in an arbitrary way */
	b->nzmax = nzmax;
	b->m = m;
	b->n = n;
	ERRMEM (b->p = realloc (b->p, sizeof (int) * (n+1)));
	ERRMEM (b->i = realloc (b->i, sizeof (int) * nzmax));
	ERRMEM (b->x = realloc (b->x, sizeof (double) * nzmax));
	ASSERT_DEBUG (p && i, "No structure pointers passed for MXCSC");
	memcpy (b->p, p, sizeof (int) * (n+1)); 
	memcpy (b->i, i, sizeof (int) * nzmax); 
	if (b->sym) cs_sfree (b->sym); /* get rid of previous factorisations */
	if (b->num) cs_nfree (b->num);
	b->sym = b->num = NULL;
      break;
    }
  }
  else return 0; /* matrix kind cannot be changed */

  return 1;
}

/* sparse to dense conversion */
static MX* csc_to_dense (MX *a)
{
  int *p, *i, k, l, n, m;
  double *ax, *bx;
  MX *b;

  ASSERT_DEBUG (a->kind == MXCSC, "Invalid matrix kind");

  b = MX_Create (MXDENSE, a->m, a->n, NULL, NULL);
  b->flags = a->flags;

  ax = a->x;
  bx = b->x;
  p = a->p;
  i = a->i;
  m = a->m;
  n = a->n;

  for (k = 0; k < n; k ++) for (l = p[k]; l < p[k+1]; l ++, ax ++) bx [m*k + i[l]] = *ax;

  return b;
}

/* add two dense matrices */
static MX* add_dense_dense (double alpha, MX *a, double beta, MX *b, MX *c)
{
  switch (MXTRANS(a)<<4|MXTRANS(b))
  {
    case 0x00:
    {
      ASSERT_DEBUG (a->m == b->m && a->n == b->n, "Incompatible dimensions");
      if (!c) c = MX_Create (MXDENSE, a->m, a->n, NULL, NULL);
      else { ASSERT_DEBUG_EXT (prepare (c, MXDENSE, a->nzmax, a->m, a->n, NULL, NULL), "Invalid output matrix"); }

      for (double *ax = a->x, *bx = b->x, *cx = c->x, *ay = (ax+a->nzmax);
	ax < ay; ax ++, bx ++, cx ++) (*cx) = alpha * (*ax) + beta * (*bx);
    }
    break;
    case 0x10:
    {
      ASSERT_DEBUG (a->m == b->n && a->n == b->m, "Incompatible dimensions");
      if (!c) c = MX_Create (MXDENSE, b->m, b->n, NULL, NULL);
      else { ASSERT_DEBUG_EXT (prepare (c, MXDENSE, b->nzmax, b->m, b->n, NULL, NULL), "Invalid output matrix"); }
      ASSERT_DEBUG (a != c, "Matrices 'a' and 'c' cannot be the same");

      double *ax = a->x,
	     *bx = b->x,
	     *cx = c->x;

      for (int j = 0, m = a->m, n = a->n; j < m; j ++)
	for (int i = 0; i < n; i ++, bx ++, cx ++)
	  (*cx) = alpha * ax [m*i+j] + beta * (*bx);
    }
    break;
    case 0x01:
    {
      ASSERT_DEBUG (a->m == b->n && a->n == b->m, "Incompatible dimensions");
      if (!c) c = MX_Create (MXDENSE, a->m, a->n, NULL, NULL);
      else { ASSERT_DEBUG_EXT (prepare (c, MXDENSE, a->nzmax, a->m, a->n, NULL, NULL), "Invalid output matrix"); }
      ASSERT_DEBUG (b != c, "Matrices 'b' and 'c' cannot be the same");

      double *ax = a->x,
	     *bx = b->x,
	     *cx = c->x;

      for (int j = 0, m = b->m, n = b->n; j < m; j ++)
	for (int i = 0; i < n; i ++, ax ++, cx ++)
	  (*cx) = alpha * (*ax) + beta * bx [m*i+j];
    }
    break;
    case 0x11:
    {
      ASSERT_DEBUG (a->m == b->n && a->n == b->m, "Incompatible dimensions");
      if (!c) c = MX_Create (MXDENSE, a->n, a->m, NULL, NULL);
      else { ASSERT_DEBUG_EXT (prepare (c, MXDENSE, a->nzmax, a->n, a->m, NULL, NULL), "Invalid output matrix"); }
      ASSERT_DEBUG (c != a && c != b, "Matrix 'c' cannot be the same as 'a' or 'b'");

      double *ax = a->x,
	     *bx = b->x,
	     *cx = c->x;

      for (int j = 0, m = c->m, n = c->n; j < m; j ++)
	for (int i = 0; i < n; i ++, ax ++, bx ++)
	  cx [m*i+j] = alpha * (*ax) + beta * (*bx);
    }
    break;
  }

  return c;
}

/* add two diagonal block matrices */
static MX* add_bd_bd (double alpha, MX *a, double beta, MX *b, MX *c)
{
  switch (MXTRANS(a)<<4|MXTRANS(b))
  {
    case 0x00:
    {
      ASSERT_DEBUG (a->m == b->m && a->n == b->n, "Incompatible dimensions");
      ASSERT_DEBUG (bdseq (a->n, a->p, a->i, b->p, b->i), "Incompatible dimensions");
      if (!c) c = MX_Create (MXBD, a->m, a->n, a->p, a->i);
      else { ASSERT_DEBUG_EXT (prepare (c, MXBD, a->nzmax, a->m, a->n, a->p, a->i), "Invalid output matrix"); }

      for (double *ax = a->x, *bx = b->x, *cx = c->x, *ay = (ax+a->nzmax);
	ax < ay; ax ++, bx ++, cx ++) (*cx) = alpha * (*ax) + beta * (*bx);
    }
    break;
    case 0x10:
    {
      ASSERT_DEBUG (a->m == b->n && a->n == b->m, "Incompatible dimensions");
      ASSERT_DEBUG (bdseq (a->n, a->p, a->i, b->p, b->i), "Incompatible dimensions");
      if (!c) c = MX_Create (MXBD, b->m, b->n, b->p, b->i);
      else { ASSERT_DEBUG_EXT (prepare (c, MXBD, b->nzmax, b->m, b->n, b->p, b->i), "Invalid output matrix"); }
      ASSERT_DEBUG (a != c, "Matrices 'a' and 'c' cannot be the same");

      double *ax = a->x, *xx = ax,
	     *bx = b->x,
	     *cx = c->x;
      int *pp = a->p,
	  *ii = a->i;

      for (int k = 0, n = a->n; k < n; k ++, ax = &xx[pp[k]])
	for (int j = 0, m = ii[k+1]-ii[k]; j < m; j ++)
	  for (int i = 0; i < m; i ++, bx ++, cx ++)
	    (*cx) = alpha * ax [m*i+j] + beta * (*bx);
    }
    break;
    case 0x01:
    {
      ASSERT_DEBUG (a->m == b->n && a->n == b->m, "Incompatible dimensions");
      ASSERT_DEBUG (bdseq (a->n, a->p, a->i, b->p, b->i), "Incompatible dimensions");
      if (!c) c = MX_Create (MXBD, a->m, a->n, a->p, a->i);
      else { ASSERT_DEBUG_EXT (prepare (c, MXBD, a->nzmax, a->m, a->n, a->p, a->i), "Invalid output matrix"); }
      ASSERT_DEBUG (b != c, "Matrices 'b' and 'c' cannot be the same");

      double *ax = a->x,
	     *bx = b->x, *xx = bx,
	     *cx = c->x;
      int *pp = b->p,
	  *ii = b->i;

      for (int k = 0, n = b->n; k < n; k ++, bx = &xx[pp[k]])
	for (int j = 0, m = ii[k+1]-ii[k]; j < m; j ++)
	  for (int i = 0; i < m; i ++, ax ++, cx ++)
	    (*cx) = alpha * (*ax) + beta * bx [m*i+j];
    }
    break;
    case 0x11:
    {
      ASSERT_DEBUG (a->m == b->n && a->n == b->m, "Incompatible dimensions");
      ASSERT_DEBUG (bdseq (a->n, a->p, a->i, b->p, b->i), "Incompatible dimensions");
      if (!c) c = MX_Create (MXBD, a->m, a->n, a->p, a->i);
      else { ASSERT_DEBUG_EXT (prepare (c, MXBD, a->nzmax, a->m, a->n, a->p, a->i), "Invalid output matrix"); }
      ASSERT_DEBUG (c != a && c != b, "Matrix 'c' cannot be the same as 'a' or 'b'");

      double *ax = a->x,
	     *bx = b->x,
	     *cx = c->x, *xx = cx;
      int *pp = c->p,
	  *ii = c->i;

      for (int k = 0, n = c->n; k < n; k ++, cx = &xx[pp[k]])
	for (int j = 0, m = ii[k+1]-ii[k]; j < m; j ++)
	  for (int i = 0; i < m; i ++, ax ++, bx ++)
	    cx [m*i+j] = alpha * (*ax) + beta * (*bx);
    }
    break;
  }

  return c;
}

/* add two sparse matrices */
static MX* add_csc_csc (double alpha, MX *a, double beta, MX *b, MX *c)
{
  MX *A, *B, *d;

  ASSERT (!(MXIFAC (a) || MXIFAC (b)), ERR_MTX_IFAC); /* adding factorized inverses is invalid */

  switch (MXTRANS(a)<<4|MXTRANS(b))
  {
    case 0x00:
    {
      ASSERT_DEBUG (a->m == b->m && a->n == b->n, "Incompatible dimensions");
      A = a;
      B = b;
    }
    break;
    case 0x10:
    {
      ASSERT_DEBUG (a->m == b->n && a->n == b->m, "Incompatible dimensions");
      A = cs_transpose (a, 1);
      B = b;
    }
    break;
    case 0x01:
    {
      ASSERT_DEBUG (a->m == b->n && a->n == b->m, "Incompatible dimensions");
      A = a;
      B = cs_transpose (b, 1);
    }
    break;
    case 0x11:
    {
      ASSERT_DEBUG (a->m == b->n && a->n == b->m, "Incompatible dimensions");
      A = cs_transpose (a, 1);
      B = cs_transpose (b, 1);
    }
    break;
  }

  ERRMEM (d = cs_add (A, B, alpha, beta));

  d->kind = MXCSC;

  if (c)
  {
    free (c->p);
    free (c->i);
    free (c->x);
   
    *c  = *d;

    free (d);
  }
  else c = d;

  if (A != a) cs_spfree (A);
  if (B != b) cs_spfree (B);

  return c;
}

/* add dense and diagonal block matrices */
static MX* add_dense_bd (double alpha, MX *a, double beta, MX *b, MX *c)
{
  switch (MXTRANS(a)<<4|MXTRANS(b))
  {
    case 0x00:
    {
      ASSERT_DEBUG (a->m == b->m && a->n == b->m, "Incompatible dimensions");
      if (!c) c = MX_Create (MXDENSE, a->m, a->n, NULL, NULL);
      else { ASSERT_DEBUG_EXT (prepare (c, MXDENSE, a->nzmax, a->m, a->n, NULL, NULL), "Invalid output matrix"); }

      double *ax = a->x, *ay = ax + a->nzmax,
	     *bx = b->x, *xx = bx,
	     *cx = c->x;
      int *pp = b->p,
	  *ii = b->i,
	  m = b->m,
	  n = b->n, k;

      for (; ax < ay; ax ++, cx ++) /* c = alpha * a */
	(*cx) = alpha * (*ax);

      for (k = 0, cx = c->x; k < n; k ++, bx = &xx[pp[k]]) /* c += beta * b */
	for (int j = ii[k], mm = ii[k+1]; j < mm; j ++)
	  for (int i = ii[k]; i < mm; i ++, bx ++)
	    cx [i+j*m] += beta * (*bx);
    }
    break;
    case 0x10:
    {
      ASSERT_DEBUG (a->m == b->m && a->n == b->m, "Incompatible dimensions");
      if (!c) c = MX_Create (MXDENSE, a->m, a->n, NULL, NULL);
      else { ASSERT_DEBUG_EXT (prepare (c, MXDENSE, a->nzmax, a->m, a->n, NULL, NULL), "Invalid output matrix"); }
      ASSERT_DEBUG (a != c, "Matrices 'a' and 'c' cannot be the same");

      double *ax = a->x,
	     *bx = b->x, *xx = bx,
	     *cx = c->x;
      int *pp = b->p,
	  *ii = b->i,
	  m = b->m,
	  n = b->n, k;

      for (int j = 0; j < m; j ++)
	for (int i = 0; i < m; i ++, cx ++) /* c = alpha * trans (a) */
	  (*cx) = alpha * ax [m*i+j];

      for (k = 0, cx = c->x; k < n; k ++, bx = &xx[pp[k]]) /* c += beta * b */
	for (int j = ii[k], mm = ii[k+1]; j < mm; j ++)
	  for (int i = ii[k]; i < mm; i ++, bx ++)
	    cx [i+j*m] += beta * (*bx);
    }
    break;
    case 0x01:
    {
      ASSERT_DEBUG (a->m == b->m && a->n == b->m, "Incompatible dimensions");
      if (!c) c = MX_Create (MXDENSE, a->m, a->n, NULL, NULL);
      else { ASSERT_DEBUG_EXT (prepare (c, MXDENSE, a->nzmax, a->m, a->n, NULL, NULL), "Invalid output matrix"); }
      ASSERT_DEBUG (b != c, "Matrices 'b' and 'c' cannot be the same");

      double *ax = a->x, *ay = ax + a->nzmax,
	     *bx = b->x, *xx = bx,
	     *cx = c->x;
      int *pp = b->p,
	  *ii = b->i,
	  m = b->m,
	  n = b->n, k;

      for (; ax < ay; ax ++, cx ++) /* c = alpha * a */
	(*cx) = alpha * (*ax);

      for (k = 0, cx = c->x; k < n; k ++, bx = &xx[pp[k]]) /* trans (c) += beta * b */
	for (int j = ii[k], mm = ii[k+1]; j < mm; j ++)
	  for (int i = ii[k]; i < mm; i ++, bx ++)
	    cx [m*i+j] += beta * (*bx);
    }
    break;
    case 0x11:
    {
      ASSERT_DEBUG (a->m == b->m && a->n == b->m, "Incompatible dimensions");
      if (!c) c = MX_Create (MXDENSE, a->m, a->n, NULL, NULL);
      else { ASSERT_DEBUG_EXT (prepare (c, MXDENSE, a->nzmax, a->m, a->n, NULL, NULL), "Invalid output matrix"); }
      ASSERT_DEBUG (c != a && c != b, "Matrix 'c' cannot be the same as 'a' or 'b'");

      double *ax = a->x,
	     *bx = b->x, *xx = bx,
	     *cx = c->x;
      int *pp = b->p,
	  *ii = b->i,
	  m = b->m,
	  n = b->n, k;

      for (int j = 0; j < m; j ++)
	for (int i = 0; i < m; i ++, cx ++) /* c = alpha * trans (a) */
	  (*cx) = alpha * ax [m*i+j];

      for (k = 0, cx = c->x; k < n; k ++, bx = &xx[pp[k]]) /* trans (c) += beta * b */
	for (int j = ii[k], mm = ii[k+1]; j < mm; j ++)
	  for (int i = ii[k]; i < mm; i ++, bx ++)
	    cx [m*i+j] += beta * (*bx);
    }
    break;
  }

  return c;
}

/* add dense and sparse matrices */
static MX* add_dense_csc (double alpha, MX *a, double beta, MX *b, MX *c)
{
  MX *B;

  ASSERT (!MXIFAC (b), ERR_MTX_IFAC); /* adding factorized inverses is invalid */

  B = csc_to_dense (b);
  c = add_dense_dense (alpha, a, beta, B, c);
  MX_Destroy (B);

  return c;
}

/* add sparse and diagonal block matrices */
static MX* add_csc_bd (double alpha, MX *a, double beta, MX *b, MX *c)
{
  MX *A;

  ASSERT (!MXIFAC (a), ERR_MTX_IFAC); /* adding factorized inverses is invalid */
  
  A = csc_to_dense (a);
  c = add_dense_bd (alpha, A, beta, b, c);
  MX_Destroy (A);

  return c;
}

/* multiply two dense matrices */
static MX* matmat_dense_dense (double alpha, MX *a, MX *b, double beta, MX *c)
{
  int m, n, k, l;
  char transa,
       transb;

  if (MXTRANS (a))
  {
    m = a->n;
    k = a->m;
    transa = 'T';
  }
  else
  {
    m = a->m;
    k = a->n;
    transa = 'N';
  }

  if (MXTRANS (b))
  {
    n = b->m;
    l = b->n;
    transb = 'T';
  }
  else
  {
    n = b->n;
    l = b->m;
    transb = 'N';
  }

  ASSERT_DEBUG (k == l, "Incompatible dimensions");
  if (c == NULL) c = MX_Create (MXDENSE, m, n, NULL, NULL);
  else { ASSERT_DEBUG_EXT (prepare (c, MXDENSE, m*n, m, n, NULL, NULL), "Invalid output matrix"); }
  ASSERT_DEBUG (a != b && b != c && c != a, "Matrices 'a','b','c' must be different");
  blas_dgemm (transa, transb, m, n, k, alpha, a->x, a->m, b->x, b->m, beta, c->x, c->m);
  
  return c;
}

/* multiply two diagonal block matrices */
static MX* matmat_bd_bd (double alpha, MX *a, MX *b, double beta, MX *c)
{
  char transa,
       transb;

  if (MXTRANS (a)) transa = 'T';
  else transa = 'N';

  if (MXTRANS (b)) transb = 'T';
  else transb = 'N';

  ASSERT_DEBUG (a->m == b->m && a->n == b->n, "Incompatible dimensions");
  ASSERT_DEBUG (bdseq (a->n, a->p, a->i, b->p, b->i), "Incompatible dimensions");
  if (c == NULL) c = MX_Create (MXBD, a->m, a->n, a->p, a->i);
  else { ASSERT_DEBUG_EXT (prepare (c, MXBD, a->nzmax, a->m, a->n, a->p, a->i), "Invalid output matrix"); }
  ASSERT_DEBUG (a != b && b != c && c != a, "Matrices 'a','b','c' must be different");

  double *ax = a->x,
	 *bx = b->x,
	 *cx = c->x;
  int *pp = a->p,
      *ii = a->i,
      n = a->n, k, m;
 
  for (k = 0; k < n; k ++)
  {
    m = ii[k+1] - ii[k];
    blas_dgemm (transa, transb, m, m, m,
      alpha, &ax [pp[k]], m, &bx [pp[k]],
      m, beta, &cx [pp[k]], m);
  }

  return c;
}

/* c = inv (a) * b */
static void inv_vec (MX *a, double *b, double *c)
{
  double *x = MXIFAC_WORK1(a);
  css *S = a->sym;
  csn *N = a->num;

  cs_ipvec (N->pinv, b, x, a->n);  /* x = permute (b) */
  cs_lsolve (N->L, x);             /* x = L \ x */
  cs_usolve (N->U, x);             /* x = U \ x */
  cs_ipvec (S->q, x, c, a->n);     /* c = inv-permute (x) */
}

/* c = b * inv (a) */
static void vec_inv (MX *a, double *b, double *c)
{
  double *x = MXIFAC_WORK1(a);
  css *S = a->sym;
  csn *N = a->num;

  cs_ipvec (S->q, b, x, a->n);     /* x = inv-permute (b) */
  cs_utsolve (N->U, x);            /* x = U' \ x */
  cs_ltsolve (N->L, x);            /* x = L' \ x */
  cs_ipvec (N->pinv, x, c, a->n);  /* c = permute (x) */
}

/* return w = a (:,j) */
static double* col (MX *a, int j, double *w)
{
  //TODO: mind transpose for dense and block diagonal
  return w;
}

/* a (i,:) = v where 'a' is dense */
static void putrow (MX *a, int i, double *v)
{
  //TODO
}

/* general 'sparse inverse' * B or B * 'sparse inverse' */
static MX* matmat_inv_general (int reverse, double alpha, MX *a, MX *b, double beta, MX *c)
{
  MX *A, *B, *d;
  double *w, *v;
  int i, m;

  ASSERT_DEBUG (a != b && b != c && c != a, "Matrices 'a','b','c' must be different");

  m = reverse ? b->n : a ->n;
  ERRMEM (w = malloc (sizeof (double [m * 2])));
  v = w + m;

  if (reverse)
  {
    switch (MXTRANS(a)<<4|MXTRANS(b))
    {
      case 0x00:
      {
	ASSERT_DEBUG (a->n == b->m, "Incompatible dimensions");
	d = MX_Create (MXDENSE, a->m, b->n, NULL, NULL);
	A = a;
	B = b;
	for (i = 0; i < A->m; i ++) vec_inv (B, col (A, i, w), v), putrow (d, i, v);
      }
      break;
      case 0x10:
      {
	ASSERT_DEBUG (a->m == b->m, "Incompatible dimensions");
	d = MX_Create (MXDENSE, a->n, b->n, NULL, NULL);
	A = a->kind == MXCSC ? cs_transpose (a, 1) : a;
	B = b;
	for (i = 0; i < A->m; i ++) vec_inv (B, col (A, i, w), v), putrow (d, i, v);
      }
      break;
      case 0x01:
      {
	ASSERT_DEBUG (a->n == b->n, "Incompatible dimensions");
	d = MX_Create (MXDENSE, a->m, b->n, NULL, NULL);
	A = a;
	B = b;
	for (i = 0; i < A->m; i ++) inv_vec (B, col (A, i, w), v), putrow (d, i, v);
      }
      break;
      case 0x11:
      {
	ASSERT_DEBUG (a->m == b->n, "Incompatible dimensions");
	d = MX_Create (MXDENSE, a->n, b->m, NULL, NULL);
	A = a->kind == MXCSC ? cs_transpose (a, 1) : a;
	B = b;
	for (i = 0; i < A->m; i ++) inv_vec (B, col (A, i, w), v), putrow (d, i, v);
      }
      break;
    }
  }
  else
  {
    switch (MXTRANS(a)<<4|MXTRANS(b))
    {
      case 0x00:
      {
	ASSERT_DEBUG (a->n == b->m, "Incompatible dimensions");
	d = MX_Create (MXDENSE, a->m, b->n, NULL, NULL);
	A = a;
	B = b;
	for (i = 0; i < B->n; i ++) inv_vec (A, col (B, i, w), col (d, i, NULL));
      }
      break;
      case 0x10:
      {
	ASSERT_DEBUG (a->m == b->m, "Incompatible dimensions");
	d = MX_Create (MXDENSE, a->n, b->n, NULL, NULL);
	A = a;
	B = b;
	for (i = 0; i < B->n; i ++) vec_inv (A, col (B, i, w), col (d, i, NULL));
      }
      break;
      case 0x01:
      {
	ASSERT_DEBUG (a->n == b->n, "Incompatible dimensions");
	d = MX_Create (MXDENSE, a->m, B->n, NULL, NULL);
	A = a;
	B = b->kind == MXCSC ? cs_transpose (b, 1) : b;
	for (i = 0; i < B->n; i ++) inv_vec (A, col (B, i, w), col (d, i, NULL));
      }
      break;
      case 0x11:
      {
	ASSERT_DEBUG (a->m == b->n, "Incompatible dimensions");
	d = MX_Create (MXDENSE, a->n, B->n, NULL, NULL);
	A = a;
	B = b->kind == MXCSC ? cs_transpose (b, 1) : b;
	for (i = 0; i < B->n; i ++) vec_inv (A, col (B, i, w), col (d, i, NULL));
      }
      break;
    }
  }

  if (c)
  {
    MX *C;

    C = MX_Copy (c, NULL);

    MX_Add (alpha, d, beta, C, c);

    MX_Destroy (C);
    MX_Destroy (d);
  }
  else
  {
    if (alpha != 1.0) MX_Scale (d, alpha);
    c = d;
  }

  if (A != a) cs_spfree (A);
  if (B != b) cs_spfree (B);

  free (w);

  return c;
}

/* inverse sparse times sparse */
#define matmat_inv_csc(alpha, a, b, beta, c)  matmat_inv_general (0, alpha, a, b, beta, c)
 
/* sparse times inverse sparse */
#define matmat_csc_inv(alpha, a, b, beta, c)  matmat_inv_general (1, alpha, a, b, beta, c) 

/* inverse sparse times dense */
#define matmat_inv_dense(alpha, a, b, beta, c) matmat_inv_general (0, alpha, a, b, beta, c)

/* dense times inverse sparse */
#define matmat_dense_inv(alpha, a, b, beta, c) matmat_inv_general (1, alpha, a, b, beta, c) 

/* inverse sparse times block diagonal */
#define matmat_inv_bd(alpha, a, b, beta, c) matmat_inv_general (0, alpha, a, b, beta, c) 

/* block diagonal times inverse sparse */
#define matmat_bd_inv(alpha, a, b, beta, c) matmat_inv_general (1, alpha, a, b, beta, c) 

/* multiply two sparse matrices */
static MX* matmat_csc_csc (double alpha, MX *a, MX *b, double beta, MX *c) 
{
  MX *A, *B, *d;

  if (MXIFAC (a)) /* inv (a) * b */
  {
    ASSERT (!MXIFAC (b), ERR_MTX_IFAC); /* multiplying two factorized inverses is invalid */

    return matmat_inv_csc (alpha, a, b, beta, c);
  }
  else if (MXIFAC (b)) /* a * inv (b) */
  {
    return matmat_csc_inv (alpha, a, b, beta, c);
  }
  
  ASSERT_DEBUG (a != b && b != c && c != a, "Matrices 'a','b','c' must be different");

  switch (MXTRANS(a)<<4|MXTRANS(b))
  {
    case 0x00:
    {
      ASSERT_DEBUG (a->n == b->m, "Incompatible dimensions");
      A = a;
      B = b;
    }
    break;
    case 0x10:
    {
      ASSERT_DEBUG (a->m == b->m, "Incompatible dimensions");
      A = cs_transpose (a, 1);
      B = b;
    }
    break;
    case 0x01:
    {
      ASSERT_DEBUG (a->n == b->n, "Incompatible dimensions");
      A = a;
      B = cs_transpose (b, 1);
    }
    break;
    case 0x11:
    {
      ASSERT_DEBUG (a->m == b->n, "Incompatible dimensions");
      A = cs_transpose (a, 1);
      B = cs_transpose (b, 1);
    }
    break;
  }

  ERRMEM (d = cs_multiply (A, B));

  d->kind = MXCSC;

  if (c)
  {
    MX *e;

    ASSERT_DEBUG (c->kind == MXCSC, "Invalid output matrix kind");

    e = add_csc_csc (alpha, d, beta, c, NULL);

    free (c->p);
    free (c->i);
    free (c->x);
   
    *c  = *e;

    free (e);
    cs_spfree (d);
  }
  else
  {
    if (alpha != 1.0) MX_Scale (d, alpha);
    c = d;
  }

  if (A != a) cs_spfree (A);
  if (B != b) cs_spfree (B);

  return c;
}

/* multiply diagonal block and dense matrices */
static MX* matmat_bd_dense (double alpha, MX *a, MX *b, double beta, MX *c)
{
  int m, n, k, l;
  char transa,
       transb;

  m = k = a->m;
  if (MXTRANS (a)) transa = 'T';
  else transa = 'N';

  if (MXTRANS (b))
  {
    n = b->m;
    l = b->n;
    transb = 'T';
  }
  else
  {
    n = b->n;
    l = b->m;
    transb = 'N';
  }

  ASSERT_DEBUG (k == l, "Incompatible dimensions");
  if (c == NULL) c = MX_Create (MXDENSE, m, n, NULL, NULL);
  else { ASSERT_DEBUG_EXT (prepare (c, MXDENSE, m*n, m, n, NULL, NULL), "Invalid output matrix"); }
  ASSERT_DEBUG (a != b && b != c && c != a, "Matrices 'a','b','c' must be different");
 
  double *ax = a->x,
	 *bx = b->x,
	 *cx = c->x,
	 *bb;
  int *pp = a->p,
      *ii = a->i,
      j, o;

  n = a->n;
  m = b->m;
  o = c->m;
 
  for (k = 0; k < n; k ++)
  {
    j = ii[k];
    l = ii[k+1] - j;

    if (transb == 'N') bb = &bx[j];
    else bb = &bx[j*m];

    blas_dgemm (transa, transb, l, m, l,
      alpha, &ax[pp[k]], l, bb, m, beta, &cx[j], o);
  }

  return c;
}

/* multiply dense and diagonal block matrices */
static MX* matmat_dense_bd (double alpha, MX *a, MX *b, double beta, MX *c)
{
  int m, n, k, l;
  char transa,
       transb;

  if (MXTRANS (a))
  {
    m = a->n;
    k = a->m;
    transa = 'T';
  }
  else
  {
    m = a->m;
    k = a->n;
    transa = 'N';
  }

  l = n = b->m;
  if (MXTRANS (b)) transb = 'T';
  else transb = 'N';

  ASSERT_DEBUG (k == l, "Incompatible dimensions");
  if (c == NULL) c = MX_Create (MXDENSE, m, n, NULL, NULL);
  else { ASSERT_DEBUG_EXT (prepare (c, MXDENSE, m*n, m, n, NULL, NULL), "Invalid output matrix"); }
  ASSERT_DEBUG (a != b && b != c && c != a, "Matrices 'a','b','c' must be different");
 
  double *ax = a->x,
	 *bx = b->x,
	 *cx = c->x,
	 *aa;
  int *pp = b->p,
      *ii = b->i,
      j, o;

  m = a->m; 
  o = c->m;

  for (k = 0; k < n; k ++)
  {
    j = ii[k];
    l = ii[k+1] - j;

    if (transa == 'N') aa = &ax[j*m];
    else aa = &ax[j];

    blas_dgemm (transa, transb, m, l, l,
      alpha, aa, m, &bx [pp[k]], l, beta, &cx[j*o], o);
  }

  return c;
}

/* multiply sparse and dense matrices */
static MX* matmat_csc_dense (double alpha, MX *a, MX *b, double beta, MX *c)
{
  MX *A;

  if (MXIFAC (a)) /* inv (a) * b */
  {
    return matmat_inv_dense (alpha, a, b, beta, c);
  }
 
  A = csc_to_dense (a);
  c = matmat_dense_dense (alpha, A, b, beta, c);
  MX_Destroy (A);

  return c;
}

/* multiply dense and sparse matrices */
static MX* matmat_dense_csc (double alpha, MX *a, MX *b, double beta, MX *c)
{
  MX *B;

  if (MXIFAC (b)) /* a * inv (b) */
  {
    return matmat_dense_inv (alpha, a, b, beta, c);
  }

  B = csc_to_dense (B);
  c = matmat_dense_dense (alpha, a, B, beta, c);
  MX_Destroy (B);

  return c;
}

/* multiply diagonal block and sparse matrices */
static MX* matmat_bd_csc (double alpha, MX *a, MX *b, double beta, MX *c)
{
  MX *B;

  if (MXIFAC (b)) /* a * inv (b) */
  {
    return matmat_bd_inv (alpha, a, b, beta, c);
  }

  B = csc_to_dense (B);
  c = matmat_bd_dense (alpha, a, B, beta, c);
  MX_Destroy (B);

  return c;
}

/* multiply sparse and diagonal block matrices */
static MX* matmat_csc_bd (double alpha, MX *a, MX *b, double beta, MX *c)
{
  MX *A;

  if (MXIFAC (a)) /* inv (a) * b */
  {
    return matmat_inv_bd (alpha, a, b, beta, c);
  }

  A = csc_to_dense (a);
  c = matmat_dense_bd (alpha, A, b, beta, c);
  MX_Destroy (A);

  return c;
}

/* invert dense matrix */
static MX* dense_inverse (MX *a, MX *b)
{
  int lwork, *ipiv;
  double *work, w;

  ASSERT_DEBUG (a->m == a->n, "Not a square matrix");
  if (b == NULL) b = MX_Create (MXDENSE, a->m, a->n, NULL, NULL);
  else { ASSERT_DEBUG_EXT (prepare (b, MXDENSE, a->nzmax, a->m, a->n, NULL, NULL), "Invalid output matrix"); }

  if (a != b) MX_Copy (a, b); /* copy content of 'a' into 'b' */
  lapack_dgetri (b->n, NULL, b->m, NULL, &w, -1); /* query for workspace size */
  lwork = (int) w;

  ERRMEM (ipiv = malloc (sizeof (int) * b->m));
  ERRMEM (work = malloc (sizeof (double) * lwork));
  ASSERT (lapack_dgetrf (b->m, b->n, b->x, b->m, ipiv) == 0, ERR_MTX_LU_FACTOR);
  ASSERT (lapack_dgetri (b->n, b->x, b->m, ipiv, work, lwork) == 0, ERR_MTX_MATRIX_INVERT);
  free (work);
  free (ipiv);

  return b;
}

/* invert diagonal block matrix */
static MX* bd_inverse (MX *a, MX *b)
{
  int m, n, k, lwork, *ipiv, *pp, *ii;
  double *work, *bx, w;

  if (b == NULL) b = MX_Create (MXBD, a->m, a->n, a->p, a->i);
  else { ASSERT_DEBUG_EXT (prepare (b, MXBD, a->nzmax, a->m, a->n, a->p, a->i), "Invalid output matrix"); }

  if (a != b) MX_Copy (a, b); /* copy content of 'a' into 'b' */
 
  n = b->n;
  pp = b->p;
  ii = b->i; 
  bx = b->x;
  
  for (lwork = k = 0; k < n; k ++)
  {
    m = ii[k+1] - ii[k];
    lapack_dgetri (m, NULL, m, NULL, &w, -1); /* query for workspace size */
    if ((int)w > lwork) lwork = (int)w;
  }

  ERRMEM (ipiv = malloc (sizeof (int) * lwork/4));
  ERRMEM (work = malloc (sizeof (double) * lwork));
  
  for (k = 0; k < n; k ++)
  {
    m = ii[k+1] - ii[k];
    ASSERT (lapack_dgetrf (m, m, &bx [pp[k]], m, ipiv) == 0, ERR_MTX_LU_FACTOR);
    ASSERT (lapack_dgetri (m, &bx[pp[k]], m, ipiv, work, lwork) == 0, ERR_MTX_MATRIX_INVERT);
  }

  free (work);
  free (ipiv);

  return b;
}

/* invert sparse matrix */
static MX* csc_inverse (MX *a, MX *b)
{
  ASSERT_DEBUG (a->m == a->n, "Not a square matrix");

  if (b != a) MX_Copy (a, b);

  if (MXIFAC (b))
  {
    b->flags &= ~MXIFAC;
    cs_sfree (b->sym);
    cs_nfree (b->num);
    b->sym = b->num = NULL;
  }
  else
  {
    b->flags |= MXIFAC;

    ERRMEM (b->sym = cs_sqr (2, b, 0));
    ASSERT (b->num = cs_lu (b, b->sym, 0.1), ERR_MTX_LU_FACTOR);
    ERRMEM (b->x = realloc (b->x, sizeof (double [b->nzmax + 2 * b->n]))); /* workspace after b->x */
  }

  return b;
}

/* compute dense matrix eigenvalues */
static void dense_eigen (MX *a, int n, double *val, MX *vec)
{
  double *bx, *z, *work;
  int *isuppz, *iwork, liwork,
      lwork, bn, bm, il, iu, m, ldz;
  char jobz;
  MX *b;

  ASSERT_DEBUG (a->m == a->n, "Not a square matrix");
  ASSERT_DEBUG (val, "NULL pointer passed for eigenvalues");
  ASSERT_DEBUG (vec && KIND (vec) == MXDENSE, "Not a dense matrix passed for eigenvectors");
  ASSERT_DEBUG (vec && ABS (n) == vec->n && a->m == vec->m, "Incompatible dimension of the eigenvectors matrix");

  b = MX_Copy (a, NULL);  /* copy, not to modify the input */
  jobz = vec ? 'V' : 'N';
  bn = b->n;
  bm = b->m;
  bx = b->x;
  il = (n < 0 ?  1 : bm - n + 1);
  iu = (n < 0 ? -n : bm);
  z  = (vec ? vec->x : NULL);
  ldz = (vec ? vec->m : 0);
  isuppz = NULL;
  
  lapack_dsyevr (jobz, 'I', 'U', bn, bx, bm,
    0.0, 0.0, il, iu, 0.0, &m, val, z, ldz,
    isuppz, val, -1, &liwork, -1); /* query workspace size */
  lwork = (int) val [0]; /* size of double workspace */
  ERRMEM (work = malloc (sizeof (double) * lwork +
    sizeof (int)*(liwork + ABS(n) * 2)));  /* allocate workspace */
  iwork = (int*) (work + lwork); /* integer workspace */
  isuppz = iwork + liwork; /* support indices */
  
  ASSERT (lapack_dsyevr (jobz, 'I', 'U', bn, bx, bm, /* compute eigenvalues/eigenvectors */
    0.0, 0.0, il, iu, 0.0, &m, val, z, ldz, isuppz,
    work, lwork, iwork, liwork) == 0, ERR_MTX_EIGEN_DECOMP);

  MX_Destroy (b);
  free (work);
}

/* compute diagonal block matrix eigenvalues */
static void bd_eigen (MX *a, int n, double *val, MX *vec)
{
  int size, m, nn, k, l, lwork, *pp, *ii;
  double *w, *work, *bx;
  MX *b;

  ASSERT_DEBUG (val, "NULL pointer passed for eigenvalues");
  ASSERT_DEBUG (vec && KIND (vec) == MXDENSE, "Not a dense matrix passed for eigenvectors");
  ASSERT_DEBUG (vec && ABS (n) == vec->n && a->m == vec->m, "Incompatible dimension of the eigenvectors matrix");

  b = MX_Copy (a, NULL);  /* copy not to modify the input */
  
  for (lwork = k = 0; k < n; k ++) /* find largest block */
  {
    m = ii[k+1] - ii[k];
    if (m > lwork) lwork = m;
  }

  lapack_dsyev (vec ? 'V' : 'N', 'U', lwork, NULL, lwork, val, val, -1); /* query workspace size */
  lwork = (int) val [0]; /* workspace size */
  size = a->m;           /* matrix size */
  
  ERRMEM (work = malloc (sizeof (double) * (size + lwork)));  /* workspace */
  w = (work + lwork);                                         /* eigenvalues */

  nn = b->n;
  pp = b->p;
  ii = b->i;
  bx = b->x;

  for (k = 0; k < nn; k ++)        /* copute values/vectors */
  {
    m = ii [k+1] - ii [k];
    ASSERT (lapack_dsyev (vec ? 'V' : 'N', 'U', m, &bx[pp[k]],
      m, &w[ii[k]], work, lwork) == 0, ERR_MTX_EIGEN_DECOMP);
  }

  /* eigenvalues/vectors are sorted in blocks;
   * it is now necessary to sort them globally */
  struct eigpair *pairs, *egp;
  ERRMEM (pairs = malloc (sizeof (struct eigpair) * size));

  if (vec) /* set up eigen vector pointers */
  {
    for (k = 0, egp = pairs; k < nn; k ++)
    {
      l = ii [k];
      m = ii [k+1] - l;
      for (int j = 0; j < m; j ++, egp ++)
      {
	egp->value = w [l+j];
	egp->shift = (l+j)*size + l;
	egp->length = m;
	egp->vector = (&bx[pp[k]]) + (j*m); /* local jth column of block matrix */
      }
    }

    MX_Zero (vec);
  }

  qsort (pairs, size, sizeof (struct eigpair), (qcmp_t)eigpaircmp); /* sort 'em */

  if (ABS (n) == a->m)
  {
    for (k = 0, egp = pairs; k < a->m; k ++, egp ++) /* copy all values & vectors */
    {
      val [k] = egp->value;
      if (vec) blas_dcopy (egp->length, egp->vector, 1, vec->x + egp->shift, 1);
    }
  }
  else if (n < 0)
  {
    for (k = 0, egp = pairs; k < -n; k ++, egp ++) /* copy |n| first values & vectors */
    {
      val [k] = egp->value;
      if (vec) blas_dcopy (egp->length, egp->vector, 1, vec->x + egp->shift, 1);
    }
  }
  else if (n > 0)
  {
    for (k = 0, egp = pairs+size-n; k < n; k ++, egp ++) /* copy |n| first values & vectors */
    {
      val [k] = egp->value;
      if (vec) blas_dcopy (egp->length, egp->vector, 1, vec->x + egp->shift, 1);
    }
  }
  
  MX_Destroy (b);
  free (work);
  free (pairs);
}

/* compute sparse matrix eigenvalues */
static void csc_eigen (MX *a, int n, double *val, MX *vec)
{
  ASSERT (0, ERR_NOT_IMPLEMENTED);
  //TODO
}

/* ------------ interface ------------- */

MX* MX_Create (short kind, int m, int n, int *p, int *i)
{
  int size = 0;
  MX *a;

  switch (kind)
  {
    case MXDENSE:
      size = m * n;
      ASSERT_DEBUG (size > 0, "Invalid size");
      ERRMEM (a = calloc (1, sizeof (MX) + size * sizeof (double)));
      a->x = (double*) (a + 1);
    break;
    case MXBD:
      ASSERT_DEBUG ((p && i) && (p != i), "Invalid structure");
      ASSERT_DEBUG (p [n] > 0, "Invalid block size");
      size = p [n];
      ERRMEM (a = calloc (1, sizeof (MX) + (2 * n + 2)
	* sizeof (int) + size * sizeof (double)));
      a->p = (int*) (a + 1);
      a->i = a->p + (n + 1);
      a->x = (double*) (a->i + (n + 1));
      memcpy (a->p, p, sizeof (int) * (n+1)); 
      memcpy (a->i, i, sizeof (int) * (n+1)); 
    break;
    case MXCSC:
      ERRMEM (a = calloc (1, sizeof (MX)));
      if (p && i) /* structure might not be provided */
      {
	size = p [n];
        ASSERT_DEBUG (p != i, "Invalid structure");
	ASSERT_DEBUG (size > 0, "Invalid nonzero size");
	ERRMEM (a->p = malloc (sizeof (int) * (n+1)));
	ERRMEM (a->i = malloc (sizeof (int) * size));
	ERRMEM (a->x = malloc (sizeof (double) * size));
	memcpy (a->p, p, sizeof (int) * (n+1)); 
	memcpy (a->i, i, sizeof (int) * size); 
	a->nz = -1; /* compressed format */
      }
    break;
    default:
      ASSERT (0, ERR_MTX_KIND);
    break;
  }

  a->kind = kind;
  a->nzmax = size;
  a->m = m;
  a->n = n;

  return a;
}

void MX_Zero (MX *a)
{
  for (double *x = a->x,
    *e = (a->x+a->nzmax);
    x < e; x ++) *x = 0.0;

  if (MXIFAC (a)) MX_Inverse (a, a); /* a is a sparse inverse => cancel the inverse */
}


void MX_Scale (MX *a, double b)
{
  for (double *x = a->x,
    *e = (a->x+a->nzmax);
    x < e; x ++) *x *= b;

  if (MXIFAC (a)) MX_Inverse (a, a), MX_Inverse (a, a); /* a is a sparse inverse => cancel the inverse and redo it */
}

MX* MX_Copy (MX *a, MX *b)
{
  if (b == a) return b;
  else if (b == NULL) b = MX_Create
    (a->kind, a->m, a->n, a->p, a->i);
  
  if (MXTRANS (a))
  {
    switch (a->kind)
    { 
      case MXDENSE:
        ASSERT_DEBUG_EXT (prepare (b, a->kind, a->nzmax, a->n,
	  a->m, NULL, NULL), "Invalid output matrix");
        dense_transpose_copy (a, b);
      break;
      case MXBD:
        ASSERT_DEBUG_EXT (prepare (b, a->kind, a->nzmax, a->m,
	  a->n, a->p, a->i), "Invalid output matrix");
	bd_transpose_copy (a, b);
      break;
      case MXCSC:
        ASSERT_DEBUG_EXT (prepare (b, a->kind, a->nzmax, a->n,
	  a->m, NULL, NULL), "Invalid output matrix");
	csc_transpose_copy (a, b);
      break;
    }

    free (a);
  }
  else
  {
    ASSERT_DEBUG_EXT (prepare (b, a->kind, a->nzmax, a->m,
      a->n, a->p, a->i), "Invalid output matrix");

    for (double *y = b->x, *x = a->x,
     *e = (a->x+a->nzmax); x < e; y ++, x++) *y = *x;
  }

  if (MXIFAC (a)) MX_Inverse (b, b); /* a was a sparse inverse => invert its copy */

  return b;
}

MX* MX_Tran (MX *a)
{
  MX *b;

  ASSERT_DEBUG (! MXTRANS (a), "Cannot transpose a transposed matrix");
  ERRMEM (b = malloc (sizeof (MX)));
  memcpy (b, a, sizeof (MX));
  b->flags |= MXTRANS;
  if (MXDSUBLK (a)) free (a);
  return b;
}

MX* MX_Diag (MX *a, int from, int to)
{
  MX *b;

  ASSERT_DEBUG (KIND (a) == MXBD, "Invalid matrix kind");
  ASSERT_DEBUG (from > to && from > 0 && to < a->n, "Invalid block index");
  ASSERT_DEBUG (! MXDSUBLK (a), "Cannot get sub-block of a sub-block");
  ERRMEM (b = malloc (sizeof (MX)));
  memcpy (b, a, sizeof (MX));
  b->flags |= MXDSUBLK;
  b->x = &a->x[a->p[from]];
  b->p = a->p + from;
  b->i = a->i + from;
  b->n = to - from + 1;
  if (MXTRANS (a)) free (a);
  return b;
}

MX* MX_Add (double alpha, MX *a, double beta, MX *b, MX *c)
{
  switch ((KIND(a)<<4)|KIND(b))
  {
    case 0x11: /* DENSE + DENSE */
      c = add_dense_dense (alpha, a, beta, b, c);
    break;
    case 0x22: /* BD + BD */
      c = add_bd_bd (alpha, a, beta, b, c);
    break;
    case 0x44: /* CSC + CSC */
      c = add_csc_csc (alpha, a, beta, b, c);
    break;
    case 0x21: /* BD + MXDENSE */
      c = add_dense_bd (beta, b, alpha, a, c);
    break;
    case 0x12: /* DENSE + MXBD */
      c = add_dense_bd (alpha, a, beta, b, c);
    break;
    case 0x41: /* CSC + MXDENSE */
      c = add_dense_csc (beta, b, alpha, a, c);
    break;
    case 0x14: /* DENSE + MXCSC */
      c = add_dense_csc (alpha, a, beta, b, c);
    break;
    case 0x24: /* BD + MXCSC */
      c = add_csc_bd (beta, b, alpha, a, c);
    break;
    case 0x42: /* CSC + MXBD */
      c = add_csc_bd (alpha, a, beta, b, c);
    break;
  }

  if (TEMPORARY (a)) free (a);
  if (TEMPORARY (b)) free (b);
  return c;
}

MX* MX_Matmat (double alpha, MX *a, MX *b, double beta, MX *c)
{
  switch ((KIND(a)<<4)|KIND(b))
  {
    case 0x11: /* DENSE * DENSE */
      c = matmat_dense_dense (alpha, a, b, beta, c);
    break;
    case 0x22: /* BD * BD */
      c = matmat_bd_bd (alpha, a, b, beta, c);
    break;
    case 0x44: /* CSC * CSC */
      c = matmat_csc_csc (alpha, a, b, beta, c);
    break;
    case 0x21: /* BD * MXDENSE */
      c = matmat_bd_dense (alpha, a, b, beta, c);
    break;
    case 0x12: /* DENSE * MXBD */
      c = matmat_dense_bd (alpha, a, b, beta, c);
    break;
    case 0x41: /* CSC * MXDENSE */
      c = matmat_csc_dense (alpha, a, b, beta, c);
    break;
    case 0x14: /* DENSE * MXCSC */
      c = matmat_dense_csc (alpha, a, b, beta, c);
    break;
    case 0x24: /* BD * MXCSC */
      c = matmat_bd_csc (alpha, a, b, beta, c);
    break;
    case 0x42: /* CSC * MXBD */
      c = matmat_csc_bd (alpha, a, b, beta, c);
    break;
  }

  if (TEMPORARY (a)) free (a);
  if (TEMPORARY (b)) free (b);
  return c;
}

void MX_Matvec (double alpha, MX *a, double *b, double beta, double *c)
{
  switch (a->kind)
  {
    case MXDENSE:
      blas_dgemv (MXTRANS(a) ? 'T' : 'N', a->m, a->n, alpha, a->x, a->m, b, 1, beta, c, 1);
    break;
    case MXBD:
    {
      int *p = a->p, *i = a->i, n = a->n, j, l;
      char transa = MXTRANS(a) ? 'T' : 'N';
      double *x = a->x;

      for (j = 0; j < n; j ++)
      {
	l = i[j+1] - i[j];
	blas_dgemv (transa, l, l, alpha, &x[p[j]], l, &b[i[j]], 1, beta, &c[i[j]], 1);
      }
    }
    break;
    case MXCSC:
    {
      int *p = a->p, *i = a->i, m = a->m, n = a->n, *j, *k, l;
      double *x = a->x, *y;

      if (MXTRANS (a))
      {
	if (MXIFAC (a))
	{
	  double *x = MXIFAC_WORK1(a),
		 *y = MXIFAC_WORK2(a);
	  css *S = a->sym;
	  csn *N = a->num;

	  cs_ipvec (S->q, b, x, a->n);     /* x = inv-permute (b) */
	  cs_utsolve (N->U, x);            /* x = U' \ x */
	  cs_ltsolve (N->L, x);            /* x = L' \ x */
	  cs_ipvec (N->pinv, x, y, a->n);  /* y = permute (x) */

	  for (int n = a->n; n > 0; n --, c ++, y ++) (*c) = alpha * (*y) + beta * (*c);
	}
	else
	{
	  for (l = 0; l < n; l ++, c ++)
	  {
	    (*c) *= beta;
	    for (j = &i[p[l]], k = &i[p[l+1]], y = &x[p[l]]; j < k; j ++, y ++)
	      (*c) += alpha * (*y) * b[*j];
	  }
	}
      }
      else
      {
	if (MXIFAC (a))
	{
	  double *x = MXIFAC_WORK1(a),
		 *y = MXIFAC_WORK2(a);
	  css *S = a->sym;
	  csn *N = a->num;

	  cs_ipvec (N->pinv, b, x, a->n);  /* x = permute (b) */
	  cs_lsolve (N->L, x);             /* x = L \ x */
	  cs_usolve (N->U, x);             /* x = U \ x */
	  cs_ipvec (S->q, x, y, a->n);     /* y = inv-permute (x) */

	  for (int n = a->n; n > 0; n --, c ++, y ++) (*c) = alpha * (*y) + beta * (*c);
	}
	else
	{
	  for (l = 0; l < m; l ++) c [l] *= beta;
	  for (l = 0; l < n; l ++, b ++)
	    for (j = &i[p[l]], k = &i[p[l+1]], y = &x[p[l]]; j < k; j ++, y ++)
	      c [*j] += alpha * (*y) * (*b);
	}
      }
    }
    break;
  }

  if (TEMPORARY (a)) free (a);
}

MX* MX_Trimat (MX *a, MX *b, MX *c, MX *d)
{
  MX *t;

  t = MX_Matmat (1.0, b, c, 0.0, NULL);
  d = MX_Matmat (1.0, a, t, 0.0, d);
  MX_Destroy (t);
  return d;
}

MX* MX_Inverse (MX *a, MX *b)
{
  switch (a->kind)
  {
    case MXDENSE:
      b = dense_inverse (a, b);
    break;
    case MXBD:
      b = bd_inverse (a, b);
    break;
    case MXCSC:
      b = csc_inverse (a, b);
    break;
  }

  if (TEMPORARY (a)) free (a);
  return b;
}

void MX_Eigen (MX *a, int n, double *val, MX *vec)
{
  switch (a->kind)
  {
    case MXDENSE:
      dense_eigen (a, n, val, vec);
    break;
    case MXBD:
      bd_eigen (a, n, val, vec);
    break;
    case MXCSC:
      csc_eigen (a, n, val, vec);
    break;
  }

  if (TEMPORARY (a)) free (a);
}

void MX_Destroy (MX *a)
{
  if (TEMPORARY (a)) free (a); /* should not happen */
  else switch (a->kind)
  {
    case MXDENSE:
    case MXBD:
      free (a);
    break;
    case MXCSC:
      free (a->p);
      free (a->i);
      free (a->x);
      if (a->sym) cs_sfree (a->sym);
      if (a->num) cs_nfree (a->num);
      free (a);
    break;
  }
}
