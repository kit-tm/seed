#include <LLDPForwardingLoop.h>
#include <algorithm>
#include <string>
#include <queue>

using namespace std;

struct comp {
    bool operator() (const pair<string,int> &a, const pair<string,int> &b) {
        return a.second > b.second;
    }
};

Define_Module(LLDPForwardingLoop);


LLDPForwardingLoop::LLDPForwardingLoop(){

}

LLDPForwardingLoop::~LLDPForwardingLoop(){

}

void LLDPForwardingLoop::initialize(){
    AbstractControllerApp::initialize();
    dropIfNoRouteFound = par("dropIfNoRouteFound");
    ignoreArpRequests = par("ignoreArpRequests");
    printMibGraph = par("printMibGraph");
    timeOut = par("timeOut");
    lldpAgent = NULL;
    version = -1;
    versionHit = 0;
    versionMiss = 0;
    cacheHit = 0;
    cacheMiss = 0;
}

void LLDPForwardingLoop::handlePacketIn(OFP_Packet_In * packet_in_msg){
    //get some details
    CommonHeaderFields headerFields = extractCommonHeaderFields(packet_in_msg);

    //ignore lldp packets
    if(headerFields.eth_type == 0x88CC){
        return;
    }

    //ignore arp requests
    if(ignoreArpRequests && headerFields.eth_type == ETHERTYPE_ARP && packet_in_msg->getMatch().OFB_ARP_OP == ARP_REQUEST){
        return;
    }

    //compute path for non arps
    std::list<LLDPPathSegment> route = computePath(headerFields.swInfo->getMacAddress(),headerFields.dst_mac.str());

    //if route empty flood
    if(route.empty()){
        if (headerFields.eth_type == ETHERTYPE_ARP) {
          int connId = controller->findSwitchInfoFor(packet_in_msg)->getConnId();
          auto fCKey = std::make_tuple(connId, headerFields.arp_op, headerFields.arp_src_adr, headerFields.arp_dst_adr);
          auto fCRes = floodCache.insert(std::make_pair(fCKey, SIMTIME_ZERO));
          if (fCRes.second || fCRes.first->second + timeOut < simTime())
          {
              floodPacket(packet_in_msg);
          }
          fCRes.first->second = simTime();
        } else if(dropIfNoRouteFound){
            dropPacket(packet_in_msg);
        } else {
            floodPacket(packet_in_msg);
        }
    } else {
        std::string computedRoute = "";
        //send packet to next hop
        sendPacket(packet_in_msg,route.front().outport);

        //iterate the rest of the route and set flow mods for switches under my control
        for (auto &seg : route){
            //concatenate route
            computedRoute += seg.chassisId + " -> ";

            oxm_basic_match match = oxm_basic_match();
            match.OFB_ETH_DST = headerFields.dst_mac;

            match.wildcards= 0;
            match.wildcards |= OFPFW_IN_PORT;
            match.wildcards |=  OFPFW_DL_SRC;
            match.wildcards |= OFPFW_DL_TYPE;

            TCPSocket * socket = controller->findSocketForChassisId(seg.chassisId);

            //is switch under our control
            if(socket != NULL){
                sendFlowModMessage(OFPFC_ADD, match, seg.outport, socket,par("flowModIdleTimeOut"),par("flowModHardTimeOut"));
            }
        }

        //clean up route
        computedRoute.erase(computedRoute.length()-4);
        EV << "Route:" << computedRoute << endl;
    }
}

void LLDPForwardingLoop::receiveSignal(cComponent *src, simsignal_t id, cObject *obj) {
    AbstractControllerApp::receiveSignal(src,id,obj);

    //set lldp link
    if(lldpAgent == NULL && controller != NULL){
        std::vector<AbstractControllerApp *>* appList = controller->getAppList();
        std::vector<AbstractControllerApp *>::iterator iterApp;

        for(iterApp=appList->begin();iterApp!=appList->end();++iterApp){
            if(dynamic_cast<LLDPAgent *>(*iterApp) != NULL) {
                LLDPAgent *lldp = (LLDPAgent *) *iterApp;
                lldpAgent = lldp;
                break;
            }
        }
    }

    if(id == PacketInSignalId){
        EV << "LLDPForwardingLoop::PacketIn" << endl;
        if (dynamic_cast<OFP_Packet_In *>(obj) != NULL) {
            OFP_Packet_In *packet_in_msg = (OFP_Packet_In *) obj;
            handlePacketIn(packet_in_msg);
        }
    }
}

