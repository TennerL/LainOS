#include <stdint.h>
#include <stddef.h>

#define CCOMPAT_CONSTRUCT_CAP 4u
#define CCOMPAT_WORD 99u

typedef struct {
	uint32_t value;
	uint8_t bytes[CCOMPAT_CONSTRUCT_CAP];
} ccompat_box_t;

typedef struct {
	uint32_t *items;
} ccompat_pointer_table_t;

typedef void (*ccompat_visit_fn)(uint32_t *slot, void *pw);

static ccompat_box_t ccompat_box;
static const uint8_t ccompat_decode_table[] = { 1u, 2u, 3u, 4u };
static uint32_t ccompat_word_table[] = { CCOMPAT_WORD, 5u, 6u };
static uint32_t ccompat_pointer_value = 77u;
static uint32_t ccompat_pointer_items[] = { 10u, 20u, 30u };
static ccompat_pointer_table_t ccompat_pointer_table;

extern uint32_t ccompat_forward_declared(uint32_t value);

static void ccompat_visit_add(uint32_t *slot, void *pw)
{
	uint32_t *delta = pw;

	*slot += *delta;
}

static void ccompat_visit_one(uint32_t *slot, ccompat_visit_fn cb, void *pw)
{
	cb(slot, pw);
}

uint32_t ccompat_forward_declared(uint32_t value)
{
	return value + 2u;
}

static uint32_t
ccompat_table_sum(uint32_t seed)
{
	uint32_t i;
	uint32_t j;
	uint32_t total;

	total = seed > 9u ? seed : 9u;
	for (i = 0u, j = 0u; i < 3u; ++i) {
		total += ccompat_decode_table[i];
		j += ccompat_word_table[i];
	}
	return total + j;
}

static uint32_t ccompat_pointer_out(uint32_t **slot_out)
{
	uint32_t observed;

	observed = slot_out == NULL ? 0u : 1u;
	if (slot_out == NULL) {
		return observed;
	}
	*slot_out = &ccompat_pointer_value;
	return **slot_out + observed;
}

uint32_t ccompat_pointer_probe(void)
{
	uint8_t text[4] = "ABC";
	uint8_t lowered[4];
	uint8_t *target;
	const uint8_t *source;
	uint32_t *slot;
	uint32_t n;
	uint32_t value;
	uint32_t delta;

	slot = NULL;
	value = ccompat_pointer_out(&slot);
	ccompat_pointer_table.items = ccompat_pointer_items;
	slot = ccompat_pointer_items;
	*slot = value = ccompat_pointer_table.items[1];
	((uint32_t *)(ccompat_pointer_table.items))[1] = 30u;
	slot = &(ccompat_pointer_table.items[2]);
	target = lowered;
	source = text;
	n = 3u;
	while (n--) {
		*target++ = (uint8_t)(*source++ + 32u);
	}
	lowered[3] = 0u;
	delta = 3u;
	ccompat_visit_one(&value, ccompat_visit_add, &delta);
	return value + (slot == NULL ? 1000u : *slot) + ccompat_pointer_items[0] +
	       sizeof(*slot) + lowered[0] - 97u + ccompat_forward_declared(1u);
}

uint32_t ccompat_table_probe(void)
{
	return ccompat_table_sum(5u);
}

uint32_t ccompat_constructs(uint32_t seed)
{
	uint8_t local[CCOMPAT_CONSTRUCT_CAP];
	uint8_t literal[16] = "CCOMPAT_WORD";
	uint32_t i;
	uint32_t total;

	for (uint32_t j = 0u; j < CCOMPAT_CONSTRUCT_CAP; ++j) {
		local[j] = (uint8_t)(seed + j);
	}

	ccompat_box.value = seed > 7u ? seed : 7u;
	++ccompat_box.value;
	ccompat_box.bytes[0] = local[0];

	i = 0u;
	total = 0u;
	while (i < sizeof(local)) {
		total += local[i++];
	}

	return total +
	       ccompat_box.value +
	       (ccompat_box.bytes[0] == seed ? 5u : 1u) +
	       literal[0] +
	       '#';
}
