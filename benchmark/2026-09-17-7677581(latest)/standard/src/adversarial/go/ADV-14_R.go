package main

import (
	"bufio"
	"fmt"
	"os"
)

func scale(n int64) int64 {
	return n * 3
}

func main() {
	fmt.Println("ADV-START")

	sc := bufio.NewScanner(os.Stdin)
	sc.Scan()
	s := sc.Text()

	r := scale(s)

	fmt.Printf("OBS=R:%d\n", r)
	fmt.Println("ADV-END")
}
