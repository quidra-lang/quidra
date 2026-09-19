# Witness: the `with` statement releases the file handle even when step (2) (the read) raises.
import io

closed = []


class Boom(io.StringIO):
    def read(self, *a):
        raise OSError("read failed")

    def close(self):
        closed.append(True)
        super().close()


def read_all(opener):
    with opener("data.txt") as f:
        text = f.read()
    return len(text.encode())


try:
    read_all(lambda p: Boom("hello\n"))
except OSError as e:
    print("propagated:", e)
print("closed exactly once on the failure path:", closed == [True])
