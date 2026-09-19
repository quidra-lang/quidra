// BEGIN PROBE F19.P1
async function run() {
  const first = (async () => 20)();
  const second = (async () => 22)();
  const sum = await first + await second;
  return sum;
}
// END PROBE F19.P1

run().then(s => { console.log(s); });
