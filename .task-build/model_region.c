struct model_buf {
	unsigned char bytes[MODEL_CAP];
	int ids[MODEL_CAP];
	size_t len;
	int has_mcast;
};

struct model_state {
	struct evbuffer *real[MODEL_NBUFS];
	struct model_buf model[MODEL_NBUFS];
	struct model_block blocks[MODEL_MAX_BLOCKS];
	size_t n_blocks;
	unsigned seed;
};

static unsigned
rng_next(unsigned *state)
{
	unsigned x = *state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return x;
}

static void
model_reset_buf(struct model_buf *m)
{
	m->len = 0;
	m->has_mcast = 0;
}

static int
model_append(struct model_buf *m, const unsigned char *data, size_t n, int id)
{
	size_t i;
	if (m->len + n > MODEL_CAP)
		return -1;
	for (i = 0; i < n; ++i) {
		m->bytes[m->len] = data[i];
		m->ids[m->len] = id;
		++m->len;
	}
	return 0;
}

static int
model_can_fit(const struct model_buf *m, size_t n)
{
	return m->len + n <= MODEL_CAP;
}

static int
model_append_copy(struct model_buf *m, const unsigned char *bytes,
    const int *ids, size_t n)
{
	if (!model_can_fit(m, n))
		return -1;
	memcpy(m->bytes + m->len, bytes, n);
	memcpy(m->ids + m->len, ids, n * sizeof(int));
	m->len += n;
	return 0;
}

static void
model_front_remove(struct model_buf *m, size_t n)
{
	memmove(m->bytes, m->bytes + n, m->len - n);
	memmove(m->ids, m->ids + n, (m->len - n) * sizeof(int));
	m->len -= n;
	/* Once empty, every chain (multicast included) has been released. */
	if (m->len == 0)
		m->has_mcast = 0;
}

/* Field-wise comparison: never rely on raw struct padding bytes. */
static int
model_buf_equal(const struct model_buf *a, const struct model_buf *b)
{
	if (a->len != b->len || a->has_mcast != b->has_mcast)
		return 0;
	if (a->len && memcmp(a->bytes, b->bytes, a->len) != 0)
		return 0;
	if (a->len && memcmp(a->ids, b->ids, a->len * sizeof(int)) != 0)
		return 0;
	return 1;
}

static int
model_block_alive(const struct model_state *ms, int id)
{
	size_t b, i;
	for (b = 0; b < MODEL_NBUFS; ++b)
		for (i = 0; i < ms->model[b].len; ++i)
			if (ms->model[b].ids[i] == id)
				return 1;
	return 0;
}

static struct buffer_model_result
model_fail(const struct model_state *ms, unsigned op_index, const char *field,
    size_t mv, size_t rv)
{
	struct buffer_model_result res;
	memset(&res, 0, sizeof(res));
	res.failed = 1;
	res.seed = ms->seed;
	res.op_index = op_index;
	res.field = field;
	res.model_value = mv;
	res.real_value = rv;
	return res;
}

/* Compare every real buffer against its model: visible bytes and length. */
static struct buffer_model_result
model_compare(const struct model_state *ms, unsigned op_index)
{
	unsigned char tmp[MODEL_CAP + 8];
	size_t b;

	for (b = 0; b < MODEL_NBUFS; ++b) {
		size_t rlen = evbuffer_get_length(ms->real[b]);
		ev_ssize_t got;
		if (rlen != ms->model[b].len)
			return model_fail(ms, op_index, "length",
			    ms->model[b].len, rlen);
		got = evbuffer_copyout(ms->real[b], tmp, rlen);
		if (got != (ev_ssize_t)rlen)
			return model_fail(ms, op_index, "copyout", rlen,
			    (size_t)got);
		if (rlen && memcmp(tmp, ms->model[b].bytes, rlen) != 0)
			return model_fail(ms, op_index, "bytes", 0, 1);
	}
	/* Cleanup safety: a cleaned origin block must have no surviving
	 * bytes anywhere. */
	{
		size_t k;
		for (k = 0; k < ms->n_blocks; ++k) {
			if (ms->blocks[k].used &&
			    ms->blocks[k].ext.cleanup_calls > 0 &&
			    model_block_alive(ms, ms->blocks[k].ext.id))
				return model_fail(ms, op_index, "cleanup_live",
				    (size_t)ms->blocks[k].ext.id,
				    (size_t)ms->blocks[k].ext.cleanup_calls);
		}
	}
	{
		struct buffer_model_result ok;
		memset(&ok, 0, sizeof(ok));
		ok.seed = ms->seed;
		return ok;
	}
}

