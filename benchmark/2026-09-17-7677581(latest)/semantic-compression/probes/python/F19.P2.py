# BEGIN PROBE F19.P2
import threading
from concurrent.futures import ThreadPoolExecutor

counter = 0
lock = threading.Lock()


def bump():
    global counter
    for _ in range(1000):
        with lock:
            counter += 1


with ThreadPoolExecutor() as pool:
    pool.submit(bump)
    pool.submit(bump)
# END PROBE F19.P2


print(counter)
