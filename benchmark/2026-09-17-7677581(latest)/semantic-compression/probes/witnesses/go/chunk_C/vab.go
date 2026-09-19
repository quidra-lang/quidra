package main

type A struct{}
type B struct{}

func main() { var x A = B{}; _ = x }
