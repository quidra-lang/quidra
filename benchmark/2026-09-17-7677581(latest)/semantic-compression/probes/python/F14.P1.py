from dataclasses import dataclass


# BEGIN PROBE F14.P1
@dataclass
class Circle:
    r: float


@dataclass
class Rect:
    w: float
    h: float


Shape = Circle | Rect
s: Shape = Circle(2.0)
# END PROBE F14.P1

print(s.r)
