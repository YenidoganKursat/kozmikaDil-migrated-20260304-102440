n = 96
repeats = 2

a = matrix_fill_affine(n, n, 13, 7, 31, 0.05)
b = matrix_fill_affine(n, n, 19, 11, 29, 0.04)
bias = matrix_fill_affine(1, n, 0, 1, 7, 0.3)
acc = matrix_fill_affine(n, n, 1, 1, 9, 0.2)

base = matmul_expected_sum(a, b)
bias_sum = bias.reduce_sum()
acc_sum = acc.reduce_sum()

total = 0.0
r = 0
while r < repeats:
  c1 = a.matmul_add(b, bias)
  c2 = a.matmul_axpby(b, 1.5, 0.25, acc)
  total = total + c1.reduce_sum() + c2.reduce_sum()
  r = r + 1

c1_expected = base + n * bias_sum
c2_expected = 1.5 * base + 0.25 * acc_sum
expected = (c1_expected + c2_expected) * repeats
diff = total - expected
if diff < 0:
  diff = -diff
ok = diff < 1e-6

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
