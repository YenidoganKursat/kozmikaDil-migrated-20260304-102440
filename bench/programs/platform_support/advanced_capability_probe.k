# Advanced capability probe for portability validation.

check_int = 0
a = 2 + 4
b = a * 3
c = b - 5
d = c / 2
if c == 13:
  if d == 6.5:
    check_int = 1

check_float = 0
f = 3.5
g = 0.5
h = f - g
h2 = h * 2.0
if h2 >= 5.9:
  if h2 <= 6.1:
    check_float = 1

check_loop = 0
i = 0
acc = 0
while i < 10:
  if i % 2 == 0:
    acc = acc + i * i
  else:
    acc = acc - i
  i = i + 1
if acc == 95:
  check_loop = 1

check_branch = 0
x = 7
if x > 4:
  y = 3
  if y == 3:
    check_branch = 1

check_edge = 0
m = 0 - 1
m = m + 10
if m == 9:
  check_edge = 1

check_all = check_int * check_float * check_loop * check_branch * check_edge
print(check_int)
print(check_float)
print(check_loop)
print(check_branch)
print(check_edge)
print(check_all)
