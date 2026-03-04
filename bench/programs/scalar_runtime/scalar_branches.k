acc = 0
for i in range(10):
  if i < 3:
    acc = acc + 1
  elif i < 6:
    acc = acc + 2
  else:
    acc = acc + 3

print(acc)
