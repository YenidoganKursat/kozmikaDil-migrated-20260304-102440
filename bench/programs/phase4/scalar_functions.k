def square(v):
  return v * v

def total(n):
  i = 0
  acc = 0
  while i < n:
    acc = acc + square(i)
    i = i + 1
  return acc

print(total(50000))
