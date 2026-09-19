from dataclasses import dataclass

from shapes import Shape


# BEGIN PROBE F14.P3
@dataclass
class Tri:
    b: float
    h: float

    def area(self) -> float:
        return 0.5 * self.b * self.h


s: Shape = Tri(3.0, 4.0)
result = s.area()
# END PROBE F14.P3

print(result)
