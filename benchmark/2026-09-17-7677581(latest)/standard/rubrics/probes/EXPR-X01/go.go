package main
import "fmt"
type Point struct{ X, Y int }
func main() { p := Point{3, 4}; fmt.Println("X01", p.X, p.Y) }
