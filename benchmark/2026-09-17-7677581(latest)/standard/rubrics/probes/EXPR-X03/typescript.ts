class Box<T> { constructor(private v: T) {} get(): T { return this.v; } }
console.log("X03", new Box<number>(5).get(), new Box<string>("hi").get());
