from dataclasses import dataclass
@dataclass
class Circle:
    r: float
@dataclass
class Rect:
    w: float
    h: float
Shape = Circle | Rect
def name(s: Shape) -> str:
    match s:
        case Circle():
            return "circle"
        case Rect():
            return "rect"
print("X02", name(Circle(1.0)), name(Rect(2.0, 3.0)))
