def inner():
    raise ValueError("boom")
def outer():
    inner()
try:
    outer()
except ValueError as e:
    print("X08 caught", e)
