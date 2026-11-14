with open("test/buffer_atomic_contract.c") as f:
    text = f.read()

rename = [
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
for name in rename:
    text = text.replace(name + "(void)", "check_" + name[5:] + "(void)")

wrappers = """
static int
case_reference_share_lifecycle(void)
{
	REQUIRE(check_reference_share_lifecycle() == 0);
	return 0;
}

static int
case_reference_rejections(void)
{
	REQUIRE(check_reference_rejections() == 0);
	return 0;
}

static int
case_transfer_oom_atomic(void)
{
	REQUIRE(check_transfer_oom_atomic() == 0);
	return 0;
}

static int
case_transfer_zero_copy(void)
{
	REQUIRE(check_transfer_zero_copy() == 0);
	return 0;
}

static int
case_transfer_lifecycle_and_reject(void)
{
	REQUIRE(check_transfer_lifecycle_and_reject() == 0);
	return 0;
}

static int
case_callbacks_failure_sync(void)
{
	REQUIRE(check_callbacks_failure_sync() == 0);
	return 0;
}

static int
case_callbacks_failure_preserves_queued(void)
{
	REQUIRE(check_callbacks_failure_preserves_queued() == 0);
	return 0;
}

static int
case_callbacks_success_bytes(void)
{
	REQUIRE(check_callbacks_success_bytes() == 0);
	return 0;
}

static int
case_model_seeded(void)
{
	REQUIRE(check_model_seeded() == 0);
	return 0;
}

"""

anchor = "/* ================================================================== */\n/* case registry"
idx = text.index(anchor)
text = text[:idx] + wrappers + text[idx:]
with open("test/buffer_atomic_contract.c", "w") as f:
    f.write(text)
print("renamed and wrapped")