std::list<LLDPPathSegment> LLDPForwardingLoop::computePath(std::string srcId, std::string dstId){
    LLDPMibGraph *mibGraph = lldpAgent->getMibGraph();
    mibGraph->removeExpiredEntries();
    std::map<std::string, std::vector<LLDPMib> > verticies = mibGraph->getVerticies();

    EV << "Finding Route in " << mibGraph->getNumOfVerticies() << " Verticies and " << mibGraph->getNumOfEdges() << " Edges" << endl;
    if(printMibGraph){
        EV << mibGraph->getStringGraph() << endl;
    }

    EV << "Version Hit: " << versionHit << " Version Miss: " << versionMiss << " Cache Hit: " << cacheHit << " Cache Miss: " << cacheMiss << endl;

    //check for route in cache
    if(lldpAgent->getMibGraph()->getVersion() == version){
        versionHit++;
        if(routeCache.count(std::pair<std::string,std::string>(srcId,dstId))>0){
            cacheHit++;
            std::list<LLDPPathSegment> tmp = routeCache[std::pair<std::string,std::string>(srcId,dstId)];
            std::list<LLDPPathSegment> res = std::list<LLDPPathSegment>();
            std::copy(tmp.begin(),tmp.end(), std::back_inserter(res));
            return res;
        }else {
            cacheMiss++;
        }
    } else {
        //update version miss and clear local cache
        versionMiss++;
        routeCache.clear();
        version = mibGraph->getVersion();
    }

    std::list<LLDPPathSegment> result = std::list<LLDPPathSegment>();

    //quick check if we have src and target in our graph
    if(verticies.count(srcId)<=0 || verticies.count(dstId)<=0){
        return result;
    }

    //dijkstra
    std::map<std::string,int> dist =  std::map<std::string,int>();
    std::map<std::string,LLDPPathSegment> prev = std::map<std::string,LLDPPathSegment >();
    std::map<std::string,bool> visited = std::map<std::string,bool>();

    //extract all vertexes
    priority_queue< pair<string,int>, vector< pair<string,int> >, comp > q;

    LLDPPathSegment seg;
    std::map<std::string,std::vector<LLDPMib> >::iterator iterKey = verticies.begin();
    while (iterKey != verticies.end()) {
        if(strcmp(iterKey->first.c_str(),srcId.c_str())!=0){
            q.push(pair<string,int>(iterKey->first,std::numeric_limits<int>::max()));

            dist[iterKey->first] = std::numeric_limits<int>::max();

            seg = LLDPPathSegment();
            seg.chassisId ="";
            seg.outport=-1;
            prev[iterKey->first]=seg;
        }
        iterKey++;
    }

    //set start vertex
    q.push(pair<string,int>(srcId,0));
    seg = LLDPPathSegment();
    seg.chassisId ="";
    seg.outport=-1;
    prev[srcId]=seg;

    std::string u;
    while(!q.empty()){

        u = q.top().first;
        q.pop();

        if(visited.count(u)>0){
            continue;
        }

        int alt;
        std::vector<LLDPMib>::iterator iterList;
        for(iterList = verticies[u].begin();iterList!=verticies[u].end();iterList++){
            if(visited.count(iterList->getDstId())<=0){
                alt = dist[u]+1;
                if(alt < dist[iterList->getDstId()]){
                    dist[iterList->getDstId()] = alt;
                    seg = LLDPPathSegment();
                    seg.chassisId = u;
                    seg.outport= (iterList)->getSrcPort();
                    prev[(iterList)->getDstId()]=seg;
                    q.push(pair<string,int>(iterList->getDstId(),alt));
                }
            }
        }

        //add u to visited
        visited[u] = true;
    }

    //back track and insert into list
    std::string trg = dstId;
    LLDPPathSegment segP;
    while(strcmp(prev[trg].chassisId.c_str(),"") != 0){
        segP = LLDPPathSegment();
        segP.chassisId = prev[trg].chassisId;
        segP.outport = prev[trg].outport;
        result.push_front(segP);
        trg = prev[trg].chassisId;
    }

    //check if there was route from src to dst
    if(strcmp(srcId.c_str(),trg.c_str()) != 0){
        result.clear();
    }else{
        //add to cache
        routeCache[std::pair<std::string,std::string>(srcId,dstId)]=result;
    }
    std::list<LLDPPathSegment> res2 = std::list<LLDPPathSegment>();
    std::copy(result.begin(),result.end(), std::back_inserter(res2));
    return res2;
}
