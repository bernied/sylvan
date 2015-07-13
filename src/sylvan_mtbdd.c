/*
 * Copyright 2011-2015 Formal Methods and Tools, University of Twente
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <sylvan_config.h>

#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomics.h>
// #include <avl.h>
#include <refs.h>
#include <sha2.h>
#include <sylvan.h>
#include <sylvan_common.h>

/**
 * MTBDD node structure
 */
typedef struct __attribute__((packed)) mtbddnode {
    uint64_t a, b;
} * mtbddnode_t; // 16 bytes

#define GETNODE(mtbdd) ((mtbddnode_t)llmsset_index_to_ptr(nodes, mtbdd&0x000000ffffffffff))

/**
 * Complement handling macros
 */
#define MTBDD_HASMARK(s)              (s&mtbdd_complement?1:0)
#define MTBDD_TOGGLEMARK(s)           (s^mtbdd_complement)
#define MTBDD_STRIPMARK(s)            (s&~mtbdd_complement)
#define MTBDD_TRANSFERMARK(from, to)  (to ^ (from & mtbdd_complement))
// Equal under mark
#define MTBDD_EQUALM(a, b)            ((((a)^(b))&(~mtbdd_complement))==0)

// Leaf: a = L=1, M, type; b = value
// Node: a = L=0, C, M, high; b = variable, low
// Only complement edge on "high"

static inline int
mtbddnode_isleaf(mtbddnode_t n)
{
    return n->a & 0x4000000000000000 ? 1 : 0;
}

static inline uint32_t
mtbddnode_gettype(mtbddnode_t n)
{
    return n->a & 0x00000000ffffffff;
}

static inline uint64_t
mtbddnode_getvalue(mtbddnode_t n)
{
    return n->b;
}

static inline int
mtbddnode_getcomp(mtbddnode_t n)
{
    return n->a & 0x8000000000000000 ? 1 : 0;
}

static inline uint64_t
mtbddnode_getlow(mtbddnode_t n)
{
    return n->b & 0x000000ffffffffff; // 40 bits
}

static inline uint64_t
mtbddnode_gethigh(mtbddnode_t n)
{
    return n->a & 0x800000ffffffffff; // 40 bits plus high bit of first
}

static inline uint32_t
mtbddnode_getvariable(mtbddnode_t n)
{
    return (uint32_t)(n->b >> 40);
}

static inline int
mtbddnode_getmark(mtbddnode_t n)
{
    return n->a & 0x2000000000000000 ? 1 : 0;
}

static inline void
mtbddnode_setmark(mtbddnode_t n, int mark)
{
    if (mark) n->a |= 0x2000000000000000;
    else n->a &= 0xdfffffffffffffff;
}

static inline void
mtbddnode_makeleaf(mtbddnode_t n, uint32_t type, uint64_t value)
{
    n->a = 0x4000000000000000 | (uint64_t)type;
    n->b = value;
}

static inline void
mtbddnode_makenode(mtbddnode_t n, uint32_t var, uint64_t low, uint64_t high)
{
    n->a = high;
    n->b = ((uint64_t)var)<<40 | low;
}

/* Primitives */
int
mtbdd_isleaf(MTBDD bdd)
{
    if (bdd == mtbdd_true || bdd == mtbdd_false) return 1;
    return mtbddnode_isleaf(GETNODE(bdd));
}

// for nodes
uint32_t
mtbdd_getvar(MTBDD node)
{
    return mtbddnode_getvariable(GETNODE(node));
}

MTBDD
node_getlow(MTBDD mtbdd, mtbddnode_t node)
{
    return MTBDD_TRANSFERMARK(mtbdd, mtbddnode_getlow(node));
}

MTBDD
node_gethigh(MTBDD mtbdd, mtbddnode_t node)
{
    return MTBDD_TRANSFERMARK(mtbdd, mtbddnode_gethigh(node));
}

