/*
 * This file is part of DGD, https://github.com/dworkin/dgd
 * Copyright (C) 1993-2010 Dworkin B.V.
 * Copyright (C) 2010-2016 DGD Authors (see the commit log for details)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

# include "dgd.h"
# include "str.h"
# include "array.h"
# include "object.h"
# include "xfloat.h"
# include "interpret.h"
# include "data.h"

# define ARR_CHUNK	128

struct arrh {
    arrh *next;			/* next in hash table chain */
    Array *arr;			/* array entry */
    Uint index;			/* building index */
};

# define MELT_CHUNK	128

struct mapelt {
    Uint hashval;		/* hash value of index */
    bool add;			/* new element? */
    Value idx;			/* index */
    Value val;			/* value */
    mapelt *next;		/* next in hash table */
};

struct maphash {
    unsigned short size;	/* # elements in hash table */
    unsigned short sizemod;	/* mapping size modification */
    Uint tablesize;		/* actual hash table size */
    mapelt *table[1];		/* hash table */
};

# define MTABLE_SIZE	16	/* most mappings are quite small */

# define ABCHUNKSZ	32

struct arrbak {
    Array *arr;			/* array backed up */
    unsigned short size;	/* original size (of mapping) */
    Value *original;		/* original elements */
    Dataplane *plane;		/* original dataplane */
};

static Chunk<Array, ARR_CHUNK> achunk;
static Chunk<mapelt, MELT_CHUNK> echunk;

static class arrhchunk : public Chunk<arrh, ARR_CHUNK> {
public:
    /*
     * NAME:		item()
     * DESCRIPTION:	delete referenced array when iterating through items
     */
    virtual bool item(arrh *h) {
	arr_del(h->arr);
	return TRUE;
    }
} hchunk;

class abchunk : public Chunk<arrbak, ABCHUNKSZ> {
public:
    /*
     * NAME:		backup()
     * DESCRIPTION:	add an array backup to the backup chunk
     */
    static void backup(abchunk **ac, Array *a, Value *elts, unsigned int size,
		       Dataplane *plane) {
	arrbak *ab;

	if (*ac == (abchunk *) NULL) {
	    *ac = new abchunk;
	}

	ab = (*ac)->alloc();
	ab->arr = a;
	ab->size = size;
	ab->original = elts;
	ab->plane = plane;
    }

    /*
     * NAME:		item()
     * DESCRIPTION:	commit or discard when iterating over items
     */
    virtual bool item(arrbak *ab) {
	if (plane != (Dataplane *) NULL) {
	    abchunk **ac;

	    /*
	     * commit
	     */
	    ac = d_commit_arr(ab->arr, plane, ab->plane);
	    if (merge) {
		if (ac != (abchunk **) NULL) {
		    /* backup on previous plane */
		    backup(ac, ab->arr, ab->original, ab->size, ab->plane);
		} else {
		    if (ab->original != (Value *) NULL) {
			Value *v;
			unsigned short i;

			for (v = ab->original, i = ab->size; i != 0; v++, --i) {
			    i_del_value(v);
			}
			FREE(ab->original);
		    }
		    arr_del(ab->arr);
		}
	    }
	} else {
	    Array *a;
	    unsigned short i;

	    /*
	     * discard
	     */
	    a = ab->arr;
	    d_discard_arr(a, ab->plane);

	    if (a->elts != (Value *) NULL) {
		Value *v;

		for (v = a->elts, i = a->size; i != 0; v++, --i) {
		    i_del_value(v);
		}
		FREE(a->elts);
	    }

	    if (a->hashed != (maphash *) NULL) {
		mapelt *e, *n, **t;

		for (i = a->hashed->size, t = a->hashed->table; i > 0; t++) {
		    for (e = *t; e != (mapelt *) NULL; e = n) {
			if (e->add) {
			    i_del_value(&e->idx);
			    i_del_value(&e->val);
			}
			n = e->next;
			echunk.del(e);
			--i;
		    }
		}
		FREE(a->hashed);
		a->hashed = (maphash *) NULL;
		a->hashmod = FALSE;
	    }

	    a->elts = ab->original;
	    a->size = ab->size;
	    arr_del(a);
	}

	return TRUE;
    }

    /*
     * NAME:		commit()
     * DESCRIPTION:	commit array backups
     */
    void commit(Dataplane *p, bool flag) {
	plane = p;
	merge = flag;
	items();
    }

    /*
     * NAME:		discard()
     * DESCRIPTION:	discard array backups
     */
    void discard() {
	plane = (Dataplane *) NULL;
	items();
    }

private:
    Dataplane *plane;		/* plane to commit to */
    bool merge;			/* merging? */
};

static unsigned long max_size;	/* max. size of array and mapping */
static Uint tag;		/* current array tag */
static arrh *aht[ARRMERGETABSZ];/* array merge table */

/*
 * NAME:	Array->init()
 * DESCRIPTION:	initialize array handling
 */
void arr_init(unsigned int size)
{
    max_size = size;
    tag = 0;
}

/*
 * NAME:	Array->alloc()
 * DESCRIPTION:	create a new array
 */
Array *arr_alloc(unsigned int size)
{
    Array *a;

    a = achunk.alloc();
    a->size = size;
    a->hashmod = FALSE;
    a->elts = (Value *) NULL;
    a->ref = 0;
    a->odcount = 0;			/* if swapped in, check objects */
    a->hashed = (maphash *) NULL;	/* only used for mappings */

    return a;
}

/*
 * NAME:	Array->new()
 * DESCRIPTION:	create a new array
 */
Array *arr_new(Dataspace *data, long size)
{
    Array *a;

    if (size > max_size) {
	error("Array too large");
    }
    a = arr_alloc((unsigned short) size);
    if (size > 0) {
	a->elts = ALLOC(Value, size);
    }
    a->tag = tag++;
    a->odcount = odcount;
    a->primary = &data->plane->alocal;
    a->prev = &data->alist;
    a->next = data->alist.next;
    a->next->prev = a;
    data->alist.next = a;
    return a;
}

/*
 * NAME:	Array->ext_new()
 * DESCRIPTION:	return an array, initialized for the benefit of the extension
 *		interface
 */
Array *arr_ext_new(Dataspace *data, long size)
{
    int i;
    Value *v;
    Array *a;

    a = arr_new(data, size);
    for (i = size, v = a->elts; i != 0; --i, v++) {
	*v = nil_value;
    }
    return a;
}

/*
 * NAME:	Array->del()
 * DESCRIPTION:	remove a reference from an array or mapping.  If none are
 *		left, the array/mapping is removed.
 */
