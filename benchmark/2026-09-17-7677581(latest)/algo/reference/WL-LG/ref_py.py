#!/usr/bin/env python3
"""
WL-LG -- LightGrad Common Subset ("LightGrad-Core", LGC)
Reference implementation, Python, of methodology/07_algorithm_workloads.md section 4.

This file is measurement infrastructure, not a benchmark submission.  It is written to be
a literal, readable transcription of the FROZEN specification: nothing here is optimised,
no floating-point accumulation is reordered, and the autodiff engine is implemented from
scratch (00_cross_language_constraints.md C-6 -- never imported).

Five logical modules, named as section 4.2 requires:
    tensor, autograd, ops, optim, app

Run:   python3 ref_py.py            -> the 34-line output schema of 4.8 on stdout
       python3 ref_py.py --error N  -> the N-th error case of 4.9 on stderr, exit 2
"""

import sys

# =====================================================================================
# module: errors  (the error-handling contract of 4.9; the mechanism is free, only the
#                  observable behaviour is scored)
# =====================================================================================


class LightGradError(Exception):
    """Carries one of the frozen error codes of 4.9."""

    def __init__(self, code):
        super().__init__(code)
        self.code = code


SHAPE_MISMATCH = "SHAPE_MISMATCH"
SIZE_MISMATCH = "SIZE_MISMATCH"
EXPAND_NOT_SCALAR = "EXPAND_NOT_SCALAR"
EMPTY_TENSOR = "EMPTY_TENSOR"
GRAD_NOT_ENABLED = "GRAD_NOT_ENABLED"


# =====================================================================================
# module: tensor
# =====================================================================================


def _product(shape):
    """product(shape); the empty shape [] denotes a scalar, product 1 (4.3)."""
    n = 1
    for length in shape:
        n *= length
    return n


class Tensor:
    """Dense, row-major, binary64 tensor with a shape.

    The shape is a list of non-negative sizes; the empty list [] denotes a scalar of
    size 1 (4.3).  A non-leaf tensor holds a reference to the Function that created it
    (the graph edge of 4.3, `autograd` module).
    """

    def __init__(self, data, shape, creator=None):
        self._data = data                 # flat, row-major, list of float
        self._shape = list(shape)
        self._creator = creator           # Function or None
        self._grad_on = False
        self._grad = None                 # Tensor or None

    # -- construction -----------------------------------------------------------------

    @staticmethod
    def from_scalar(v):
        """Scalar tensor, shape [], size 1."""
        return Tensor([float(v)], [])

    @staticmethod
    def from_array(values, shape):
        """product(shape) must equal len(values), else SIZE_MISMATCH."""
        if _product(shape) != len(values):
            raise LightGradError(SIZE_MISMATCH)
        return Tensor([float(v) for v in values], shape)

    @staticmethod
    def filled(value, shape):
        """A tensor of `value` repeated over `shape` (used for zero grads and seeds)."""
        return Tensor([float(value)] * _product(shape), shape)

    @staticmethod
    def from_creator(creator, shape):
        """The output tensor of a Function: data is filled in by that Function."""
        return Tensor([0.0] * _product(shape), shape, creator)

    # -- inspection -------------------------------------------------------------------

    def shape(self):
        return list(self._shape)

    def size(self):
        return len(self._data)

    def data(self):
        return self._data

    def at(self, i):
        return self._data[i]

    def scalar(self):
        if self.size() != 1:
            raise LightGradError(SIZE_MISMATCH)
        return self._data[0]

    def creator(self):
        return self._creator

    # -- gradient bookkeeping ---------------------------------------------------------

    def new_grad(self):
        """Enable gradients and install a FRESH zero gradient tensor (4.4).

        Installing a fresh object rather than zeroing the existing one in place is
        load-bearing: `differential` keeps a reference to the previous gradient tensor,
        which must survive unchanged.
        """
        self._grad_on = True
        self._grad = Tensor.filled(0.0, self._shape)

    def delete_grad(self):
        self._grad_on = False
        self._grad = None

    def grad_enabled(self):
        return self._grad_on

    def grad(self):
        if not self._grad_on:
            raise LightGradError(GRAD_NOT_ENABLED)
        return self._grad

    def detach(self):
        """A NEW tensor with a COPY of the data, no creator, no gradient (4.3).

        Never shares storage, so gradient never flows through it (4.4 consequence 4).
        """
        return Tensor(list(self._data), self._shape)

    # -- the backward pass (4.4, normative) -------------------------------------------

    def backward(self, seed=None):
        """
        Tensor.backward(seed):
            if this tensor has gradients enabled:
                this.grad = ops.add(this.grad, seed)   # a graph-building add
            if this tensor has a creator:
                this.creator.backward(seed)

        Plain recursive walk of the creator chain: no topological sort, no visited set,
        depth-first, input1 before input2.  A tensor reachable by two paths is visited
        twice and contributes twice, which is the correct answer.
        """
        if seed is None:
            seed = Tensor.filled(1.0, self._shape)   # ones of this tensor's shape
        if self._grad_on:
            self._grad = ops.add(self._grad, seed)
        if self._creator is not None:
            self._creator.backward(seed)