MTBDD
mtbdd_getlow(MTBDD mtbdd)
{
    return node_getlow(mtbdd, GETNODE(mtbdd));
}

MTBDD
mtbdd_gethigh(MTBDD mtbdd)
{
    return node_gethigh(mtbdd, GETNODE(mtbdd));
}

// for leafs
uint32_t
mtbdd_gettype(MTBDD leaf)
{
    return mtbddnode_gettype(GETNODE(leaf));
}

uint64_t
mtbdd_getvalue(MTBDD leaf)
{
    return mtbddnode_getvalue(GETNODE(leaf));
}

double
mtbdd_getdouble(MTBDD leaf)
{
    uint64_t value = mtbdd_getvalue(leaf);
    double dv = *(double*)&value;
    if (mtbdd_isnegated(leaf)) return -dv;
    else return dv;
}

/**
 * Implementation of garbage collection
 */

/* Recursively mark MDD nodes as 'in use' */
VOID_TASK_IMPL_1(mtbdd_gc_mark_rec, MDD, mtbdd)
{
    if (mtbdd <= mtbdd_true) return;

    if (llmsset_mark(nodes, mtbdd)) {
        mtbddnode_t n = GETNODE(mtbdd);
        if (!mtbddnode_isleaf(n)) {
            SPAWN(mtbdd_gc_mark_rec, mtbddnode_getlow(n));
            CALL(mtbdd_gc_mark_rec, mtbddnode_gethigh(n));
            SYNC(mtbdd_gc_mark_rec);
        }
    }
}

/**
 * External references
 */

refs_table_t mtbdd_refs;
refs_table_t mtbdd_protected;
static int mtbdd_protected_created = 0;

MDD
mtbdd_ref(MDD a)
{
    if (a == mtbdd_true || a == mtbdd_false) return a;
    refs_up(&mtbdd_refs, a);
    return a;
}

void
mtbdd_deref(MDD a)
{
    if (a == mtbdd_true || a == mtbdd_false) return;
    refs_down(&mtbdd_refs, a);
}

size_t
mtbdd_count_refs()
{
    return refs_count(&mtbdd_refs);
}

void
mtbdd_protect(MTBDD *a)
{
    if (!mtbdd_protected_created) {
        // In C++, sometimes mtbdd_protect is called before Sylvan is initialized. Just create a table.
        protect_create(&mtbdd_protected, 4096);
        mtbdd_protected_created = 1;
    }
    protect_up(&mtbdd_protected, (size_t)a);
}

void
mtbdd_unprotect(MTBDD *a)
{
    protect_down(&mtbdd_protected, (size_t)a);
}

size_t
mtbdd_count_protected()
{
    return protect_count(&mtbdd_protected);
}

/* Called during garbage collection */
VOID_TASK_0(mtbdd_gc_mark_external_refs)
{
    // iterate through refs hash table, mark all found
    size_t count=0;
    uint64_t *it = refs_iter(&mtbdd_refs, 0, mtbdd_refs.refs_size);
    while (it != NULL) {
        SPAWN(mtbdd_gc_mark_rec, refs_next(&mtbdd_refs, &it, mtbdd_refs.refs_size));
        count++;
    }
    while (count--) {
        SYNC(mtbdd_gc_mark_rec);
    }
}

VOID_TASK_0(mtbdd_gc_mark_protected)
{
    // iterate through refs hash table, mark all found
    size_t count=0;
    uint64_t *it = protect_iter(&mtbdd_protected, 0, mtbdd_protected.refs_size);
    while (it != NULL) {
        BDD *to_mark = (BDD*)protect_next(&mtbdd_protected, &it, mtbdd_protected.refs_size);
        SPAWN(mtbdd_gc_mark_rec, *to_mark);
        count++;
    }
    while (count--) {
        SYNC(mtbdd_gc_mark_rec);
    }
}

