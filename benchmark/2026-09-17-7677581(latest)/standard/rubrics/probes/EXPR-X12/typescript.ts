class Rec { a: number = 0; b: string = ""; }
const names = Object.getOwnPropertyNames(new Rec());
console.log("X12 meta", names.length);
