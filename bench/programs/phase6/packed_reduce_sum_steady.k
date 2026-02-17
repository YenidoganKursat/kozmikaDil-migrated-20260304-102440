N = 120000
ITER = 80

i = 0
values = []
while i < N:
  values.append(i)
  i = i + 1

warm = values.reduce_sum()
acc = 0
i = 0
while i < ITER:
  acc = acc + values.reduce_sum()
  i = i + 1

stats = values.cache_stats()
print(acc + warm)
print(stats[5])
print(stats[0])
print(stats[1])
print(stats[2])
print(stats[3])
print(values.cache_bytes())
print(values.plan_id())
