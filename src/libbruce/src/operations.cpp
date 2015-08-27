#include "operations.h"

#include "nodes.h"
#include "serializing.h"

#include <boost/lexical_cast.hpp>
#include <boost/foreach.hpp>

// Macro because we don't have C++11
#define to_string boost::lexical_cast<std::string>

namespace bruce {

mutable_tree::mutable_tree(be::be &be, maybe_nodeid rootID, tree_functions fns)
    : m_be(be), m_rootID(rootID), m_fns(fns), m_frozen(false)
{
}

node_ptr &mutable_tree::root()
{
    if (!m_root)
    {
        if (m_rootID)
            m_root = load(*m_rootID);
        else
            // No existing tree, create a new leaf node
            m_root = boost::make_shared<LeafNode>();
    }

    return m_root;
}

const node_ptr &mutable_tree::child(node_branch &branch)
{
    if (!branch.child) branch.child = load(branch.nodeID);
    return branch.child;
}

node_ptr mutable_tree::load(nodeid_t id)
{
    m_oldIDs.push_back(id);
    memory mem = m_be.get(id);
    return ParseNode(mem, m_fns);
}

void mutable_tree::insert(const memory &key, const memory &value)
{
    checkNotFrozen();
    validateKVSize(key, value);

    const node_ptr &r = root();
    splitresult_t rootSplit = insertRec(r, key, value);

    if (rootSplit.didSplit)
    {
        // Replace root with a new internal node
        internalnode_ptr newRoot = boost::make_shared<InternalNode>();
        newRoot->insert(0, node_branch(memory(), rootSplit.left, rootSplit.leftCount));
        newRoot->insert(1, node_branch(rootSplit.splitKey, rootSplit.right, rootSplit.rightCount));
        m_root = newRoot;
    }
}

void mutable_tree::validateKVSize(const memory &key, const memory &value)
{
    uint32_t maxSize = m_be.maxBlockSize();
    if (key.size() + value.size() > maxSize)
        throw std::runtime_error("Key/value too large to insert, max size: " + to_string(maxSize));
}

splitresult_t mutable_tree::insertRec(const node_ptr &node, const memory &key, const memory &value)
{
    if (node->isLeafNode())
    {
        leafnode_ptr leaf = boost::static_pointer_cast<LeafNode>(node);

        keycount_t i = FindLeafKey(leaf, key, m_fns);
        leaf->insert(i, kv_pair(key, value));

        LeafNodeSize size(leaf, m_be.maxBlockSize());
        if (!size.shouldSplit())
            return splitresult_t(leaf->count());

        keycount_t j = size.splitIndex();
        leafnode_ptr left = boost::make_shared<LeafNode>(leaf->pairs().begin(), leaf->pairs().begin() + j);
        leafnode_ptr right = boost::make_shared<LeafNode>(leaf->pairs().begin() + j, leaf->pairs().end());

        return splitresult_t(left, left->count(),
                             right->minKey(),
                             right, right->count());
    }
    else
    {
        internalnode_ptr internal = boost::static_pointer_cast<InternalNode>(node);

        keycount_t i = FindInternalKey(internal, key, m_fns);

        node_branch &leftBranch = internal->branches()[i];

        splitresult_t childSplit = insertRec(child(leftBranch), key, value);

        // Child didn't split
        if (!childSplit.didSplit)
        {
            leftBranch.itemCount = childSplit.leftCount;
            return splitresult_t(internal->itemCount());
        }

        // Child did split, replace single branch with two branches (in effect,
        // treat current as left and add one to the right).
        node_branch rightBranch(childSplit.splitKey, childSplit.right, childSplit.rightCount);
        leftBranch.itemCount = childSplit.leftCount;
        leftBranch.child = childSplit.left;
        internal->insert(i + 1, rightBranch);

        InternalNodeSize size(internal, m_be.maxBlockSize());

        // Maybe split this leaf
        if (!size.shouldSplit())
            return splitresult_t(internal->itemCount());

        keycount_t j = size.splitIndex();
        internalnode_ptr left = boost::make_shared<InternalNode>(internal->branches().begin(), internal->branches().begin() + j);
        internalnode_ptr right = boost::make_shared<InternalNode>(internal->branches().begin() + j, internal->branches().end());

        return splitresult_t(left, left->count(),
                             right->minKey(),
                             right, right->count());
    }
}

void mutable_tree::checkNotFrozen()
{
    if (m_frozen)
        throw std::runtime_error("Can't mutate tree anymore; already flushed");
}

mutation mutable_tree::flush()
{
    m_frozen = true;

    // Root not loaded == no changes
    if (!m_root)
        return mutation(m_rootID);

    m_newIDsRequired = 1; // For the root
    collectNewIDsRec(root());

    m_be.newIdentifiers(m_newIDsRequired).swap(m_newIDs); // Swaptimization

    m_putBlocks.reserve(m_newIDsRequired);
    m_newIDsRequired = 1; // For the root
    collectBlocksToPutRec(root(), m_newIDs[0]);

    m_be.put_all(m_putBlocks);

    return collectMutation();
}

mutation mutable_tree::collectMutation()
{
    mutation ret(m_newIDs[0]);
    bool failed = false;

    BOOST_FOREACH(const be::putblock_t &put, m_putBlocks)
    {
        if (put.success)
            ret.addCreated(put.id);
        else
            failed = true;
    }
    if (failed)
        ret.fail("Failed to write some blocks to the block engine");

    BOOST_FOREACH(nodeid_t id, m_oldIDs)
        ret.addObsolete(id);

    return ret;
}

/**
 * Traverse the tree, and record that we need a new ID for every node that was loaded
 *
 * Every loaded node got loaded because we changed it, so it needs to be
 * written to a new page. Every branch with a child pointer gets its nodeID
 * replaced with an index into the m_nodeIDs array. After provisioning the new
 * IDs, that array will have the actual IDs for every page.
 */
void mutable_tree::collectNewIDsRec(node_ptr &node)
{
    // FIXME: Check if we actually did any changes. Otherwise we have nothing to write.

    if (node->isLeafNode())
        return;

    internalnode_ptr internal = boost::static_pointer_cast<InternalNode>(node);
    BOOST_FOREACH(node_branch &b, internal->branches())
    {
        if (b.child)
        {
            b.nodeID = m_newIDsRequired++;
            collectNewIDsRec(b.child);
        }
    }
}

void mutable_tree::collectBlocksToPutRec(node_ptr &node, nodeid_t id)
{
    if (!node->isLeafNode())
    {
        internalnode_ptr internal = boost::static_pointer_cast<InternalNode>(node);
        BOOST_FOREACH(node_branch &b, internal->branches())
        {
            if (b.child)
            {
                // In this case, nodeID is an index into the array of IDs
                b.nodeID = m_newIDs[b.nodeID];
                collectBlocksToPutRec(b.child, b.nodeID);
            }
        }
    }

    // Serialize this node
    m_putBlocks.push_back(be::putblock_t(id, SerializeNode(node)));
}


}
