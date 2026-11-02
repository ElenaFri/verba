#include "vec.h"
#include "util.h"

#include <stdlib.h>

void vec_push(struct vec *v, void *item)
{
	if (v->len == v->cap) {
		v->cap = v->cap ? v->cap * 2 : 8;
		v->data = xrealloc(v->data, v->cap * sizeof(*v->data));
	}
	v->data[v->len++] = item;
}

void vec_free(struct vec *v, void (*free_item)(void *))
{
	if (free_item)
		for (size_t i = 0; i < v->len; i++)
			free_item(v->data[i]);
	free(v->data);
	v->data = NULL;
	v->len = 0;
	v->cap = 0;
}
