// BEGIN PROBE F19.P1
async function run(): Promise<number> {
  const [a, b] = await Promise.all([(async () => 20)(), (async () => 22)()]);
  const sum = a + b;
  return sum;
}
// END PROBE F19.P1

run().then(s => { console.log(s); });
