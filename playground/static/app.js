"use strict";
const examples=[
["Hello",`string name = "Quidra"\nprint("Hello, {name}")\n`],
["Value and storage",`int value = 41\nint &alias = &value\nalias = alias + 1\nprint(value)\n`],
["Arrays",`int[] values = [1, 2, 3]\nint[] extended = values.append(4)\nprint(values[0])\nprint(len(extended))\n`],
["Tensor shape",`tensor<float32><2, 3> values = tensor.ones<float32>([2, 3])\nprint(values[0, 0].item())\n`]
];
const $=s=>document.querySelector(s), source=$("#source"),out=$("#out"),status=$("#status"),time=$("#time"),run=$("#run"),format=$("#format"),select=$("#examples"),tabs=[...document.querySelectorAll("nav button")];
let token="",max=262144,busy=false,cache=new Map();
examples.forEach(([n],i)=>{const o=document.createElement("option");o.value=i;o.textContent=n;select.append(o)});
source.value=localStorage.getItem("quidra-playground-source-v1")??examples[0][1];
source.addEventListener("input",()=>{cache.clear();localStorage.setItem("quidra-playground-source-v1",source.value)});
source.addEventListener("keydown",e=>{if(e.key==="Tab"){e.preventDefault();source.setRangeText("    ",source.selectionStart,source.selectionEnd,"end")}if((e.ctrlKey||e.metaKey)&&e.key==="Enter"){e.preventDefault();execute(e.shiftKey?"check":"run")}});
select.addEventListener("change",()=>{source.value=examples[+select.value][1];cache.clear();source.dispatchEvent(new Event("input"))});
tabs.forEach(t=>t.addEventListener("click",()=>execute(t.dataset.mode)));
run.addEventListener("click",()=>execute("run"));format.addEventListener("click",()=>execute("fmt"));
const meta=fetch("/api/meta").then(r=>r.json()).then(m=>{token=m.request_token;max=m.max_source_bytes;$("#version").textContent=m.version});
function activate(mode){tabs.forEach(t=>t.classList.toggle("active",t.dataset.mode===mode))}
function show(v,mode){status.textContent=v.timed_out?"Timed out":v.ok?"Success":"Failed";status.className=v.ok?"success":"failure";time.textContent=`${v.elapsed_ms} ms`;let text=v.stdout||"";if(mode==="check"&&text){try{const j=JSON.parse(text);text=j.ok?"No diagnostics.\n":j.diagnostics.map(d=>`${d.span?.start?.line??"?"}:${d.span?.start?.column??"?"}  ${d.code}  ${d.message}`).join("\n")}catch{}}if(v.stderr)text+=(text&&!text.endsWith("\n")?"\n":"")+v.stderr;out.textContent=text||(v.ok?"Completed with no output.":"Command failed with no output.")}
async function execute(mode){if(busy)return;if(new TextEncoder().encode(source.value).length>max){status.textContent="Source too large";status.className="failure";return}await meta;busy=true;run.disabled=format.disabled=true;if(mode!=="fmt")activate(mode);status.textContent="Working…";status.className="";time.textContent="";try{const r=await fetch("/api/execute",{method:"POST",headers:{"Content-Type":"application/json","X-Quidra-Playground-Token":token},body:JSON.stringify({mode,source:source.value})});const v=await r.json();if(!r.ok)throw new Error(v.error||`HTTP ${r.status}`);if(mode==="fmt"){if(v.ok){source.value=v.stdout;source.dispatchEvent(new Event("input"));status.textContent="Formatted";status.className="success";time.textContent=`${v.elapsed_ms} ms`}else{activate("check");show(v,"check")}}else{cache.set(mode,v);show(v,mode)}}catch(e){status.textContent="Playground error";status.className="failure";out.textContent=e.message}finally{busy=false;run.disabled=format.disabled=false}}
