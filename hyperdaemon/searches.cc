// Copyright (c) 2011, Cornell University
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//     * Redistributions of source code must retain the above copyright notice,
//       this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//     * Neither the name of HyperDex nor the names of its contributors may be
//       used to endorse or promote products derived from this software without
//       specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#define __STDC_LIMIT_MACROS

// Google Log
#include <glog/logging.h>

// e
#include <e/endian.h>

// HyperDisk
#include "hyperdisk/hyperdisk/disk.h"
#include "hyperdisk/hyperdisk/returncode.h"

// HyperDex
#include "hyperdex/hyperdex/packing.h"

// HyperDaemon
#include "hyperdaemon/datalayer.h"
#include "hyperdaemon/logical.h"
#include "hyperdaemon/searches.h"

using hyperdex::coordinatorlink;
using hyperdex::entityid;
using hyperdex::regionid;
using hyperspacehashing::search;
using hyperspacehashing::mask::coordinate;

class hyperdaemon::searches::search_state
{
    public:
        search_state(const hyperdex::regionid& region,
                     const hyperspacehashing::mask::coordinate& search_coord,
                     std::auto_ptr<e::buffer> msg,
                     const hyperspacehashing::search& terms,
                     e::intrusive_ptr<hyperdisk::snapshot> snap);
        ~search_state() throw ();

    public:
        po6::threads::mutex lock;
        const hyperdex::regionid region;
        const hyperspacehashing::mask::coordinate search_coord;
        const std::auto_ptr<e::buffer> backing;
        hyperspacehashing::search terms;
        e::intrusive_ptr<hyperdisk::snapshot> snap;

    private:
        friend class e::intrusive_ptr<search_state>;

    private:
        void inc() { __sync_add_and_fetch(&m_ref, 1); }
        void dec() { if (__sync_sub_and_fetch(&m_ref, 1) == 0) delete this; }

    private:
        size_t m_ref;
};

class hyperdaemon::searches::search_id
{
    public:
        search_id(const hyperdex::regionid re,
                  const hyperdex::entityid cl,
                  uint64_t sn)
        : region(re), client(cl), search_number(sn) {}
        ~search_id() throw () {}

    public:
        int compare(const search_id& other) const
        { return e::tuple_compare(region, client, search_number,
                                  other.region, other.client, other.search_number); }

    public:
        bool operator < (const search_id& rhs) const { return compare(rhs) < 0; }
        bool operator <= (const search_id& rhs) const { return compare(rhs) <= 0; }
        bool operator == (const search_id& rhs) const { return compare(rhs) == 0; }
        bool operator != (const search_id& rhs) const { return compare(rhs) != 0; }
        bool operator >= (const search_id& rhs) const { return compare(rhs) >= 0; }
        bool operator > (const search_id& rhs) const { return compare(rhs) > 0; }

    public:
        hyperdex::regionid region;
        hyperdex::entityid client;
        uint64_t search_number;
};

hyperdaemon :: searches :: searches(coordinatorlink* cl,
                                    datalayer* data,
                                    logical* comm)
    : m_cl(cl)
    , m_data(data)
    , m_comm(comm)
    , m_config()
    , m_searches(16)
{
}

hyperdaemon :: searches :: ~searches() throw ()
{
}

void
hyperdaemon :: searches :: prepare(const hyperdex::configuration&,
                                   const hyperdex::instance&)
{
}

void
hyperdaemon :: searches :: reconfigure(const hyperdex::configuration& newconfig,
                                       const hyperdex::instance&)
{
    m_config = newconfig;
}

void
hyperdaemon :: searches :: cleanup(const hyperdex::configuration&,
                                   const hyperdex::instance&)
{
}

void
hyperdaemon :: searches :: start(const hyperdex::entityid& us,
                                 const hyperdex::entityid& client,
                                 uint64_t search_num,
                                 uint64_t nonce,
                                 std::auto_ptr<e::buffer> msg,
                                 const hyperspacehashing::search& terms)
{
    search_id key(us.get_region(), client, search_num);

    if (m_searches.contains(key))
    {
        LOG(INFO) << "DROPPED";
        return;
    }

    schema* sc = m_config.get_schema(us.get_space());
    assert(sc);

    if (sc->attrs_sz != terms.size())
    {
        LOG(INFO) << "DROPPED";
        return;
    }

    flush(us.get_region());

    hyperspacehashing::mask::hasher hasher(m_config.disk_hasher(us.get_subspace()));
    hyperspacehashing::mask::coordinate coord(hasher.hash(terms));
    e::intrusive_ptr<hyperdisk::snapshot> snap = m_data->make_snapshot(us.get_region(), terms);
    e::intrusive_ptr<search_state> state = new search_state(us.get_region(), coord, msg, terms, snap);
    m_searches.insert(key, state);
    next(us, client, search_num, nonce);
}

