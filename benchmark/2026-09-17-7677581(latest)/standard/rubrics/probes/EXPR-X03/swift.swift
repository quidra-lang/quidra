struct Box<T> { let v: T; func get() -> T { v } }
print("X03", Box(v: 5).get(), Box(v: "hi").get())
