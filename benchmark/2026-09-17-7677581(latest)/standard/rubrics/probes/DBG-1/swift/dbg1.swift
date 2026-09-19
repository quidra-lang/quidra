// DBG-1
struct Item {
    var count: Int
    var name: String
}

let ITERATIONS: Int = 300000000
let MODULUS: Int = 1000000007

func accumulate(_ items: [Item], _ iterations: Int) -> Int {
    var total: Int = 0
    for _ in 0..<iterations {
        for item in items {
            total = (total * 31 + item.count + item.name.count) % MODULUS
        }
    }
    return total
}

var items: [Item] = []
items.append(Item(count: 7, name: "alpha"))
items.append(Item(count: 11, name: "bravo"))
items.append(Item(count: 13, name: "charlie"))
let checksum = accumulate(items, ITERATIONS)
print("checksum=\(checksum)")
