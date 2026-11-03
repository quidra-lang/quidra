#!/usr/bin/env python3
"""Direct backend observation: private temporary reports, no benchmarks."""
import json
import os
import pathlib
import subprocess
import sys
import tempfile

probe=pathlib.Path(sys.argv[1]).resolve()
failure=pathlib.Path(sys.argv[2]).resolve() if len(sys.argv)>2 else None
with tempfile.TemporaryDirectory(prefix="quidra-device-observation-") as temp:
    directory=pathlib.Path(temp)
    def run(name, *, fake=False, timeline=True, profile=True, split=False, hold=False, mixed_core=False):
        env={k:v for k,v in os.environ.items() if not k.startswith("QUIDRA_")}
        env.update(QUIDRA_COUNTERS=str(directory/(name+".counters")),
                   QUIDRA_METAL_IDLE_OPS="100000",QUIDRA_METAL_IDLE_NS="1000000000")
        if fake: env["QUIDRA_TEST_FAKE_GPU_COUNT"]="1"
        if timeline: env["QUIDRA_TIMELINE"]=str(directory/(name+".timeline"))
        if profile: env["QUIDRA_PROFILE"]=str(directory/(name+".profile"))
        if split: env["QUIDRA_PROFILE_SPLIT"]="1"
        result=subprocess.run([str(probe)]+(["--hold"] if hold else [])+(["--mixed-core"] if mixed_core else []),env=env,capture_output=True,text=True,timeout=20)
        if result.returncode==77: return None
        assert result.returncode==0,(name,result.returncode,result.stdout,result.stderr)
        def document(suffix):return json.loads((directory/(name+suffix)).read_text())
        counts=[json.loads(x) for x in (directory/(name+".counters")).read_text().splitlines() if x.strip()][-1]
        return (document(".timeline") if timeline else None,document(".profile") if profile else None,counts)
    fake=run("fake",fake=True)
    t,p,c=fake
    assert t["complete"] and len(t["device_capabilities"])==1
    assert t["device_capabilities"][0]["backend"]=="test"
    assert not t["gpu"] and not t["gpu_steps"] # Host-backed fake device has no measured GPU clock.
    assert any(x["point"]=="device_init_begin" for x in t["marks"])
    assert p["traceEvents"]
    fake_off=run("fake-off",fake=True,timeline=False,profile=False)
    assert c==fake_off[2],"observation changed fake physical counters"
    hardware=run("hardware-off",timeline=False,profile=False)
    if hardware is not None:
        t,p,on=run("hardware-on")
        assert t["complete"] and t["device_capabilities"]
        backend=t["device_capabilities"][0]["backend"]
        if backend!="metal":
            assert backend in {"cuda","hip"}
            assert not t["gpu"] and not t["gpu_steps"],"native alignment has not been supplied"
            assert any(g["reason"]=="gpu_clock_alignment_unavailable" for g in t["gpu_sampling_gaps"])
            for key in ["core_dispatches","allocations","allocation_bytes"]:
                assert on[key]==hardware[2][key],(key,on[key],hardware[2][key])
        else:
            assert t["gpu"]
            assert all(x["gpu_begin_ns"] and x["gpu_end_ns"]>=x["gpu_begin_ns"] for x in t["gpu"])
            sampled=[x for x in t["gpu_steps"] if x["gpu_begin_ns"]]
            if sampled:
                assert {x["step"] for x in sampled}>={0,1}
                extra=sum(x["additional_compute_encoders"] for x in t["gpu_sampling"])
                assert on["metal_compute_encoders"]-hardware[2]["metal_compute_encoders"]==extra==1
            else:
                assert t["gpu_sampling_gaps"] or any(x["setup_gap"] not in {"","none"} for x in t["gpu_sampling"])
            for key in ["core_dispatches","metal_command_buffers","metal_marker_buffers","metal_blit_encoders","allocations","allocation_bytes","metal_new_buffers"]:
                assert on[key]==hardware[2][key],(key,on[key],hardware[2][key])
            held,_,held_counts=run("metal-held",hold=True)
            assert any(x["reason"]=="package_hold_at_boundary" for x in held["gpu_sampling_gaps"])
            assert sum(x["additional_compute_encoders"] for x in held["gpu_sampling"])==0
            _,profile_only,profile_counts=run("metal-profile-only",timeline=False)
            assert profile_only["traceEvents"] and profile_counts["metal_compute_encoders"]==hardware[2]["metal_compute_encoders"]
            coarse=[r for r in profile_only["aggregate"] if r["gpu_ns"] and r["kernel"]!="metal command buffer"]
            assert any(r["owner"]=="core" and r["kernel"]=="metal compute encoder" and not r["site"]["node_id"] for r in coarse),"multi-Core encoder falsely identifies one leaf"
            _,mixed_profile,_=run("metal-mixed-profile",timeline=False,hold=True)
            mixed=[r for r in mixed_profile["aggregate"] if r["gpu_ns"] and r["kernel"]!="metal command buffer"]
            assert any(r["owner"]=="mixed" and r["kernel"]=="metal mixed encoder" and not r["site"]["node_id"] for r in mixed),"package borrow relabeled earlier Core work"
            _,multi_kernel,_=run("metal-multiple-kernels",timeline=False,mixed_core=True)
            assert any(r["owner"]=="core" and r["kernel"]=="metal compute encoder" and r["gpu_ns"] for r in multi_kernel["aggregate"]),"different Core pipelines falsely identify one leaf"
            split_t,_,split_counts=run("metal-split",split=True)
            assert split_t["gpu"] and split_counts["core_dispatches"]==hardware[2]["core_dispatches"]
    if failure:
        env={k:v for k,v in os.environ.items() if not k.startswith("QUIDRA_")}
        env.update(QUIDRA_TEST_FAKE_GPU_COUNT="1",QUIDRA_COUNTERS=str(directory/"failure.counters"),
                   QUIDRA_TIMELINE=str(directory/"failure.timeline"),QUIDRA_PROFILE=str(directory/"failure.profile"))
        result=subprocess.run([str(failure)],env=env,capture_output=True,text=True,timeout=5)
        assert result.returncode==101 and "BOUNDS" in result.stderr,(result.returncode,result.stderr)
        assert (directory/"failure.counters").read_text().strip()
        failed=json.loads((directory/"failure.timeline").read_text())
        assert any(x["point"]=="exit_begin" for x in failed["marks"])
        assert any(x["point"]=="exit_end" for x in failed["marks"])
        assert json.loads((directory/"failure.profile").read_text())["traceEvents"]
print("Device observation backend reports: ok")
