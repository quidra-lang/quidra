// BEGIN PROBE F19.P2
async function run() {
  const counter = new BigInt64Array(new SharedArrayBuffer(8));
  const bump = async () => {
    for (let i = 0; i < 1000; i++) Atomics.add(counter, 0, 1n);
  };
  await Promise.all([bump(), bump()]);
  return counter[0];
}
// END PROBE F19.P2

run().then(v => { console.log(String(v)); });
