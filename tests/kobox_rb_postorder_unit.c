/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "kobox/shim.h"

struct node {
    uintptr_t parent_color;
    struct node *right, *left;
    unsigned id;
};
struct root { struct node *node; };
_Static_assert(offsetof(struct node, right) == 8, "native Linux rb_node layout");
_Static_assert(offsetof(struct node, left) == 16, "native Linux rb_node layout");

static struct node *make(unsigned id, struct node *parent)
{
    struct node *node = calloc(1, sizeof(*node));
    assert(node);
    node->id = id;
    node->parent_color = (uintptr_t)parent | (id & 3u);
    return node;
}

static void mixed(void)
{
    struct root root = {make(1, NULL)};
    root.node->left = make(2, root.node);
    root.node->right = make(3, root.node);
    root.node->left->right = make(4, root.node->left);
    root.node->right->left = make(5, root.node->right);
    root.node->right->right = make(6, root.node->right);
    const unsigned order[] = {4, 2, 5, 6, 3, 1};
    size_t count = 0;
    for (struct node *node = kb_rb_first_postorder(&root); node != NULL;) {
        assert(count < sizeof(order)/sizeof(order[0]));
        assert(node->id == order[count++]);
        struct node *next = kb_rb_next_postorder(node);
        free(node);
        node = next;
    }
    assert(count == sizeof(order)/sizeof(order[0]));
}

static void chain(unsigned count, int right)
{
    struct root root = {make(0, NULL)};
    struct node *tail = root.node;
    for (unsigned i = 1; i < count; ++i) {
        struct node *next = make(i, tail);
        if (right) tail->right = next;
        else tail->left = next;
        tail = next;
    }
    unsigned expected = count;
    for (struct node *node = kb_rb_first_postorder(&root); node != NULL;) {
        assert(expected && node->id == --expected);
        struct node *next = kb_rb_next_postorder(node);
        free(node);
        node = next;
    }
    assert(expected == 0);
}

int main(void)
{
    struct root empty = {0};
    assert(kb_rb_first_postorder(&empty) == NULL);
    assert(kb_rb_first_postorder(NULL) == NULL);
    assert(kb_rb_next_postorder(NULL) == NULL);
    mixed();
    chain(1, 0);
    chain(4096, 0);
    chain(4096, 1);
    puts("kobox rb postorder: PASS empty, singleton, mixed, deep, free-after-next");
    return 0;
}
