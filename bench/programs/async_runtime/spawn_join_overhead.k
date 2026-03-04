def identity(x):
  return x + 1

n = 8000
i = 0
acc = 0
while i < n:
  t = spawn(identity, i)
  acc = acc + join(t)
  i = i + 1

stats = scheduler_stats()
print(acc)
print(n)
print(stats[1])
print(stats[2])
