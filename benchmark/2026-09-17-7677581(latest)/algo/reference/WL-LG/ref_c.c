/*
 * WL-LG - LightGrad Common Subset ("LightGrad-Core", LGC)
 * REFERENCE implementation in C.
 *
 * Frozen specification: methodology/07_algorithm_workloads.md section 4
 *                       (with the universal conventions of section 1)
 * Frozen constraints:   methodology/00_cross_language_constraints.md (C-6: the
 *                       autodiff engine is IMPLEMENTED here, never imported)
 *
 * Build: clang -O2 -ffp-contract=off ref_c.c -o REF_c -lm
 *
 * This file is NOT one of the ten measured languages. It exists only to
 * establish the expected output. It is therefore written as the clearest,
 * most literal transcription of the frozen specification that I could manage:
 * no optimisation, no reordering of floating-point accumulation, no clever
 * memory tricks.
 *
 * Memory note: every Tensor and Function allocated during the run is recorded
 * in a global registry and released once, at the very end. The spec's
 * "memory management" capability is scored on the ten measured languages via
 * Peak RSS; this reference is never timed or scored, so it deliberately
 * chooses the obviously-correct lifetime policy (nothing can be freed too
 * early) over a small resident set. Peak RSS here is roughly 100 MB.
 *
 * The five logical modules required by 4.2 are marked by the banners below and
 * are distinguished by identifier prefix: tensor_, autograd_, ops_, optim_, app_.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ==========================================================================
 * shared: error contract (spec 4.9)
 *
 * On an error the program prints nothing to stdout, prints exactly
 * "ERROR: <CODE>\n" to stderr, and exits with status 2.
 * ========================================================================== */

static void raise_error(const char *code) {
    fprintf(stderr, "ERROR: %s\n", code);
    exit(2);
}

/* An internal invariant failure; unreachable for a conforming run. */
static void internal_error(const char *what) {
    fprintf(stderr, "INTERNAL: %s\n", what);
    exit(1);
}

/* ==========================================================================
 * shared: allocation registry
 *
 * Everything allocated is recorded so it can be released exactly once at the
 * end of the program. See the memory note at the top of the file.
 * ========================================================================== */

static void **g_allocations = NULL;
static size_t g_allocation_count = 0;
static size_t g_allocation_capacity = 0;

static void *mem_alloc(size_t bytes) {
    void *p = malloc(bytes);
    if (p == NULL) {
        internal_error("out of memory");
    }
    if (g_allocation_count == g_allocation_capacity) {
        size_t new_capacity = (g_allocation_capacity == 0) ? 1024 : g_allocation_capacity * 2;
        void **grown = (void **)realloc(g_allocations, new_capacity * sizeof(void *));
        if (grown == NULL) {
            internal_error("out of memory");
        }
        g_allocations = grown;
        g_allocation_capacity = new_capacity;
    }
    g_allocations[g_allocation_count++] = p;
    return p;
}

static void mem_release_all(void) {
    size_t i;
    for (i = 0; i < g_allocation_count; i++) {
        free(g_allocations[i]);
    }
    free(g_allocations);
    g_allocations = NULL;
    g_allocation_count = 0;
    g_allocation_capacity = 0;
}

/* ==========================================================================
 * module: tensor  (spec 4.3, "tensor module")
 *
 * Dense, row-major, binary64. A shape is a list of non-negative sizes; the
 * empty list denotes a scalar of size 1.
 * ========================================================================== */

#define TENSOR_MAX_DIMS 8

typedef struct Function Function;
typedef struct Tensor Tensor;

struct Tensor {
    size_t ndim;                        /* 0 == scalar */
    size_t shape[TENSOR_MAX_DIMS];
    size_t size;                        /* product of shape; 1 when ndim == 0 */
    double *data;                       /* row-major, `size` elements */
    int grad_on;                        /* gradient accumulation enabled */
    Tensor *grad;                       /* accumulated gradient, itself a Tensor */
    Function *creator;                  /* NULL for a leaf */
};

