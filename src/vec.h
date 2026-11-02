#ifndef VERBA_VEC_H
#define VERBA_VEC_H

#include <stddef.h>

/* A minimal growable array of owned/borrowed pointers. */
struct vec {
	void **data;
	size_t len;
	size_t cap;
};

void vec_push(struct vec *v, void *item);

/* Free the backing store. If free_item is non-NULL it is called on each element
 * first. Safe to call on a zero-initialised vec. */
void vec_free(struct vec *v, void (*free_item)(void *));

#endif /* VERBA_VEC_H */
