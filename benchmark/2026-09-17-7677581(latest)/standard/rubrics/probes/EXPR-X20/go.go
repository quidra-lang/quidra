package main
import ("encoding/json"; "fmt")
type P struct { Name string `json:"name"`; Age int `json:"age"` }
func main() { b, _ := json.Marshal(P{"alice", 30}); var o P; json.Unmarshal(b, &o)
	fmt.Println("X20", o.Name, o.Age) }