void arr_del(Array *a)
{
    if (--(a->ref) == 0) {
	Value *v;
	unsigned short i;
	static Array *dlist;

	a->prev->next = a->next;
	a->next->prev = a->prev;
	a->prev = (Array *) NULL;
	if (dlist != (Array *) NULL) {
	    dlist->prev = a;
	    dlist = a;
	    return;
	}
	dlist = a;

	do {
	    Array *prev;

	    if ((v=a->elts) != (Value *) NULL) {
		for (i = a->size; i > 0; --i) {
		    i_del_value(v++);
		}
		FREE(a->elts);
	    }

	    if (a->hashed != (maphash *) NULL) {
		mapelt *e, *n, **t;

		/*
		 * delete the hashtable of a mapping
		 */
		for (i = a->hashed->size, t = a->hashed->table; i > 0; t++) {
		    for (e = *t; e != (mapelt *) NULL; e = n) {
			if (e->add) {
			    i_del_value(&e->idx);
			    i_del_value(&e->val);
			}
			n = e->next;
			echunk.del(e);
			--i;
		    }
		}
		FREE(a->hashed);
	    }

	    prev = a->prev;
	    achunk.del(a);
	    a = prev;
	} while (a != (Array *) NULL);

	dlist = (Array *) NULL;
    }
}

/*
 * NAME:	Array->freelist()
 * DESCRIPTION:	free all left-over arrays in a dataspace
 */
void arr_freelist(Array *alist)
{
    Array *a;
    Value *v;
    unsigned short i;
    mapelt *e, *n, **t;

    a = alist;
    do {
	Array *prev;

	if ((v=a->elts) != (Value *) NULL) {
	    for (i = a->size; i > 0; --i) {
		if (v->type == T_STRING) {
		    str_del(v->u.string);
		}
		v++;
	    }
	    FREE(a->elts);
	}

	if (a->hashed != (maphash *) NULL) {
	    /*
	     * delete the hashtable of a mapping
	     */
	    for (i = a->hashed->size, t = a->hashed->table; i > 0; t++) {
		for (e = *t; e != (mapelt *) NULL; e = n) {
		    if (e->add) {
			if (e->idx.type == T_STRING) {
			    str_del(e->idx.u.string);
			}
			if (e->val.type == T_STRING) {
			    str_del(e->val.u.string);
			}
		    }
		    n = e->next;
		    echunk.del(e);
		    --i;
		}
	    }
	    FREE(a->hashed);
	}

	prev = a->prev;
	achunk.del(a);
	a = prev;
    } while (a != alist);
}

/*
 * NAME:	Array->freeall()
 * DESCRIPTION:	free all array chunks and mapping element chunks
 */
void arr_freeall()
{
    achunk.clean();
    echunk.clean();
}

/*
 * NAME:	Array->merge()
 * DESCRIPTION:	prepare the array merge table
 */
void arr_merge()
{
    memset(&aht, '\0', ARRMERGETABSZ * sizeof(arrh *));
}

/*
 * NAME:	Array->put()
 * DESCRIPTION:	Put an array in the merge table, and return its "index".
 */
Uint arr_put(Array *a, Uint idx)
{
    arrh **h;

    for (h = &aht[(uintptr_t) a % ARRMERGETABSZ]; *h != (arrh *) NULL;
	 h = &(*h)->next) {
	if ((*h)->arr == a) {
	    return (*h)->index;
	}
    }
    /*
     * Add a new entry to the hash table.
     */
    *h = hchunk.alloc();
    (*h)->next = (arrh *) NULL;
    arr_ref((*h)->arr = a);
    (*h)->index = idx;

    return idx;
}

/*
 * NAME:	Array->clear()
 * DESCRIPTION:	clear the array merge table
 */
void arr_clear()
{
    hchunk.items();
    hchunk.clean();
}


/*
 * NAME:	Array->backup()
 * DESCRIPTION:	make a backup of the current elements of an array or mapping
 */
void arr_backup(abchunk **ac, Array *a)
{
    Value *elts;
    unsigned short i;

# ifdef DEBUG
    if (a->hashmod) {
	fatal("backing up unclean mapping");
    }
# endif
    if (a->size != 0) {
	memcpy(elts = ALLOC(Value, a->size), a->elts, a->size * sizeof(Value));
	for (i = a->size; i != 0; --i) {
	    switch (elts->type) {
	    case T_STRING:
		str_ref(elts->u.string);
		break;

	    case T_ARRAY:
	    case T_MAPPING:
	    case T_LWOBJECT:
		arr_ref(elts->u.array);
		break;
	    }
	    elts++;
	}
	elts -= a->size;
    } else {
	elts = (Value *) NULL;
    }
    abchunk::backup(ac, a, elts, a->size, a->primary->plane);
    arr_ref(a);
}

/*
 * NAME:	Array->commit()
 * DESCRIPTION:	commit current array values and discard originals
 */
void arr_commit(abchunk **ac, Dataplane *plane, bool merge)
{
    if (*ac != (abchunk *) NULL) {
	(*ac)->commit(plane, merge);
	if (merge) {
	    delete *ac;
	    *ac = (abchunk *) NULL;
	}
    }
}

/*
 * NAME:	Array->discard()
 * DESCRIPTION:	restore originals and discard current values
 */
void arr_discard(abchunk **ac)
{
    if (*ac != (abchunk *) NULL) {
	(*ac)->discard();
	delete *ac;
    }
}


/*
 * NAME:	copytmp()
 * DESCRIPTION:	make temporary copies of values
 */
static void copytmp(Dataspace *data, Value *v1, Array *a)
{
    Value *v2, *o;
    unsigned short n;

    v2 = d_get_elts(a);
    if (a->odcount == odcount) {
	/*
	 * no need to check for destructed objects
	 */
	memcpy(v1, v2, a->size * sizeof(Value));
    } else {
	/*
	 * Copy and check for destructed objects.  If destructed objects are
	 * found, they will be replaced by nil in the original array.
	 */
	a->odcount = odcount;
	for (n = a->size; n != 0; --n) {
	    switch (v2->type) {
	    case T_OBJECT:
		if (DESTRUCTED(v2)) {
		    d_assign_elt(data, a, v2, &nil_value);
		}
		break;

	    case T_LWOBJECT:
		o = d_get_elts(v2->u.array);
		if (o->type == T_OBJECT && DESTRUCTED(o)) {
		    d_assign_elt(data, a, v2, &nil_value);
		}
		break;
	    }
	    *v1++ = *v2++;
	}
    }
}

/*
 * NAME:	Array->add()
 * DESCRIPTION:	add two arrays
 */
Array *arr_add(Dataspace *data, Array *a1, Array *a2)
{
    Array *a;

    a = arr_new(data, (long) a1->size + a2->size);
    i_copy(a->elts, d_get_elts(a1), a1->size);
    i_copy(a->elts + a1->size, d_get_elts(a2), a2->size);
    d_ref_imports(a);

    return a;
}

static int cmp (cvoid*, cvoid*);

/*
 * NAME:	cmp()
 * DESCRIPTION:	compare two values
 */
