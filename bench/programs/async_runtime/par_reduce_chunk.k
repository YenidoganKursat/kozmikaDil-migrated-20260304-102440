def heavy(x):
  y = x + 1
  j = 0
  while j < 200:
    y = (y * 1664525 + 1013904223) % 2147483647
    j = j + 1
  return y

def add(a, b):
  return a + b

xs = range(1, 8193)
mapped = par_map(xs, heavy)
out = par_reduce(mapped, 0, add)
print(out)
print(len(xs))
