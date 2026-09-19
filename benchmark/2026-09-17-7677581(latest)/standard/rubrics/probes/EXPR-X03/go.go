package main
import "fmt"
type Box[T any] struct{ V T }
func (b Box[T]) Get() T { return b.V }
func main() { fmt.Println("X03", Box[int]{5}.Get(), Box[string]{"hi"}.Get()) }