static int cmp(cvoid *cv1, cvoid *cv2)
{
    Value *v1, *v2;
    int i;
    Float f1, f2;

    v1 = (Value *) cv1;
    v2 = (Value *) cv2;
    i = v1->type - v2->type;
    if (i != 0) {
	return i;	/* order by type */
    }

    switch (v1->type) {
    case T_NIL:
	return 0;

    case T_INT:
	return (v1->u.number <= v2->u.number) ?
		(v1->u.number < v2->u.number) ? -1 : 0 :
		1;

    case T_FLOAT:
	GET_FLT(v1, f1);
	GET_FLT(v2, f2);
	return f1.cmp(f2);

    case T_STRING:
	return str_cmp(v1->u.string, v2->u.string);

    case T_OBJECT:
	return (v1->oindex <= v2->oindex) ?
		(v1->oindex < v2->oindex) ? -1 : 0 :
		1;

    case T_ARRAY:
    case T_MAPPING:
    case T_LWOBJECT:
	return (v1->u.array->tag <= v2->u.array->tag) ?
		(v1->u.array->tag < v2->u.array->tag) ? -1 : 0 :
		1;
    }

    return 0;
}

/*
 * NAME:	search()
 * DESCRIPTION:	search for a value in an array
 */
static int search(Value *v1, Value *v2, unsigned short h, int step, bool place)
{
    unsigned short l, m;
    Int c;
    Value *v3;
    unsigned short mask;

    mask = -step;
    l = 0;
    while (l < h) {
	m = ((l + h) >> 1) & mask;
	v3 = v2 + m;
	c = cmp(v1, v3);
	if (c == 0) {
	    if (T_INDEXED(v1->type) && v1->u.array != v3->u.array) {
		/*
		 * It is possible for one object to export an array, both
		 * objects being swapped out after that, and the other object
		 * exporting the array back again.  This gives two arrays with
		 * identical tags, which do not point to the same actual values
		 * and are not guaranteed to contain the same values, either.
		 * A possible way out is to give new tags to imported arrays
		 * and to resort all mappings before swapping them out.
		 * The solution used here is to check every array with this tag,
		 * and hope this kind of thing doesn't occur too often...
		 */
		/* search forward */
		for (;;) {
		    m += step;
		    v3 += step;
		    if (m == h || !T_INDEXED(v3->type)) {
			break;	/* out of range */
		    }
		    if (v1->u.array == v3->u.array) {
			return m;	/* found the right one */
		    }
		    if (v1->u.array->tag != v3->u.array->tag) {
			break;		/* wrong tag */
		    }
		}
		/* search backward */
		m = ((l + h) >> 1) & mask;
		v3 = v2 + m;
		for (;;) {
		    v3 -= step;
		    if (m == l || !T_INDEXED(v3->type)) {
			break;	/* out of range */
		    }
		    m -= step;
		    if (v1->u.array == v3->u.array) {
			return m;	/* found the right one */
		    }
		    if (v1->u.array->tag != v3->u.array->tag) {
			break;		/* wrong tag */
		    }
		}
		break;		/* not found */
	    }
	    return m;		/* found */
	} else if (c < 0) {
	    h = m;		/* search in lower half */
	} else {
	    l = (m + step);	/* search in upper half */
	}
    }
    /*
     * not found
     */
    return (place) ? l : -1;
}

/*
 * NAME:	Array->sub()
 * DESCRIPTION:	subtract one array from another
 */
Array *arr_sub(Dataspace *data, Array *a1, Array *a2)
{
    Value *v1, *v2, *v3, *o;
    Array *a3;
    unsigned short n, size;

    if (a2->size == 0) {
	/*
	 * array - ({ })
	 * Return a copy of the first array.
	 */
	a3 = arr_new(data, (long) a1->size);
	i_copy(a3->elts, d_get_elts(a1), a1->size);
	d_ref_imports(a3);
	return a3;
    }

    /* create new array */
    a3 = arr_new(data, (long) a1->size);
    if (a3->size == 0) {
	/* subtract from empty array */
	return a3;
    }
    size = a2->size;

    /* copy and sort values of subtrahend */
    copytmp(data, v2 = ALLOCA(Value, size), a2);
    qsort(v2, size, sizeof(Value), cmp);

    v1 = d_get_elts(a1);
    v3 = a3->elts;
    if (a1->odcount == odcount) {
	for (n = a1->size; n > 0; --n) {
	    if (search(v1, v2, size, 1, FALSE) < 0) {
		/*
		 * not found in subtrahend: copy to result array
		 */
		i_ref_value(v1);
		*v3++ = *v1;
	    }
	    v1++;
	}
    } else {
	a1->odcount = odcount;
	for (n = a1->size; n > 0; --n) {
	    switch (v1->type) {
	    case T_OBJECT:
		if (DESTRUCTED(v1)) {
		    /* replace destructed object by nil */
		    d_assign_elt(a1->primary->data, a1, v1, &nil_value);
		}
		break;

	    case T_LWOBJECT:
		o = d_get_elts(v1->u.array);
		if (o->type == T_OBJECT && DESTRUCTED(o)) {
		    /* replace destructed object by nil */
		    d_assign_elt(a1->primary->data, a1, v1, &nil_value);
		}
		break;
	    }
	    if (search(v1, v2, size, 1, FALSE) < 0) {
		/*
		 * not found in subtrahend: copy to result array
		 */
		i_ref_value(v1);
		*v3++ = *v1;
	    }
	    v1++;
	}
    }
    AFREE(v2);	/* free copy of values of subtrahend */

    a3->size = v3 - a3->elts;
    if (a3->size == 0) {
	FREE(a3->elts);
	a3->elts = (Value *) NULL;
    }

    d_ref_imports(a3);
    return a3;
}

/*
 * NAME:	Array->intersect()
 * DESCRIPTION:	A - (A - B).  If A and B are sets, the result is a set also.
 */
