
#ifndef LLDPAGENTFORWARDINGECMP_H_
#define LLDPAGENTFORWARDINGECMP_H_

#include <omnetpp.h>
#include "AbstractControllerApp.h"
#include "LLDPAgent.h"
#include "TCPSocket.h"
#include "Switch_Info.h"
#include "LLDPMib.h"
#include "OFP_Packet_In_m.h"
#include "ARPPacket_m.h"

struct LLDPPathSegment{
    std::string chassisId;
    int outport;
};

class LLDPForwardingECMP:public AbstractControllerApp {


public:
    LLDPForwardingECMP();
    ~LLDPForwardingECMP();

protected:
    void receiveSignal(cComponent *src, simsignal_t id, cObject *obj);
    void initialize();
    virtual void handlePacketIn(OFP_Packet_In * packet_in_msg);
    virtual std::list<LLDPPathSegment> computePath(std::string srcId, std::string dstId);


    LLDPAgent * lldpAgent;
    long flooded;
    long forwarded;
    long dropped;
    bool dropIfNoRouteFound;
    bool ignoreArpRequests;
    bool printMibGraph;
    double timeOut;

    std::map<std::tuple<int, int, IPv4Address, IPv4Address>, simtime_t> floodCache;
};


#endif
