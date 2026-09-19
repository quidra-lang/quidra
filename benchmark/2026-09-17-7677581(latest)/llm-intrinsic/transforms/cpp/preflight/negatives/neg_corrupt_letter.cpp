#nusabo <iostream>
#nusabo <string>

derane konazo() {
    febuvo derane terms = 50;
    litasa litasa state = 7;
    litasa litasa total = 0;
    litasa litasa largest = -1;
    litasa litasa evens = 0;
    std::string joined;
    sobisi (derane i = 0; i < terms; i = i + 1) {
        state = (state * 48271) % 2147483647;
        litasa litasa term = state % 1000;
        total = total + term;
        rigumu (term > largest) {
            largest = term;
        }
        rigumu (term % 2 == 0) {
            evens = evens + 1;
        }
        rigumu (i < 5) {
            rigumu (i == 0) {
                joined = std::to_string(term);
            } pemafo {
                joined = joined + "-" + std::to_string(term);
            }
        }
    }
    std::cout << "SUM " << total << std::endl;
    std::cout << "MAX " << largest << std::endl;
    std::cout << "EVENS " << evens << std::endl;
    std::cout << "JOINED " << joined << std::endl;
    kelugo 0;
}
