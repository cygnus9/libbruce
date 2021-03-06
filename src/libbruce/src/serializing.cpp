/**
 * IMPLEMENTATION NOTES
 *
 * Parsing an incomplete block with the sizeinator may go off the end of the
 * block. However, any reasonable block size <16k will simply be allocated by
 * the memory manager, so we won't go off the page, and we'll detect the size
 * mismatch at the next item or at the end.
 *
 * Valgrind may complain though.
 */
#include "serializing.h"

#include "leaf_node.h"
#include "internal_node.h"
#include "overflow_node.h"

#include <cmath>
#include <boost/lexical_cast.hpp>

#define to_string boost::lexical_cast<std::string>

#define VALIDATE_OFFSET \
    if (m_offset >= m_input.size()) \
        throw std::runtime_error((std::string("End of block while parsing node data: ") + to_string(m_offset) + " >= " + to_string(m_input.size())).c_str())

namespace libbruce {


struct NodeParser
{
    NodeParser(const mempage &input, const tree_functions &fns)
        : m_input(input), fns(fns), m_offset(sizeof(flags_t) + sizeof(keycount_t)) { }

    leafnode_ptr parseLeafNode()
    {
        keycount_t count = keyCount();

        std::vector<kv_pair> items;
        items.reserve(count);

        // Read N keys
        for (keycount_t i = 0; i < count; i++)
        {
            VALIDATE_OFFSET;
            uint32_t size = fns.keySize(m_input.at<const char>(m_offset));

            // Push back
            items.push_back(kv_pair(m_input.slice(m_offset, size), memslice()));

            m_offset += size;
        }

        // Read N values
        for (keycount_t i = 0; i < count; i++)
        {
            VALIDATE_OFFSET;
            uint32_t size = fns.valueSize(m_input.at<const char>(m_offset));

            // Couple value to key
            items[i].second = m_input.slice(m_offset, size);

            m_offset += size;
        }

        leafnode_ptr ret = boost::make_shared<LeafNode>(&items, fns);

        VALIDATE_OFFSET;
        ret->overflow.count = *m_input.at<itemcount_t>(m_offset);
        m_offset += sizeof(itemcount_t);

        VALIDATE_OFFSET;
        ret->overflow.nodeID = *m_input.at<nodeid_t>(m_offset);
        m_offset += sizeof(nodeid_t);

        validateAtEnd();

        return ret;
    }

    overflownode_ptr parseOverflowNode()
    {
        overflownode_ptr ret = boost::make_shared<OverflowNode>(keyCount());

        // Read N values
        for (keycount_t i = 0; i < keyCount(); i++)
        {
            VALIDATE_OFFSET;
            uint32_t size = fns.valueSize(m_input.at<const char>(m_offset));

            // Couple value to key
            ret->values.push_back(m_input.slice(m_offset, size));

            m_offset += size;
        }

        VALIDATE_OFFSET;
        ret->next.count = *m_input.at<itemcount_t>(m_offset);
        m_offset += sizeof(itemcount_t);

        VALIDATE_OFFSET;
        ret->next.nodeID = *m_input.at<nodeid_t>(m_offset);
        m_offset += sizeof(nodeid_t);

        validateAtEnd();

        return ret;
    }

