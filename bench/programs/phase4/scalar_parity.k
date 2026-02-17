x = 50001
y = 0
while x > 0:
  if x % 2 == 0:
    y = y + 1
  else:
    y = y - 1
  x = x - 1

print(y)