/* Infrastructure for internal markings */
DECLARE_THREAD_LOCAL(mtbdd_refs_key, mtbdd_refs_internal_t);

VOID_TASK_0(mtbdd_refs_mark_task)
{
    LOCALIZE_THREAD_LOCAL(mtbdd_refs_key, mtbdd_refs_internal_t);
    size_t i, j=0;
    for (i=0; i<mtbdd_refs_key->r_count; i++) {
        if (j >= 40) {
            while (j--) SYNC(mtbdd_gc_mark_rec);
            j=0;
        }
        SPAWN(mtbdd_gc_mark_rec, mtbdd_refs_key->results[i]);
        j++;
    }
    for (i=0; i<mtbdd_refs_key->s_count; i++) {
        Task *t = mtbdd_refs_key->spawns[i];
        if (!TASK_IS_STOLEN(t)) break;
        if (TASK_IS_COMPLETED(t)) {
            if (j >= 40) {
                while (j--) SYNC(mtbdd_gc_mark_rec);
                j=0;
            }
            SPAWN(mtbdd_gc_mark_rec, *(BDD*)TASK_RESULT(t));
            j++;
        }
    }
    while (j--) SYNC(mtbdd_gc_mark_rec);
}

VOID_TASK_0(mtbdd_refs_mark)
{
    TOGETHER(mtbdd_refs_mark_task);
}

VOID_TASK_0(mtbdd_refs_init_task)
{
    mtbdd_refs_internal_t s = (mtbdd_refs_internal_t)malloc(sizeof(struct mtbdd_refs_internal));
    s->r_size = 128;
    s->r_count = 0;
    s->s_size = 128;
    s->s_count = 0;
    s->results = (BDD*)malloc(sizeof(BDD) * 128);
    s->spawns = (Task**)malloc(sizeof(Task*) * 128);
    SET_THREAD_LOCAL(mtbdd_refs_key, s);
}

VOID_TASK_0(mtbdd_refs_init)
{
    INIT_THREAD_LOCAL(mtbdd_refs_key);
    TOGETHER(mtbdd_refs_init_task);
    sylvan_gc_add_mark(10, TASK(mtbdd_refs_mark));
}

/**
 * Initialize and quit functions
 */

static void
mtbdd_quit()
{
    refs_free(&mtbdd_refs);
    if (mtbdd_protected_created) {
        protect_free(&mtbdd_protected);
        mtbdd_protected_created = 0;
    }
}

void
sylvan_init_mtbdd()
{
    sylvan_register_quit(mtbdd_quit);
    sylvan_gc_add_mark(10, TASK(mtbdd_gc_mark_external_refs));
    sylvan_gc_add_mark(10, TASK(mtbdd_gc_mark_protected));

    // Sanity check
    if (sizeof(struct mtbddnode) != 16) {
        fprintf(stderr, "Invalid size of mtbdd nodes: %ld\n", sizeof(struct mtbddnode));
        exit(1);
    }

    refs_create(&mtbdd_refs, 1024);
    if (!mtbdd_protected_created) {
        protect_create(&mtbdd_protected, 4096);
        mtbdd_protected_created = 1;
    }

    LACE_ME;
    CALL(mtbdd_refs_init);
}

/**
 * Primitives
 */
MTBDD
mtbdd_makeleaf(uint32_t type, uint64_t value)
{
    struct mtbddnode n;
    mtbddnode_makeleaf(&n, type, value);

    int created;
    uint64_t index = llmsset_lookup(nodes, n.a, n.b, &created);
    if (index == 0) {
        LACE_ME;

        sylvan_gc();

        index = llmsset_lookup(nodes, n.a, n.b, &created);
        if (index == 0) {
            fprintf(stderr, "BDD Unique table full, %zu of %zu buckets filled!\n", llmsset_count_marked(nodes), llmsset_get_size(nodes));
            exit(1);
        }
    }

    return (MTBDD)index;
}

