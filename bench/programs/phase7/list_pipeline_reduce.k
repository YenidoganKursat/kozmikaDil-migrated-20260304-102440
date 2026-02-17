n = 120000
values = []
i = 0
while i < n:
  values.append(i % 97)
  i = i + 1

iters = 24
j = 0
acc = 0.0
while j < iters:
  acc = acc + values.map_add(1).filter_gt(15).map_mul(2).reduce_sum()
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
