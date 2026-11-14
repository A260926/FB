with open("test/buffer_atomic_contract.c") as f:
    text = f.read()

wrappers = [
    "case_reference_oom_atomic",
    "case_reference_share_lifecycle",
    "case_reference_rejections",
    "case_transfer_oom_atomic",
    "case_transfer_zero_copy",
    "case_transfer_lifecycle_and_reject",
    "case_callbacks_failure_sync",
    "case_callbacks_failure_preserves_queued",
    "case_callbacks_success_bytes",
    "case_model_seeded",
]
for name in wrappers:
    old = "{\n\tREQUIRE(check_" + name[5:] + "() == 0);\n\treturn 0;\n}"
    new = "{\n\tensure_allocator();\n\tREQUIRE(check_" + name[5:] + "() == 0);\n\treturn 0;\n}"
    if old not in text:
        raise SystemExit("missing wrapper for " + name)
    text = text.replace(old, new)

with open("test/buffer_atomic_contract.c", "w") as f:
    f.write(text)
print("ensured")
