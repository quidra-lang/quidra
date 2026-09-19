package main

import "fmt"

func main() {
	fmt.Println("ADV-START")
	v := int(-9223372036854775808) - 3
	fmt.Printf("OBS=V:%d\n", v)
	fmt.Println("ADV-END")
}
