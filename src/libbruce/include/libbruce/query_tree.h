#ifndef BRUCE_QUERY_H
#define BRUCE_QUERY_H

#include <libbruce/be/be.h>
#include <libbruce/traits.h>
#include <libbruce/query_iterator.h>
#include <boost/make_shared.hpp>

namespace bruce {

class query_tree_impl;
typedef boost::shared_ptr<query_tree_impl> query_tree_impl_ptr;

struct query_tree_unsafe
{
    query_tree_unsafe(nodeid_t id, be::be &be, const tree_functions &fns);

    void queue_insert(const memory &key, const memory &value);
    void queue_remove(const memory &key, bool guaranteed);
    void queue_remove(const memory &key, const memory &value, bool guaranteed);

    bool get(const memory &key, memory *value);
    query_iterator_unsafe find(const memory &key);
private:
    query_tree_impl_ptr m_impl;
};

template<typename K, typename V>
struct query_tree
{
    typedef typename boost::optional<V> maybe_v;
    typedef query_iterator<K, V> iterator;

    query_tree(nodeid_t id, be::be &be)
        : m_unsafe(id, be, fns()) { }

    void queue_insert(const K &key, const V &value)
    {
        m_unsafe.queue_insert(traits::convert<K>::to_bytes(key), traits::convert<V>::to_bytes(value));
    }

    void queue_remove(const K &key, bool guaranteed)
    {
        m_unsafe.queue_remove(traits::convert<K>::to_bytes(key), guaranteed);
    }

    void queue_remove(const K &key, const V &value, bool guaranteed)
    {
        m_unsafe.queue_remove(traits::convert<K>::to_bytes(key), traits::convert<V>::to_bytes(value), guaranteed);
    }

    maybe_v get(const K &key)
    {
        memory value;
        if (m_unsafe.get(traits::convert<V>::to_bytes(key), &value))
            return traits::convert<V>::from_bytes(value);
        else
            return maybe_v();
    }

    iterator find(const K &key)
    {
        return query_iterator<K,V>(m_unsafe.find(traits::convert<V>::to_bytes(key)));
    }

    static tree_functions fns() { return tree_functions(&traits::convert<K>::compare, &traits::convert<V>::compare, &traits::convert<K>::size, &traits::convert<V>::size); }
private:
    query_tree_unsafe m_unsafe;
};


}

#endif