static struct model_block *
model_new_block(struct model_state *ms)
{
	struct model_block *mb;
	size_t len;
	if (ms->n_blocks >= MODEL_MAX_BLOCKS)
		return NULL;
	mb = &ms->blocks[ms->n_blocks];
	memset(mb, 0, sizeof(*mb));
	len = 1 + (ms->n_blocks * 3) % 9; /* 1..9 bytes, distinct per block */
	block_init(&mb->ext, (int)ms->n_blocks + 1, len);
	mb->used = 1;
	++ms->n_blocks;
	return mb;
}

/* Whether the model will attempt a share on this pair.  Unsupported pairs
 * are skipped rather than guessed at, so the model only predicts outcomes
 * for combinations the interface explicitly supports. */
static int
model_share_allowed(const struct model_state *ms, int dst, int src)
{
	if (dst == src)
		return 0;
	if (ms->model[src].len == 0)
		return 1; /* documented no-op success */
	if (ms->model[src].has_mcast)
		return 0;
	return 1;
}

struct buffer_model_result
buffer_model_run(unsigned seed, unsigned max_ops)
{
	struct model_state ms;
	unsigned rng, op_index;
	size_t b, k;
	struct buffer_model_result res;

	ensure_allocator();
	buffer_test_allocator_reset();
	memset(&ms, 0, sizeof(ms));
	ms.seed = seed;
	rng = seed ? seed : 0x9e3779b9u;
	max_ops = max_ops ? max_ops : MODEL_MAX_OPS;
	if (max_ops > MODEL_MAX_OPS)
		max_ops = MODEL_MAX_OPS;

	for (b = 0; b < MODEL_NBUFS; ++b) {
		ms.real[b] = evbuffer_new();
		model_reset_buf(&ms.model[b]);
		if (!ms.real[b]) {
			res.failed = 1;
			res.field = "evbuffer_new";
			res.seed = seed;
			goto fail;
		}
	}

	res = model_compare(&ms, 0);
	if (res.failed)
		goto fail;

	for (op_index = 0; op_index < max_ops; ++op_index) {
		unsigned op = rng_next(&rng) % MOP_NTYPES;
		int b1 = (int)(rng_next(&rng) % MODEL_NBUFS);
		int b2 = (int)(rng_next(&rng) % MODEL_NBUFS);
		unsigned amt = 1 + rng_next(&rng) % 11; /* bounded: 1..11 */
		int rc;

		switch (op) {
		case MOP_ADD: {
			unsigned char scratch[11];
			if (!model_can_fit(&ms.model[b1], amt))
				break; /* skip before touching the real buffer */
			for (k = 0; k < amt; ++k)
				scratch[k] =
				    (unsigned char)('0' + (op_index + k) % 10);
			rc = evbuffer_add(ms.real[b1], scratch, amt);
			if (rc != 0) {
				res = model_fail(&ms, op_index, "add", 0, 1);
				goto fail;
			}
			if (model_append(&ms.model[b1], scratch, amt, -1) < 0) {
				res = model_fail(&ms, op_index, "add_cap", 0, 1);
				goto fail;
			}
			break;
		}
		case MOP_ADD_REFERENCE: {
			struct model_block *mb = model_new_block(&ms);
			if (!mb) {
				res = model_fail(&ms, op_index, "block", 0, 1);
				goto fail;
			}
			if (!model_can_fit(&ms.model[b1], mb->ext.len)) {
				block_discard_unused(&mb->ext);
				--ms.n_blocks;
				break;
			}
			rc = add_block_reference(ms.real[b1], &mb->ext);
			if (rc != 0) {
				res = model_fail(&ms, op_index, "add_ref", 0, 1);
				goto fail;
			}
			if (model_append(&ms.model[b1], mb->ext.data,
			        mb->ext.len, mb->ext.id) < 0) {
				res = model_fail(&ms, op_index, "add_ref_cap",
				    0, 1);
				goto fail;
			}
			break;
		}
		case MOP_ADD_BUFFER_REFERENCE: {
			size_t n = ms.model[b2].len;
			if (!model_share_allowed(&ms, b1, b2) ||
			    !model_can_fit(&ms.model[b1], n))
				break;
			rc = evbuffer_add_buffer_reference(ms.real[b1],
			    ms.real[b2]);
			if (rc != 0) {
				res = model_fail(&ms, op_index, "share_rc",
				    0, 1);
				goto fail;
			}
			if (model_append_copy(&ms.model[b1],
			        ms.model[b2].bytes, ms.model[b2].ids,
			        n) < 0) {
				res = model_fail(&ms, op_index, "share_cap",
				    0, 1);
				goto fail;
			}
			if (n)
				ms.model[b1].has_mcast = 1;
			break;
		}
		case MOP_REMOVE_BUFFER: {
			size_t n;
			if (b1 == b2) {
				/* Self transfer is a documented no-op. */
				rc = evbuffer_remove_buffer(ms.real[b2],
				    ms.real[b1], amt);
				if (rc != 0) {
					res = model_fail(&ms, op_index,
					    "xfer_self", 0, 1);
					goto fail;
				}
				break;
			}
			n = amt < ms.model[b2].len ? amt : ms.model[b2].len;
			if (n == 0 || !model_can_fit(&ms.model[b1], n))
				break;
			rc = evbuffer_remove_buffer(ms.real[b2],
			    ms.real[b1], amt);
			if (rc < 0) {
				res = model_fail(&ms, op_index, "xfer_rc",
				    0, 1);
				goto fail;
			}
			if ((size_t)rc != n) {
				res = model_fail(&ms, op_index, "xfer_n", n,
				    (size_t)rc);
				goto fail;
			}
			if (model_append_copy(&ms.model[b1],
			        ms.model[b2].bytes, ms.model[b2].ids,
			        n) < 0) {
				res = model_fail(&ms, op_index, "xfer_cap",
				    0, 1);
				goto fail;
			}
			model_front_remove(&ms.model[b2], n);
			break;
		}
		case MOP_DRAIN: {
			size_t n = amt < ms.model[b1].len
			    ? amt : ms.model[b1].len;
			rc = evbuffer_drain(ms.real[b1], amt);
			if (rc != 0) {
				res = model_fail(&ms, op_index, "drain_rc",
				    0, 1);
				goto fail;
			}
			model_front_remove(&ms.model[b1], n);
			break;
		}
		case MOP_PULLUP: {
			ev_ssize_t want = ms.model[b1].len
			    ? (ev_ssize_t)(1 +
			        rng_next(&rng) % ms.model[b1].len)
			    : 0;
			(void)evbuffer_pullup(ms.real[b1], want);
			/* content is contractually unchanged */
			break;
		}
		case MOP_FREE_RECREATE: {
			evbuffer_free(ms.real[b1]);
			ms.real[b1] = evbuffer_new();
			if (!ms.real[b1]) {
				res = model_fail(&ms, op_index, "recreate",
				    0, 1);
				goto fail;
			}
			model_reset_buf(&ms.model[b1]);
			break;
		}
		case MOP_OOM_REFERENCE: {
			struct model_buf before_dst = ms.model[b1];
			struct model_buf before_src = ms.model[b2];
			size_t n = ms.model[b2].len;
			if (!model_share_allowed(&ms, b1, b2) || n == 0 ||
			    !model_can_fit(&ms.model[b1], n))
				break;
			fault_arm(1 + (long)(rng_next(&rng) % 2));
			rc = evbuffer_add_buffer_reference(ms.real[b1],
			    ms.real[b2]);
			if (fault_hits) {
				fault_disarm();
				if (rc != -1) {
					res = model_fail(&ms, op_index,
					    "oom_share_rc", (size_t)-1, 0);
					goto fail;
				}
				if (!model_buf_equal(&ms.model[b1],
				        &before_dst) ||
				    !model_buf_equal(&ms.model[b2],
				        &before_src)) {
					res = model_fail(&ms, op_index,
					    "oom_share_state", 0, 1);
					goto fail;
				}
			} else {
				fault_disarm();
				if (rc != 0) {
					res = model_fail(&ms, op_index,
					    "oom_share_miss", 0, 1);
					goto fail;
				}
				if (model_append_copy(&ms.model[b1],
				        ms.model[b2].bytes,
				        ms.model[b2].ids, n) < 0) {
					res = model_fail(&ms, op_index,
					    "oom_share_cap", 0, 1);
					goto fail;
				}
				ms.model[b1].has_mcast = 1;
			}
			break;
		}
		case MOP_OOM_TRANSFER: {
			struct model_buf before_dst = ms.model[b1];
			struct model_buf before_src = ms.model[b2];
			size_t n;
			if (b1 == b2 || ms.model[b2].len < 2)
				break;
			amt = 1 + rng_next(&rng) % (ms.model[b2].len - 1);
			n = amt;
			if (!model_can_fit(&ms.model[b1], n))
				break;
			fault_arm(1 + (long)(rng_next(&rng) % 2));
			rc = evbuffer_remove_buffer(ms.real[b2],
			    ms.real[b1], amt);
			if (fault_hits) {
				fault_disarm();
				if (rc != -1) {
					res = model_fail(&ms, op_index,
					    "oom_xfer_rc", (size_t)-1, 0);
					goto fail;
				}
				if (!model_buf_equal(&ms.model[b1],
				        &before_dst) ||
				    !model_buf_equal(&ms.model[b2],
				        &before_src)) {
					res = model_fail(&ms, op_index,
					    "oom_xfer_state", 0, 1);
					goto fail;
				}
			} else {
				fault_disarm();
				if (rc < 0 || (size_t)rc != n) {
					res = model_fail(&ms, op_index,
					    "oom_xfer_miss", n,
					    rc < 0 ? (size_t)-1 : (size_t)rc);
					goto fail;
				}
				if (model_append_copy(&ms.model[b1],
				        ms.model[b2].bytes,
				        ms.model[b2].ids, n) < 0) {
					res = model_fail(&ms, op_index,
					    "oom_xfer_cap", 0, 1);
					goto fail;
				}
				model_front_remove(&ms.model[b2], n);
			}
			break;
		}
		default:
			break;
		}

		res = model_compare(&ms, op_index);
		if (res.failed)
			goto fail;
	}

	/* Tear everything down, then require exactly one cleanup per block. */
	for (b = 0; b < MODEL_NBUFS; ++b) {
		evbuffer_free(ms.real[b]);
		ms.real[b] = NULL;
	}
	memset(&res, 0, sizeof(res));
	res.seed = seed;
	for (k = 0; k < ms.n_blocks; ++k) {
		if (ms.blocks[k].ext.cleanup_calls != 1) {
			res = model_fail(&ms, max_ops, "cleanup_once", 1,
			    (size_t)ms.blocks[k].ext.cleanup_calls);
			goto fail;
		}
	}
	return res;

fail:
	for (b = 0; b < MODEL_NBUFS; ++b) {
		if (ms.real[b])
			evbuffer_free(ms.real[b]);
	}
	for (k = 0; k < ms.n_blocks; ++k)
		block_discard_unused(&ms.blocks[k].ext);
	return res;
}
