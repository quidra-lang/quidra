package main
import "fmt"
func makeAdder(n int) func(int) int { return func(x int) int { return x + n } }
func main() { f := makeAdder(10); fmt.Println("X05", f(5)) }
