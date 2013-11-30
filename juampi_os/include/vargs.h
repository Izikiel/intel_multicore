#ifndef __VARGS_H
#define __VARGS_H

typedef void * varg_list;

#define varg_set(list, last) list = &(last)
#define varg_yield(list, type) *((type *) (list+=(sizeof(type) < 4 ? 4 : sizeof(type))))
#define varg_end(list) list = 0

#endif