Array *arr_intersect(Dataspace *data, Array *a1, Array *a2)
{
    Value *v1, *v2, *v3, *o;
    Array *a3;
    unsigned short n, size;

    if (a1->size == 0 || a2->size == 0) {
	/* array & ({ }) */
	return arr_new(data, 0L);
    }

    /* create new array */
    a3 = arr_new(data, (long) a1->size);
    size = a2->size;

    /* copy and sort values of 2nd array */
    copytmp(data, v2 = ALLOCA(Value, size), a2);
    qsort(v2, size, sizeof(Value), cmp);

    v1 = d_get_elts(a1);
    v3 = a3->elts;
    if (a1->odcount == odcount) {
	for (n = a1->size; n > 0; --n) {
	    if (search(v1, v2, a2->size, 1, FALSE) >= 0) {
		/*
		 * element is in both arrays: copy to result array
		 */
		i_ref_value(v1);
		*v3++ = *v1;
	    }
	    v1++;
	}
    } else {
	a1->odcount = odcount;
	for (n = a1->size; n > 0; --n) {
	    switch (v1->type) {
	    case T_OBJECT:
		if (DESTRUCTED(v1)) {
		    /* replace destructed object by nil */
		    d_assign_elt(a1->primary->data, a1, v1, &nil_value);
		}
		break;

	    case T_LWOBJECT:
		o = d_get_elts(v1->u.array);
		if (o->type == T_OBJECT && DESTRUCTED(o)) {
		    /* replace destructed object by nil */
		    d_assign_elt(a1->primary->data, a1, v1, &nil_value);
		}
		break;
	    }
	    if (search(v1, v2, a2->size, 1, FALSE) >= 0) {
		/*
		 * element is in both arrays: copy to result array
		 */
		i_ref_value(v1);
		*v3++ = *v1;
	    }
	    v1++;
	}
    }
    AFREE(v2);	/* free copy of values of 2nd array */

    a3->size = v3 - a3->elts;
    if (a3->size == 0) {
	FREE(a3->elts);
	a3->elts = (Value *) NULL;
    }

    d_ref_imports(a3);
    return a3;
}

/*
 * NAME:	Array->setadd()
 * DESCRIPTION:	A + (B - A).  If A and B are sets, the result is a set also.
 */
Array *arr_setadd(Dataspace *data, Array *a1, Array *a2)
{
    Value *v, *v1, *v2, *o;
    Value *v3;
    Array *a3;
    unsigned short n, size;

    if (a1->size == 0) {
	/* ({ }) | array */
	a3 = arr_new(data, (long) a2->size);
	i_copy(a3->elts, d_get_elts(a2), a2->size);
	d_ref_imports(a3);
	return a3;
    }
    if (a2->size == 0) {
	/* array | ({ }) */
	a3 = arr_new(data, (long) a1->size);
	i_copy(a3->elts, d_get_elts(a1), a1->size);
	d_ref_imports(a3);
	return a3;
    }

    /* make room for elements to add */
    v3 = ALLOCA(Value, a2->size);

    /* copy and sort values of 1st array */
    copytmp(data, v1 = ALLOCA(Value, size = a1->size), a1);
    qsort(v1, size, sizeof(Value), cmp);

    v = v3;
    v2 = d_get_elts(a2);
    if (a2->odcount == odcount) {
	for (n = a2->size; n > 0; --n) {
	    if (search(v2, v1, size, 1, FALSE) < 0) {
		/*
		 * element is only in second array: copy to result array
		 */
		*v++ = *v2;
	    }
	    v2++;
	}
    } else {
	a2->odcount = odcount;
	for (n = a2->size; n > 0; --n) {
	    switch (v2->type) {
	    case T_OBJECT:
		if (DESTRUCTED(v2)) {
		    /* replace destructed object by nil */
		    d_assign_elt(a2->primary->data, a2, v2, &nil_value);
		}
		break;

	    case T_LWOBJECT:
		o = d_get_elts(v2->u.array);
		if (o->type == T_OBJECT && DESTRUCTED(o)) {
		    /* replace destructed object by nil */
		    d_assign_elt(a2->primary->data, a2, v2, &nil_value);
		}
		break;
	    }
	    if (search(v2, v1, size, 1, FALSE) < 0) {
		/*
		 * element is only in second array: copy to result array
		 */
		*v++ = *v2;
	    }
	    v2++;
	}
    }
    AFREE(v1);	/* free copy of values of 1st array */

    n = v - v3;
    if ((long) size + n > max_size) {
	AFREE(v3);
	error("Array too large");
    }

    a3 = arr_new(data, (long) size + n);
    i_copy(a3->elts, a1->elts, size);
    i_copy(a3->elts + size, v3, n);
    AFREE(v3);

    d_ref_imports(a3);
    return a3;
}

/*
 * NAME:	Array->setxadd()
 * DESCRIPTION:	(A - B) + (B - A).  If A and B are sets, the result is a set
 *		also.
 */
Array *arr_setxadd(Dataspace *data, Array *a1, Array *a2)
{
    Value *v, *w, *v1, *v2;
    Value *v3;
    Array *a3;
    unsigned short n, size;
    unsigned short num;

    if (a1->size == 0) {
	/* ({ }) ^ array */
	a3 = arr_new(data, (long) a2->size);
	i_copy(a3->elts, d_get_elts(a2), a2->size);
	d_ref_imports(a3);
	return a3;
    }
    if (a2->size == 0) {
	/* array ^ ({ }) */
	a3 = arr_new(data, (long) a1->size);
	i_copy(a3->elts, d_get_elts(a1), a1->size);
	d_ref_imports(a3);
	return a3;
    }

    /* copy values of 1st array */
    copytmp(data, v1 = ALLOCA(Value, size = a1->size), a1);

    /* copy and sort values of 2nd array */
    copytmp(data, v2 = ALLOCA(Value, size = a2->size), a2);
    qsort(v2, size, sizeof(Value), cmp);

    /* room for first half of result */
    v3 = ALLOCA(Value, a1->size);

    v = v3;
    w = v1;
    for (n = a1->size; n > 0; --n) {
	if (search(v1, v2, size, 1, FALSE) < 0) {
	    /*
	     * element is only in first array: copy to result array
	     */
	    *v++ = *v1;
	} else {
	    /*
	     * element is in both: keep it for the next round
	     */
	    *w++ = *v1;
	}
	v1++;
    }
    num = v - v3;

    /* sort copy of 1st array */
    v1 -= a1->size;
    qsort(v1, size = w - v1, sizeof(Value), cmp);

    v = v2;
    w = a2->elts;
    for (n = a2->size; n > 0; --n) {
	if (search(w, v1, size, 1, FALSE) < 0) {
	    /*
	     * element is only in second array: copy to 2nd result array
	     */
	    *v++ = *w;
	}
	w++;
    }

    n = v - v2;
    if ((long) num + n > max_size) {
	AFREE(v3);
	AFREE(v2);
	AFREE(v1);
	error("Array too large");
    }

    a3 = arr_new(data, (long) num + n);
    i_copy(a3->elts, v3, num);
    i_copy(a3->elts + num, v2, n);
    AFREE(v3);
    AFREE(v2);
    AFREE(v1);

    d_ref_imports(a3);
    return a3;
}

/*
 * NAME:	Array->index()
 * DESCRIPTION:	index an array
 */
unsigned short arr_index(Array *a, long l)
{
    if (l < 0 || l >= (long) a->size) {
	error("Array index out of range");
    }
    return l;
}

/*
 * NAME:	Array->ckrange()
 * DESCRIPTION:	check an array subrange
 */
void arr_ckrange(Array *a, long l1, long l2)
{
    if (l1 < 0 || l1 > l2 + 1 || l2 >= (long) a->size) {
	error("Invalid array range");
    }
}

/*
 * NAME:	Array->range()
 * DESCRIPTION:	return a subrange of an array
 */
