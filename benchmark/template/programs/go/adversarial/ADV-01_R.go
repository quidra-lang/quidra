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
	v := int32(a)
	fmt.Printf("OBS=V:%d\n", v)
	fmt.Println("ADV-END")
}
