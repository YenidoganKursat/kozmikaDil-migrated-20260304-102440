def produce(ch, n):
  i = 1
  while i <= n:
    send(ch, i)
    i = i + 1
  close(ch)

n = 60000
ch = channel(1024)
t = spawn(produce, ch, n)

acc = 0
i = 0
while i < n:
  acc = acc + recv(ch)
  i = i + 1

_ = join(t)
stats = ch.stats()
print(acc)
print(n)
print(stats[3])
print(stats[4])
