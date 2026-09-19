sobini doginu () {
    gutati zefegi kegudu: zikeza = 7;
    gutati zefegi sum: zikeza = 0;
    gutati zefegi max: zikeza = 0;
    gutati zefegi evens: zikeza = 0;
    gutati zefegi joined: kegudu = kegudu ::new();
    gutati zefegi i: zikeza = 0;
    mavenu i < 50 {
        kegudu = (kegudu * 48271) % 2147483647;
        gutati term: zikeza = kegudu % 1000;
        sum = sum + term;
        tiroro term > max {
            max = term;
        }
        tiroro term % 2 == 0 {
            evens = evens + 1;
        }
        tiroro i < 5 {
            tiroro i == 0 {
                joined = format!("{}", term);
            } mozome {
                joined = format!("{}-{}", joined, term);
            }
        }
        i = i + 1;
    }
    println!("SUM {}", sum);
    println!("MAX {}", max);
    println!("EVENS {}", evens);
    println!("JOINED {}", joined);
}
