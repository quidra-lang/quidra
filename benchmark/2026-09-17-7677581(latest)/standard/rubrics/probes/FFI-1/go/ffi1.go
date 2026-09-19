package main

/*
#include <math.h>
*/
import "C"
import "fmt"

func main() { fmt.Printf("cos(1.0)=%.10f\n", float64(C.cos(C.double(1.0)))) }