MTBDD
mtbdd_makenode(uint32_t var, MTBDD low, MTBDD high)
{
    if (low == high) return low;

    // Normalization to keep canonicity
    // low will have no mark

    struct mtbddnode n;
    int mark, created;

    if (MTBDD_HASMARK(low)) {
        mark = 1;
        low = MTBDD_TOGGLEMARK(low);
        high = MTBDD_TOGGLEMARK(high);
    } else {
        mark = 0;
    }

    mtbddnode_makenode(&n, var, low, high);

    MTBDD result;
    uint64_t index = llmsset_lookup(nodes, n.a, n.b, &created);
    if (index == 0) {
        LACE_ME;

        mtbdd_refs_push(low);
        mtbdd_refs_push(high);
        sylvan_gc();
        mtbdd_refs_pop(2);

        index = llmsset_lookup(nodes, n.a, n.b, &created);
        if (index == 0) {
            fprintf(stderr, "BDD Unique table full, %zu of %zu buckets filled!\n", llmsset_count_marked(nodes), llmsset_get_size(nodes));
            exit(1);
        }
    }

    result = index;
    return mark ? result | mtbdd_complement : result;
}

/* Operations */

/**
 * Create leafs of unsigned/signed integers and doubles
 */

MTBDD
mtbdd_uint64(uint64_t value)
{
    return mtbdd_makeleaf(0, value);
}

MTBDD
mtbdd_double(double value)
{
    if (value < 0.0) {
        value = -value;
        return mtbdd_negate(mtbdd_makeleaf(1, *(uint64_t*)&value));
    } else {
        return mtbdd_makeleaf(1, *(uint64_t*)&value);
    }
}

/**
 * Create the cube of variables in arr.
 */
MTBDD
mtbdd_fromarray(uint32_t* arr, size_t length)
{
    if (length == 0) return mtbdd_true;
    else if (length == 1) return mtbdd_makenode(*arr, mtbdd_false, mtbdd_true);
    else return mtbdd_makenode(*arr, mtbdd_false, mtbdd_fromarray(arr+1, length-1));
}

/**
 * Create a MTBDD cube representing the conjunction of variables in their positive or negative
 * form depending on whether the cube[idx] equals 0 (negative), 1 (positive) or 2 (any).
 * Use cube[idx]==3 for "s=s'" in interleaved variables (matches with next variable)
 * <variables> is the cube of variables
 */
MTBDD
mtbdd_cube(MTBDD variables, uint8_t *cube, MTBDD terminal)
{
    if (variables == mtbdd_true) return terminal;
    mtbddnode_t n = GETNODE(variables);

    BDD result;
    switch (*cube) {
    case 0:
        result = mtbdd_cube(node_gethigh(variables, n), cube+1, terminal);
        result = mtbdd_makenode(mtbddnode_getvariable(n), result, mtbdd_false);
        return result;
    case 1:
        result = mtbdd_cube(node_gethigh(variables, n), cube+1, terminal);
        result = mtbdd_makenode(mtbddnode_getvariable(n), mtbdd_false, result);
        return result;
    case 2:
        return mtbdd_cube(node_gethigh(variables, n), cube+1, terminal);
    case 3:
    {
        MTBDD variables2 = node_gethigh(variables, n);
        mtbddnode_t n2 = GETNODE(variables2);
        uint32_t var2 = mtbddnode_getvariable(n2);
        result = mtbdd_cube(node_gethigh(variables2, n2), cube+2, terminal);
        BDD low = mtbdd_makenode(var2, result, mtbdd_false);
        mtbdd_refs_push(low);
        BDD high = mtbdd_makenode(var2, mtbdd_false, result);
        mtbdd_refs_pop(1);
        result = mtbdd_makenode(mtbddnode_getvariable(n), low, high);
        return result;
    }
    default:
        return mtbdd_false; // ?
    }
}

