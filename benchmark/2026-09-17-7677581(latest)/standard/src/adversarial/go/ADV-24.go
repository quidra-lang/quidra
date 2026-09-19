package main

import "fmt"

func main() {
	fmt.Println("ADV-START")

	var v int = 9223372036854775809

	fmt.Printf("OBS=V:%d\n", v)
	fmt.Println("ADV-END")
}