Array *arr_range(Dataspace *data, Array *a, long l1, long l2)
{
    Array *range;

    if (l1 < 0 || l1 > l2 + 1 || l2 >= (long) a->size) {
	error("Invalid array range");
    }

    range = arr_new(data, l2 - l1 + 1);
    i_copy(range->elts, d_get_elts(a) + l1, (unsigned short) (l2 - l1 + 1));
    d_ref_imports(range);
    return range;
}


/*
 * NAME:	mapping->new()
 * DESCRIPTION:	create a new mapping
 */
Array *map_new(Dataspace *data, long size)
{
    Array *m;

    if (size > max_size << 1) {
	error("Mapping too large");
    }
    m = arr_alloc((unsigned short) size);
    if (size > 0) {
	m->elts = ALLOC(Value, size);
    }
    m->tag = tag++;
    m->odcount = odcount;
    m->primary = &data->plane->alocal;
    m->prev = &data->alist;
    m->next = data->alist.next;
    m->next->prev = m;
    data->alist.next = m;
    return m;
}

/*
 * NAME:	mapping->sort()
 * DESCRIPTION:	prune and sort a mapping
 */
void map_sort(Array *m)
{
    unsigned short i, sz;
    Value *v, *w;

    for (i = m->size, sz = 0, v = w = m->elts; i > 0; i -= 2) {
	if (!VAL_NIL(v + 1)) {
	    *w++ = *v++;
	    *w++ = *v++;
	    sz += 2;
	} else {
	    /* delete index and skip zero value */
	    i_del_value(v);
	    v += 2;
	}
    }

    if (sz != 0) {
	qsort(v = m->elts, i = sz >> 1, 2 * sizeof(Value), cmp);
	while (--i != 0) {
	    if (cmp((cvoid *) v, (cvoid *) &v[2]) == 0 &&
		(!T_INDEXED(v->type) || v->u.array == v[2].u.array)) {
		error("Identical indices in mapping");
	    }
	    v += 2;
	}
    } else if (m->size > 0) {
	FREE(m->elts);
	m->elts = (Value *) NULL;
    }
    m->size = sz;
}

/*
 * NAME:	mapping->dehash()
 * DESCRIPTION:	commit changes from the hash table to the array part
 */
static void map_dehash(Dataspace *data, Array *m, bool clean)
{
    unsigned short size, i, j;
    Value *v1, *v2, *v3;
    mapelt *e, **t, **p;

    if (clean && m->size != 0) {
	/*
	 * remove destructed objects from array part
	 */
	size = 0;
	v1 = v2 = d_get_elts(m);
	for (i = m->size; i > 0; i -= 2) {
	    switch (v2->type) {
	    case T_OBJECT:
		if (DESTRUCTED(v2)) {
		    /*
		     * index is destructed object
		     */
		    d_assign_elt(data, m, v2 + 1, &nil_value);
		    v2 += 2;
		    continue;
		}
		break;

	    case T_LWOBJECT:
		v3 = d_get_elts(v2->u.array);
		if (v3->type == T_OBJECT && DESTRUCTED(v3)) {
		    /*
		     * index is destructed object
		     */
		    d_assign_elt(data, m, v2++, &nil_value);
		    d_assign_elt(data, m, v2++, &nil_value);
		    continue;
		}
		break;
	    }
	    switch (v2[1].type) {
	    case T_OBJECT:
		if (DESTRUCTED(&v2[1])) {
		    /*
		     * value is destructed object
		     */
		    d_assign_elt(data, m, v2, &nil_value);
		    v2 += 2;
		    continue;
		}
		break;

	    case T_LWOBJECT:
		v3 = d_get_elts(v2[1].u.array);
		if (v3->type == T_OBJECT && DESTRUCTED(v3)) {
		    /*
		     * value is destructed object
		     */
		    d_assign_elt(data, m, v2++, &nil_value);
		    d_assign_elt(data, m, v2++, &nil_value);
		    continue;
		}
		break;
	    }

	    *v1++ = *v2++;
	    *v1++ = *v2++;
	    size += 2;
	}

	if (size != m->size) {
	    d_change_map(m);
	    m->size = size;
	    if (size == 0) {
		FREE(m->elts);
		m->elts = (Value *) NULL;
	    }
	}
    }

    if (m->hashmod ||
	(clean && m->hashed != (maphash *) NULL && m->hashed->size != 0)) {
	/*
	 * merge copy of hashtable with sorted array
	 */
	size = m->hashed->size;
	v2 = ALLOCA(Value, size << 1);
	t = m->hashed->table;
	if (clean) {
	    for (i = size, size = j = 0; i > 0; ) {
		for (p = t++; (e=*p) != (mapelt *) NULL; --i) {
		    switch (e->idx.type) {
		    case T_OBJECT:
			if (DESTRUCTED(&e->idx)) {
			    /*
			     * index is destructed object
			     */
			    if (e->add) {
				d_assign_elt(data, m, &e->val, &nil_value);
			    }
			    *p = e->next;
			    echunk.del(e);
			    continue;
			}
			break;

		    case T_LWOBJECT:
			v3 = d_get_elts(e->idx.u.array);
			if (v3->type == T_OBJECT && DESTRUCTED(v3)) {
			    /*
			     * index is destructed object
			     */
			    if (e->add) {
				d_assign_elt(data, m, &e->idx, &nil_value);
				d_assign_elt(data, m, &e->val, &nil_value);
			    }
			    *p = e->next;
			    echunk.del(e);
			    continue;
			}
			break;
		    }
		    switch (e->val.type) {
		    case T_OBJECT:
			if (DESTRUCTED(&e->val)) {
			    /*
			     * value is destructed object
			     */
			    if (e->add) {
				d_assign_elt(data, m, &e->idx, &nil_value);
			    }
			    *p = e->next;
			    echunk.del(e);
			    continue;
			}
			break;

		    case T_LWOBJECT:
			v3 = d_get_elts(e->val.u.array);
			if (v3->type == T_OBJECT && DESTRUCTED(v3)) {
			    /*
			     * value is destructed object
			     */
			    if (e->add) {
				d_assign_elt(data, m, &e->idx, &nil_value);
				d_assign_elt(data, m, &e->val, &nil_value);
			    }
			    *p = e->next;
			    echunk.del(e);
			    continue;
			}
			break;
		    }

		    if (e->add) {
			e->add = FALSE;
			*v2++ = e->idx;
			*v2++ = e->val;
			size++;
		    }
		    j++;

		    p = &e->next;
		}
	    }

	    if (j != m->hashed->size) {
		m->hashed->size = j;
		d_change_map(m);
	    }
	} else {
	    size = m->hashed->sizemod;
	    for (i = size; i > 0; ) {
		for (e = *t++; e != (mapelt *) NULL; e = e->next) {
		    if (e->add) {
			e->add = FALSE;
			*v2++ = e->idx;
			*v2++ = e->val;
			if (--i == 0) {
			    break;
			}
		    }
		}
	    }
	}
	m->hashed->sizemod = 0;
	m->hashmod = FALSE;

	if (size != 0) {
	    size <<= 1;
	    qsort(v2 -= size, size >> 1, sizeof(Value) << 1, cmp);

	    /*
	     * merge the two value arrays
	     */
	    v1 = m->elts;
	    v3 = ALLOC(Value, m->size + size);
	    for (i = m->size, j = size; i > 0 && j > 0; ) {
		if (cmp(v1, v2) <= 0) {
		    *v3++ = *v1++;
		    *v3++ = *v1++;
		    i -= 2;
		} else {
		    *v3++ = *v2++;
		    *v3++ = *v2++;
		    j -= 2;
		}
	    }

	    /*
	     * copy tails of arrays
	     */
	    memcpy(v3, v1, i * sizeof(Value));
	    v3 += i;
	    memcpy(v3, v2, j * sizeof(Value));
	    v3 += j;

	    v2 -= (size - j);
	    if (m->size > 0) {
		FREE(m->elts);
	    }
	    m->size += size;
	    m->elts = v3 - m->size;
	}

	AFREE(v2);
    }
}

