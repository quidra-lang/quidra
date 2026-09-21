package main

import "fmt"

type A struct{ v int64 }

type B struct{ v int64 }

func main() {
	fmt.Println("ADV-START")
	var r any = A{v: 42}
	b := r.(B)
	fmt.Printf("OBS=V:%d\n", b.v)
	fmt.Println("ADV-END")
}
