/*
 * Copyright information and license terms for this software can be
 * found in the file LICENSE that is included with the distribution
 */

#include <list>

#include <epicsMutex.h>
#include <epicsGuard.h>
#include <errlog.h>

#include <shareLib.h>
#include <pv/sharedPtr.h>
#include <pv/noDefaultMethods.h>
#include <pv/sharedVector.h>
#include <pv/bitSet.h>
#include <pv/pvData.h>
#include <pv/createRequest.h>
#include <pv/status.h>
#include <pv/reftrack.h>

#define epicsExportSharedSymbols
#include "sharedstateimpl.h"

namespace pvas {

size_t SharedChannel::num_instances;


SharedChannel::SharedChannel(const std::tr1::shared_ptr<SharedPV> &owner,
                             const pva::ChannelProvider::shared_pointer provider,
                             const std::string& channelName,
                             const requester_type::shared_pointer& requester)
    :owner(owner)
    ,channelName(channelName)
    ,requester(requester)
    ,provider(provider)
{
    REFTRACE_INCREMENT(num_instances);

    if(owner->debugLvl>5) {
        errlogPrintf("%s : Open channel to %s > %p\n",
                     requester->getRequesterName().c_str(),
                     channelName.c_str(),
                     this);
    }

    SharedPV::Handler::shared_pointer handler;
    {
        Guard G(owner->mutex);
        if(owner->channels.empty())
            handler = owner->handler;
        owner->channels.push_back(this);
    }
    if(handler) {
        handler->onFirstConnect(owner);
    }
}

SharedChannel::~SharedChannel()
{
    std::tr1::shared_ptr<SharedPV::Handler> handler;
    {
        Guard G(owner->mutex);
        owner->channels.remove(this);
        if(owner->channels.empty()) {
            Guard G(owner->mutex);
            handler = owner->handler;
        }
    }
    if(handler) {
        handler->onLastDisconnect(owner);
    }
    if(owner->debugLvl>5)
    {
        pva::ChannelRequester::shared_pointer req(requester.lock());
        errlogPrintf("%s : Open channel to %s > %p\n",
                     req ? req->getRequesterName().c_str() : "<Defunct>",
                     channelName.c_str(),
                     this);
    }

    REFTRACE_DECREMENT(num_instances);
}

void SharedChannel::destroy() {}

std::tr1::shared_ptr<pva::ChannelProvider> SharedChannel::getProvider()
{
    return provider.lock();
}

std::string SharedChannel::getRemoteAddress()
{
    return getChannelName(); // for lack of anything better to do...
}

std::string SharedChannel::getChannelName()
{
    return channelName;
}

std::tr1::shared_ptr<pva::ChannelRequester> SharedChannel::getChannelRequester()
{
    return requester.lock();
}

void SharedChannel::getField(pva::GetFieldRequester::shared_pointer const & requester,std::string const & subField)
{
    epics::pvData::FieldConstPtr desc;
    {
        Guard G(owner->mutex);
        if(owner->type)
            desc = owner->type;
        else
            owner->getfields.push_back(requester);
    }
    if(desc)
        requester->getDone(pvd::Status(), desc);
}

pva::ChannelPut::shared_pointer SharedChannel::createChannelPut(
        pva::ChannelPutRequester::shared_pointer const & requester,
        pvd::PVStructure::shared_pointer const & pvRequest)
{
    std::tr1::shared_ptr<SharedPut> ret(new SharedPut(shared_from_this(), requester, pvRequest));

    pvd::StructureConstPtr type;
    {
        Guard G(owner->mutex);
        // ~SharedPut removes
        owner->puts.push_back(ret.get());
        type = owner->type;
    }
    if(type)
        requester->channelPutConnect(pvd::Status(), ret, type);
    return ret;
}

pva::ChannelRPC::shared_pointer SharedChannel::createChannelRPC(
        pva::ChannelRPCRequester::shared_pointer const & requester,
        pvd::PVStructure::shared_pointer const & pvRequest)
{
    std::tr1::shared_ptr<SharedRPC> ret(new SharedRPC(shared_from_this(), requester, pvRequest));
    bool opened;
    {
        Guard G(owner->mutex);
        owner->rpcs.push_back(ret.get());
        opened = !!owner->type;
    }
    if(opened)
        requester->channelRPCConnect(pvd::Status(), ret);
    return ret;
}

pva::Monitor::shared_pointer SharedChannel::createMonitor(
        pva::MonitorRequester::shared_pointer const & requester,
        pvd::PVStructure::shared_pointer const & pvRequest)
{
    std::tr1::shared_ptr<SharedMonitorFIFO> ret(new SharedMonitorFIFO(shared_from_this(), requester, pvRequest));
    bool notify;
    {
        Guard G(owner->mutex);
        owner->monitors.push_back(ret.get());
        notify = !!owner->type;
        if(notify) {
            ret->open(owner->type);
            // post initial update
            ret->post(*owner->current, owner->valid);
        }
    }
    if(notify)
        ret->notify();
    return ret;
}


SharedMonitorFIFO::SharedMonitorFIFO(const std::tr1::shared_ptr<SharedChannel>& channel,
                                     const requester_type::shared_pointer& requester,
                                     const pvd::PVStructure::const_shared_pointer &pvRequest)
    :pva::MonitorFIFO(requester, pvRequest)
    ,channel(channel)
{}

SharedMonitorFIFO::~SharedMonitorFIFO()
{
    Guard G(channel->owner->mutex);
    channel->owner->monitors.remove(this);
}

Operation::Operation(const std::tr1::shared_ptr<Impl> impl)
    :impl(impl)
{}

const epics::pvData::PVStructure& Operation::pvRequest() const
{
    return *impl->pvRequest;
}

const epics::pvData::PVStructure& Operation::value() const
{
    return *impl->value;
}

const epics::pvData::BitSet& Operation::changed() const
{
    return impl->changed;
}

std::string Operation::channelName() const
{
    std::string ret;
    std::tr1::shared_ptr<epics::pvAccess::Channel> chan(impl->getChannel());
    if(chan) {
        ret = chan->getChannelName();
    }
    return ret;
}

void Operation::complete()
{
    impl->complete(pvd::Status(), 0);
}

void Operation::complete(const epics::pvData::Status& sts)
{
    impl->complete(sts, 0);
}

void Operation::complete(const epics::pvData::PVStructure &value,
                         const epics::pvData::BitSet &changed)
{
    impl->complete(pvd::Status(), &value);
}

void Operation::info(const std::string& msg)
{
    pva::ChannelBaseRequester::shared_pointer req(impl->getRequester());
    if(req)
        req->message(msg, pvd::infoMessage);
}

void Operation::warn(const std::string& msg)
{
    pva::ChannelBaseRequester::shared_pointer req(impl->getRequester());
    if(req)
        req->message(msg, pvd::warningMessage);
}

int Operation::isDebug() const
{
    Guard G(impl->mutex);
    return impl->debugLvl;
}

std::tr1::shared_ptr<epics::pvAccess::Channel> Operation::getChannel()
{
    return impl->getChannel();
}

std::tr1::shared_ptr<pva::ChannelBaseRequester> Operation::getRequester()
{
    return impl->getRequester();
}

bool Operation::valid() const
{
    return !!impl;
}

void Operation::Impl::Cleanup::operator ()(Operation::Impl* impl) {
    bool err;
    {
        Guard G(impl->mutex);
        err = !impl->done;
    }
    if(err)
        impl->complete(pvd::Status::error("Implicit Cancel"), 0);

    delete impl;
}

} // namespace pvas