# =====================================================================================
# module: autograd
# =====================================================================================


class Function:
    """Abstract graph node.  Each Function holds references to its input tensors."""

    def backward(self, grad):
        raise NotImplementedError

    def type_name(self):
        raise NotImplementedError


class Identity(Function):
    def forward(self, a):
        if a.size() == 0:
            raise LightGradError(EMPTY_TENSOR)
        self.input = a
        out = Tensor.from_creator(self, a.shape())
        src, dst = a.data(), out.data()
        for i in range(a.size()):
            dst[i] = src[i]
        return out

    def backward(self, grad):
        self.input.backward(grad)

    def type_name(self):
        return "Identity"


class View(Function):
    def forward(self, a, shape):
        if a.size() == 0:
            raise LightGradError(EMPTY_TENSOR)
        if _product(shape) != a.size():
            raise LightGradError(SIZE_MISMATCH)
        self.input = a
        out = Tensor.from_creator(self, shape)
        src, dst = a.data(), out.data()
        for i in range(a.size()):          # data copied in flat order
            dst[i] = src[i]
        return out

    def backward(self, grad):
        self.input.backward(ops.view(grad, self.input.shape()))

    def type_name(self):
        return "View"


class Sum(Function):
    def forward(self, a):
        if a.size() == 0:
            raise LightGradError(EMPTY_TENSOR)
        self.input = a
        out = Tensor.from_creator(self, [])
        src, dst = a.data(), out.data()
        dst[0] = 0.0
        for i in range(a.size()):          # accumulate over i ascending; do not reorder
            dst[0] += src[i]
        return out

    def backward(self, grad):
        self.input.backward(ops.expand(grad, self.input.shape()))

    def type_name(self):
        return "Sum"


class Expand(Function):
    def forward(self, a, shape):
        if a.size() == 0:
            raise LightGradError(EMPTY_TENSOR)
        if a.size() != 1 or len(a.shape()) != 0:
            raise LightGradError(EXPAND_NOT_SCALAR)
        if _product(shape) == 0:
            raise LightGradError(EMPTY_TENSOR)
        self.input = a
        value = a.scalar()
        out = Tensor.from_creator(self, shape)
        dst = out.data()
        for i in range(out.size()):
            dst[i] = value
        return out

    def backward(self, grad):
        self.input.backward(ops.sum(grad))

    def type_name(self):
        return "Expand"


class Addition(Function):
    def forward(self, a, b):
        if a.size() == 0 or b.size() == 0:
            raise LightGradError(EMPTY_TENSOR)
        if a.shape() != b.shape():
            raise LightGradError(SHAPE_MISMATCH)
        self.input1 = a
        self.input2 = b
        out = Tensor.from_creator(self, a.shape())
        d1, d2, dst = a.data(), b.data(), out.data()
        for i in range(a.size()):
            dst[i] = d1[i] + d2[i]
        return out

    def backward(self, grad):
        self.input1.backward(grad)         # input1 before input2
        self.input2.backward(grad)

    def type_name(self):
        return "Addition"


class Multiplication(Function):
    def forward(self, a, b):
        if a.size() == 0 or b.size() == 0:
            raise LightGradError(EMPTY_TENSOR)
        if a.shape() != b.shape():
            raise LightGradError(SHAPE_MISMATCH)
        self.input1 = a
        self.input2 = b
        out = Tensor.from_creator(self, a.shape())
        d1, d2, dst = a.data(), b.data(), out.data()
        for i in range(a.size()):
            dst[i] = d1[i] * d2[i]
        return out

    def backward(self, grad):
        self.input1.backward(ops.mul(grad, self.input2))   # input1 before input2
        self.input2.backward(ops.mul(grad, self.input1))

    def type_name(self):
        return "Multiplication"


# The six concrete Function subtypes required by 4.3 / 4.8's NODE_TYPES.
CONCRETE_FUNCTION_TYPES = (Identity, View, Sum, Expand, Addition, Multiplication)


# =====================================================================================
# module: ops
# =====================================================================================


