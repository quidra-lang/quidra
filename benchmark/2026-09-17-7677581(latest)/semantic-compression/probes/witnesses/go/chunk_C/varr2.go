package main

import "fmt"

func f(xs *[5]int) int { part := xs[1:4]; return part[0] }

func main() { a := [5]int{10, 20, 30, 40, 50}; fmt.Println(f(&a)); a[1] = 77; fmt.Println(f(&a)) }
