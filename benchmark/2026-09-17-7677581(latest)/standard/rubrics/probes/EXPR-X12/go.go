package main
import ("fmt"; "reflect")
type Rec struct { A int; B string }
func main() { fmt.Println("X12 meta", reflect.TypeOf(Rec{}).NumField()) }
