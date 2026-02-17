N = 400000

i = 0
values = []
while i < N:
  values.append(i)
  i = i + 1

sum = 0
for x in values:
  sum = sum + x

print(sum)
