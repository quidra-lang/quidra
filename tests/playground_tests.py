#!/usr/bin/env python3
import importlib.util,json,os,stat,sys,tempfile,threading,urllib.error,urllib.request
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]; SERVER=ROOT/"playground"/"server.py"
spec=importlib.util.spec_from_file_location("quidra_playground",SERVER); assert spec and spec.loader
mod=importlib.util.module_from_spec(spec); sys.modules[spec.name]=mod; spec.loader.exec_module(mod)

def fake_quidra(root:Path)->Path:
    path=root/"quidra"; path.write_text("#!/usr/bin/env python3\nimport json,pathlib,sys\nif sys.argv[1:]==['--version']: print('quidra test');raise SystemExit()\nm=sys.argv[1];p=pathlib.Path(sys.argv[2]);t=p.read_text()\nif m=='fmt':p.write_text(t.rstrip()+chr(10))\nelif m=='check':print(json.dumps({'ok':True,'diagnostics':[]}))\nelif m=='run':print('run:'+t.strip())\nelif m=='ir':print('typed-ir')\nelif m=='llvm':print('; llvm-ir')\nelse:raise SystemExit(2)\n"); path.chmod(path.stat().st_mode|stat.S_IXUSR); return path

def main()->int:
    for name in ("index.html","styles.css","app.js"): assert (ROOT/"playground"/"static"/name).is_file()
    with tempfile.TemporaryDirectory() as tmp:
        backend=mod.Backend(fake_quidra(Path(tmp)),2); assert backend.version=="quidra test"
        assert json.loads(backend.execute("check",'print("x")\n')["stdout"])["ok"]
        assert backend.execute("fmt",'print("x")\n\n')["stdout"]=='print("x")\n'
        assert "run:print" in backend.execute("run",'print("x")\n')["stdout"]
        server=mod.Server(("127.0.0.1",0),backend); thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
        try:
            host,port=server.server_address[:2];base=f"http://{host}:{port}"
            with urllib.request.urlopen(base+"/api/meta",timeout=3) as r: meta=json.load(r)
            assert meta["local_only"] and meta["version"]=="quidra test"
            data=json.dumps({"mode":"run","source":'print("web")\n'}).encode()
            req=urllib.request.Request(base+"/api/execute",data=data,headers={"Content-Type":"application/json","X-Quidra-Playground-Token":meta["request_token"]})
            with urllib.request.urlopen(req,timeout=3) as r: result=json.load(r)
            assert result["ok"] and "run:print" in result["stdout"]
            try: urllib.request.urlopen(base+"/../server.py",timeout=3); raise AssertionError("path traversal")
            except urllib.error.HTTPError as e: assert e.code==404
        finally: server.shutdown();server.server_close();thread.join(3)
    print("playground tests: ok"); return 0
if __name__=="__main__": raise SystemExit(main())
