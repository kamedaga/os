typedef unsigned long size_t;

struct item {
    int id;
    long value;
    char name[16];
};

static long mix(long x)
{
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return x;
}

static int cmp_item(struct item *a, struct item *b)
{
    if (a->value < b->value) return -1;
    if (a->value > b->value) return 1;
    return a->id - b->id;
}

static void swap_item(struct item *a, struct item *b)
{
    struct item t = *a;
    *a = *b;
    *b = t;
}

static void sort_items(struct item *items, int n)
{
    for (int i = 1; i < n; i++) {
        for (int j = i; j > 0 && cmp_item(&items[j], &items[j - 1]) < 0; j--) {
            swap_item(&items[j], &items[j - 1]);
        }
    }
}

static long fold_items(struct item *items, int n)
{
    long acc = 0;
    for (int i = 0; i < n; i++) {
        acc += items[i].value * (i + 3);
        acc ^= items[i].id << (i & 7);
        for (int j = 0; items[i].name[j]; j++) {
            acc += items[i].name[j] * (j + 1);
        }
    }
    return acc;
}

#define GEN_FUNC(N) \
static long stage_##N(long seed) \
{ \
    struct item items[32]; \
    for (int i = 0; i < 32; i++) { \
        items[i].id = i + N * 100; \
        items[i].value = mix(seed + i * (N + 17)); \
        items[i].name[0] = 's'; \
        items[i].name[1] = 't'; \
        items[i].name[2] = 'g'; \
        items[i].name[3] = '0' + (N % 10); \
        items[i].name[4] = 0; \
    } \
    sort_items(items, 32); \
    return fold_items(items, 32) ^ mix(seed + N); \
}

GEN_FUNC(0)
GEN_FUNC(1)
GEN_FUNC(2)
GEN_FUNC(3)
GEN_FUNC(4)
GEN_FUNC(5)
GEN_FUNC(6)
GEN_FUNC(7)

typedef long (*stage_fn)(long);

static stage_fn stages[] = {
    stage_0, stage_1, stage_2, stage_3, stage_4, stage_5, stage_6, stage_7,
};

int main(void)
{
    long acc = 0x12345678;
    for (int round = 0; round < 16; round++) {
        for (size_t i = 0; i < sizeof(stages) / sizeof(stages[0]); i++) {
            acc ^= stages[i](acc + round);
        }
    }
    return (int)(acc & 255);
}
