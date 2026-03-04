def burn(i, iters):
  x = i + 1
  j = 0
  while j < iters:
    x = (x * 1103515245 + 12345) % 2147483647
    j = j + 1
  return x

n = 4096
iters = 1024
parallel_for(0, n, burn, iters)
acc = n * iters

stats = scheduler_stats()
print(acc)
print(n)
print(stats[0])
print(stats[1])
print(stats[2])
print(stats[3])
