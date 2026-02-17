acc = 0
for i in range(50000):
  if i < 10:
    acc = acc + 1
  elif i < 20:
    acc = acc + 2
  else:
    acc = acc + 3

print(acc)
