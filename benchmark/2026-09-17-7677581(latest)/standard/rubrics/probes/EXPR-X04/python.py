from typing import Protocol
import sys
class Speaker(Protocol):
    def speak(self) -> str: ...
class Dog:
    def speak(self) -> str: return "woof"
class Cat:
    def speak(self) -> str: return "meow"
def pick(n: int) -> Speaker:
    return Dog() if n == 0 else Cat()
out = [pick(int(c)).speak() for c in "01"]
print("X04", out[0], out[1])
