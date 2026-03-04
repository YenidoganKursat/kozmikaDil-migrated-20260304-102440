backend = gpu_backend()
print(backend)

ops = ["map", "filter", "set_union", "object_method", "reduce", "matmul_f64"]
plan = gpu_orchestrate("cuda", ops)
print(plan)

print(gpu_supports("cuda", "set_union"))
print(gpu_supports("cuda", "object_method"))