void
hyperdaemon :: searches :: next(const hyperdex::entityid& us,
                                const hyperdex::entityid& client,
                                uint64_t search_num,
                                uint64_t nonce)
{
    search_id key(us.get_region(), client, search_num);
    e::intrusive_ptr<search_state> state;

    if (!m_searches.lookup(key, &state))
    {
        LOG(INFO) << "DROPPED";
        return;
    }

    po6::threads::mutex::hold hold(&state->lock);

    while (state->snap->valid())
    {
        if (state->search_coord.intersects(state->snap->coordinate()))
        {
            if (state->terms.matches(state->snap->key(), state->snap->value()))
            {
                size_t sz = m_comm->header_size() + sizeof(uint64_t)
                          + sizeof(uint32_t) + state->snap->key().size()
                          + hyperdex::packspace(state->snap->value());
                std::auto_ptr<e::buffer> msg(e::buffer::create(sz));
                msg->pack_at(m_comm->header_size())
                    << nonce
                    << state->snap->key()
                    << state->snap->value();
                m_comm->send(us, client, hyperdex::RESP_SEARCH_ITEM, msg);
                state->snap->next();
                return;
            }
        }

        state->snap->next();
    }

    std::auto_ptr<e::buffer> msg(e::buffer::create(m_comm->header_size() + sizeof(uint64_t)));
    msg->pack_at(m_comm->header_size()) << nonce;
    m_comm->send(us, client, hyperdex::RESP_SEARCH_DONE, msg);
    stop(us, client, search_num);
}

void
hyperdaemon :: searches :: stop(const hyperdex::entityid& us,
                                const hyperdex::entityid& client,
                                uint64_t search_num)
{
    m_searches.remove(search_id(us.get_region(), client, search_num));
}

void
hyperdaemon :: searches :: group_keyop(const hyperdex::entityid& us,
                                       const hyperdex::entityid& client,
                                       uint64_t nonce,
                                       const hyperspacehashing::search& terms,
                                       enum hyperdex::network_msgtype reqtype,
                                       const e::slice& remain)
{
    schema* sc = m_config.get_schema(us.get_space());
    assert(sc);

    if (sc->attrs_sz != terms.size())
    {
        size_t sz = m_comm->header_size() + sizeof(uint64_t) + sizeof(uint16_t);
        std::auto_ptr<e::buffer> msg(e::buffer::create(sz));
        e::buffer::packer pa = msg->pack_at(m_comm->header_size());
        pa = pa << nonce << static_cast<uint16_t>(hyperdex::NET_BADDIMSPEC);
        m_comm->send(us, client, hyperdex::RESP_GROUP_DEL, msg);
        return;
    }

    flush(us.get_region());

    hyperspacehashing::mask::hasher hasher(m_config.disk_hasher(us.get_subspace()));
    hyperspacehashing::mask::coordinate coord(hasher.hash(terms));
    e::intrusive_ptr<hyperdisk::snapshot> snap = m_data->make_snapshot(us.get_region(), terms);

    while (snap->valid())
    {
        if (coord.intersects(snap->coordinate()) &&
            terms.matches(snap->key(), snap->value()))
        {
            size_t sz = m_comm->header_size()
                      + sizeof(uint64_t)
                      + sizeof(uint32_t)
                      + snap->key().size()
                      + remain.size();
            std::auto_ptr<e::buffer> msg(e::buffer::create(sz));
            e::buffer::packer pa = msg->pack_at(m_comm->header_size());
            pa = pa << static_cast<uint64_t>(0) << snap->key();
            pa = pa.copy(remain);

            // Figure out who to talk with.
            hyperdex::entityid dst_ent;
            hyperdex::instance dst_inst;

            if (m_config.point_leader_entity(us.get_space(), snap->key(), &dst_ent, &dst_inst))
            {
                m_comm->send(us, dst_ent, reqtype, msg);
            }
            else
            {
                // If it fails to find the point leader entity, something is
                // horribly wrong.  We make no guarantees, so we just log and move
                // on.
                LOG(ERROR) << "Could not find point leader for GROUP_DEL (serious bug; please report)";
            }
        }

        snap->next();
    }

    size_t sz = m_comm->header_size() + sizeof(uint64_t) + sizeof(uint16_t);
    std::auto_ptr<e::buffer> msg(e::buffer::create(sz));
    e::buffer::packer pa = msg->pack_at(m_comm->header_size());
    pa = pa << nonce << static_cast<uint16_t>(hyperdex::NET_SUCCESS);
    m_comm->send(us, client, hyperdex::RESP_GROUP_DEL, msg);
}

