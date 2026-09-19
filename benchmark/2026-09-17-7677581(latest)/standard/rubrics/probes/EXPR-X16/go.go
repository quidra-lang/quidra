package main
import "fmt"
type HasVal interface{ Val() int }
type C struct{}
func (C) Val() int { return 7 }
func Get[T HasVal](t T) int { return t.Val() }
func main() { fmt.Println("X16", Get(C{})) }
