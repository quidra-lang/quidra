from dataclasses import dataclass


@dataclass
class Circle:
    r: float


@dataclass
class Rect:
    w: float
    h: float


Shape = Circle | Rect


def area_of(s: Shape) -> float:
    # BEGIN PROBE F14.P2
    match s:
        case Circle(r):
            area = 3.141592653589793 * r * r
        case Rect(w, h):
            area = w * h
    return area
    # END PROBE F14.P2


print(area_of(Circle(2.0)))
