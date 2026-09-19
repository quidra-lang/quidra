import threading
box = []
t = threading.Thread(target=lambda: box.append(42))
t.start(); t.join()
print("X19", box[0])