class ops:
    """Namespace of the graph-building operations of 4.3."""

    @staticmethod
    def identity(a):
        return Identity().forward(a)

    @staticmethod
    def view(a, shape):
        return View().forward(a, shape)

    @staticmethod
    def sum(a):
        return Sum().forward(a)

    @staticmethod
    def expand(a, shape):
        return Expand().forward(a, shape)

    @staticmethod
    def add(a, b):
        return Addition().forward(a, b)

    @staticmethod
    def mul(a, b):
        return Multiplication().forward(a, b)

    @staticmethod
    def differential(y, x, order=1):
        """
        differential(y, x, order):
            target = y
            repeat `order` times:
                x.newGrad()
                target.backward()          # seed = ones of target's shape
                target = x.grad()
            x.deleteGrad()
            return target
        """
        target = y
        for _ in range(order):
            x.new_grad()
            target.backward()
            target = x.grad()
        x.delete_grad()
        return target


# =====================================================================================
# module: optim
# =====================================================================================


class Optimizer:
    """Abstract optimizer: set_params(params, lr), reset(), step()."""

    def set_params(self, params, lr):
        raise NotImplementedError

    def reset(self):
        raise NotImplementedError

    def step(self):
        raise NotImplementedError


class SGD(Optimizer):
    def __init__(self, params=None, lr=0.1):
        self.params = []
        self.lr = lr
        if params is not None:
            self.set_params(params, lr)

    def set_params(self, params, lr):
        for p in self.params:
            p.delete_grad()
        self.params = list(params)
        self.lr = lr
        for p in self.params:              # registering enables gradients (4.6)
            p.new_grad()

    def reset(self):
        for p in self.params:
            p.new_grad()

    def step(self):
        for p in self.params:
            data = p.data()
            grad = p.grad().data()
            for i in range(p.size()):      # i ascending, in place, own storage
                data[i] = data[i] - self.lr * grad[i]


# =====================================================================================
# module: app
# =====================================================================================


def f6(v):
    """F6: fixed-point, exactly 6 decimals, never scientific, no negative zero (1.4)."""
    if v == 0.0:
        v = 0.0
    return "%.6f" % v


def line(key, *values):
    if values:
        return key + " " + " ".join(values)
    return key


def test_a():
    """4.5 -- scalar higher-order and mixed autodiff."""
    x1 = Tensor.from_scalar(2.0)
    x2 = Tensor.from_scalar(3.0)
    x3 = Tensor.from_scalar(5.0)

    # y = x1^3 * x2^2 + x1 * x3, built with exactly this association
    y = ops.add(
        ops.mul(ops.mul(ops.mul(ops.mul(x1, x1), x1), x2), x2),
        ops.mul(x1, x3),
    )

    out = []
    out.append(line("SCALAR_Y", f6(y.scalar())))
    out.append(line("SCALAR_DY_DX1", f6(ops.differential(y, x1, 1).scalar())))
    out.append(line("SCALAR_D2Y_DX1", f6(ops.differential(y, x1, 2).scalar())))
    out.append(line("SCALAR_D3Y_DX1", f6(ops.differential(y, x1, 3).scalar())))
    out.append(line("SCALAR_D4Y_DX1", f6(ops.differential(y, x1, 4).scalar())))
    out.append(line("SCALAR_DY_DX2", f6(ops.differential(y, x2, 1).scalar())))
    out.append(line("SCALAR_D2Y_DX2", f6(ops.differential(y, x2, 2).scalar())))
    out.append(line("SCALAR_DY_DX3", f6(ops.differential(y, x3, 1).scalar())))
    out.append(line("SCALAR_D2Y_DX3", f6(ops.differential(y, x3, 2).scalar())))
    out.append(line(
        "SCALAR_D2Y_DX1DX2",
        f6(ops.differential(ops.differential(y, x1, 1), x2, 1).scalar()),
    ))
    return out