    internalnode_ptr parseInternalNode()
    {
        internalnode_ptr ret = boost::make_shared<InternalNode>(keyCount());

        keycount_t editCount = *m_input.at<keycount_t>(m_offset);
        m_offset += sizeof(keycount_t);

        //------------------------------------------------
        //  Read branches

        // Read N-1 keys, starting at 1
        ret->branches.push_back(node_branch(memslice(), nodeid_t(), 0));
        for (keycount_t i = 1; i < keyCount(); i++)
        {
            VALIDATE_OFFSET;
            uint32_t size = fns.keySize(m_input.at<const char>(m_offset));

            ret->branches.push_back(node_branch(m_input.slice(m_offset, size), nodeid_t(), 0));

            m_offset += size;
        }

        // Read N node identifiers
        for (keycount_t i = 0; i < keyCount(); i++)
        {
            VALIDATE_OFFSET;
            uint32_t size = sizeof(nodeid_t);

            ret->branches[i].nodeID = *m_input.at<nodeid_t>(m_offset);

            m_offset += size;
        }

        // Read N item counts
        for (keycount_t i = 0; i < keyCount(); i++)
        {
            VALIDATE_OFFSET;
            uint32_t size = sizeof(itemcount_t);

            ret->branches[i].itemCount = *m_input.at<itemcount_t>(m_offset);

            m_offset += size;
        }

        //------------------------------------------------
        //  Read pending edits
        std::vector<edit_t> editTypes;
        std::vector<memslice> editKeys;
        editTypes.reserve(editCount);
        editKeys.reserve(editCount);

        // Edit types
        for (keycount_t i = 0; i < editCount; i++)
        {
            VALIDATE_OFFSET;
            editTypes.push_back((edit_t)*m_input.at<uint8_t>(m_offset));
            m_offset += sizeof(uint8_t);
        }

        // Keys
        for (keycount_t i = 0; i < editCount; i++)
        {
            VALIDATE_OFFSET;

            uint32_t size = fns.keySize(m_input.at<const char>(m_offset));
            editKeys.push_back(m_input.slice(m_offset, size));

            m_offset += size;
        }

        // Values
        for (keycount_t i = 0; i < editCount; i++)
        {
            memslice value;

            if (editTypes[i] != REMOVE_KEY) // Doesn't have a value
            {
                VALIDATE_OFFSET;

                uint32_t size = fns.valueSize(m_input.at<const char>(m_offset));
                value = m_input.slice(m_offset, size);
            }

            ret->editQueue.push_back(pending_edit(editTypes[i], editKeys[i], value, false));

            m_offset += value.size();
        }

        validateAtEnd();

        return ret;
    }

private:
    keycount_t keyCount()
    {
        return *m_input.at<keycount_t>(sizeof(flags_t));
    }

    void validateAtEnd()
    {
        if (m_offset != m_input.size())
            throw std::runtime_error((std::string("Trailing bytes found: ") +
                                     boost::lexical_cast<std::string>(m_input.size()
                                                                      -
                                                                      m_offset)
                                     + " bytes left").c_str());
    }

