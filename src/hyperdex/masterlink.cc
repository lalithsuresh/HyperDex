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

// Google Log
#include <glog/logging.h>

// po6
#include <po6/threads/thread.h>

// HyperDex
#include <hyperdex/masterlink.h>

#define RECONNECT_INTERVAL (1.)

hyperdex :: masterlink :: masterlink(const po6::net::location& loc,
                                     const std::string& announce,
                                     std::tr1::function<void (const hyperdex::configuration&)> inst)
    : m_continue(true)
    , m_loc(loc)
    , m_sock(loc.address.family(), SOCK_STREAM, IPPROTO_TCP)
    , m_thread(std::tr1::bind(std::tr1::function<void (hyperdex::masterlink*)>(&hyperdex::masterlink::run), this))
    , m_dl()
    , m_wake(m_dl)
    , m_io(m_dl)
    , m_timer(m_dl)
    , m_partial()
    , m_config()
    , m_config_valid(true)
    , m_inst(inst)
    , m_announce(announce)
{
    m_wake.set<masterlink, &masterlink::wake>(this);
    m_wake.start();
    m_timer.set<masterlink, &masterlink::time>(this);
    connect();
    m_thread.start();
}

hyperdex :: masterlink :: ~masterlink()
{
    if (m_continue)
    {
        m_wake.send();
        LOG(INFO) << "Master link object not cleanly shutdown.";
    }

    m_thread.join();
}

void
hyperdex :: masterlink :: shutdown()
{
    m_wake.send();
}

void
hyperdex :: masterlink :: connect()
{
    m_sock.connect(m_loc);

    if (m_sock.xwrite(m_announce.c_str(), m_announce.size()) != m_announce.size())
    {
        throw po6::error(errno);
    }

    m_sock.nonblocking();
    m_io.set<masterlink, &masterlink::io>(this);
    m_io.set(m_sock.get(), ev::READ);
    m_io.start();
}

void
hyperdex :: masterlink :: io(ev::io&, int revents)
{
    if ((revents & ev::READ))
    {
        read();
    }
    else if ((revents & ev::WRITE))
    {
        LOG(INFO) << "Write event";
    }
}

void
hyperdex :: masterlink :: read()
{
    size_t ret = e::read(&m_sock, &m_partial, 2048);

    if (ret == 0)
    {
        m_io.stop();
        m_sock.close();

        try
        {
            m_sock.reset(m_loc.address.family(), SOCK_STREAM, IPPROTO_TCP);
            connect();
        }
        catch (po6::error& e)
        {
            m_sock.close();
            m_timer.set(RECONNECT_INTERVAL, 0);
            m_timer.start();
            LOG(INFO) << "could not connect to master";
        }
    }
    else
    {
        size_t index;

        while ((index = m_partial.index('\n')) < m_partial.size())
        {
            std::string s(static_cast<const char*>(m_partial.get()), index);
            m_partial.trim_prefix(index + 1);

            if (s == "end\tof\tline")
            {
                if (m_config_valid)
                {
                    LOG(INFO) << "Installing new configuration file.";
                    install();
                }
                else
                {
                    LOG(WARNING) << "Coordinator sent us a bad configuration file.";
                }

                m_config = hyperdex::configuration();
                m_config_valid = true;
            }
            else
            {
                m_config_valid &= m_config.add_line(s);
            }
        }
    }
}

void
hyperdex :: masterlink :: run()
{
    sigset_t ss;

    if (sigfillset(&ss) < 0)
    {
        PLOG(ERROR) << "sigfillset";
        return;
    }

    if (pthread_sigmask(SIG_BLOCK, &ss, NULL) < 0)
    {
        PLOG(ERROR) << "pthread_sigmask";
        return;
    }

    while (m_continue)
    {
        m_dl.loop(EVLOOP_ONESHOT);
    }
}

void
hyperdex :: masterlink :: time(ev::timer&, int)
{
    try
    {
        m_sock.reset(m_loc.address.family(), SOCK_STREAM, IPPROTO_TCP);
        connect();
        m_timer.stop();
    }
    catch (po6::error& e)
    {
        m_sock.close();
        m_timer.set(RECONNECT_INTERVAL, 0);
        m_timer.start();
        LOG(INFO) << "could not connect to master";
    }
}

void
hyperdex :: masterlink :: wake(ev::async&, int)
{
    m_continue = false;
}

void
hyperdex :: masterlink :: install()
{
    m_inst(m_config);
}
