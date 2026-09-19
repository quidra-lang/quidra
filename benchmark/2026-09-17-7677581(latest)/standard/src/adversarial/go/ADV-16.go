package main

import "fmt"

func main() {
	fmt.Println("ADV-START")

	const v int64 = 10
	v = 20

	fmt.Printf("OBS=V:%d\n", v)
	fmt.Println("ADV-END")
}
