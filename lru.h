#pragma once
#include <string>
#include <unordered_map>
#include <vector>

// LRU cache: hash map (O(1) lookup) + doubly linked list (O(1) reorder).
// List order: head_ side = most recently used, tail_ side = least recently used.
class LRUCache {
public:
    explicit LRUCache(size_t capacity) : capacity_(capacity) {
        // Two empty "sentinel" nodes mean we never have to special-case an empty list
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

    // A copy would share raw pointers and double-delete, so forbid it
    LRUCache(const LRUCache&) = delete;
    LRUCache& operator=(const LRUCache&) = delete;

    // Returns true and fills 'out' if the key exists. Reading counts as "using" it.
    bool get(const std::string& key, std::string& out) {
        auto it = map_.find(key);
        if (it == map_.end()) return false;
        Node* n = it->second;
        move_to_front(n);
        out = n->value;
        return true;
    }

    // Insert or update. Evicts the least recently used key if over capacity.
    void put(const std::string& key, const std::string& value) {
        auto it = map_.find(key);
        if (it != map_.end()) {              // key exists: update and refresh
            it->second->value = value;
            move_to_front(it->second);
            return;
        }
        Node* n = new Node(key, value);      // new key: add at the front
        map_[key] = n;
        insert_at_front(n);
        if (map_.size() > capacity_) evict_lru();
    }

    // Returns true if the key existed
    bool erase(const std::string& key) {
        auto it = map_.find(key);
        if (it == map_.end()) return false;
        Node* n = it->second;
        unlink(n);
        map_.erase(it);
        delete n;
        return true;
    }

    size_t size() const { return map_.size(); }
    size_t capacity() const { return capacity_; }
    size_t evictions() const { return evictions_; }

    // For tests: keys from most recently used to least recently used
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

    // Detach a node from the list (does not delete it)
    void unlink(Node* n) {
        n->prev->next = n->next;
        n->next->prev = n->prev;
    }

    // Put a node right after head_ (the most recently used position)
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

    // Remove the node just before tail_ (the least recently used)
    void evict_lru() {
        Node* victim = tail_->prev;
        if (victim == head_) return;         // list is empty
        unlink(victim);
        map_.erase(victim->key);             // erase from the map BEFORE deleting the node
        delete victim;
        evictions_++;
    }

    size_t capacity_;
    size_t evictions_ = 0;
    Node* head_;
    Node* tail_;
    std::unordered_map<std::string, Node*> map_;
};