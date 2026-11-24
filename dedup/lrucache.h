#ifndef DEDUP_LRUCACHE_H
#define DEDUP_LRUCACHE_H

/**
 * @file lrucache.h
 * @brief The small LRU used by the dedup window's per-pair image cache.
 *
 * Moved out of gallery_info's `utils.h` together with the dedup module
 * (MERGE-01): the window header stores one as a member, so the template is part
 * of the module's public surface now. gallery_info's `utils.h` includes this
 * header instead of defining its own copy - two definitions in one translation
 * unit would be a redefinition, and `main.cpp` includes both headers.
 */

#include <cstddef>
#include <functional>
#include <list>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>

template <
    typename Key,          // 缓存的键类型
    typename Value,        // 缓存的值类型（支持自定义类型）
    auto Capacity,         // 缓存容量（作为非类型模板参数，编译期确定）
    typename KeyHasher = std::hash<Key>,
    typename KeyEqual = std::equal_to<Key>
    >
class LRUCache {
    static_assert(Capacity > 0, "LRUCache capacity must be greater than 0");

private:
    using Node = typename std::pair<Key, Value>;

    // 1. 双向链表：维护访问顺序。头部是最近使用，尾部是最久未使用。
    std::list<Node> lruList;

    // 2. 哈希表：实现 O(1) 查找。存储键 -> 链表迭代器的映射。
    std::unordered_map<Key, typename std::list<Node>::iterator, KeyHasher, KeyEqual> cacheMap;

public:
    LRUCache() = default;

    // 获取缓存中的元素
    // 如果命中，将元素移到链表头部（标记为最近使用），并返回值的引用
    Value& get(const Key& key) {
        auto it = cacheMap.find(key);
        if (it == cacheMap.end()) {
            throw std::out_of_range("Key not found in LRUCache");
        }

        // 将访问到的节点移动到链表头部
        lruList.splice(lruList.begin(), lruList, it->second);
        return it->second->second;
    }

    // 放入一个键值对
    std::optional<Key> put(const Key& key, const Value& value) {
        auto it = cacheMap.find(key);
        std::optional<Key> ret;
        // 1. 如果键已存在，更新值并移到表头
        if (it != cacheMap.end()) {
            it->second->second = value;
            lruList.splice(lruList.begin(), lruList, it->second);
            return ret;
        }

        // 2. 如果键不存在，插入新节点
        // 先检查容量是否已满
        if (cacheMap.size() >= Capacity) {
            // 容量已满，淘汰尾部元素（最久未使用）
            auto& oldestNode = lruList.back();
            cacheMap.erase(oldestNode.first); // 从哈希表中删除
            ret = oldestNode.first;
            lruList.pop_back();               // 从链表中删除
        }

        // 插入新节点到链表头部，并在哈希表中记录其位置
        lruList.push_front({key, value});
        cacheMap[key] = lruList.begin();
        return ret;
    }
    bool hasKey(const Key& key) {
        auto it = cacheMap.find(key);
        if(it != cacheMap.end()){
            lruList.splice(lruList.begin(), lruList, it->second);
            return true;
        }
        else
            return false;
    }
    /*
     * 只摘掉一个键(不碰其它项)。
     * 缓存表里记着"已在缓存"而矩阵其实已经释放时,需要精确地去掉这一条 ——
     * 用 clear() 会把整个 LRU 一起清空,导致后面每一张图都要重新解码。
     */
    bool erase(const Key& key) {
        auto it = cacheMap.find(key);
        if (it == cacheMap.end()) return false;
        lruList.erase(it->second);
        cacheMap.erase(it);
        return true;
    }
    // 辅助函数：检查缓存是否为空
    bool empty() const { return cacheMap.empty(); }

    // 辅助函数：获取当前元素数量
    size_t size() const { return cacheMap.size(); }
    void clear() noexcept {
        cacheMap.clear();
        lruList.clear();
    }
};

#endif // DEDUP_LRUCACHE_H