/**
 * Same as mtbdd_cube, but also performs "or" with existing MTBDD,
 * effectively adding an item to the set
 */
TASK_IMPL_4(MTBDD, mtbdd_union_cube, MTBDD, mtbdd, MTBDD, vars, uint8_t*, cube, MTBDD, terminal)
{
    /* Terminal cases */
    if (mtbdd == terminal) return terminal;
    if (mtbdd == mtbdd_false) return mtbdd_cube(vars, cube, terminal);
    if (vars == mtbdd_true) return terminal;

    sylvan_gc_test();

    mtbddnode_t nv = GETNODE(vars);
    uint32_t v = mtbddnode_getvariable(nv);

    mtbddnode_t na = GETNODE(mtbdd);
    uint32_t va = mtbddnode_getvariable(na);

    if (va < v) {
        MTBDD low = node_getlow(mtbdd, na);
        MTBDD high = node_gethigh(mtbdd, na);
        SPAWN(mtbdd_union_cube, high, vars, cube, terminal);
        BDD new_low = mtbdd_union_cube(low, vars, cube, terminal);
        mtbdd_refs_push(new_low);
        BDD new_high = SYNC(mtbdd_union_cube);
        mtbdd_refs_pop(1);
        if (new_low != low || new_high != high) return mtbdd_makenode(va, new_low, new_high);
        else return mtbdd;
    } else if (va == v) {
        MTBDD low = node_getlow(mtbdd, na);
        MTBDD high = node_gethigh(mtbdd, na);
        switch (*cube) {
        case 0:
        {
            MTBDD new_low = mtbdd_union_cube(low, node_gethigh(vars, nv), cube+1, terminal);
            if (new_low != low) return mtbdd_makenode(v, new_low, high);
            else return mtbdd;
        }
        case 1:
        {
            MTBDD new_high = mtbdd_union_cube(high, node_gethigh(vars, nv), cube+1, terminal);
            if (new_high != high) return mtbdd_makenode(v, low, new_high);
            return mtbdd;
        }
        case 2:
        {
            SPAWN(mtbdd_union_cube, high, node_gethigh(vars, nv), cube+1, terminal);
            MTBDD new_low = mtbdd_union_cube(low, node_gethigh(vars, nv), cube+1, terminal);
            mtbdd_refs_push(new_low);
            MTBDD new_high = SYNC(mtbdd_union_cube);
            mtbdd_refs_pop(1);
            if (new_low != low || new_high != high) return mtbdd_makenode(v, new_low, new_high);
            return mtbdd;
        }
        case 3:
        {
            return mtbdd_false; // currently not implemented
        }
        default:
            return mtbdd_false;
        }
    } else /* va > v */ {
        switch (*cube) {
        case 0:
        {
            MTBDD new_low = mtbdd_union_cube(mtbdd, node_gethigh(vars, nv), cube+1, terminal);
            return mtbdd_makenode(v, new_low, mtbdd_false);
        }
        case 1:
        {
            MTBDD new_high = mtbdd_union_cube(mtbdd, node_gethigh(vars, nv), cube+1, terminal);
            return mtbdd_makenode(v, mtbdd_false, new_high);
        }
        case 2:
        {
            SPAWN(mtbdd_union_cube, mtbdd, node_gethigh(vars, nv), cube+1, terminal);
            MTBDD new_low = mtbdd_union_cube(mtbdd, node_gethigh(vars, nv), cube+1, terminal);
            mtbdd_refs_push(new_low);
            MTBDD new_high = SYNC(mtbdd_union_cube);
            mtbdd_refs_pop(1);
            return mtbdd_makenode(v, new_low, new_high);
        }
        case 3:
        {
            return mtbdd_false; // currently not implemented
        }
        default:
            return mtbdd_false;
        }
    }
}

/**
 * Helper function for recursive unmarking
 */
