package main
import ("fmt"; "iter")
func Upto(n int) iter.Seq[int] {
	return func(yield func(int) bool) { for i := 0; i < n; i++ { if !yield(i) { return } } }
}
func main() { fmt.Print("X07"); for v := range Upto(3) { fmt.Print(" ", v) }; fmt.Println() }