    size_t m_offset;
    const mempage &m_input;
    const tree_functions &fns;
};

//----------------------------------------------------------------------

node_ptr ParseNode(const mempage &input, const tree_functions &fns)
{
    NodeParser parser(input, fns);

    switch (*input.at<flags_t>(0))
    {
        case TYPE_INTERNAL:
            return parser.parseInternalNode();
        case TYPE_LEAF:
            return parser.parseLeafNode();
        case TYPE_OVERFLOW:
            return parser.parseOverflowNode();
    }

    throw std::runtime_error("Unknown node type");
}

//----------------------------------------------------------------------

NodeSize::NodeSize(uint32_t blockSize)
    : m_blockSize(blockSize), m_size(sizeof(flags_t) + sizeof(keycount_t))
{
}

bool NodeSize::shouldSplit()
{
    return m_blockSize && m_blockSize < m_size;
}

//----------------------------------------------------------------------

LeafNodeSize::LeafNodeSize(const leafnode_ptr &node, uint32_t blockSize)
    : NodeSize(blockSize)
{
    m_size += sizeof(itemcount_t) + sizeof(nodeid_t);  // For the chained overflow block
    uint32_t splitSize = m_size; // Header

    m_size += node->elementsSize();

    if (shouldSplit() && !node->pairs.empty())
    {
        uint32_t pieceSize = std::ceil(m_blockSize / 2.0);

        pairlist_t::const_iterator here;
        pairlist_t::const_iterator startOfThisKey = node->pairs.begin();

        for (pairlist_t::const_iterator it = node->pairs.begin(); it != node->pairs.end(); ++it)
        {
            if (!(it->first == startOfThisKey->first))
                startOfThisKey = it;

            splitSize += it->first.size() + it->second.size();
            if (splitSize > pieceSize)
            {
                here = it;
                break;
            }
        }

        // 'here' is in the middle of the highest key range that should still be included.
        //
        // [K][K............K)[L]
        //    |     |         |
        //    |    here      splitStart
        //   overflowstart

        // Move the split index forwards while we're on the same key
        m_splitStart = here;
        while (m_splitStart != node->pairs.end() && m_splitStart->first == here->first)
            ++m_splitStart;

        // Move the overflow start back while there is still a key before it that is the same
        m_overflowStart = startOfThisKey;
        ++m_overflowStart;
    }
}

//----------------------------------------------------------------------

OverflowNodeSize::OverflowNodeSize(const overflownode_ptr &node, uint32_t blockSize)
    : NodeSize(blockSize)
{
    m_size += sizeof(itemcount_t) + sizeof(nodeid_t);  // For the chained overflow block
    uint32_t baseSize = m_size;

    for (valuelist_t::const_iterator it = node->values.begin(); it != node->values.end(); ++it)
    {
        m_size += it->size();
    }

    if (shouldSplit())
    {
        uint32_t pieceSize = m_blockSize;
        uint32_t splitSize = baseSize;

        // Find the split index
        for (m_splitIndex = 0; m_splitIndex < node->valueCount(); m_splitIndex++)
        {
            splitSize += node->values[m_splitIndex].size();
            if (splitSize > pieceSize)
                break;
        }
    }
}

InternalNodeSize::InternalNodeSize(const internalnode_ptr &node, uint32_t blockSize, uint32_t maxEditQueueSize)
    : NodeSize(blockSize), m_maxEditQueueSize(maxEditQueueSize), m_editQueueSize(0)
{
    m_blockSize = blockSize - maxEditQueueSize; // The "block size" excludes the queue area

    m_size += sizeof(keycount_t); // Size of edit queue

    for (branchlist_t::const_iterator it = node->branches.begin(); it != node->branches.end(); ++it)
    {
        // We never store the first key
        if (it != node->branches.begin())
            m_size += it->minKey.size();
        m_size += sizeof(nodeid_t) + sizeof(itemcount_t);
    }

    for (editlist_t::const_iterator it = node->editQueue.begin(); it != node->editQueue.end(); ++it)
    {
        m_editQueueSize += sizeof(uint8_t); // 1 byte for the edit type
        m_editQueueSize += it->key.size() + it->value.size();
    }

    if (shouldSplit())
    {
        uint32_t splitSize = m_size;
        uint32_t pieceSize = std::ceil(m_size / 2.0);

        for (m_splitIndex = 1; m_splitIndex < node->branchCount(); m_splitIndex++)
        {
            if (m_splitIndex != 1) splitSize += node->branch(m_splitIndex-1).minKey.size();
            splitSize += sizeof(nodeid_t) + sizeof(itemcount_t);

            if (splitSize > pieceSize)
                return;
        }
    }
}

bool InternalNodeSize::shouldApplyEditQueue() const
{
    return m_editQueueSize > m_maxEditQueueSize;
}

//----------------------------------------------------------------------

mempage SerializeLeafNode(const leafnode_ptr &node)
{
    LeafNodeSize size(node, 0);
    mempage mem(size.size());

    uint32_t offset = 0;

    // Flags
    *mem.at<flags_t>(offset) = node->nodeType();
    offset += sizeof(flags_t);

    // Count
    *mem.at<keycount_t>(offset) = node->pairCount();
    offset += sizeof(keycount_t);

    // Keys
    for (pairlist_t::const_iterator it = node->pairs.begin(); it != node->pairs.end(); ++it)
    {
        memcpy(mem.at<char>(offset), it->first.ptr(), it->first.size());
        offset += it->first.size();
    }

    // Values
    for (pairlist_t::const_iterator it = node->pairs.begin(); it != node->pairs.end(); ++it)
    {
        memcpy(mem.at<char>(offset), it->second.ptr(), it->second.size());
        offset += it->second.size();
    }

    // Overflow block
    *mem.at<itemcount_t>(offset) = node->overflow.count;
    offset += sizeof(itemcount_t);

    *mem.at<nodeid_t>(offset) = node->overflow.nodeID;
    offset += sizeof(nodeid_t);

    return mem;
}

mempage SerializeOverflowNode(const overflownode_ptr &node)
{
    OverflowNodeSize size(node, 0);
    mempage mem(size.size());

    uint32_t offset = 0;

    // Flags
    *mem.at<flags_t>(offset) = node->nodeType();
    offset += sizeof(flags_t);

    // Count
    *mem.at<keycount_t>(offset) = node->valueCount();
    offset += sizeof(keycount_t);

    // Values
    for (valuelist_t::const_iterator it = node->values.begin(); it != node->values.end(); ++it)
    {
        memcpy(mem.at<char>(offset), it->ptr(), it->size());
        offset += it->size();
    }

    // Next overflow block
    *mem.at<itemcount_t>(offset) = node->next.count;
    offset += sizeof(itemcount_t);

    *mem.at<nodeid_t>(offset) = node->next.nodeID;
    offset += sizeof(nodeid_t);

    return mem;
}

mempage SerializeInternalNode(const internalnode_ptr &node)
{
    InternalNodeSize size(node, 0, 0);
    mempage mem(size.size() + size.editQueueSize());

    uint32_t offset = 0;

    // Flags
    *mem.at<flags_t>(offset) = node->nodeType();
    offset += sizeof(flags_t);

    // Count
    *mem.at<keycount_t>(offset) = node->branchCount();
    offset += sizeof(keycount_t);

    // Edit queue count
    *mem.at<keycount_t>(offset) = node->editQueue.size();
    offset += sizeof(keycount_t);

    // Keys (except the 1st one)
    bool first = true;
    for (branchlist_t::const_iterator it = node->branches.begin(); it != node->branches.end(); ++it)
    {
        if (!first)
        {
            memcpy(mem.at<char>(offset), it->minKey.ptr(), it->minKey.size());
            offset += it->minKey.size();
        }
        first = false;
    }

    // IDs
    for (branchlist_t::const_iterator it = node->branches.begin(); it != node->branches.end(); ++it)
    {
        *mem.at<nodeid_t>(offset) = it->nodeID;
        offset += sizeof(nodeid_t);
    }

    // Item counts
    for (branchlist_t::const_iterator it = node->branches.begin(); it != node->branches.end(); ++it)
    {
        *mem.at<itemcount_t>(offset) = it->itemCount;
        offset += sizeof(itemcount_t);
    }

    // Edit types
    for (editlist_t::const_iterator it = node->editQueue.begin(); it != node->editQueue.end(); ++it)
    {
        *mem.at<uint8_t>(offset) = it->edit;
        offset += sizeof(uint8_t);
    }

    // Edit keys
    for (editlist_t::const_iterator it = node->editQueue.begin(); it != node->editQueue.end(); ++it)
    {
        memcpy(mem.at<char>(offset), it->key.ptr(), it->key.size());
        offset += it->key.size();
    }

    // Edit values
    for (editlist_t::const_iterator it = node->editQueue.begin(); it != node->editQueue.end(); ++it)
    {
        if (it->edit != REMOVE_KEY) // Doesn't have a value
        {
            memcpy(mem.at<char>(offset), it->value.ptr(), it->value.size());
            offset += it->value.size();
        }
    }

    assert(offset == mem.size());

    return mem;
}

mempage SerializeNode(const node_ptr &node)
{
    switch (node->nodeType())
    {
        case TYPE_LEAF:
            return SerializeLeafNode(boost::static_pointer_cast<LeafNode>(node));
        case TYPE_INTERNAL:
            return SerializeInternalNode(boost::static_pointer_cast<InternalNode>(node));
        case TYPE_OVERFLOW:
            return SerializeOverflowNode(boost::static_pointer_cast<OverflowNode>(node));
    }
}

}
