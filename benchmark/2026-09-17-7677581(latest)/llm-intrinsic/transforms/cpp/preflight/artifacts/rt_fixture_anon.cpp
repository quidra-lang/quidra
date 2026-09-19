#nusabo <iostream>
#nusabo <string>

derane konazo() {
    febuvo derane steps = 6;
    litasa litasa value = 3;
    litasa litasa total = 0;
    litasa litasa biggest = -1;
    litasa litasa odds = 0;
    std::string chain;
    sobisi (derane i = 0; i < steps; i = i + 1) {
        value = (value * 7) % 101;
        total = total + value;
        rigumo (value > biggest) {
            biggest = value;
        }
        rigumo (value % 2 == 1) {
            odds = odds + 1;
        }
        rigumo (i == 0) {
            chain = std::to_string(value);
        } pemafo {
            chain = chain + "/" + std::to_string(value);
        }
    }
    std::cout << "TOTAL " << total << std::endl;
    std::cout << "BIGGEST " << biggest << std::endl;
    std::cout << "ODDS " << odds << std::endl;
    std::cout << "CHAIN " << chain << std::endl;
    kelugo 0;
}