/*
 * NAME:	map->rmhash()
 * DESCRIPTION:	delete hash table of mapping
 */
void map_rmhash(Array *m)
{
    if (m->hashed != (maphash *) NULL) {
	unsigned short i;
	mapelt *e, *n, **t;

	if (m->hashmod) {
	    map_dehash(m->primary->data, m, FALSE);
	}
	for (i = m->hashed->size, t = m->hashed->table; i > 0; t++) {
	    for (e = *t; e != (mapelt *) NULL; e = n) {
		n = e->next;
		echunk.del(e);
		--i;
	    }
	}
	FREE(m->hashed);
	m->hashed = (maphash *) NULL;
    }
}

/*
 * NAME:	mapping->compact()
 * DESCRIPTION:	compact a mapping: copy new elements from the hash table into
 *		the array, and remove destructed objects
 */
void map_compact(Dataspace *data, Array *m)
{
    if (m->hashmod || m->odcount != odcount) {
	if (m->hashmod &&
	    (!THISPLANE(m->primary) || !SAMEPLANE(data, m->primary->data))) {
	    map_dehash(data, m, FALSE);
	}

	map_dehash(data, m, TRUE);
	m->odcount = odcount;
    }
}

/*
 * NAME:	mapping->size()
 * DESCRIPTION:	return the size of a mapping
 */
unsigned short map_size(Dataspace *data, Array *m)
{
    map_compact(data, m);
    return m->size >> 1;
}

/*
 * NAME:	mapping->add()
 * DESCRIPTION:	add two mappings
 */
Array *map_add(Dataspace *data, Array *m1, Array *m2)
{
    Value *v1, *v2, *v3;
    unsigned short n1, n2;
    Int c;
    Array *m3;

    map_compact(data, m1);
    map_compact(data, m2);
    m3 = map_new(data, (long) m1->size + m2->size);
    if (m3->size == 0) {
	/* add two empty mappings */
	return m3;
    }

    v1 = m1->elts;
    v2 = m2->elts;
    v3 = m3->elts;
    for (n1 = m1->size, n2 = m2->size; n1 > 0 && n2 > 0; ) {
	c = cmp(v1, v2);
	if (c < 0) {
	    /* the smaller element is in m1 */
	    i_copy(v3, v1, 2);
	    v1 += 2; v3 += 2; n1 -= 2;
	} else {
	    /* the smaller - or overriding - element is in m2 */
	    i_copy(v3, v2, 2);
	    v3 += 2;
	    if (c == 0) {
		/* equal elements? */
		if (T_INDEXED(v1->type) && v1->u.array != v2->u.array) {
		    Value *v;
		    unsigned short n;

		    /*
		     * The array tags are the same, but the arrays are not.
		     * Check ahead to see if the array is somewhere else
		     * in m2; if not, copy the element from m1 as well.
		     */
		    v = v2; n = n2;
		    for (;;) {
			v += 2; n -= 2;
			if (n == 0 || !T_INDEXED(v->type) ||
			    v->u.array->tag != v1->u.array->tag) {
			    /* not in m2 */
			    i_copy(v3, v1, 2);
			    v3 += 2;
			    break;
			}
			if (v->u.array == v1->u.array) {
			    /* also in m2 */
			    break;
			}
		    }
		}
		/* skip m1 */
		v1 += 2; n1 -= 2;
	    }
	    v2 += 2; n2 -= 2;
	}
    }

    /* copy tail part of m1 */
    i_copy(v3, v1, n1);
    v3 += n1;
    /* copy tail part of m2 */
    i_copy(v3, v2, n2);
    v3 += n2;

    m3->size = v3 - m3->elts;
    if (m3->size == 0) {
	FREE(m3->elts);
	m3->elts = (Value *) NULL;
    }

    d_ref_imports(m3);
    return m3;
}

/*
 * NAME:	mapping->sub()
 * DESCRIPTION:	subtract an array from a mapping
 */
Array *map_sub(Dataspace *data, Array *m1, Array *a2)
{
    Value *v1, *v2, *v3;
    unsigned short n1, n2, size;
    Int c;
    Array *m3;

    map_compact(data, m1);
    m3 = map_new(data, (long) m1->size);
    if (m1->size == 0) {
	/* subtract from empty mapping */
	return m3;
    }
    if ((size=a2->size) == 0) {
	/* subtract empty array */
	i_copy(m3->elts, m1->elts, m1->size);
	d_ref_imports(m3);
	return m3;
    }

    /* copy and sort values of array */
    copytmp(data, v2 = ALLOCA(Value, size), a2);
    qsort(v2, size, sizeof(Value), cmp);

    v1 = m1->elts;
    v3 = m3->elts;
    for (n1 = m1->size, n2 = size; n1 > 0 && n2 > 0; ) {
	c = cmp(v1, v2);
	if (c < 0) {
	    /* the smaller element is in m1 */
	    i_copy(v3, v1, 2);
	    v1 += 2; v3 += 2; n1 -= 2;
	} else if (c > 0) {
	    /* the smaller element is in a2 */
	    v2++; --n2;
	} else {
	    /* equal elements? */
	    if (T_INDEXED(v1->type) && v1->u.array != v2->u.array) {
		Value *v;
		unsigned short n;

		/*
		 * The array tags are the same, but the arrays are not.
		 * Check ahead to see if the array is somewhere else
		 * in a2; if not, copy the element from m1.
		 */
		v = v2; n = n2;
		for (;;) {
		    v++; --n;
		    if (n == 0 || !T_INDEXED(v->type) ||
			v->u.array->tag != v1->u.array->tag) {
			/* not in a2 */
			i_copy(v3, v1, 2);
			v3 += 2;
			break;
		    }
		    if (v->u.array == v1->u.array) {
			/* also in a2 */
			break;
		    }
		}
	    }
	    /* skip m1 */
	    v1 += 2; n1 -= 2;
	}
    }
    AFREE(v2 - (size - n2));

    /* copy tail part of m1 */
    i_copy(v3, v1, n1);
    v3 += n1;

    m3->size = v3 - m3->elts;
    if (m3->size == 0) {
	FREE(m3->elts);
	m3->elts = (Value *) NULL;
    }

    d_ref_imports(m3);
    return m3;
}

