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
	p, _ := strconv.ParseInt(sc.Text(), 10, 32)
	a := int32(p)
	sc.Scan()
	q, _ := strconv.ParseUint(sc.Text(), 10, 32)
	b := uint32(q)
	v := a < b
	fmt.Printf("OBS=CMP:%t\n", v)
	fmt.Println("ADV-END")
}
