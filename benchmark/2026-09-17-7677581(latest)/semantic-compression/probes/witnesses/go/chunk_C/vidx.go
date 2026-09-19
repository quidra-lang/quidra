package main

type T struct{ a int }

func f(xs T, i int) int { e := xs[i]; return e }

func main() { _ = f(T{}, 0) }
