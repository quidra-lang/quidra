function main(): void {
    console.log("ADV-START");

    const x: bigint = 10n;
    x = 20n;

    console.log("OBS=V:" + String(x));
    console.log("ADV-END");
}

main();