void
hyperdaemon :: searches :: count(const hyperdex::entityid& us,
                                 const hyperdex::entityid& client,
                                 uint64_t nonce,
                                 const hyperspacehashing::search& terms)
{
    schema* sc = m_config.get_schema(us.get_space());
    assert(sc);

    if (sc->attrs_sz != terms.size())
    {
        size_t sz = m_comm->header_size() + sizeof(uint64_t) + sizeof(uint16_t);
        std::auto_ptr<e::buffer> msg(e::buffer::create(sz));
        e::buffer::packer pa = msg->pack_at(m_comm->header_size());
        pa = pa << nonce << static_cast<uint16_t>(hyperdex::NET_BADDIMSPEC);
        m_comm->send(us, client, hyperdex::RESP_COUNT, msg);
        return;
    }

    flush(us.get_region());
    hyperspacehashing::mask::hasher hasher(m_config.disk_hasher(us.get_subspace()));
    hyperspacehashing::mask::coordinate coord(hasher.hash(terms));
    e::intrusive_ptr<hyperdisk::snapshot> snap = m_data->make_snapshot(us.get_region(), terms);

    uint64_t result = 0;

    while (snap->valid())
    {
        if (coord.intersects(snap->coordinate()) &&
            terms.matches(snap->key(), snap->value()))
        {
            ++result;
        }

        snap->next();
    }

    size_t sz = m_comm->header_size() + sizeof(uint64_t) + sizeof(uint16_t) + sizeof(uint64_t);
    std::auto_ptr<e::buffer> msg(e::buffer::create(sz));
    e::buffer::packer pa = msg->pack_at(m_comm->header_size());
    pa = pa << nonce << static_cast<uint16_t>(hyperdex::NET_SUCCESS) << result;
    m_comm->send(us, client, hyperdex::RESP_COUNT, msg);
}

struct sorted_search_item
{
    sorted_search_item(const hyperdisk::reference& ref,
                       const e::slice& key,
                       const std::vector<e::slice>& value,
                       uint16_t sort_by);
    hyperdisk::reference ref;
    e::slice key;
    std::vector<e::slice> value;
    uint16_t sort_by;
};

sorted_search_item :: sorted_search_item(const hyperdisk::reference& r,
                                         const e::slice& k,
                                         const std::vector<e::slice>& v,
                                         uint16_t s)
    : ref(r)
    , key(k)
    , value(v)
    , sort_by(s)
{
}

static bool
sorted_search_lt_string(const sorted_search_item& ssilhs,
                        const sorted_search_item& ssirhs)
{
    e::slice lhs(ssilhs.sort_by > 0 ? ssilhs.value[ssilhs.sort_by - 1] : ssilhs.key);
    e::slice rhs(ssirhs.sort_by > 0 ? ssirhs.value[ssirhs.sort_by - 1] : ssirhs.key);
    int cmp = memcmp(lhs.data(), rhs.data(), std::min(lhs.size(), rhs.size()));

    if (cmp == 0)
    {
        return lhs.size() < rhs.size();
    }

    return cmp < 0;
}

static bool
sorted_search_gt_string(const sorted_search_item& lhs,
                        const sorted_search_item& rhs)
{
    return sorted_search_lt_string(rhs, lhs);
}

static bool
sorted_search_lt_int64(const sorted_search_item& ssilhs,
                       const sorted_search_item& ssirhs)
{
    e::slice lhs(ssilhs.sort_by > 0 ? ssilhs.value[ssilhs.sort_by - 1] : ssilhs.key);
    e::slice rhs(ssirhs.sort_by > 0 ? ssirhs.value[ssirhs.sort_by - 1] : ssirhs.key);
    uint8_t buflhs[sizeof(int64_t)];
    uint8_t bufrhs[sizeof(int64_t)];
    memset(buflhs, 0, sizeof(int64_t));
    memset(bufrhs, 0, sizeof(int64_t));
    memmove(buflhs, lhs.data(), lhs.size());
    memmove(bufrhs, rhs.data(), rhs.size());
    int64_t ilhs;
    int64_t irhs;
    e::unpack64le(buflhs, &ilhs);
    e::unpack64le(bufrhs, &irhs);
    return ilhs < irhs;
}

static bool
sorted_search_gt_int64(const sorted_search_item& lhs,
                       const sorted_search_item& rhs)
{
    return sorted_search_lt_int64(rhs, lhs);
}