static size_t tensor_shape_product(const size_t *shape, size_t ndim) {
    size_t product = 1;
    size_t i;
    for (i = 0; i < ndim; i++) {
        product *= shape[i];
    }
    return product;
}

static int tensor_shape_equal(const Tensor *a, const Tensor *b) {
    size_t i;
    if (a->ndim != b->ndim) {
        return 0;
    }
    for (i = 0; i < a->ndim; i++) {
        if (a->shape[i] != b->shape[i]) {
            return 0;
        }
    }
    return 1;
}

/* Allocate an uninitialised tensor with the given shape. */
static Tensor *tensor_allocate(const size_t *shape, size_t ndim) {
    Tensor *t;
    size_t i;

    if (ndim > TENSOR_MAX_DIMS) {
        internal_error("shape has too many dimensions");
    }
    t = (Tensor *)mem_alloc(sizeof(Tensor));
    t->ndim = ndim;
    for (i = 0; i < ndim; i++) {
        t->shape[i] = shape[i];
    }
    t->size = tensor_shape_product(shape, ndim);
    /* malloc(0) is allowed to return NULL, so always ask for at least one. */
    t->data = (double *)mem_alloc((t->size == 0 ? 1 : t->size) * sizeof(double));
    t->grad_on = 0;
    t->grad = NULL;
    t->creator = NULL;
    return t;
}

/* A tensor of the given shape with every element set to `value`. */
static Tensor *tensor_filled(double value, const size_t *shape, size_t ndim) {
    Tensor *t = tensor_allocate(shape, ndim);
    size_t i;
    for (i = 0; i < t->size; i++) {
        t->data[i] = value;
    }
    return t;
}

/* Tensor.fromScalar(v) - scalar tensor, shape [], size 1. */
static Tensor *tensor_from_scalar(double value) {
    Tensor *t = tensor_allocate(NULL, 0);
    t->data[0] = value;
    return t;
}

/* Tensor.fromArray(values, shape) - product(shape) must equal len(values). */
static Tensor *tensor_from_array(const double *values, size_t value_count,
                                 const size_t *shape, size_t ndim) {
    Tensor *t;
    size_t i;

    if (tensor_shape_product(shape, ndim) != value_count) {
        raise_error("SIZE_MISMATCH");
    }
    t = tensor_allocate(shape, ndim);
    for (i = 0; i < t->size; i++) {
        t->data[i] = values[i];
    }
    return t;
}

/* t.size() */
static size_t tensor_size(const Tensor *t) {
    return t->size;
}

/* t.at(i) */
static double tensor_at(const Tensor *t, size_t i) {
    if (i >= t->size) {
        internal_error("element index out of range");
    }
    return t->data[i];
}

/* t.scalar() - value of a size-1 tensor. */
static double tensor_scalar(const Tensor *t) {
    if (t->size != 1) {
        internal_error("scalar() on a tensor whose size is not 1");
    }
    return t->data[0];
}

/* t.detach() - a NEW tensor with a COPY of the data, no creator, no gradient. */
static Tensor *tensor_detach(const Tensor *t) {
    Tensor *out = tensor_allocate(t->shape, t->ndim);
    size_t i;
    for (i = 0; i < t->size; i++) {
        out->data[i] = t->data[i];
    }
    out->grad_on = 0;
    out->grad = NULL;
    out->creator = NULL;
    return out;
}

/*
 * t.newGrad() - enable gradient accumulation and install a FRESH zero gradient
 * tensor of t's shape. Installing a fresh tensor (rather than zeroing the
 * existing one) is load-bearing: a previously returned gradient tensor must
 * survive unchanged, which is what makes differential() of order >= 2 correct.
 */
static void tensor_new_grad(Tensor *t) {
    t->grad_on = 1;
    t->grad = tensor_filled(0.0, t->shape, t->ndim);
}

