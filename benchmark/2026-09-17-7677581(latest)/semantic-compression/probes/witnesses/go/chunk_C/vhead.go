package main

func head[T any](xs []T) T { return xs[0] }

func main() { head = nil }