void
hyperdaemon :: searches :: sorted_search(const hyperdex::entityid& us,
                                         const hyperdex::entityid& client,
                                         uint64_t nonce,
                                         const hyperspacehashing::search& terms,
                                         uint64_t num,
                                         uint16_t sort_by,
                                         bool maximize)
{
    schema* sc = m_config.get_schema(us.get_space());
    assert(sc);

    if (sc->attrs_sz != terms.size() || sort_by >= sc->attrs_sz ||
        (sc->attrs[sort_by].type != HYPERDATATYPE_STRING && sc->attrs[sort_by].type != HYPERDATATYPE_INT64))
    {
        size_t sz = m_comm->header_size() + sizeof(uint64_t) + sizeof(uint16_t);
        std::auto_ptr<e::buffer> errmsg(e::buffer::create(sz));
        e::buffer::packer pa = errmsg->pack_at(m_comm->header_size());
        pa = pa << nonce << static_cast<uint16_t>(hyperdex::NET_BADDIMSPEC);
        m_comm->send(us, client, hyperdex::RESP_SORTED_SEARCH, errmsg);
        return;
    }

    flush(us.get_region());

    bool (*cmp)(const sorted_search_item& lhs, const sorted_search_item& rhs);

    if (sc->attrs[sort_by].type == HYPERDATATYPE_STRING)
    {
        if (maximize)
        {
            cmp = sorted_search_gt_string;
        }
        else
        {
            cmp = sorted_search_lt_string;
        }
    }
    else if (sc->attrs[sort_by].type == HYPERDATATYPE_INT64)
    {
        if (maximize)
        {
            cmp = sorted_search_gt_int64;
        }
        else
        {
            cmp = sorted_search_lt_int64;
        }
    }
    else
    {
        abort();
    }

    hyperspacehashing::mask::hasher hasher(m_config.disk_hasher(us.get_subspace()));
    hyperspacehashing::mask::coordinate coord(hasher.hash(terms));
    e::intrusive_ptr<hyperdisk::snapshot> snap = m_data->make_snapshot(us.get_region(), terms);

    std::vector<sorted_search_item> top_n;

    if (num < UINT32_MAX)
    {
        top_n.reserve(num);
    }

    while (snap->valid())
    {
        if (coord.intersects(snap->coordinate()) &&
            terms.matches(snap->key(), snap->value()))
        {
            top_n.push_back(sorted_search_item(snap->ref(), snap->key(), snap->value(), sort_by));
            std::push_heap(top_n.begin(), top_n.end(), cmp);

            if (top_n.size() > num)
            {
                std::pop_heap(top_n.begin(), top_n.end(), cmp);
                top_n.pop_back();
            }
        }

        snap->next();
    }

    size_t sz = m_comm->header_size() + sizeof(uint64_t) + sizeof(uint16_t) + sizeof(uint64_t);

    for (size_t i = 0; i < top_n.size(); ++i)
    {
        sz += sizeof(uint32_t) + top_n[i].key.size()
            + hyperdex::packspace(top_n[i].value);
    }

    uint64_t numitems = top_n.size();
    std::auto_ptr<e::buffer> msg(e::buffer::create(sz));
    e::buffer::packer pa = msg->pack_at(m_comm->header_size());
    pa = pa << nonce << static_cast<uint16_t>(hyperdex::NET_SUCCESS) << numitems;

    for (size_t i = 0; i < top_n.size(); ++i)
    {
        pa = pa << top_n[i].key << top_n[i].value;
    }

    m_comm->send(us, client, hyperdex::RESP_SORTED_SEARCH, msg);
}

uint64_t
hyperdaemon :: searches :: hash(const search_id& si)
{
    return si.region.hash() + si.client.hash() + si.search_number;
}

void
hyperdaemon :: searches :: flush(const hyperdex::regionid& r)
{
    bool done = false;

    while(!done)
    {
        hyperdisk::returncode ret = m_data->flush(r, -1, false);

        if (ret == hyperdisk::SUCCESS)
        {
            done = true;
        }
        else if (ret == hyperdisk::DIDNOTHING)
        {
            done = true;
        }
        else if (ret == hyperdisk::DATAFULL || ret == hyperdisk::SEARCHFULL)
        {
            hyperdisk::returncode ioret;
            ioret = m_data->do_mandatory_io(r);

            if (ioret != hyperdisk::SUCCESS && ioret != hyperdisk::DIDNOTHING)
            {
                PLOG(ERROR) << "Disk I/O returned " << ioret;
            }
        }
        else
        {
            PLOG(ERROR) << "Disk flush returned " << ret;
        }
    }
}

hyperdaemon :: searches :: search_state :: search_state(const regionid& r,
                                                        const coordinate& sc,
                                                        std::auto_ptr<e::buffer> msg,
                                                        const hyperspacehashing::search& t,
                                                        e::intrusive_ptr<hyperdisk::snapshot> s)
    : lock()
    , region(r)
    , search_coord(sc)
    , backing(msg)
    , terms(t)
    , snap(s)
    , m_ref(0)
{
}

hyperdaemon :: searches :: search_state :: ~search_state() throw ()
{
}
