n = 80000
values = []
i = 0
while i < n:
  values.append(i % 53)
  if i % 7 == 0:
    values.append(False)
  i = i + 1

iters = 20
j = 0
acc = 0.0
while j < iters:
  acc = acc + values.map_add(1).reduce_sum()
  j = j + 1

stats = values.pipeline_stats()
print(acc)
print(stats[0])
print(stats[1])
print(stats[2])
print(stats[3])
print(stats[4])
print(stats[5])
print(stats[6])
print(stats[7])
