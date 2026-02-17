N = 50000
acc = 0
for v in range(N):
  if v % 2 == 0:
    acc = acc + v
  else:
    acc = acc + 2

print(acc)