/* t.deleteGrad() - disable gradient accumulation and drop t.grad. */
static void tensor_delete_grad(Tensor *t) {
    t->grad_on = 0;
    t->grad = NULL;
}

/* t.grad() - the accumulated gradient tensor. */
static Tensor *tensor_grad(const Tensor *t) {
    if (!t->grad_on || t->grad == NULL) {
        raise_error("GRAD_NOT_ENABLED");
    }
    return t->grad;
}

/* ==========================================================================
 * module: autograd  (spec 4.3 "autograd module", 4.4 semantics)
 *
 * `Function` is the abstract graph node: backward(grad) and typeName().
 * There are exactly six concrete subtypes, realised in C as six constructors
 * that install their own backward/typeName implementations:
 *
 *   Identity, View, Sum, Expand, Addition, Multiplication
 * ========================================================================== */

#define AUTOGRAD_NODE_TYPES 6

struct Function {
    void (*backward)(Function *self, Tensor *grad);
    const char *(*type_name)(void);
    Tensor *input1;
    Tensor *input2;    /* NULL for the unary node types */
};

/* Forward declarations: the backward pass calls back into the ops module,
 * because in this design backward itself builds graph nodes (spec 4.4). */
static Tensor *ops_add(Tensor *a, Tensor *b);
static Tensor *ops_mul(Tensor *a, Tensor *b);
static Tensor *ops_sum(Tensor *a);
static Tensor *ops_expand(Tensor *a, const size_t *shape, size_t ndim);
static Tensor *ops_view(Tensor *a, const size_t *shape, size_t ndim);
static void tensor_backward(Tensor *t, Tensor *seed);

/*
 * Tensor.backward(seed) - spec 4.4, verbatim:
 *
 *     if this tensor has gradients enabled:
 *         this.grad = ops.add(this.grad, seed)   # a graph-building add
 *     if this tensor has a creator:
 *         this.creator.backward(seed)
 *
 * There is deliberately NO topological sort and NO visited set: a tensor
 * reachable by two paths is visited twice and contributes twice.
 */
static void tensor_backward(Tensor *t, Tensor *seed) {
    if (t->grad_on) {
        t->grad = ops_add(t->grad, seed);
    }
    if (t->creator != NULL) {
        t->creator->backward(t->creator, seed);
    }
}

/* t.backward() with the default seed: a tensor of ones with t's shape. */
static void tensor_backward_default(Tensor *t) {
    tensor_backward(t, tensor_filled(1.0, t->shape, t->ndim));
}

/* ---- concrete node type 1: Identity ---- */

static const char *autograd_identity_type_name(void) {
    return "Identity";
}

static void autograd_identity_backward(Function *self, Tensor *grad) {
    tensor_backward(self->input1, grad);
}

/* ---- concrete node type 2: View ---- */

static const char *autograd_view_type_name(void) {
    return "View";
}

static void autograd_view_backward(Function *self, Tensor *grad) {
    tensor_backward(self->input1,
                    ops_view(grad, self->input1->shape, self->input1->ndim));
}

/* ---- concrete node type 3: Sum ---- */

static const char *autograd_sum_type_name(void) {
    return "Sum";
}

static void autograd_sum_backward(Function *self, Tensor *grad) {
    tensor_backward(self->input1,
                    ops_expand(grad, self->input1->shape, self->input1->ndim));
}

/* ---- concrete node type 4: Expand ---- */

static const char *autograd_expand_type_name(void) {
    return "Expand";
}

static void autograd_expand_backward(Function *self, Tensor *grad) {
    tensor_backward(self->input1, ops_sum(grad));
}

/* ---- concrete node type 5: Addition ---- */

static const char *autograd_addition_type_name(void) {
    return "Addition";
}

static void autograd_addition_backward(Function *self, Tensor *grad) {
    tensor_backward(self->input1, grad);
    tensor_backward(self->input2, grad);
}

/* ---- concrete node type 6: Multiplication ---- */