/*
 * NAME:	mapping->intersect()
 * DESCRIPTION:	intersect a mapping with an array
 */
Array *map_intersect(Dataspace *data, Array *m1, Array *a2)
{
    Value *v1, *v2, *v3;
    unsigned short n1, n2, size;
    Int c;
    Array *m3;

    map_compact(data, m1);
    if ((size=a2->size) == 0) {
	/* intersect with empty array */
	return map_new(data, 0L);
    }
    m3 = map_new(data, (long) m1->size);
    if (m1->size == 0) {
	/* intersect with empty mapping */
	return m3;
    }

    /* copy and sort values of array */
    copytmp(data, v2 = ALLOCA(Value, size), a2);
    qsort(v2, size, sizeof(Value), cmp);

    v1 = m1->elts;
    v3 = m3->elts;
    for (n1 = m1->size, n2 = size; n1 > 0 && n2 > 0; ) {
	c = cmp(v1, v2);
	if (c < 0) {
	    /* the smaller element is in m1 */
	    v1 += 2; n1 -= 2;
	} else if (c > 0) {
	    /* the smaller element is in a2 */
	    v2++; --n2;
	} else {
	    /* equal elements? */
	    if (T_INDEXED(v1->type) && v1->u.array != v2->u.array) {
		Value *v;
		unsigned short n;

		/*
		 * The array tags are the same, but the arrays are not.
		 * Check ahead to see if the array is somewhere else
		 * in a2; if not, don't copy the element from m1.
		 */
		v = v2; n = n2;
		for (;;) {
		    v++; --n;
		    if (n == 0 || !T_INDEXED(v->type) ||
			v->u.array->tag != v1->u.array->tag) {
			/* not in a2 */
			break;
		    }
		    if (v->u.array == v1->u.array) {
			/* also in a2 */
			i_copy(v3, v1, 2);
			v3 += 2; v1 += 2; n1 -= 2;
			break;
		    }
		}
	    } else {
		/* equal */
		i_copy(v3, v1, 2);
		v3 += 2; v1 += 2; n1 -= 2;
	    }
	    v2++; --n2;
	}
    }
    AFREE(v2 - (size - n2));

    m3->size = v3 - m3->elts;
    if (m3->size == 0) {
	FREE(m3->elts);
	m3->elts = (Value *) NULL;
    }

    d_ref_imports(m3);
    return m3;
}

/*
 * NAME:	mapping->grow()
 * DESCRIPTION:	add an element to a mapping
 */
static mapelt *map_grow(Dataspace *data, Array *m, Uint hashval, bool add)
{
    maphash *h;
    mapelt *e;
    Uint i;

    h = m->hashed;
    if (add &&
	(m->size >> 1) + ((h == (maphash *) NULL) ? 0 : h->sizemod) >= max_size)
    {
	map_compact(data, m);
	if (m->size >> 1 >= max_size) {
	    error("Mapping too large to grow");
	}
    }

    if (h == (maphash *) NULL) {
	/*
	 * add hash table to this mapping
	 */
	m->hashed = h = (maphash *)
	    ALLOC(char, sizeof(maphash) + (MTABLE_SIZE - 1) * sizeof(mapelt*));
	h->size = 0;
	h->sizemod = 0;
	h->tablesize = MTABLE_SIZE;
	memset(h->table, '\0', MTABLE_SIZE * sizeof(mapelt*));
    } else if (h->size << 2 >= h->tablesize * 3) {
	mapelt *n, **t;
	unsigned short j;

	/*
	 * extend hash table for this mapping
	 */
	i = h->tablesize << 1;
	h = (maphash *) ALLOC(char,
			      sizeof(maphash) + (i - 1) * sizeof(mapelt*));
	h->size = m->hashed->size;
	h->sizemod = m->hashed->sizemod;
	h->tablesize = i;
	memset(h->table, '\0', i * sizeof(mapelt*));
	/*
	 * copy entries from old hashtable to new hashtable
	 */
	for (j = h->size, t = m->hashed->table; j > 0; t++) {
	    for (e = *t; e != (mapelt *) NULL; e = n) {
		n = e->next;
		i = e->hashval % h->tablesize;
		e->next = h->table[i];
		h->table[i] = e;
		--j;
	    }
	}
	FREE(m->hashed);
	m->hashed = h;
    }
    h->size++;

    e = echunk.alloc();
    e->hashval = hashval;
    e->add = FALSE;
    e->idx = nil_value;
    e->val = nil_value;
    i = hashval % h->tablesize;
    e->next = h->table[i];
    h->table[i] = e;

    return e;
}

/*
 * NAME:	mapping->index()
 * DESCRIPTION:	Index a mapping with a value. If a third argument is supplied,
 *		perform an assignment; otherwise return the indexed value.
 */
