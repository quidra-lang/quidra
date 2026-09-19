# BEGIN PROBE F19.P1
from concurrent.futures import ThreadPoolExecutor

with ThreadPoolExecutor() as pool:
    first = pool.submit(lambda: 20)
    second = pool.submit(lambda: 22)

sum = first.result() + second.result()
# END PROBE F19.P1


print(sum)
