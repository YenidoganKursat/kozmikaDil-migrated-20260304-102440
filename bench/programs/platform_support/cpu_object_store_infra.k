# CPU Object Store Infrastructure (flat + handle)
# ------------------------------------------------
# Bu altyapı, iç içe objeleri CPU'da pointer yerine handle/index ile taşır.
# Aynı model GPU backend için de uygundur (flat bellek + offset erişimi).

# Type ids
TYPE_USER = 1
TYPE_ADDRESS = 2
TYPE_ITEM = 3

# Object header columns (row = object handle)
obj_type = []
obj_i64_0 = []
obj_i64_1 = []
obj_f64_0 = []
obj_f64_1 = []
obj_ref_0 = []
obj_ref_1 = []
obj_vec_begin = []
obj_vec_count = []

# Variable-length reference arena (nested list fields)
obj_vec_data = []

def os_new(type_id):
  h = len(obj_type)
  obj_type.append(type_id)
  obj_i64_0.append(0)
  obj_i64_1.append(0)
  obj_f64_0.append(0.0)
  obj_f64_1.append(0.0)
  obj_ref_0.append(-1)
  obj_ref_1.append(-1)
  obj_vec_begin.append(0)
  obj_vec_count.append(0)
  return h

def os_set_i64_0(h, v):
  obj_i64_0[h] = v

def os_set_i64_1(h, v):
  obj_i64_1[h] = v

def os_set_f64_0(h, v):
  obj_f64_0[h] = v

def os_set_f64_1(h, v):
  obj_f64_1[h] = v

def os_set_ref_0(h, ref_h):
  obj_ref_0[h] = ref_h

def os_set_ref_1(h, ref_h):
  obj_ref_1[h] = ref_h

def os_get_i64_0(h):
  return obj_i64_0[h]

def os_get_i64_1(h):
  return obj_i64_1[h]

def os_get_f64_0(h):
  return obj_f64_0[h]

def os_get_f64_1(h):
  return obj_f64_1[h]

def os_get_ref_0(h):
  return obj_ref_0[h]

def os_get_ref_1(h):
  return obj_ref_1[h]

# Reset nested vector field for object h.
def os_vec_clear(h):
  obj_vec_begin[h] = len(obj_vec_data)
  obj_vec_count[h] = 0

# Push nested object handle into object h vector field.
def os_vec_push(h, ref_h):
  if obj_vec_count[h] == 0:
    obj_vec_begin[h] = len(obj_vec_data)
  obj_vec_data.append(ref_h)
  obj_vec_count[h] = obj_vec_count[h] + 1

def os_vec_len(h):
  return obj_vec_count[h]

def os_vec_get(h, j):
  return obj_vec_data[obj_vec_begin[h] + j]

# ---------------- Domain layer on top of store ----------------
# Address: i64_0=city_id, i64_1=zip
def new_address(city_id, zip_code):
  h = os_new(TYPE_ADDRESS)
  os_set_i64_0(h, city_id)
  os_set_i64_1(h, zip_code)
  return h

# Item: i64_0=item_id, f64_0=price
def new_item(item_id, price):
  h = os_new(TYPE_ITEM)
  os_set_i64_0(h, item_id)
  os_set_f64_0(h, price)
  return h

# User: i64_0=user_id, ref_0=address_handle, vec=items
def new_user(user_id, address_h):
  h = os_new(TYPE_USER)
  os_set_i64_0(h, user_id)
  os_set_ref_0(h, address_h)
  os_vec_clear(h)
  return h

def user_add_item(user_h, item_h):
  os_vec_push(user_h, item_h)

def user_city(user_h):
  addr_h = os_get_ref_0(user_h)
  return os_get_i64_0(addr_h)

def user_zip(user_h):
  addr_h = os_get_ref_0(user_h)
  return os_get_i64_1(addr_h)

def user_item_price_sum(user_h):
  n = os_vec_len(user_h)
  j = 0
  total = 0.0
  while j < n:
    item_h = os_vec_get(user_h, j)
    total = total + os_get_f64_0(item_h)
    j = j + 1
  return total

# ---------------- Example usage ----------------
addr_a = new_address(34, 34000)
addr_b = new_address(6, 61000)

item_a1 = new_item(1001, 10.5)
item_a2 = new_item(1002, 4.25)
item_b1 = new_item(2001, 7.75)

user_a = new_user(90001, addr_a)
user_b = new_user(90002, addr_b)

user_add_item(user_a, item_a1)
user_add_item(user_a, item_a2)
user_add_item(user_b, item_b1)

print(os_get_i64_0(user_a))      # user_id
print(user_city(user_a))         # address.city_id
print(user_zip(user_a))          # address.zip
print(user_item_price_sum(user_a))

u = 0
grand_total = 0.0
while u < len(obj_type):
  if obj_type[u] == TYPE_USER:
    grand_total = grand_total + user_item_price_sum(u)
  u = u + 1
print(grand_total)
