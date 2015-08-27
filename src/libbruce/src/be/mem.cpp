#include <libbruce/be/mem.h>

namespace bruce { namespace be {

mem::mem(uint32_t maxBlockSize)
    : m_ctr(0), m_maxBlockSize(maxBlockSize)
{
}

mem::~mem()
{
}

nodeid_t mem::blockCount() const
{
    return m_ctr;
}

std::vector<nodeid_t> mem::newIdentifiers(int n)
{
    std::vector<nodeid_t> ret;

    for (int i = 0; i < n; i++)
         ret.push_back(m_ctr++);

    return ret;
}

memory mem::get(const nodeid_t &id)
{
    blockmap::iterator i = m_blocks.find(id);
    if (i == m_blocks.end()) throw block_not_found(id);
    return i->second;
}

void mem::put_all(putblocklist_t &blocklist)
{
    for (putblocklist_t::iterator it = blocklist.begin(); it != blocklist.end(); ++it)
    {
        if (m_ctr < it->id)
            throw std::runtime_error("Illegal ID");

        if (it->mem.size() > m_maxBlockSize)
            throw std::runtime_error("Block too large");

        m_blocks[it->id] = it->mem;

        it->success = true;
    }
}

void mem::del_all(delblocklist_t &ids)
{
    for (delblocklist_t::iterator it = ids.begin(); it != ids.end(); ++it)
    {
        blockmap::iterator i = m_blocks.find(it->id);
        if (i != m_blocks.end()) {
            m_blocks.erase(i);
            it->success = true;
        }
    }
}

uint32_t mem::maxBlockSize()
{
    return m_maxBlockSize;
}

}}