static const char *autograd_multiplication_type_name(void) {
    return "Multiplication";
}

static void autograd_multiplication_backward(Function *self, Tensor *grad) {
    /* input1 before input2, depth first (spec 4.4 consequence 3). */
    tensor_backward(self->input1, ops_mul(grad, self->input2));
    tensor_backward(self->input2, ops_mul(grad, self->input1));
}

/* The one place a Function is built. */
static Function *autograd_new_function(void (*backward)(Function *, Tensor *),
                                       const char *(*type_name)(void),
                                       Tensor *input1, Tensor *input2) {
    Function *f = (Function *)mem_alloc(sizeof(Function));
    f->backward = backward;
    f->type_name = type_name;
    f->input1 = input1;
    f->input2 = input2;
    return f;
}

/* The graph edge: `out` was created by `creator`. */
static void autograd_attach(Tensor *out, Function *creator) {
    out->creator = creator;
}

/* Reported as NODE_TYPES. */
static int autograd_node_type_count(void) {
    /* The six concrete Function subtypes defined above. */
    const char *(*const type_names[AUTOGRAD_NODE_TYPES])(void) = {
        autograd_identity_type_name,
        autograd_view_type_name,
        autograd_sum_type_name,
        autograd_expand_type_name,
        autograd_addition_type_name,
        autograd_multiplication_type_name
    };
    return (int)(sizeof(type_names) / sizeof(type_names[0]));
}

/* ==========================================================================
 * module: ops  (spec 4.3 "ops module")
 *
 * Every op takes and returns Tensors and builds a graph node.
 * Every op rejects a zero-element input with EMPTY_TENSOR.
 * ========================================================================== */

/* identity(a): out[i] = a[i] */
static Tensor *ops_identity(Tensor *a) {
    Tensor *out;
    size_t i;

    if (tensor_size(a) == 0) {
        raise_error("EMPTY_TENSOR");
    }
    out = tensor_allocate(a->shape, a->ndim);
    for (i = 0; i < out->size; i++) {
        out->data[i] = a->data[i];
    }
    autograd_attach(out, autograd_new_function(autograd_identity_backward,
                                               autograd_identity_type_name,
                                               a, NULL));
    return out;
}

/* add(a, b): shapes must be equal; out[i] = a[i] + b[i] */
static Tensor *ops_add(Tensor *a, Tensor *b) {
    Tensor *out;
    size_t i;

    if (tensor_size(a) == 0 || tensor_size(b) == 0) {
        raise_error("EMPTY_TENSOR");
    }
    if (!tensor_shape_equal(a, b)) {
        raise_error("SHAPE_MISMATCH");
    }
    out = tensor_allocate(a->shape, a->ndim);
    for (i = 0; i < out->size; i++) {
        out->data[i] = a->data[i] + b->data[i];
    }
    autograd_attach(out, autograd_new_function(autograd_addition_backward,
                                               autograd_addition_type_name,
                                               a, b));
    return out;
}

/* mul(a, b): shapes must be equal; out[i] = a[i] * b[i] */
static Tensor *ops_mul(Tensor *a, Tensor *b) {
    Tensor *out;
    size_t i;

    if (tensor_size(a) == 0 || tensor_size(b) == 0) {
        raise_error("EMPTY_TENSOR");
    }
    if (!tensor_shape_equal(a, b)) {
        raise_error("SHAPE_MISMATCH");
    }
    out = tensor_allocate(a->shape, a->ndim);
    for (i = 0; i < out->size; i++) {
        out->data[i] = a->data[i] * b->data[i];
    }
    autograd_attach(out, autograd_new_function(autograd_multiplication_backward,
                                               autograd_multiplication_type_name,
                                               a, b));
    return out;
}

