#include <cassert>
#include <iostream>
#include "lru.h"

using Keys = std::vector<std::string>;

void test_basic_put_get() {
    LRUCache c(3);
    c.put("a", "1");
    std::string v;
    assert(c.get("a", v) && v == "1");
    assert(!c.get("zzz", v));
    assert(c.size() == 1);
}

void test_evicts_least_recently_used() {
    LRUCache c(3);
    c.put("a", "1"); c.put("b", "2"); c.put("c", "3");
    c.put("d", "4");                         // over capacity: "a" is oldest
    std::string v;
    assert(!c.get("a", v));
    assert(c.get("b", v) && c.get("c", v) && c.get("d", v));
    assert(c.size() == 3);
    assert(c.evictions() == 1);
}

void test_get_refreshes_recency() {
    LRUCache c(3);
    c.put("a", "1"); c.put("b", "2"); c.put("c", "3");
    std::string v;
    c.get("a", v);                           // "a" is now the newest, "b" the oldest
    c.put("d", "4");
    assert(c.get("a", v));
    assert(!c.get("b", v));
}

void test_update_does_not_grow_and_refreshes() {
    LRUCache c(2);
    c.put("a", "1"); c.put("b", "2");
    c.put("a", "99");                        // update: no growth, "a" becomes newest
    assert(c.size() == 2 && c.evictions() == 0);
    c.put("c", "3");                         // evicts "b", not "a"
    std::string v;
    assert(c.get("a", v) && v == "99");
    assert(!c.get("b", v));
}

void test_erase() {
    LRUCache c(3);
    c.put("a", "1"); c.put("b", "2");
    assert(c.erase("a"));
    assert(!c.erase("a"));
    std::string v;
    assert(!c.get("a", v));
    assert(c.size() == 1);
}

void test_capacity_one() {
    LRUCache c(1);
    c.put("a", "1");
    c.put("b", "2");
    std::string v;
    assert(!c.get("a", v));
    assert(c.get("b", v) && v == "2");
}

void test_order() {
    LRUCache c(3);
    c.put("a", "1"); c.put("b", "2"); c.put("c", "3");
    assert((c.keys_in_order() == Keys{"c", "b", "a"}));
    std::string v;
    c.get("a", v);
    assert((c.keys_in_order() == Keys{"a", "c", "b"}));
}

int main() {
    test_basic_put_get();
    test_evicts_least_recently_used();
    test_get_refreshes_recency();
    test_update_does_not_grow_and_refreshes();
    test_erase();
    test_capacity_one();
    test_order();
    std::cout << "All LRU tests passed\n";
}