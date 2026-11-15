#include <cassert>
#include <iostream>
#include <string>
#include <utility>
#include <vector>
#include "lru.h"

using Keys = std::vector<std::string>;

Keys lru_to_mru(const LRUCache& c) {
    Keys k;
    c.for_each_lru_to_mru([&](const std::string& key, const std::string&) { k.push_back(key); });
    return k;
}

void test_order_and_no_side_effects() {
    LRUCache c(3);
    c.put("a", "1"); c.put("b", "2"); c.put("c", "3");
    std::string v;
    c.get("a", v);                                   // newest-to-oldest is now: a, c, b
    assert((lru_to_mru(c) == Keys{"b", "c", "a"}));  // oldest first
    assert((c.keys_in_order() == Keys{"a", "c", "b"}));  // walking did not change the order
}

void test_replay_rebuilds_same_order() {
    LRUCache a(3);
    a.put("a", "1"); a.put("b", "2"); a.put("c", "3");
    std::string v;
    a.get("a", v);

    std::vector<std::pair<std::string, std::string>> dump;
    a.for_each_lru_to_mru([&](const std::string& k, const std::string& val) {
        dump.push_back({k, val});
    });

    LRUCache b(3);
    for (auto& kv : dump) b.put(kv.first, kv.second);

    assert(a.keys_in_order() == b.keys_in_order());
    assert(b.get("c", v) && v == "3");
}

int main() {
    test_order_and_no_side_effects();
    test_replay_rebuilds_same_order();
    std::cout << "All order tests passed\n";
}