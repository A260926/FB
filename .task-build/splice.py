with open("test/buffer_atomic_contract.c") as f:
    text = f.read()
with open(".task-build/model_region.c") as f:
    region = f.read()
start_anchor = "struct model_buf {\n"
end_anchor = "static int\ncase_model_seeded(void)"
si = text.index(start_anchor)
ei = text.index(end_anchor)
text = text[:si] + region + "\n" + text[ei:]
with open("test/buffer_atomic_contract.c", "w") as f:
    f.write(text)
print("spliced ok")
