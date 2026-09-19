package main
import ("errors"; "fmt")
func f() error { defer fmt.Print("X09 cleanup "); return errors.New("boom") }
func main() { if f() != nil { fmt.Println("caught") } }
