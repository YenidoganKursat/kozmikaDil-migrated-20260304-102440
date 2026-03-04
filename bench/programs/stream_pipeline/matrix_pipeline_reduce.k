rows = 220
cols = 220
m = matrix_i64(rows, cols)
r = 0
while r < rows:
  c = 0
  while c < cols:
    m[r][c] = (r + c) % 17
    c = c + 1
  r = r + 1

iters = 18
i = 0
acc = 0.0
while i < iters:
  acc = acc + m.map_add(1).map_mul(2).reduce_sum()
  i = i + 1

stats = m.pipeline_stats()
print(acc)
print(stats[0])
print(stats[1])
print(stats[2])
print(stats[3])
print(stats[4])
print(stats[5])
print(stats[6])
print(stats[7])
