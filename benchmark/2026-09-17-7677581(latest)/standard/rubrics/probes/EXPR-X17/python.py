import math, sys
assert sys.float_info.mant_dig == 53 and sys.float_info.radix == 2
print("X17", round(math.sqrt(4.0)), round(math.exp(0.0)), round(math.log(1.0)),
      round(math.sin(0.0)), round(math.cos(0.0)))
