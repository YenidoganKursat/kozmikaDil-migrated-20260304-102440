TYPE_USER = 1
TYPE_ADDRESS = 2
TYPE_ITEM = 3

obj_type = []
obj_i64_0 = []
obj_i64_1 = []
obj_f64_0 = []
obj_ref_0 = []
obj_vec_begin = []
obj_vec_count = []
obj_vec_data = []
user_handles = []

users = 300
items_per_user = 8

u = 0
while u < users:
  addr_h = len(obj_type)
  obj_type.append(TYPE_ADDRESS)
  obj_i64_0.append((u % 81) + 1)
  obj_i64_1.append(10000 + (u % 90000))
  obj_f64_0.append(0.0)
  obj_ref_0.append(-1)
  obj_vec_begin.append(0)
  obj_vec_count.append(0)

  user_h = len(obj_type)
  obj_type.append(TYPE_USER)
  obj_i64_0.append(100000 + u)
  obj_i64_1.append(0)
  obj_f64_0.append(0.0)
  obj_ref_0.append(addr_h)
  obj_vec_begin.append(len(obj_vec_data))
  obj_vec_count.append(0)
  user_handles.append(user_h)

  j = 0
  while j < items_per_user:
    item_h = len(obj_type)
    obj_type.append(TYPE_ITEM)
    obj_i64_0.append(1000 + j)
    obj_i64_1.append(0)
    obj_f64_0.append((j + 1) * 0.75 + (u % 7) * 0.01)
    obj_ref_0.append(-1)
    obj_vec_begin.append(0)
    obj_vec_count.append(0)

    if obj_vec_count[user_h] == 0:
      obj_vec_begin[user_h] = len(obj_vec_data)
    obj_vec_data.append(item_h)
    obj_vec_count[user_h] = obj_vec_count[user_h] + 1
    j = j + 1
  u = u + 1

total = 0.0
ui = 0
while ui < len(user_handles):
  user_h = user_handles[ui]
  n_items = obj_vec_count[user_h]
  j = 0
  while j < n_items:
    item_h = obj_vec_data[obj_vec_begin[user_h] + j]
    total = total + obj_f64_0[item_h]
    j = j + 1
  ui = ui + 1

n = 32
a = matrix_fill_affine(n, n, 17, 3, 19, 0.125)
b = matrix_fill_affine(n, n, 11, 5, 23, 0.1)
expected = matmul_expected_sum(a, b)
out = a.matmul_f64(b)
got = out.reduce_sum()
diff = got - expected
if diff < 0:
  diff = -diff
ok = diff < 1e-6

schedule = a.matmul_schedule()
backend_id = 0
if len(schedule) > 9:
  backend_id = schedule[9]
elif len(schedule) > 0:
  backend_id = schedule[len(schedule) - 1]

print(total)
print(diff)
print(ok)
print(backend_id)