/* sum(a): scalar; out = sum over i ascending of a[i]  (order is normative) */
static Tensor *ops_sum(Tensor *a) {
    Tensor *out;
    size_t i;

    if (tensor_size(a) == 0) {
        raise_error("EMPTY_TENSOR");
    }
    out = tensor_allocate(NULL, 0);
    out->data[0] = 0.0;
    for (i = 0; i < a->size; i++) {
        out->data[0] += a->data[i];
    }
    autograd_attach(out, autograd_new_function(autograd_sum_backward,
                                               autograd_sum_type_name,
                                               a, NULL));
    return out;
}

/* expand(a, shape): a must be a scalar; out[i] = a.scalar() for all i */
static Tensor *ops_expand(Tensor *a, const size_t *shape, size_t ndim) {
    Tensor *out;
    double value;
    size_t i;

    if (tensor_size(a) == 0) {
        raise_error("EMPTY_TENSOR");
    }
    if (a->ndim != 0 || tensor_size(a) != 1) {
        raise_error("EXPAND_NOT_SCALAR");
    }
    if (tensor_shape_product(shape, ndim) == 0) {
        raise_error("EMPTY_TENSOR");
    }
    value = tensor_scalar(a);
    out = tensor_allocate(shape, ndim);
    for (i = 0; i < out->size; i++) {
        out->data[i] = value;
    }
    autograd_attach(out, autograd_new_function(autograd_expand_backward,
                                               autograd_expand_type_name,
                                               a, NULL));
    return out;
}

/* view(a, shape): product(shape) must equal a.size(); data copied flat */
static Tensor *ops_view(Tensor *a, const size_t *shape, size_t ndim) {
    Tensor *out;
    size_t i;

    if (tensor_size(a) == 0) {
        raise_error("EMPTY_TENSOR");
    }
    if (tensor_shape_product(shape, ndim) != tensor_size(a)) {
        raise_error("SIZE_MISMATCH");
    }
    out = tensor_allocate(shape, ndim);
    for (i = 0; i < out->size; i++) {
        out->data[i] = a->data[i];
    }
    autograd_attach(out, autograd_new_function(autograd_view_backward,
                                               autograd_view_type_name,
                                               a, NULL));
    return out;
}

/*
 * differential(y, x, order) - spec 4.4, verbatim:
 *
 *     target = y
 *     repeat `order` times:
 *         x.newGrad()
 *         target.backward()          # seed = ones of target's shape
 *         target = x.grad()
 *     x.deleteGrad()
 *     return target
 */
static Tensor *ops_differential(Tensor *y, Tensor *x, int order) {
    Tensor *target = y;
    int i;

    for (i = 0; i < order; i++) {
        tensor_new_grad(x);
        tensor_backward_default(target);
        target = tensor_grad(x);
    }
    tensor_delete_grad(x);
    return target;
}

/* ==========================================================================
 * module: optim  (spec 4.3 "optim module")
 *
 * `Optimizer` is abstract: setParams(params, lr), reset(), step().
 * `SGD` is its one concrete implementation.
 * ========================================================================== */

typedef struct Optimizer Optimizer;

struct Optimizer {
    void (*reset)(Optimizer *self);
    void (*step)(Optimizer *self);
    Tensor **params;
    size_t param_count;
    double lr;
};

/* SGD.reset() - calls newGrad() on every parameter. */
static void optim_sgd_reset(Optimizer *self) {
    size_t k;
    for (k = 0; k < self->param_count; k++) {
        tensor_new_grad(self->params[k]);
    }
}

/*
 * SGD.step() - p[i] = p[i] - lr * p.grad[i] for every parameter, i ascending,
 * in place, on the parameter's own storage.
 */
static void optim_sgd_step(Optimizer *self) {
    size_t k;
    for (k = 0; k < self->param_count; k++) {
        Tensor *p = self->params[k];
        Tensor *g = tensor_grad(p);
        size_t i;
        for (i = 0; i < p->size; i++) {
            p->data[i] = p->data[i] - self->lr * g->data[i];
        }
    }
}

