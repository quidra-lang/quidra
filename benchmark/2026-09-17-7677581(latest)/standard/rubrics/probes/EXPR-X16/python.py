from typing import TypeVar, Protocol
class HasVal(Protocol):
    def val(self) -> int: ...
T = TypeVar("T", bound=HasVal)
def get(x: T) -> int:
    return x.val()
class C:
    def val(self) -> int: return 7
print("X16", get(C()))
