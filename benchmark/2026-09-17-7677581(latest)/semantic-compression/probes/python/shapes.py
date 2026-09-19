from dataclasses import dataclass
from typing import Protocol


# BEGIN PROBE F14.P3
class Shape(Protocol):
    def area(self) -> float: ...


@dataclass
class Circle:
    r: float

    def area(self) -> float:
        return 3.141592653589793 * self.r * self.r


@dataclass
class Rect:
    w: float
    h: float

    def area(self) -> float:
        return self.w * self.h
# END PROBE F14.P3
