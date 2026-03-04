N = 50000

i = 0
acc = 0
while i < N:
  x = i % 17
  if x == 0:
    acc = acc + (x * x)
  elif x < 12:
    acc = acc + x
  else:
    acc = acc - x
  i = i + 1

print(acc)
