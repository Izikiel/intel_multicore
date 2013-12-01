//+++2003-12-12
//    Copyright (C) 2001,2002,2003  Mike Rieker, Beverly, MA USA
//
//    This program is free software; you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation; version 2 of the License.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with this program; if not, write to the Free Software
//    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//---2003-12-12

#include "ozone.h"
#include "oz_knl_hw.h"

#if 000
void *memchr (const void *src, int c, unsigned long len)

{
  while (len > 0) {
    if (*(unsigned char *)src == (unsigned char)c) return ((void *)src);
    -- len;
    (unsigned char *)src ++;
  }
  return (NULL);
}
#endif

#if 000
void *memchrnot (const void *src, int c, unsigned long len)

{
  while (len > 0) {
    if (*(unsigned char *)src != (unsigned char)c) return ((void *)src);
    -- len;
    (unsigned char *)src ++;
  }
  return (NULL);
}
#endif

#if 000
int memcmp (const void *left, const void *right, unsigned long len)

{
  int rc;

  while (len > 0) {
    rc = (int)*(unsigned char *)left - (int)*(unsigned char *)right;
    if (rc != 0) return (rc);
    -- len;
    (unsigned char *)left ++;
    (unsigned char *)right ++;
  }
  return (0);
}
#endif

#if 000
void *memcpy (void *dst, const void *src, unsigned long len)

{
  char *d;
  char const *s;

  if ((len > 0) && ((d = dst) != (s = src))) {
    do *(d ++) = *(s ++);
    while (-- len > 0);
  }

  return (dst);
}
#endif

#if 000
void *memmove (void *dst, const void *src, unsigned long len)

{
  char *d;
  char const *s;

  if ((len > 0) && ((d = dst) != (s = src))) {
    if (d > s) {
      s += len;
      d += len;
      do *(-- d) = *(-- s);
      while (-- len > 0);
    } else {
      do *(d ++) = *(s ++);
      while (-- len > 0);
    }
  }

  return (dst);
}
#endif

#if 000
void *memset (void *dst, int val, unsigned long len)

{
  char *d;

  if (len > 0) {
    d = dst;
    do *(d ++) = val;
    while (-- len > 0);
  }

  return (dst);
}
#endif

void movc4 (unsigned long slen, const void *src, unsigned long dlen, void *dst)

{
  if (dlen <= slen) memmove (dst, src, dlen);
  else {
    memmove (dst, src, slen);
    memset (dst + slen, 0, dlen - slen);
  }
}

#if 000
int strcasecmp (char const *left, char const *right)

{
  int l, r, rc;

  while (1) {
    l = *(unsigned char *)left;
    r = *(unsigned char *)right;
    if ((l >= 'A') && (l <= 'Z')) l += 'a' - 'A';
    if ((r >= 'A') && (r <= 'Z')) r += 'a' - 'A';
    if (l == 0) break;
    if (l != r) break;
    left ++;
    right ++;
  }
  return (l - r);
}
#endif

char *strcat (char *dst, char const *src)

{
  return (strcpy (dst + strlen (dst), src));
}

#if 000
char *strchr (char const *src, int c)

{
  char z;

  while ((z = *src) != 0) {
    if (z == (char)c) return ((char *)src);
    src ++;
  }
  return (NULL);
}
#endif

#if 000
int strcmp (char const *left, char const *right)

{
  int l, r, rc;

  while (1) {
    l = *(unsigned char *)left;
    r = *(unsigned char *)right;
    if (l == 0) break;
    if (l != r) break;
    left ++;
    right ++;
  }
  return (l - r);
}
#endif

#if 000
char *strcpy (char *dst, char const *src)

{
  char *d = dst;
  while ((*(d ++) = *(src ++)) != 0) {}
  return (dst);
}
#endif

#if 000
unsigned long strlen (char const *src)

{
  char const *s;

  s = src;
  while (*s != 0) s ++;
  return (s - src);
}
#endif

#if 000
int strncasecmp (char const *left, char const *right, unsigned long len)

{
  int l, r, rc;

  if (len == 0) return (0);
  do {
    l = *(unsigned char *)left;
    r = *(unsigned char *)right;
    if ((l >= 'A') && (l <= 'Z')) l += 'a' - 'A';
    if ((r >= 'A') && (r <= 'Z')) r += 'a' - 'A';
    if (l == 0) break;
    if (l != r) break;
    left ++;
    right ++;
  } while (-- len > 0);
  return (l - r);
}
#endif

char *strncat (char *dst, char const *src, unsigned long len)

{
  return (strncpy (dst + strlen (dst), src, len));
}

#if 000
int strncmp (char const *left, char const *right, unsigned long len)

{
  int l, r, rc;

  if (len == 0) return (0);
  do {
    l = *(unsigned char *)left;
    r = *(unsigned char *)right;
    if (l == 0) break;
    if (l != r) break;
    left ++;
    right ++;
  } while (-- len > 0);
  return (l - r);
}
#endif

/* doesn't guarantee a null terminator */

#if 000
char *strncpy (char *dst, char const *src, unsigned long len)

{
  char *d;

  if (len != 0) {
    d = dst;
    do if ((*(d ++) = *(src ++)) == 0) break;
    while (-- len > 0);
  }
  return (dst);
}
#endif

/* guarantees at least one null terminator */

#if 000
char *strncpyz (char *dst, char const *src, unsigned long len)

{
  char *d;

  if (len > 0) {
    d = dst;
    if (-- len > 0) {
      do if ((*(d ++) = *(src ++)) == 0) goto rtn;
      while (-- len > 0);
    }
    *d = 0;
  }
rtn:
  return (dst);
}
#endif

#if 000
unsigned long strnlen (char const *src, unsigned long len)

{
  char const *s;

  s = src;
  while ((len > 0) && (*s != 0)) { -- len; s ++; }
  return (s - src);
}
#endif

#if 000
char *strrchr (char const *src, int c)

{
  char d;
  char const *p;
  char const *q;

  p = src;
  q = NULL;
  while ((d = *p) != 0) {
    if (d == (char)c) q = p;
    p ++;
  }
  return ((char *)q);
}
#endif

char *strstr (char const *haystack, char const *needle)

{
  char const *p;
  char const *q;
  int l;

  l = strlen (needle);
  if (l == 0) return ((char *)haystack);
  for (p = haystack; (q = strchr (p, needle[0])) != NULL; p = ++ q) {
    if (memcmp (q, needle, l) == 0) break;
  }
  return ((char *)q);
}
