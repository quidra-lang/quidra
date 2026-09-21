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
	a, _ := strconv.ParseInt(sc.Text(), 10, 64)
	sc.Scan()
	b, _ := strconv.ParseInt(sc.Text(), 10, 64)
	q := a / b
	fmt.Printf("OBS=QUOT:%d\n", q)
	fmt.Println("ADV-END")
}
