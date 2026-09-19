class Res:
    def __enter__(self): return self
    def __exit__(self, *a):
        print("X09 cleanup", end=" ")
        return False
try:
    with Res():
        raise RuntimeError("boom")
except RuntimeError:
    print("caught")
