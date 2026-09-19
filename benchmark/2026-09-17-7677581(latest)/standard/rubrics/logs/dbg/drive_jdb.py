import subprocess, sys, time, threading
cmd = sys.argv[1:]
sep = cmd.index('--')
jdb_cmd, steps = cmd[:sep], cmd[sep+1:]
p = subprocess.Popen(jdb_cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=subprocess.STDOUT, text=True, bufsize=1)
out = []
def reader():
    for line in p.stdout:
        out.append(line)
t = threading.Thread(target=reader, daemon=True); t.start()
for s in steps:
    time.sleep(2.0)
    print("### >>> " + s, flush=True)
    try:
        p.stdin.write(s + "\n"); p.stdin.flush()
    except BrokenPipeError:
        break
time.sleep(3.0)
try: p.kill()
except Exception: pass
time.sleep(0.5)
sys.stdout.write("".join(out))
