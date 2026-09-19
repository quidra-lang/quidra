package main

func f(xs [2]int) int { part := xs[1:4]; return part[0] }

func main() { _ = f([2]int{1, 2}) }
