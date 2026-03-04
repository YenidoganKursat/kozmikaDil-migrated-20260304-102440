N = 2000000

def step(v):
  if (v % 3) == 0:
    return v * 5 + 1
  elif (v % 3) == 1:
    return v * 3 + 7
  else:
    return v * 2 + 11

i = 0
total = 0
while i < N:
  total = total + step(i % 1024)
  i = i + 1

print(total)
