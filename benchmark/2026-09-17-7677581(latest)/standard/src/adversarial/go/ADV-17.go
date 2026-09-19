package main

import "fmt"

func main() {
	fmt.Println("ADV-START")

	var p *string
	n := len(*p)

	fmt.Printf("OBS=LEN:%d\n", n)
	fmt.Println("ADV-END")
}
