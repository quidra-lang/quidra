package main
import ("fmt"; "math/big")
func main() { v := new(big.Int).Exp(big.NewInt(2), big.NewInt(70), nil); fmt.Println("X24", v.String()) }