/* setParams(params, lr) - registering the parameters enables gradients on them. */
static void optim_set_params(Optimizer *self, Tensor **params, size_t param_count, double lr) {
    size_t k;
    for (k = 0; k < self->param_count; k++) {
        tensor_delete_grad(self->params[k]);
    }
    self->params = params;
    self->param_count = param_count;
    self->lr = lr;
    for (k = 0; k < self->param_count; k++) {
        tensor_new_grad(self->params[k]);
    }
}

static Optimizer optim_new_sgd(Tensor **params, size_t param_count, double lr) {
    Optimizer opt;
    opt.reset = optim_sgd_reset;
    opt.step = optim_sgd_step;
    opt.params = NULL;
    opt.param_count = 0;
    opt.lr = 0.0;
    optim_set_params(&opt, params, param_count, lr);
    return opt;
}

/* ==========================================================================
 * module: app  (entry point and output formatting, spec 1.4 and 4.8)
 * ========================================================================== */

/*
 * F6 - fixed point, exactly 6 digits after the decimal point, never scientific
 * notation. Negative zero is forbidden in the output, so a value that formats
 * as "-0.000000" is normalised to "0.000000".
 */
static void app_format_f6(double value, char *buffer, size_t buffer_size) {
    snprintf(buffer, buffer_size, "%.6f", value);
    if (strcmp(buffer, "-0.000000") == 0) {
        snprintf(buffer, buffer_size, "%s", "0.000000");
    }
}

static void app_print_scalar(const char *key, double value) {
    char buffer[64];
    app_format_f6(value, buffer, sizeof buffer);
    printf("%s %s\n", key, buffer);
}

static void app_print_tensor(const char *key, const Tensor *t) {
    char buffer[64];
    size_t i;
    printf("%s", key);
    for (i = 0; i < tensor_size(t); i++) {
        app_format_f6(tensor_at(t, i), buffer, sizeof buffer);
        printf(" %s", buffer);
    }
    printf("\n");
}

static void app_print_int(const char *key, long value) {
    printf("%s %ld\n", key, value);
}

/* ---- Test A: scalar higher-order and mixed autodiff (spec 4.5) ---- */

static void app_test_a(void) {
    Tensor *x1 = tensor_from_scalar(2.0);
    Tensor *x2 = tensor_from_scalar(3.0);
    Tensor *x3 = tensor_from_scalar(5.0);
    Tensor *y;
    Tensor *inner;

    /*
     * y = add( mul( mul( mul( mul(x1, x1), x1 ), x2 ), x2 ), mul(x1, x3) )
     *   = x1^3 * x2^2 + x1 * x3
     * built with exactly this left-to-right association.
     */
    y = ops_add(ops_mul(ops_mul(ops_mul(ops_mul(x1, x1), x1), x2), x2),
                ops_mul(x1, x3));

    /* The ten quantities, computed in the schema's order, each by a fresh
     * differential() call on the same y graph. */
    app_print_scalar("SCALAR_Y", tensor_scalar(y));
    app_print_scalar("SCALAR_DY_DX1", tensor_scalar(ops_differential(y, x1, 1)));
    app_print_scalar("SCALAR_D2Y_DX1", tensor_scalar(ops_differential(y, x1, 2)));
    app_print_scalar("SCALAR_D3Y_DX1", tensor_scalar(ops_differential(y, x1, 3)));
    app_print_scalar("SCALAR_D4Y_DX1", tensor_scalar(ops_differential(y, x1, 4)));
    app_print_scalar("SCALAR_DY_DX2", tensor_scalar(ops_differential(y, x2, 1)));
    app_print_scalar("SCALAR_D2Y_DX2", tensor_scalar(ops_differential(y, x2, 2)));
    app_print_scalar("SCALAR_DY_DX3", tensor_scalar(ops_differential(y, x3, 1)));
    app_print_scalar("SCALAR_D2Y_DX3", tensor_scalar(ops_differential(y, x3, 2)));

    /* The mixed partial: differential( differential(y, x1, 1), x2, 1 ). */
    inner = ops_differential(y, x1, 1);
    app_print_scalar("SCALAR_D2Y_DX1DX2", tensor_scalar(ops_differential(inner, x2, 1)));
}

