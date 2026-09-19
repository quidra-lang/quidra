from typing import TypeVar, Generic
T = TypeVar("T")
class Box(Generic[T]):
    def __init__(self, v: T) -> None:
        self.v = v
    def get(self) -> T:
        return self.v
print("X03", Box[int](5).get(), Box[str]("hi").get())
