rows = 256
cols = 256

mat_a = matrix_i64(rows, cols)
mat_b = matrix_i64(rows, cols)
r = 0
while r < rows:
  c = 0
  while c < cols:
    mat_a[r, c] = (r * 31 + c * 17) % 97
    mat_b[r, c] = (r * 19 + c * 7) % 101
    c = c + 1
  r = r + 1

sum = 0
r = 0
while r < rows:
  c = 0
  while c < cols:
    v = mat_a[r, c] * mat_b[r, c] + 1
    sum = sum + v
    c = c + 1
  r = r + 1

print(sum)
