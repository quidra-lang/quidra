package main

import (
	"bufio"
	"fmt"
	"os"
	"strconv"
)

func main() {
	fmt.Println("ADV-START")

	sc := bufio.NewScanner(os.Stdin)
	sc.Scan()
	n, _ := strconv.ParseInt(sc.Text(), 10, 64)

	r := n * 2

	fmt.Printf("OBS=R:%d\n", r)
	fmt.Println("ADV-END")
}
