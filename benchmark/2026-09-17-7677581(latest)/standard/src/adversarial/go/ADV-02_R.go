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
	p, _ := strconv.ParseInt(sc.Text(), 10, 64)
	a := int(p)
	v := a + 3
	fmt.Printf("OBS=V:%d\n", v)
	fmt.Println("ADV-END")
}
