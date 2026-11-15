#include <cassert>
#include <iostream>
#include <string>
#include <vector>
#include "lru.h"

void test_contains_does_not_refresh() {
    LRUCache c(2);
    c.put("a", "1"); c.put("b", "2");
    assert(c.contains("a"));
    assert(!c.contains("zzz"));
    c.put("c", "3");                 // "a" is still the oldest, so it is evicted
    assert(!c.contains("a"));
    assert(c.contains("b") && c.contains("c"));
}

void test_evict_callback() {
    LRUCache c(2);
    std::vector<std::string> evicted;
    c.set_evict_callback([&](const std::string& k) { evicted.push_back(k); });
    c.put("a", "1"); c.put("b", "2"); c.put("c", "3"); c.put("d", "4");
    assert((evicted == std::vector<std::string>{"a", "b"}));
    c.erase("c");                    // an explicit erase must NOT fire the callback
    assert(evicted.size() == 2);
}

int main() {
    test_contains_does_not_refresh();
    test_evict_callback();
    std::cout << "All expiry-support tests passed\n";
}
