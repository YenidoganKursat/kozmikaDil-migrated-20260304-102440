n = 512
repeats = 1

a = matrix_fill_affine(n, n, 17, 3, 19, 0.125)
b = matrix_fill_affine(n, n, 11, 5, 23, 0.1)
expected_single = matmul_expected_sum(a, b)

r = 0
total = 0.0
while r < repeats:
  out = a.matmul_f64(b)
  total = total + out.reduce_sum()
  r = r + 1

expected = expected_single * repeats
diff = total - expected
if diff < 0:
  diff = -diff
ok = diff < 1e-4

stats = a.matmul_stats()
schedule = a.matmul_schedule()

print(total)
print(expected)
print(diff)
print(ok)
print(stats[0])
print(stats[1])
print(stats[2])
print(stats[5])
print(stats[6])
print(stats[7])
print(schedule[1])
print(schedule[2])
print(schedule[3])
print(schedule[9])
