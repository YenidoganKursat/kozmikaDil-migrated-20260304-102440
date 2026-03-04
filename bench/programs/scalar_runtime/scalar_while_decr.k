x = 50000
acc = 0
while x > 0:
  if x % 3 == 0:
    acc = acc + 3
  else:
    acc = acc + 1
  x = x - 1

print(acc)