static void
mtbdd_unmark_rec(MTBDD mtbdd)
{
    mtbddnode_t n = GETNODE(mtbdd);
    if (!mtbddnode_getmark(n)) return;
    mtbddnode_setmark(n, 0);
    if (mtbddnode_isleaf(n)) return;
    mtbdd_unmark_rec(mtbddnode_getlow(n));
    mtbdd_unmark_rec(mtbddnode_gethigh(n));
}

/**
 * Count number of nodes in MTBDD
 */

static size_t
mtbdd_nodecount_mark(MTBDD mtbdd)
{
    if (mtbdd <= mtbdd_true) return 0; // do not count true/false leaf
    mtbddnode_t n = GETNODE(mtbdd);
    if (mtbddnode_getmark(n)) return 0;
    mtbddnode_setmark(n, 1);
    if (mtbddnode_isleaf(n)) return 1; // count leaf as 1
    return 1 + mtbdd_nodecount_mark(mtbddnode_getlow(n)) + mtbdd_nodecount_mark(mtbddnode_gethigh(n));
}

size_t
mtbdd_nodecount(MTBDD mtbdd)
{
    size_t result = mtbdd_nodecount_mark(mtbdd);
    mtbdd_unmark_rec(mtbdd);
    return result;
}

/**
 * Export to .dot file
 */

static void
mtbdd_fprintdot_rec(FILE *out, MTBDD mtbdd, print_terminal_label_cb cb)
{
    mtbddnode_t n = GETNODE(mtbdd); // also works for mtbdd_false
    if (mtbddnode_getmark(n)) return;
    mtbddnode_setmark(n, 1);

    if (mtbdd == mtbdd_true || mtbdd == mtbdd_false) {
        fprintf(out, "0 [shape=box, style=filled, label=\"F\"];\n");
    } else if (mtbddnode_isleaf(n)) {
        uint32_t type = mtbddnode_gettype(n);
        uint64_t value = mtbddnode_getvalue(n);
        fprintf(out, "%" PRIu64 " [shape=box, style=filled, label=\"", MTBDD_STRIPMARK(mtbdd));
        switch (type) {
        case 0:
            fprintf(out, "%" PRIu64, value);
            break;
        case 1:
            fprintf(out, "%f", *(double*)&value);
            break;
        default:
            cb(out, type, value);
            break;
        }
        fprintf(out, "\"];\n");
    } else {
        fprintf(out, "%" PRIu64 " [label=\"%" PRIu32 "\"];\n",
                MTBDD_STRIPMARK(mtbdd), mtbddnode_getvariable(n));

        mtbdd_fprintdot_rec(out, mtbddnode_getlow(n), cb);
        mtbdd_fprintdot_rec(out, mtbddnode_gethigh(n), cb);

        fprintf(out, "%" PRIu64 " -> %" PRIu64 " [style=dashed];\n",
                mtbdd, mtbddnode_getlow(n));
        fprintf(out, "%" PRIu64 " -> %" PRIu64 " [style=solid dir=both arrowtail=%s];\n",
                mtbdd, MTBDD_STRIPMARK(mtbddnode_gethigh(n)),
                mtbddnode_getcomp(n) ? "dot" : "none");
    }
}

void
mtbdd_fprintdot(FILE *out, MTBDD mtbdd, print_terminal_label_cb cb)
{
    fprintf(out, "digraph \"DD\" {\n");
    fprintf(out, "graph [dpi = 300];\n");
    fprintf(out, "center = true;\n");
    fprintf(out, "edge [dir = forward];\n");
    fprintf(out, "root [style=invis];\n");
    fprintf(out, "root -> %" PRIu64 " [style=solid dir=both arrowtail=%s];\n",
            MTBDD_STRIPMARK(mtbdd), MTBDD_HASMARK(mtbdd) ? "dot" : "none");

    mtbdd_fprintdot_rec(out, mtbdd, cb);
    mtbdd_unmark_rec(mtbdd);

    fprintf(out, "}\n");
}
