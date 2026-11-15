#pragma once
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// LRU cache: hash map (O(1) lookup) + doubly linked list (O(1) reorder).
// List order: head_ side = most recently used, tail_ side = least recently used.
class LRUCache {
public:
    explicit LRUCache(size_t capacity) : capacity_(capacity) {
        head_ = new Node();
        tail_ = new Node();
        head_->next = tail_;
        tail_->prev = head_;
    }

    ~LRUCache() {
        Node* cur = head_;
        while (cur) {
            Node* nxt = cur->next;
            delete cur;
            cur = nxt;
        }
    }

    LRUCache(const LRUCache&) = delete;
    LRUCache& operator=(const LRUCache&) = delete;

    // Called with the key whenever the cache EVICTS a key to make room.
    // (Not called for explicit erase().) Runs while the caller holds its lock.
    void set_evict_callback(std::function<void(const std::string&)> cb) {
        on_evict_ = std::move(cb);
    }

    // Returns true and fills 'out' if the key exists. Reading counts as "using" it.
    bool get(const std::string& key, std::string& out) {
        auto it = map_.find(key);
        if (it == map_.end()) return false;
        Node* n = it->second;
        move_to_front(n);
        out = n->value;
        return true;
    }

    // Existence check that does NOT refresh recency
    bool contains(const std::string& key) const {
        return map_.count(key) > 0;
    }

    void put(const std::string& key, const std::string& value) {
        auto it = map_.find(key);
        if (it != map_.end()) {
            it->second->value = value;
            move_to_front(it->second);
            return;
        }
        Node* n = new Node(key, value);
        map_[key] = n;
        insert_at_front(n);
        if (map_.size() > capacity_) evict_lru();
    }

    bool erase(const std::string& key) {
        auto it = map_.find(key);
        if (it == map_.end()) return false;
        Node* n = it->second;
        unlink(n);
        map_.erase(it);
        delete n;
        return true;
    }

    // Visit every entry from LEAST recently used to MOST recently used.
    // Does not change recency. Replaying the entries into an empty cache with put(),
    // in this order, rebuilds the same recency order (the last put ends up newest).
    template <typename F>
    void for_each_lru_to_mru(F fn) const {
        for (Node* n = tail_->prev; n != head_; n = n->prev) fn(n->key, n->value);
    }

    size_t size() const { return map_.size(); }
    size_t capacity() const { return capacity_; }
    size_t evictions() const { return evictions_; }

    // Keys from most recently used to least recently used
    std::vector<std::string> keys_in_order() const {
        std::vector<std::string> keys;
        for (Node* n = head_->next; n != tail_; n = n->next) keys.push_back(n->key);
        return keys;
    }

private:
    struct Node {
        std::string key, value;
        Node* prev = nullptr;
        Node* next = nullptr;
        Node() = default;
        Node(const std::string& k, const std::string& v) : key(k), value(v) {}
    };

    void unlink(Node* n) {
        n->prev->next = n->next;
        n->next->prev = n->prev;
    }

    void insert_at_front(Node* n) {
        n->next = head_->next;
        n->prev = head_;
        head_->next->prev = n;
        head_->next = n;
    }

    void move_to_front(Node* n) {
        unlink(n);
        insert_at_front(n);
    }

    void evict_lru() {
        Node* victim = tail_->prev;
        if (victim == head_) return;
        if (on_evict_) on_evict_(victim->key);
        unlink(victim);
        map_.erase(victim->key);
        delete victim;
        evictions_++;
    }

    size_t capacity_;
    size_t evictions_ = 0;
    Node* head_;
    Node* tail_;
    std::unordered_map<std::string, Node*> map_;
    std::function<void(const std::string&)> on_evict_;
};