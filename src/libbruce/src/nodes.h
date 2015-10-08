#pragma once
#ifndef BRUCE_NODES_H
#define BRUCE_NODES_H

#include <utility>
#include <libbruce/memory.h>
#include <libbruce/types.h>
#include <boost/make_shared.hpp>

#include <vector>

namespace libbruce {

enum node_type_t {
    TYPE_LEAF,
    TYPE_INTERNAL,
    TYPE_OVERFLOW
};

class Node;

typedef boost::shared_ptr<Node> node_ptr;

struct kv_pair
{
    kv_pair(const memory &key, const memory &value)
        : key(key), value(value) { }

    memory key;
    memory value;
};

typedef std::vector<kv_pair> pairlist_t;
typedef std::vector<memory> valuelist_t;

struct node_branch {
    node_branch(const memory &minKey, nodeid_t nodeID, itemcount_t itemCount)
        : minKey(minKey), nodeID(nodeID), itemCount(itemCount) { }
    node_branch(const memory &minKey, const node_ptr &child, itemcount_t itemCount)
        : minKey(minKey), nodeID(), itemCount(itemCount), child(child) { }

    void inc() { itemCount++; }

    memory minKey;
    nodeid_t nodeID;
    itemcount_t itemCount;

    node_ptr child; // Only valid while mutating the tree
};

typedef std::vector<node_branch> branchlist_t;

struct overflow_t
{
    overflow_t() : count(0), nodeID() { }

    itemcount_t count;
    nodeid_t nodeID;
    node_ptr node; // Only valid while mutating the tree

    bool empty() const { return !(node || count); }
};


/**
 * Base class for Nodes
 */
struct Node
{
    Node(node_type_t nodeType);
    virtual ~Node();

    virtual itemcount_t itemCount() const = 0; // Items in this node and below
    virtual const memory &minKey() const = 0;

    node_type_t nodeType() const { return m_nodeType; }
private:
    node_type_t m_nodeType;
};

/**
 * Leaf node type
 */
struct LeafNode : public Node
{
    LeafNode(size_t sizeHint=0);
    LeafNode(pairlist_t::const_iterator begin, pairlist_t::const_iterator end);

    keycount_t pairCount() const { return pairs.size(); }
    virtual const memory &minKey() const;
    virtual itemcount_t itemCount() const;

    void insert(size_t i, const kv_pair &item) { pairs.insert(pairs.begin() + i, item); }
    void erase(size_t i) { pairs.erase(pairs.begin() + i); }
    void append(const kv_pair &item) { pairs.push_back(item); }

    pairlist_t::const_iterator at(keycount_t i) const { return pairs.begin() + i; }
    pairlist_t::iterator at(keycount_t i) { return pairs.begin() + i; }

    kv_pair &pair(keycount_t i) { return pairs[i]; }

    void setOverflow(const node_ptr &node);

    pairlist_t pairs;
    overflow_t overflow;
};

/**
 * Overflow nodes contain lists of values with the same key as the last key in a leaf
 */
struct OverflowNode : public Node
{
    OverflowNode(size_t sizeHint=0);
    OverflowNode(pairlist_t::const_iterator begin, pairlist_t::const_iterator end);
    OverflowNode(valuelist_t::const_iterator begin, valuelist_t::const_iterator end);

    virtual itemcount_t itemCount() const;
    virtual const memory &minKey() const;

    keycount_t valueCount() const { return values.size(); }

    void append(const memory &item) { values.push_back(item); }
    void erase(size_t i) { values.erase(values.begin() + i); }

    valuelist_t::const_iterator at(keycount_t i) const { return values.begin() + i; }
    valuelist_t::iterator at(keycount_t i) { return values.begin() + i; }

    void setNext(const node_ptr &node);

    valuelist_t values;
    overflow_t next;
};

/**
 * Internal node type
 */
struct InternalNode : public Node
{
    InternalNode(size_t sizeHint=0);
    InternalNode(branchlist_t::const_iterator begin, branchlist_t::const_iterator end);

    keycount_t branchCount() const { return branches.size(); }
    virtual const memory &minKey() const;

    void insert(size_t i, const node_branch &branch) { branches.insert(branches.begin() + i, branch); }
    void erase(size_t i) { branches.erase(branches.begin() + i); }
    void append(const node_branch &branch) { branches.push_back(branch); }

    branchlist_t::const_iterator at(keycount_t i) const { return branches.begin() + i; }

    itemcount_t itemCount() const;

    node_branch &branch(keycount_t i) { return branches[i]; }

    void setBranch(size_t i, const node_ptr &node);

    branchlist_t branches;
};

typedef boost::shared_ptr<LeafNode> leafnode_ptr;
typedef boost::shared_ptr<OverflowNode> overflownode_ptr;
typedef boost::shared_ptr<InternalNode> internalnode_ptr;

/**
 * Return the index where to INSERT a particular key.
 *
 * POST: leaf[ret-1].key <= key < leaf[ret].key
 */
keycount_t FindLeafInsertKey(const leafnode_ptr &leaf, const memory &key, const tree_functions &fns);

/**
 * Return the index where to UPDATE or INSERT a particular key in a leaf.
 */
keycount_t FindLeafUpsertKey(const leafnode_ptr &leaf, const memory &key, const tree_functions &fns);

/**
 * Return the index where the subtree for a particular key will be located
 *
 * POST: branch[ret-1] < branch[ret].key <= key
 */
keycount_t FindInternalKey(const internalnode_ptr &node, const memory &key, const tree_functions &fns);

/**
 * Find the index where this item can go with the least amount of child nodes.
 *
 * POST: branch[ret].key <= key <= branch[ret+1].key
 */
keycount_t FindShallowestInternalKey(const internalnode_ptr &node, const memory &key, const tree_functions &fns);

std::ostream &operator <<(std::ostream &os, const libbruce::Node &x);

}


#endif
