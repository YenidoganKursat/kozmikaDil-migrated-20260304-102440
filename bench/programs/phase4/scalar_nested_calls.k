def plus(a, b):
  return a + b

def inc(v):
  return plus(v, 1)

def repeat(v):
  return inc(inc(v))

print(repeat(6))
