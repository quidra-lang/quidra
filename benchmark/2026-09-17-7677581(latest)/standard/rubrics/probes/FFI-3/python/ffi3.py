import ctypes
from ctypes.util import find_library


class Record(ctypes.Structure):
    _fields_ = [("count", ctypes.c_int32), ("weight", ctypes.c_double)]


libc = ctypes.CDLL(find_library("c"))
CMP = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p)
libc.qsort.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_size_t, CMP]
libc.qsort.restype = None


def _cmp_double(a, b):
    x = ctypes.cast(a, ctypes.POINTER(ctypes.c_double)).contents.value
    y = ctypes.cast(b, ctypes.POINTER(ctypes.c_double)).contents.value
    return (x > y) - (x < y)


def _cmp_record(a, b):
    x = ctypes.cast(a, ctypes.POINTER(Record)).contents.count
    y = ctypes.cast(b, ctypes.POINTER(Record)).contents.count
    return (x > y) - (x < y)


cmp_double = CMP(_cmp_double)
cmp_record = CMP(_cmp_record)

print("sizeof=%d offset=%d" % (ctypes.sizeof(Record), Record.weight.offset))

# A ctypes array is a C-layout buffer, not a Python list: qsort reorders it in place.
xs = (ctypes.c_double * 5)(3.5, 1.25, 4.75, 1.5, 2.25)
libc.qsort(ctypes.cast(xs, ctypes.c_void_p), 5, ctypes.sizeof(ctypes.c_double), cmp_double)
print(" ".join("%.2f" % xs[i] for i in range(5)))

rs = (Record * 3)(Record(3, 1.5), Record(1, 4.0), Record(2, 2.5))
libc.qsort(ctypes.cast(rs, ctypes.c_void_p), 3, ctypes.sizeof(Record), cmp_record)
print(" ".join("%d:%.2f" % (rs[i].count, rs[i].weight) for i in range(3)))