Value *map_index(Dataspace *data, Array *m, Value *val, Value *elt,
		 Value *verify)
{
    Uint i;
    mapelt *e, **p;
    bool del, add, hash;

    i = 0;

    if (elt != (Value *) NULL && VAL_NIL(elt)) {
	elt = (Value *) NULL;
	del = TRUE;
    } else {
	del = FALSE;
    }

    if (m->hashmod &&
	(!THISPLANE(m->primary) || !SAMEPLANE(data, m->primary->data))) {
	map_dehash(data, m, FALSE);
    }

    switch (val->type) {
    case T_NIL:
	i = 4747;
	break;

    case T_INT:
	i = val->u.number;
	break;

    case T_FLOAT:
	i = VFLT_HASH(val);
	break;

    case T_STRING:
	i = Hashtab::hashstr(val->u.string->text, STRMAPHASHSZ) ^
							    val->u.string->len;
	break;

    case T_OBJECT:
	i = val->oindex;
	break;

    case T_ARRAY:
    case T_MAPPING:
    case T_LWOBJECT:
	i = (unsigned short) ((uintptr_t) val->u.array >> 3);
	break;
    }

    hash = FALSE;
    if (m->hashed != (maphash *) NULL) {
	for (p = &m->hashed->table[i % m->hashed->tablesize];
	     (e=*p) != (mapelt *) NULL; p = &e->next) {
	    if (cmp(val, &e->idx) == 0 &&
		(!T_INDEXED(val->type) || val->u.array == e->idx.u.array)) {
		/*
		 * found in the hashtable
		 */
		hash = TRUE;
		if (elt != (Value *) NULL &&
		    (verify == (Value *) NULL ||
		     (e->val.type == T_STRING &&
		      e->val.u.string == verify->u.string))) {
		    /*
		     * change element
		     */
		    if (val->type == T_OBJECT) {
			e->idx.u.objcnt = val->u.objcnt;	/* refresh */
		    }
		    if (e->add) {
			d_assign_elt(data, m, &e->val, elt);
		    } else {
			/* "real" assignment later in array part */
			e->val = *elt;
			break;
		    }
		} else if (del ||
			   (val->type == T_OBJECT &&
			    val->u.objcnt != e->idx.u.objcnt)) {
		    /*
		     * delete element
		     */
		    add = e->add;
		    if (add) {
			d_assign_elt(data, m, &e->idx, &nil_value);
			d_assign_elt(data, m, &e->val, &nil_value);
			if (--m->hashed->sizemod == 0) {
			    m->hashmod = FALSE;
			}
		    }

		    *p = e->next;
		    echunk.del(e);
		    m->hashed->size--;

		    if (!add) {
			break;		/* change array part also */
		    }
		    return &nil_value;
		}
		return &e->val;
	    }
	}
    }

    add = TRUE;
    if (m->size > 0) {
	int n;
	Value *v;

	n = search(val, d_get_elts(m), m->size, 2, FALSE);
	if (n >= 0) {
	    /*
	     * found in the array
	     */
	    v = &m->elts[n];
	    if (elt != (Value *) NULL &&
		(verify == (Value *) NULL ||
		 (v[1].type == T_STRING && v[1].u.string == verify->u.string)))
	    {
		/*
		 * change the element
		 */
		d_assign_elt(data, m, v + 1, elt);
		if (val->type == T_OBJECT) {
		    v->modified = TRUE;
		    v->u.objcnt = val->u.objcnt;	/* refresh */
		}
	    } else if (del ||
		       (val->type == T_OBJECT &&
			val->u.objcnt != v->u.objcnt)) {
		/*
		 * delete the element
		 */
		d_assign_elt(data, m, v, &nil_value);
		d_assign_elt(data, m, v + 1, &nil_value);

		m->size -= 2;
		if (m->size == 0) {
		    /* last element removed */
		    FREE(m->elts);
		    m->elts = (Value *) NULL;
		} else {
		    /* move tail */
		    memmove(v, v + 2, (m->size - n) * sizeof(Value));
		}
		d_change_map(m);
		return &nil_value;
	    }
	    val = v;
	    elt = v + 1;
	    add = FALSE;
	}
    }

    if (elt == (Value *) NULL) {
	return &nil_value;	/* not found */
    }

    if (!hash) {
	/*
	 * extend mapping
	 */
	e = map_grow(data, m, i, add);
	if (add) {
	    e->add = TRUE;
	    d_assign_elt(data, m, &e->idx, val);
	    d_assign_elt(data, m, &e->val, elt);
	    m->hashed->sizemod++;
	    m->hashmod = TRUE;
	    d_change_map(m);
	} else {
	    e->idx = *val;
	    e->val = *elt;
	}
    }

    return elt;
}

/*
 * NAME:	mapping->range()
 * DESCRIPTION:	return a mapping value subrange
 */
Array *map_range(Dataspace *data, Array *m, Value *v1, Value *v2)
{
    unsigned short from, to;
    Array *range;

    map_compact(data, m);

    /* determine subrange */
    from = (v1 == (Value *) NULL) ? 0 : search(v1, m->elts, m->size, 2, TRUE);
    if (v2 == (Value *) NULL) {
	to = m->size;
    } else {
	to = search(v2, m->elts, m->size, 2, TRUE);
	if (to < m->size && cmp(v2, &m->elts[to]) == 0 &&
	    (!T_INDEXED(v2->type) || v2->u.array == m->elts[to].u.array)) {
	    /*
	     * include last element
	     */
	    to += 2;
	}
    }
    if (from >= to) {
	return map_new(data, 0L);	/* empty subrange */
    }

    /* copy subrange */
    range = map_new(data, (long) (to -= from));
    i_copy(range->elts, m->elts + from, to);

    d_ref_imports(range);
    return range;
}

/*
 * NAME:	mapping->indices()
 * DESCRIPTION:	return the indices of a mapping
 */
Array *map_indices(Dataspace *data, Array *m)
{
    Array *indices;
    Value *v1, *v2;
    unsigned short n;

    map_compact(data, m);
    indices = arr_new(data, (long) (n = m->size >> 1));
    v1 = indices->elts;
    for (v2 = m->elts; n > 0; v2 += 2, --n) {
	i_ref_value(v2);
	*v1++ = *v2;
    }

    d_ref_imports(indices);
    return indices;
}

/*
 * NAME:	mapping->values()
 * DESCRIPTION:	return the values of a mapping
 */
Array *map_values(Dataspace *data, Array *m)
{
    Array *values;
    Value *v1, *v2;
    unsigned short n;

    map_compact(data, m);
    values = arr_new(data, (long) (n = m->size >> 1));
    v1 = values->elts;
    for (v2 = m->elts + 1; n > 0; v2 += 2, --n) {
	i_ref_value(v2);
	*v1++ = *v2;
    }

    d_ref_imports(values);
    return values;
}


/*
 * NAME:	lwobject->new()
 * DESCRIPTION:	create a new light-weight object
 */
Array *lwo_new(Dataspace *data, Object *obj)
{
    Control *ctrl;
    Array *a;
    Float flt;

    o_lwobj(obj);
    ctrl = o_control(obj);
    a = arr_alloc(ctrl->nvariables + 2);
    a->elts = ALLOC(Value, ctrl->nvariables + 2);
    PUT_OBJVAL(&a->elts[0], obj);
    flt.high = FALSE;
    flt.low = obj->update;
    PUT_FLTVAL(&a->elts[1], flt);
    d_new_variables(ctrl, a->elts + 2);
    a->tag = tag++;
    a->odcount = odcount;
    a->primary = &data->plane->alocal;
    a->prev = &data->alist;
    a->next = data->alist.next;
    a->next->prev = a;
    data->alist.next = a;
    return a;
}

/*
 * NAME:	lwobject->copy()
 * DESCRIPTION:	copy a light-weight object
 */
Array *lwo_copy(Dataspace *data, Array *a)
{
    Array *copy;

    copy = arr_alloc(a->size);
    i_copy(copy->elts = ALLOC(Value, a->size), a->elts, a->size);
    copy->tag = tag++;
    copy->odcount = odcount;
    copy->primary = &data->plane->alocal;
    copy->prev = &data->alist;
    copy->next = data->alist.next;
    copy->next->prev = copy;
    data->alist.next = copy;
    d_ref_imports(copy);
    return copy;
}
