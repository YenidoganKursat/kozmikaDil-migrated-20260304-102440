class User(slots):
  id = 0
  name = ""
  age = 0

  def init(self, id, name, age):
    self.id = id
    self.name = name
    self.age = age

  def birthday(self):
    self.age = self.age + 1

  def get_age(self):
    return self.age

u = User(101, "Ada", 30)
u.birthday()
print(u.id)
print(u.name)
print(u.get_age())
