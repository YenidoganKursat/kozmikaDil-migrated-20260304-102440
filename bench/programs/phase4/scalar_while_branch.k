x = 0
acc = 0
while x < 50000:
  if x < 7:
    acc = acc + x
  elif x < 14:
    acc = acc - 2
  else:
    acc = acc + 3
  x = x + 1

print(acc)
