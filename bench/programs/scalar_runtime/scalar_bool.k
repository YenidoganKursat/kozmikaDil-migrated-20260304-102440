i = 0
y = 0
while i < 50000:
  if (i % 2 == 0) or (i % 3 == 0):
    y = y + 1
  else:
    y = y - 1
  i = i + 1

print(y)
