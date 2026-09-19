state=7; terms=[]
for _ in range(50):
    state=(state*48271)%2147483647
    terms.append(state%1000)
print("SUM %d"%sum(terms))
print("MAX %d"%max(terms))
print("EVENS %d"%sum(1 for t in terms if t%2==0))
print("JOINED %s"%"-".join(str(t) for t in terms[:5]))
