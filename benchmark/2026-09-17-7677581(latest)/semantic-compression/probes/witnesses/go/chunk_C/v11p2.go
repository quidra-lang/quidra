package main

import "fmt"

func sliceCase(xs []int) int  { part := xs[1:4]; return part[0] }
func arrayCase(xs *[5]int) int { part := xs[1:4]; return part[0] }
func stringCase(xs string) byte { part := xs[1:4]; return part[0] }

func main() {
	xs := []int{10, 20, 30, 40, 50}
	fmt.Println("slice", sliceCase(xs))
	xs[1] = 99
	fmt.Println("slice-after-write", sliceCase(xs))
	ar := [5]int{10, 20, 30, 40, 50}
	fmt.Println("array", arrayCase(&ar))
	fmt.Println("string", stringCase("abcde"))
}
