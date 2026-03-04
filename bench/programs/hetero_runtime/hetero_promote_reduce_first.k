N = 120000

i = 0
values = []
while i < N:
  if (i % 2) == 0:
    values.append(i)
  else:
    values.append(i + 0.5)
  i = i + 1

total = values.reduce_sum()
stats = values.cache_stats()
print(total)
print(stats[5])
print(stats[0])
print(stats[1])
print(stats[2])
print(stats[3])
print(values.cache_bytes())
print(values.plan_id())
