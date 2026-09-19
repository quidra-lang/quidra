import dis, io
def f():
    return 1 * 2 * 3 * 4 * 5
buf = io.StringIO(); dis.dis(f, file=buf)
assert "LOAD_SMALL_INT" in buf.getvalue() and "120" in buf.getvalue(), buf.getvalue()
print("X11", f())
