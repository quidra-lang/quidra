import json
s = json.dumps({"name": "alice", "age": 30})
o = json.loads(s)
print("X20", o["name"], o["age"])
