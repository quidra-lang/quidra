import Foundation
struct Person: Codable { var name: String; var age: Int }
let data = try! JSONEncoder().encode(Person(name: "alice", age: 30))
let q = try! JSONDecoder().decode(Person.self, from: data)
print("X20", q.name, q.age)
