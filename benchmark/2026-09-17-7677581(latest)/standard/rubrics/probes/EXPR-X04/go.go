package main
import "fmt"
type Speaker interface{ Speak() string }
type Dog struct{}
type Cat struct{}
func (Dog) Speak() string { return "woof" }
func (Cat) Speak() string { return "meow" }
func pick(n int) Speaker { if n == 0 { return Dog{} }; return Cat{} }
func main() { s := "01"; a := pick(int(s[0] - '0')); b := pick(int(s[1] - '0'))
	fmt.Println("X04", a.Speak(), b.Speak()) }
