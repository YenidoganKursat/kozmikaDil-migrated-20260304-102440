def inc(v):
  return v + 1

def dbl(v):
  return inc(v) * 2

def quad(v):
  return dbl(v) + dbl(v)

N = 500000
acc = 0
i = 0
while i < N:
  acc = acc + quad(i)
  i = i + 1

print(acc)
