//+++2001-10-06
//    Copyright (C) 2001, Mike Rieker, Beverly, MA USA
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
//---2001-10-06

/************************************************************************/
/*									*/
/*  C Runtime library routines that are part of the kernel		*/
/*									*/
/************************************************************************/

#include "ozone.h"

char *strncpy (char *dest, const char *src, unsigned int n)

{
  char c, *d;
  const char *s;

  s = src;
  d = dest;

  while (n > 1) {
    -- n;
    *(d ++) = c = *(s ++);
    if (c == 0) break;
  }
  while (n > 0) {
    -- n;
    *(d ++) = 0;
  }

  return (dest);
}

void *memset (void *s, int c, unsigned int n)

{
  char *p;

  p = s;
  while (n > 0) { *(p ++) = c; -- n; }
  return (s);
}

unsigned int strlen (const char *s)

{
  int i;

  for (i = 0; s[i] != 0; i ++) {}
  return (i);
}

char *strcpy (char *dest, const char *src)

{
  char c, *d;
  const char *s;

  s = src;
  d = dest;

  while (1) {
    *(d ++) = c = *(s ++);
    if (c == 0) break;
  }

  return (dest);
}

char *strcat (char *dest, const char *src)

{
  return (strcpy (dest + strlen (dest), src));
}

int strcasecmp (const char *s1, const char *s2)

{
  char c1, c2;

  do {
    c1 = *(s1 ++);
    c2 = *(s2 ++);
    if ((c1 >= 'A') && (c1 <= 'Z')) c1 += 'a' - 'A';
    if ((c2 >= 'A') && (c2 <= 'Z')) c2 += 'a' - 'A';
  } while ((c1 == c2) && (c1 != 0));

  return (((int)c1) - ((int)c2));
}

int strncasecmp (const char *s1, const char *s2, unsigned int n)

{
  char c1, c2;

  c1 = 0;
  c2 = 0;
  while (n != 0) {
    n --;
    c1 = *(s1 ++);
    c2 = *(s2 ++);
    if ((c1 >= 'A') && (c1 <= 'Z')) c1 += 'a' - 'A';
    if ((c2 >= 'A') && (c2 <= 'Z')) c2 += 'a' - 'A';
    if ((c1 != c2) || (c1 == 0)) break;
  }

  return (((int)c1) - ((int)c2));
}

int strcmp (const char *s1, const char *s2)

{
  char c1, c2;

  do {
    c1 = *(s1 ++);
    c2 = *(s2 ++);
  } while ((c1 == c2) && (c2 != 0));

  return (((int)c1) - ((int)c2));
}

int strncmp (const char *s1, const char *s2, unsigned int n)

{
  char c1, c2;

  c1 = 0;
  c2 = 0;
  while (n != 0) {
    n --;
    c1 = *(s1 ++);
    c2 = *(s2 ++);
    if ((c1 != c2) || (c1 == 0)) break;
  }

  return (((int)c1) - ((int)c2));
}

char *strchr (const char *s, int c)

{
  const char *p;

  for (p = s; *p != (char)c; p ++) {
    if (*p == 0) return (NULL);
  }
  return ((char *)p);
}

char *strrchr (const char *s, int c)

{
  const char *p, *pp;

  pp = NULL;
  for (p = s; *p != 0; p ++) {
    if (*p == c) pp = p;
  }
  return ((char *)pp);
}

void *memcpy (void *dest, const void *src, unsigned int n)

{
  char *d;
  const char *s;

  s = src;
  d = dest;

  while (n != 0) {
    -- n;
    *(d ++) = *(s ++);
  }

  return (dest);
}

void *memmove (void *dest, const void *src, unsigned int n)

{
  char *d;
  const char *s;

  s = src;
  d = dest;

  if (d <= s) {

    while (n != 0) {
      -- n;
      *(d ++) = *(s ++);
    }

  } else {

    s += n;
    d += n;
    while (n != 0) {
      -- n;
      *(-- d) = *(-- s);
    }

  }

  return (dest);
}

int memcmp (const void *s1, const void *s2, unsigned int n)

{
  const char *c1, *c2;
  int i;

  c1 = s1;
  c2 = s2;
  i = 0;
  while (n != 0) {
    -- n;
    i = ((int)*(c1 ++)) - ((int)*(c2 ++));
    if (i != 0) break;
  }
  return (i);
}

void movc4 (unsigned int srclen, const void *srcbuf, unsigned int dstlen, void *dstbuf)

{
  if (srclen < dstlen) {
    memcpy (dstbuf, srcbuf, srclen);
    memset (((char *)dstbuf) + srclen, 0, dstlen - srclen);
  } else {
    memcpy (dstbuf, srcbuf, dstlen);
  }
}