def test_b():
    """4.6 -- tensor forward/backward and SGD."""
    shape = [2, 2, 3]
    a1 = Tensor.from_array([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12], shape)
    a2 = Tensor.from_array([3, 4, 5, 6, 7, 8, 9, 8, 7, 6, 5, 4], shape)
    a3 = Tensor.from_array([2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1], shape)
    a4 = Tensor.from_scalar(2.0)

    optimizer = SGD([a1, a2, a3, a4], lr=0.1)

    # Forward
    b1 = ops.add(a1, a2)
    b2 = ops.add(ops.mul(a1, a2), a3.detach())
    b3 = ops.expand(a4, shape)
    c = ops.mul(ops.add(b1, b2), b3)
    d = ops.sum(c)

    optimizer.reset()
    d.backward()
    optimizer.step()

    def row(key, t):
        return line(key, *[f6(v) for v in t.data()])

    out = []
    out.append(row("TENSOR_B1", b1))
    out.append(row("TENSOR_B2", b2))
    out.append(row("TENSOR_B3", b3))
    out.append(row("TENSOR_C", c))
    out.append(line("TENSOR_D", f6(d.scalar())))
    out.append(row("GRAD_A1", a1.grad()))
    out.append(row("GRAD_A2", a2.grad()))
    out.append(row("GRAD_A3", a3.grad()))
    out.append(line("GRAD_A4", f6(a4.grad().scalar())))
    out.append(row("NEW_A1", a1))
    out.append(row("NEW_A2", a2))
    out.append(row("NEW_A3", a3))
    out.append(line("NEW_A4", f6(a4.scalar())))
    return out


def test_c():
    """4.7 -- training loop, performance and memory."""
    NE = 4096
    SHAPE = [64, 64]
    STEPS = 150
    LR_C = 0.01

    # Deterministic parameter initialization, no RNG.
    p_init = [((i % 7) + 1) / 8.0 for i in range(NE)]
    q_init = [((i % 5) + 1) / 16.0 for i in range(NE)]
    P = Tensor.from_array(p_init, SHAPE)
    Q = Tensor.from_array(q_init, SHAPE)

    optimizer = SGD([P, Q], lr=LR_C)

    recorded_loss = {}
    grad_p0 = 0.0
    grad_q0 = 0.0

    for step in range(STEPS):
        h = ops.mul(P, Q)
        h = ops.add(h, P)
        h = ops.mul(h, h)          # SAME tensor twice: double-path accumulation
        loss = ops.sum(h)
        if step in (0, 50, 100):
            recorded_loss[step] = loss.scalar()
        optimizer.reset()
        loss.backward()
        if step == 0:
            grad_p0 = P.grad().at(0)
            grad_q0 = Q.grad().at(0)
        optimizer.step()

    # One extra forward, no backward.
    final_loss = ops.sum(
        ops.mul(
            ops.add(ops.mul(P, Q), P),
            ops.add(ops.mul(P, Q), P),
        )
    )

    # Parameter sums, accumulated over i ascending.
    p_sum = 0.0
    for i in range(P.size()):
        p_sum += P.at(i)
    q_sum = 0.0
    for i in range(Q.size()):
        q_sum += Q.at(i)

    out = []
    out.append(line("STRESS_STEPS", str(STEPS)))
    out.append(line("STRESS_GRAD_P0", f6(grad_p0)))
    out.append(line("STRESS_GRAD_Q0", f6(grad_q0)))
    out.append(line("STRESS_LOSS_0", f6(recorded_loss[0])))
    out.append(line("STRESS_LOSS_50", f6(recorded_loss[50])))
    out.append(line("STRESS_LOSS_100", f6(recorded_loss[100])))
    out.append(line("STRESS_LOSS_FINAL", f6(final_loss.scalar())))
    out.append(line("STRESS_P_SUM", f6(p_sum)))
    out.append(line("STRESS_Q_SUM", f6(q_sum)))
    return out


def run_error_case(n):
    """4.9 -- perform ONLY the corresponding erroneous call."""
    if n == 1:
        ops.add(Tensor.filled(1.0, [2, 3]), Tensor.filled(1.0, [3, 2]))
    elif n == 2:
        ops.view(Tensor.filled(1.0, [6]), [4])
    elif n == 3:
        ops.expand(Tensor.filled(1.0, [2, 2]), [4, 4])
    elif n == 4:
        ops.sum(Tensor.from_array([], [0]))
    elif n == 5:
        Tensor.from_scalar(1.0).grad()
    else:
        raise ValueError("error selector must be in 1..5")


def main(argv):
    if len(argv) >= 3 and argv[1] == "--error":
        try:
            run_error_case(int(argv[2]))
        except LightGradError as e:
            sys.stderr.write("ERROR: " + e.code + "\n")
            return 2
        # Reaching here means the mandated error was not raised at all.
        sys.stderr.write("ERROR: NOT_RAISED\n")
        return 3

    lines = []
    lines.append(line("LIGHTGRAD_VERSION", "1"))
    lines.extend(test_a())
    lines.extend(test_b())
    lines.extend(test_c())
    lines.append(line("NODE_TYPES", str(len(CONCRETE_FUNCTION_TYPES))))

    sys.stdout.write("\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    # The backward walk nests only ~15 frames deep on these graphs, so the default
    # recursion limit is ample; no limit is raised here.
    sys.exit(main(sys.argv))