/* ---- Test B: tensor forward/backward and SGD (spec 4.6) ---- */

static void app_test_b(void) {
    static const double a1_values[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
    static const double a2_values[12] = { 3, 4, 5, 6, 7, 8, 9, 8, 7, 6, 5, 4 };
    static const double a3_values[12] = { 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1 };
    static const size_t shape[3] = { 2, 2, 3 };
    const double lr = 0.1;

    Tensor *a1 = tensor_from_array(a1_values, 12, shape, 3);
    Tensor *a2 = tensor_from_array(a2_values, 12, shape, 3);
    Tensor *a3 = tensor_from_array(a3_values, 12, shape, 3);
    Tensor *a4 = tensor_from_scalar(2.0);
    Tensor *params[4];
    Optimizer optimizer;
    Tensor *b1, *b2, *b3, *c, *d;

    params[0] = a1;
    params[1] = a2;
    params[2] = a3;
    params[3] = a4;
    optimizer = optim_new_sgd(params, 4, lr);

    /* Forward. */
    b1 = ops_add(a1, a2);
    b2 = ops_add(ops_mul(a1, a2), tensor_detach(a3));
    b3 = ops_expand(a4, shape, 3);
    c = ops_mul(ops_add(b1, b2), b3);
    d = ops_sum(c);

    /* Backward and update. */
    optimizer.reset(&optimizer);
    tensor_backward_default(d);

    app_print_tensor("TENSOR_B1", b1);
    app_print_tensor("TENSOR_B2", b2);
    app_print_tensor("TENSOR_B3", b3);
    app_print_tensor("TENSOR_C", c);
    app_print_scalar("TENSOR_D", tensor_scalar(d));
    app_print_tensor("GRAD_A1", tensor_grad(a1));
    app_print_tensor("GRAD_A2", tensor_grad(a2));
    app_print_tensor("GRAD_A3", tensor_grad(a3));
    app_print_scalar("GRAD_A4", tensor_scalar(tensor_grad(a4)));

    optimizer.step(&optimizer);

    app_print_tensor("NEW_A1", a1);
    app_print_tensor("NEW_A2", a2);
    app_print_tensor("NEW_A3", a3);
    app_print_scalar("NEW_A4", tensor_scalar(a4));
}

/* ---- Test C: training loop, performance and memory (spec 4.7) ---- */

#define STRESS_NE    4096
#define STRESS_STEPS 150
#define STRESS_LR_C  0.01

static void app_test_c(void) {
    static const size_t shape[2] = { 64, 64 };
    double p_values[STRESS_NE];
    double q_values[STRESS_NE];
    Tensor *P;
    Tensor *Q;
    Tensor *params[2];
    Optimizer optimizer;
    double loss_0 = 0.0;
    double loss_50 = 0.0;
    double loss_100 = 0.0;
    double grad_p0 = 0.0;
    double grad_q0 = 0.0;
    double final_loss;
    size_t i;
    int step;

    /* Deterministic initialisation, no RNG. */
    for (i = 0; i < STRESS_NE; i++) {
        p_values[i] = (double)((i % 7) + 1) / 8.0;
        q_values[i] = (double)((i % 5) + 1) / 16.0;
    }
    P = tensor_from_array(p_values, STRESS_NE, shape, 2);
    Q = tensor_from_array(q_values, STRESS_NE, shape, 2);

    params[0] = P;
    params[1] = Q;
    optimizer = optim_new_sgd(params, 2, STRESS_LR_C);

    for (step = 0; step < STRESS_STEPS; step++) {
        Tensor *h;
        Tensor *loss;

        h = ops_mul(P, Q);
        h = ops_add(h, P);
        h = ops_mul(h, h);          /* SAME tensor twice: double-path accumulation */
        loss = ops_sum(h);

        if (step == 0) {
            loss_0 = tensor_scalar(loss);
        } else if (step == 50) {
            loss_50 = tensor_scalar(loss);
        } else if (step == 100) {
            loss_100 = tensor_scalar(loss);
        }

        optimizer.reset(&optimizer);
        tensor_backward_default(loss);

        if (step == 0) {
            grad_p0 = tensor_at(tensor_grad(P), 0);
            grad_q0 = tensor_at(tensor_grad(Q), 0);
        }

        optimizer.step(&optimizer);
    }

    /* One extra forward, no backward. */
    final_loss = tensor_scalar(ops_sum(ops_mul(ops_add(ops_mul(P, Q), P),
                                               ops_add(ops_mul(P, Q), P))));

    app_print_int("STRESS_STEPS", (long)STRESS_STEPS);
    app_print_scalar("STRESS_GRAD_P0", grad_p0);
    app_print_scalar("STRESS_GRAD_Q0", grad_q0);
    app_print_scalar("STRESS_LOSS_0", loss_0);
    app_print_scalar("STRESS_LOSS_50", loss_50);
    app_print_scalar("STRESS_LOSS_100", loss_100);
    app_print_scalar("STRESS_LOSS_FINAL", final_loss);
    app_print_scalar("STRESS_P_SUM", tensor_scalar(ops_sum(P)));
    app_print_scalar("STRESS_Q_SUM", tensor_scalar(ops_sum(Q)));
}

/* ---- Error-handling contract (spec 4.9) ---- */

static void app_run_error_case(int which) {
    static const double six[6] = { 1, 2, 3, 4, 5, 6 };
    static const double four[4] = { 1, 2, 3, 4 };

    if (which == 1) {
        /* add( shape [2,3], shape [3,2] ) -> SHAPE_MISMATCH */
        static const size_t shape_a[2] = { 2, 3 };
        static const size_t shape_b[2] = { 3, 2 };
        Tensor *a = tensor_from_array(six, 6, shape_a, 2);
        Tensor *b = tensor_from_array(six, 6, shape_b, 2);
        (void)ops_add(a, b);
    } else if (which == 2) {
        /* view( a 6-element tensor, shape [4] ) -> SIZE_MISMATCH */
        static const size_t shape_a[1] = { 6 };
        static const size_t shape_v[1] = { 4 };
        Tensor *a = tensor_from_array(six, 6, shape_a, 1);
        (void)ops_view(a, shape_v, 1);
    } else if (which == 3) {
        /* expand( a shape-[2,2] tensor, shape [4,4] ) -> EXPAND_NOT_SCALAR */
        static const size_t shape_a[2] = { 2, 2 };
        static const size_t shape_e[2] = { 4, 4 };
        Tensor *a = tensor_from_array(four, 4, shape_a, 2);
        (void)ops_expand(a, shape_e, 2);
    } else if (which == 4) {
        /* sum( a tensor with shape [0] ) -> EMPTY_TENSOR */
        static const size_t shape_a[1] = { 0 };
        Tensor *a = tensor_from_array(six, 0, shape_a, 1);
        (void)ops_sum(a);
    } else if (which == 5) {
        /* grad() on a tensor with gradients disabled -> GRAD_NOT_ENABLED */
        Tensor *a = tensor_from_scalar(1.0);
        (void)tensor_grad(a);
    } else {
        internal_error("unknown error case");
    }

    /* Unreachable: the case above must have raised. */
    internal_error("error case did not raise");
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--error") == 0) {
        app_run_error_case(atoi(argv[2]));
        return 2;
    }

    printf("LIGHTGRAD_VERSION 1\n");
    app_test_a();
    app_test_b();
    app_test_c();
    app_print_int("NODE_TYPES", (long)autograd_node_type_count());

    /* Unused in the pinned flow, but part of the frozen API surface (4.3). */
    (void)ops_identity;

    mem_release_all();
    return 0;
}
