package main
import "fmt"
func main() { buf := []int{0, 0, 0, 0}; view := buf[1:3]; view[0] = 99; view[1] = 99
	fmt.Println("X23", buf[1], buf[2]) }
