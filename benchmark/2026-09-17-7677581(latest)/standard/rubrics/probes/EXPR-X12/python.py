import dataclasses
@dataclasses.dataclass
class Rec:
    a: int
    b: str
print("X12 meta", len(dataclasses.fields(Rec)))
