from typing import Protocol


# BEGIN PROBE F15.P3
class Named(Protocol):
    def tag(self) -> str: ...


class A:
    def tag(self) -> str:
        return "a"


class B:
    def tag(self) -> str:
        return "b"


items: list[Named] = [A(), B()]
first = items[0].tag()
# END PROBE F15.P3

print(first)
