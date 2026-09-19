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
	p, _ := strconv.ParseUint(sc.Text(), 10, 32)
	a := uint32(p)
	sc.Scan()
	q, _ := strconv.ParseUint(sc.Text(), 10, 32)
	b := uint32(q)
	v := a - b
	fmt.Printf("OBS=SUB:%d\n", v)
	fmt.Println("ADV-END")
}
