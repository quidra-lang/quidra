package main

func stringCase(xs string) byte { part := xs[1:4]; part[0] = 'z'; return part[0] }

func main() { _ = stringCase("abcde") }
