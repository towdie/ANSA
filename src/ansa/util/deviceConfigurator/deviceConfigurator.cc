//
// Marek Cerny, 2MSK
// FIT VUT 2011
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
// 
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/.
// 

#include "deviceConfigurator.h"

#include "IPv6Address.h"
#include "IPv6InterfaceData.h"
#include "xmlParser.h"

Define_Module(DeviceConfigurator);

using namespace std;

void DeviceConfigurator::initialize(int stage){

    const char *deviceType = par("deviceType");
    const char *deviceId = par("deviceId");
    const char *configFile = par("configFile");
    cXMLElement *device = NULL;

   // interfaces and routing table are not ready before stage 2
    if (stage == 2){

        // get table of interfaces of this device
        ift = InterfaceTableAccess().get();
        if (ift == NULL){
           throw cRuntimeError("InterfaceTable not found");
        }
        // get routing table of this device
        rt = AnsaRoutingTableAccess().getIfExists();
        if (rt != NULL){
           //throw cRuntimeError("AnsaRoutingTable not found");

            for (int i=0; i<ift->getNumInterfaces(); ++i)
                rt->configureInterfaceForIPv4(ift->getInterface(i));

            const char *routerIdStr = par("deviceId").stringValue();
            this->readRoutingTableFromXml(configFile, routerIdStr);
        }

      // get routing table of this device
      rt6 = RoutingTable6Access().getIfExists();
      if (rt6 != NULL){
          //throw cRuntimeError("RoutingTable6 not found");

          // RFC 4861 specifies that sending RAs should be disabled by default
          for (int i = 0; i < ift->getNumInterfaces(); i++){
             ift->getInterface(i)->ipv6Data()->setAdvSendAdvertisements(false);
          }

          device = xmlParser::GetDevice(deviceType, deviceId, configFile);
          if (device == NULL){
             ev << "No configuration found for this device (" << deviceType << " id=" << deviceId << ")" << endl;
             return;
          }

          // configure interfaces - addressing
          cXMLElement *iface = xmlParser::GetInterface(NULL, device);
          if (iface == NULL){
             ev << "No interface configuration found for this device (" << deviceType << " id=" << deviceId << ")" << endl;
          }else{
             loadInterfaceConfig(iface);
          }


          // configure static routing
          cXMLElement *route = xmlParser::GetStaticRoute6(NULL, device);
          if (route == NULL && strcmp(deviceType, "Router") == 0){
             ev << "No static routing configuration found for this device (" << deviceType << " id=" << deviceId << ")" << endl;
          }else{
             loadStaticRouting(route);
          }

          // Adding default route requires routing table lookup to pick the right output
          // interface. This needs to be performed when all IPv6 addresses are already assigned
          // and there are matching records in the routing table.
          cXMLElement *gateway = device->getFirstChildWithTag("DefaultRouter");
          if (gateway == NULL && strcmp(deviceType, "Host") == 0){
             ev << "No default-router configuration found for this device (" << deviceType << " id=" << deviceId << ")" << endl;
          }else{
             loadDefaultRouter(gateway);
          }
      }
   }
   else if(stage == 3)
     {
         device = xmlParser::GetDevice(deviceType, deviceId, configFile);
         if (device == NULL){
            ev << "No configuration found for this device (" << deviceType << " id=" << deviceId << ")" << endl;
            return;
         }

         if (isMulticastEnabled(device))
         {
             // get PIM interface table of this device
             pimIft = PimInterfaceTableAccess().get();
             if (pimIft == NULL){
                 throw cRuntimeError("PimInterfaces not found");
             }

             // is multicast routing enabled on the device?
             if (!isMulticastEnabled(device))
             {
                 EV<< "Multicast routing is not enable for this device (" << deviceType << " id=" << deviceId << ")" << endl;
             }
             else
             {
                 // fill pim interfaces table from config file
                 cXMLElement *iface = xmlParser::GetInterface(NULL, device);
                 if (iface != NULL)
                     loadPimInterfaceConfig(iface);
                 else
                     EV<< "No PIM interface is configured for this device (" << deviceType << " id=" << deviceId << ")" << endl;
             }
         }
     }
}


void DeviceConfigurator::loadInterfaceConfig(cXMLElement *iface){

   // for each interface node
   while (iface != NULL){

      // get interface name and find matching interface in interface table
      const char *ifaceName = iface->getAttribute("name");
      InterfaceEntry *ie = ift->getInterfaceByName(ifaceName);
      if (ie == NULL){
         throw cRuntimeError("No interface called %s on this device", ifaceName);
      }

      // for each IPv6 address
      cXMLElement *addr = xmlParser::GetIPv6Address(NULL, iface);
      while (addr != NULL){

         // get address string
         string addrFull = addr->getNodeValue();
         IPv6Address ipv6;
         int prefixLen;

         // check if it's a valid IPv6 address string with prefix and get prefix
         if (!ipv6.tryParseAddrWithPrefix(addrFull.c_str(), prefixLen)){
            throw cRuntimeError("Unable to set IPv6 address %s on interface %s", addrFull.c_str(), ifaceName);
         }

         ipv6 = IPv6Address(addrFull.substr(0, addrFull.find_last_of('/')).c_str());

         // IPv6NeighbourDiscovery doesn't implement DAD for non-link-local addresses
         // -> we have to set the address as non-tentative
         ie->ipv6Data()->assignAddress(ipv6, false, 0, 0);

         // adding directly connected route to the routing table
         IPv6Route *route = new IPv6Route(ipv6.getPrefix(prefixLen), prefixLen, IPv6Route::STATIC);
         route->setInterfaceId(ie->getInterfaceId());
         route->setNextHop(IPv6Address::UNSPECIFIED_ADDRESS);
         route->setMetric(0);

         rt6->addStaticRoute(ipv6.getPrefix(prefixLen), prefixLen, ie->getInterfaceId(), IPv6Address::UNSPECIFIED_ADDRESS, 0);


         // get next IPv6 address
         addr = xmlParser::GetIPv6Address(addr, NULL);
      }


      // for each parameter
      for (cXMLElement *param = iface->getFirstChild(); param; param = param->getNextSibling()){

         if(strcmp(param->getTagName(), "NdpAdvSendAdvertisements") == 0){
            bool value = false;
            if (!xmlParser::Str2Bool(&value, param->getNodeValue())){
               throw cRuntimeError("Invalid NdpAdvSendAdvertisements value on interface %s", ie->getName());
            }
            ie->ipv6Data()->setAdvSendAdvertisements(value);
         }

         if(strcmp(param->getTagName(), "NdpMaxRtrAdvInterval") == 0){
            int value = 0;
            if (!xmlParser::Str2Int(&value, param->getNodeValue())){
               throw cRuntimeError("Unable to parse valid NdpMaxRtrAdvInterval %s on interface %s", value, ifaceName);
            }
            if (value < 4 || value > 1800){
               value = 600;
            }
            ie->ipv6Data()->setMaxRtrAdvInterval(value);
         }

         if(strcmp(param->getTagName(), "NdpMinRtrAdvInterval") == 0){
            int value = 0;
            if (!xmlParser::Str2Int(&value, param->getNodeValue())){
               throw cRuntimeError("Unable to parse valid NdpMinRtrAdvInterval %s on interface %s", value, ifaceName);
            }
            if (value < 3 || value > 1350){
               value = 200;
            }
            ie->ipv6Data()->setMinRtrAdvInterval(value);
         }
      }

      // for each IPv6 prefix
      cXMLElement *prefix = xmlParser::GetAdvPrefix(NULL, iface);
      while (prefix != NULL){

         // get address string
         string addrFull = prefix->getNodeValue();
         IPv6InterfaceData::AdvPrefix advPrefix;
         int prefixLen;

         // check if it's a valid IPv6 address string with prefix and get prefix
         if (!advPrefix.prefix.tryParseAddrWithPrefix(addrFull.c_str(), prefixLen)){
            throw cRuntimeError("Unable to parse IPv6 prefix %s on interface %s", addrFull.c_str(), ifaceName);
         }
         advPrefix.prefix =  IPv6Address(addrFull.substr(0, addrFull.find_last_of('/')).c_str());
         advPrefix.prefixLength = prefixLen;

         const char *validLifeTime = prefix->getAttribute("valid");
         const char *preferredLifeTime = prefix->getAttribute("preferred");
         int value;

         value = 2592000;
         if (validLifeTime != NULL){
            if (!xmlParser::Str2Int(&value, validLifeTime)){
               throw cRuntimeError("Unable to parse valid lifetime %s on IPv6 prefix %s on interface %s", validLifeTime, addrFull.c_str(), ifaceName);
            }
            advPrefix.advValidLifetime = value;
         }

         value = 604800;
         if (preferredLifeTime != NULL){
            if (!xmlParser::Str2Int(&value, preferredLifeTime)){
               throw cRuntimeError("Unable to parse preferred lifetime %s on IPv6 prefix %s on interface %s", preferredLifeTime, addrFull.c_str(), ifaceName);
            }
            advPrefix.advPreferredLifetime = value;
         }

         advPrefix.advOnLinkFlag = true;
         advPrefix.advAutonomousFlag = true;

         // adding prefix
         ie->ipv6Data()->addAdvPrefix(advPrefix);

         // get next IPv6 address
         prefix = xmlParser::GetAdvPrefix(prefix, NULL);
      }



      // get next interface
      iface = xmlParser::GetInterface(iface, NULL);
   }
}


void DeviceConfigurator::loadStaticRouting(cXMLElement *route){

   // for each static route
   while (route != NULL){

      // get network address string with prefix
      cXMLElement *network = route->getFirstChildWithTag("NetworkAddress");
      if (network == NULL){
         throw cRuntimeError("IPv6 network address for static route not set");
      }

      string addrFull = network->getNodeValue();
      IPv6Address addrNetwork;
      int prefixLen;

      // check if it's a valid IPv6 address string with prefix and get prefix
      if (!addrNetwork.tryParseAddrWithPrefix(addrFull.c_str(), prefixLen)){
         throw cRuntimeError("Unable to set IPv6 network address %s for static route", addrFull.c_str());
      }

      addrNetwork = IPv6Address(addrFull.substr(0, addrFull.find_last_of('/')).c_str());


      // get IPv6 next hop address string without prefix
      cXMLElement *nextHop = route->getFirstChildWithTag("NextHopAddress");
      if (nextHop == NULL){
         throw cRuntimeError("IPv6 next hop address for static route not set");
      }

      IPv6Address addrNextHop = IPv6Address(nextHop->getNodeValue());


      // optinal argument - administrative distance is set to 1 if not set
      cXMLElement *distance = route->getFirstChildWithTag("AdministrativeDistance");
      int adminDistance = 1;
      if (distance != NULL){
         if (!xmlParser::Str2Int(&adminDistance, distance->getNodeValue())){
            adminDistance = 0;
         }
      }

      if (adminDistance < 1 || adminDistance > 255){
         throw cRuntimeError("Invalid administrative distance for static route (%d)", adminDistance);
      }


      // current INET routing lookup implementation is not recursive
      // -> nextHop needs to be known network and we have to set output interface manually

      // browse connected routes and find one that matches next hop address
      const IPv6Route *record = rt6->doLongestPrefixMatch(addrNextHop);
      if (record == NULL){
         ev << "No directly connected route for IPv6 next hop address " << addrNextHop << " found" << endl;
      }else{
         // add static route
         rt6->addStaticRoute(addrNetwork, prefixLen, record->getInterfaceId(), addrNextHop, adminDistance);
      }


      // get next static route
      route = xmlParser::GetStaticRoute6(route, NULL);
   }
}


void DeviceConfigurator::loadDefaultRouter(cXMLElement *gateway){

   if (gateway == NULL)
      return;

   // get default-router address string (without prefix)
   IPv6Address nextHop;
   nextHop = IPv6Address(gateway->getNodeValue());

   // browse routing table to find the best route to default-router
   const IPv6Route *route = rt6->doLongestPrefixMatch(nextHop);
   if (route == NULL){
      return;
   }

   // add default static route
   rt6->addStaticRoute(IPv6Address::UNSPECIFIED_ADDRESS, 0, route->getInterfaceId(), nextHop, 1);
}


void DeviceConfigurator::handleMessage(cMessage *msg){
   throw cRuntimeError("This module does not receive messages");
   delete msg;
}

bool DeviceConfigurator::readRoutingTableFromXml(const char *filename, const char *RouterId)
{
    cXMLElement* routerConfig = ev.getXMLDocument(filename);
    if (routerConfig == NULL) {
        return false;
    }

    // load information on this router
    std::string routerXPath("Router[@id='");
    routerXPath += RouterId;
    routerXPath += "']";

    cXMLElement* routerNode = routerConfig->getElementByPath(routerXPath.c_str());
    if (routerNode == NULL)
        opp_error("No configuration for Router ID: %s", RouterId);

    cXMLElement* IntNode = routerNode->getFirstChildWithTag("Interfaces");
    if (IntNode)
        readInterfaceFromXml(IntNode);

    cXMLElement* routingNode = routerNode->getFirstChildWithTag("Routing");
    if (routingNode){
       cXMLElement* staticNode = routingNode->getFirstChildWithTag("Static");
       if (staticNode)
          readStaticRouteFromXml(staticNode);
    }
    return true;
}

void DeviceConfigurator::readInterfaceFromXml(cXMLElement* Node)
{
    InterfaceEntry* ie;

    cXMLElementList intConfig = Node->getChildren();
    for (cXMLElementList::iterator intConfigIt = intConfig.begin(); intConfigIt != intConfig.end(); intConfigIt++)
    {
      std::string nodeName = (*intConfigIt)->getTagName();
      if (nodeName == "Interface" && (*intConfigIt)->getAttribute("name"))
      {
        std::string intName=(*intConfigIt)->getAttribute("name");
        std::string typeName=intName.substr(0,3);

        ie=ift->getInterfaceByName(intName.c_str());

        if (!ie)
          opp_error("Error in routing file: interface name `%s' not registered by any L2 module", intName.c_str());

        //implicitne nastavenia
        if (typeName=="eth")
              ie->setBroadcast(true);
        if (typeName=="ppp")
              ie->setPointToPoint(true);

        //register multicast groups
        ie->ipv4Data()->addMulticastListener(IPv4Address("224.0.0.1"));
        ie->ipv4Data()->addMulticastListener(IPv4Address("224.0.0.2"));

        ie->ipv4Data()->setMetric(1);
        ie->setMtu(1500);

        cXMLElementList ifDetails = (*intConfigIt)->getChildren();
        for (cXMLElementList::iterator ifElemIt = ifDetails.begin(); ifElemIt != ifDetails.end(); ifElemIt++)
        {
          std::string nodeName = (*ifElemIt)->getTagName();

          if (nodeName=="IPAddress")
          {
            ie->ipv4Data()->setIPAddress(IPv4Address((*ifElemIt)->getNodeValue()));
          }

          if (nodeName=="Mask")
          {
            ie->ipv4Data()->setNetmask(IPv4Address((*ifElemIt)->getNodeValue()));
          }

          if (nodeName=="MTU")
          {
            ie->setMtu(atoi((*ifElemIt)->getNodeValue()));
          }

        }

      }
    }
}

void DeviceConfigurator::readStaticRouteFromXml(cXMLElement* Node)
{
  cXMLElementList intConfig = Node->getChildren();
  for (cXMLElementList::iterator intConfigIt = intConfig.begin(); intConfigIt != intConfig.end(); intConfigIt++)
  {
    std::string nodeName = (*intConfigIt)->getTagName();
    if (nodeName == "Route")
    {
        IPv4Route *e = new IPv4Route();
        cXMLElementList ifDetails = (*intConfigIt)->getChildren();
        for (cXMLElementList::iterator ifElemIt = ifDetails.begin(); ifElemIt != ifDetails.end(); ifElemIt++)
        {
          std::string nodeName = (*ifElemIt)->getTagName();

          if (nodeName=="NetworkAddress")
          {
            e->setDestination(IPv4Address((*ifElemIt)->getNodeValue()));
            EV << "Address = " << e->getDestination() << endl;
          }

          if (nodeName=="NetworkMask")
          {
            e->setNetmask(IPv4Address((*ifElemIt)->getNodeValue()));
            EV << "NetworkMask = " << e->getNetmask() << endl;
          }

          if (nodeName=="NextHopAddress")
          {
            e->setGateway(IPv4Address((*ifElemIt)->getNodeValue()));
            InterfaceEntry *intf=NULL;
            for (int i=0; i<ift->getNumInterfaces(); i++)
            {
              intf = ift->getInterface(i);
              if (((intf->ipv4Data()->getIPAddress()).doAnd(intf->ipv4Data()->getNetmask()))==((e->getGateway()).doAnd(intf->ipv4Data()->getNetmask())))
                  break;

            }
            if (intf)
              e->setInterface(intf);
            else
              opp_error("Error.");
            e->setMetric(1);
          }
          if (nodeName=="ExitInterface")
          {
            InterfaceEntry *ie=ift->getInterfaceByName((*ifElemIt)->getNodeValue());
            if (!ie)
                opp_error("Interface does not exists");

            e->setInterface(ie);
            e->setGateway(IPv4Address::UNSPECIFIED_ADDRESS);
            e->setMetric(0);
          }
          if (nodeName=="StaticRouteMetric")
          {
            e->setMetric(atoi((*ifElemIt)->getNodeValue()));
          }
        }
        rt->addRoute(e);
    }

  }
}


//
//
//- configuration for RIPng
//
//
void DeviceConfigurator::loadRIPngConfig(RIPngRouting *RIPngModule){

    ASSERT(RIPngModule != NULL);

    // get access to device node from XML
    const char *deviceType = par("deviceType");
    const char *deviceId = par("deviceId");
    const char *configFile = par("configFile");
    cXMLElement *device = xmlParser::GetDevice(deviceType, deviceId, configFile);

    if (device == NULL){
        ev << "No configuration found for this device (" << deviceType << " id=" << deviceId << ")" << endl;
             return;
     }

    // interfaces config
    cXMLElement *interface;
    RIPng::Interface *ripngInterface;
    std::string RIPngInterfaceStatus;
    std::string RIPngInterfacePassiveStatus;
    std::string RIPngInterfaceSplitHorizon;
    std::string RIPngInterfacePoisonReverse;

      //get first router's interface
      interface = xmlParser::GetInterface(NULL, device);
      while (interface != NULL)
      {// process all interfaces in config file
          const char *interfaceName = interface->getAttribute("name");
          RIPngInterfaceStatus = xmlParser::getInterfaceRIPngStatus(interface);
          if (RIPngInterfaceStatus == "enable")
          {
              ripngInterface = RIPngModule->enableRIPngOnInterface(interfaceName);
              // add prefixes from int to the RIPng routing table
              loadPrefixesFromInterfaceToRIPngRT(RIPngModule, interface);

              RIPngInterfacePassiveStatus = xmlParser::getRIPngInterfacePassiveStatus(interface);
              if (RIPngInterfacePassiveStatus == "enable")
              {// set the interface as passive (interface is "active" by default)
                  RIPngModule->setInterfacePassiveStatus(ripngInterface, true);
              }

              RIPngInterfaceSplitHorizon = xmlParser::getRIPngInterfaceSplitHorizon(interface);
              if (RIPngInterfaceSplitHorizon == "disable")
              {// disable Split Horizon on the interface (Split Horizon is enabled by default)
                  RIPngModule->setInterfaceSplitHorizon(ripngInterface, false);
              }

              RIPngInterfacePoisonReverse = xmlParser::getRIPngInterfacePoisonReverse(interface);
              if (RIPngInterfacePoisonReverse == "enable")
              {// enable Poison Reverse on the interface (Poison Reverse is disabled by default)
                  RIPngModule->setInterfacePoisonReverse(ripngInterface, true);
              }
          }

          // process next interface
          interface = xmlParser::GetInterface(interface, NULL);
      }
}

void DeviceConfigurator::loadPrefixesFromInterfaceToRIPngRT(RIPngRouting *RIPngModule, cXMLElement *interface)
{
    const char *interfaceName = interface->getAttribute("name");
    InterfaceEntry *interfaceEntry = ift->getInterfaceByName(interfaceName);
    int interfaceId = interfaceEntry->getInterfaceId();

    RIPng::RoutingTableEntry *route;

    // process next interface
    cXMLElement *eIpv6address;
    std::string sIpv6address;
    IPv6Address ipv6address;
    int prefixLen;
        // get first ipv6 address
        eIpv6address = xmlParser::GetIPv6Address(NULL, interface);
        while (eIpv6address != NULL)
        {// process all ipv6 addresses on the interface
            sIpv6address = eIpv6address->getNodeValue();

            // check if it's a valid IPv6 address string with prefix and get prefix
            if (!ipv6address.tryParseAddrWithPrefix(sIpv6address.c_str(), prefixLen))
            {
               throw cRuntimeError("Unable to set IPv6 network address %s for static route", sIpv6address.c_str());
            }

            if (!ipv6address.isLinkLocal() && !ipv6address.isMulticast())
            {
                // make directly connected route
                route = new RIPng::RoutingTableEntry(ipv6address.getPrefix(prefixLen), prefixLen);
                route->setInterfaceId(interfaceId);
                route->setNextHop(IPv6Address::UNSPECIFIED_ADDRESS);  // means directly connected network
                route->setMetric(RIPngModule->getConnNetworkMetric());
                RIPngModule->addRoutingTableEntry(route, false);

                // directly connected routes do not need a RIPng route timer
            }

            eIpv6address = xmlParser::GetIPv6Address(eIpv6address, NULL);
        }
}

bool DeviceConfigurator::isMulticastEnabled(cXMLElement *device)
{
    // Routing element
    cXMLElement* routingNode = device->getElementByPath("Routing");
    if (routingNode == NULL)
         return false;

    // Multicast element
    cXMLElement* multicastNode = routingNode->getElementByPath("Multicast");
    if (multicastNode == NULL)
       return false;


    // Multicast has to be enabled
    const char* enableAtt = multicastNode->getAttribute("enable");
    if (strcmp(enableAtt, "1"))
        return false;

    return true;
}

void DeviceConfigurator::loadPimInterfaceConfig(cXMLElement *iface)
{
    // for each interface node
    while (iface != NULL)
    {
        // get PIM node
        cXMLElement* pimNode = iface->getElementByPath("Pim");
        if (pimNode == NULL)
          continue;

        // create new PIM interface
        PimInterface newentry;
        InterfaceEntry *interface = ift->getInterfaceByName(iface->getAttribute("name"));
        newentry.setInterfaceID(interface->getInterfaceId());
        newentry.setInterfacePtr(interface);

        // get PIM mode for interface
        cXMLElement* pimMode = pimNode->getElementByPath("Mode");
        if (pimMode == NULL)
          continue;

        const char *mode = pimMode->getNodeValue();
        //EV << "PimSplitter::PIM interface mode = "<< mode << endl;
        if (!strcmp(mode, "dense-mode"))
          newentry.setMode(Dense);
        else if (!strcmp(mode, "sparse-mode"))
          newentry.setMode(Sparse);
        else
          continue;

        // get PIM mode for interface
        cXMLElement* stateRefreshMode = pimNode->getElementByPath("StateRefresh");
        if (stateRefreshMode != NULL)
        {
            EV << "Nasel State Refresh" << endl;
            // will router send state refresh msgs?
            cXMLElement* origMode = stateRefreshMode->getElementByPath("OriginationInterval");
            if (origMode != NULL)
            {
                EV << "Nasel Orig" << endl;
                newentry.setSR(true);
            }
        }

        // register pim multicast address 224.0.0.13 (all PIM routers) on Pim interface
        std::vector<IPv4Address> intMulticastAddresses;

        cXMLElement* IPaddress = iface->getElementByPath("IPAddress");                  //Register 226.1.1.1 to R2 router
        std::string intfToRegister = IPaddress->getNodeValue();

        if (intfToRegister == "192.168.12.2" || intfToRegister == "192.168.22.2")
                interface->ipv4Data()->addMulticastListener(IPv4Address("226.1.1.1"));

        interface->ipv4Data()->addMulticastListener(IPv4Address("224.0.0.13"));
        intMulticastAddresses = interface->ipv4Data()->getReportedMulticastGroups();

        for(int i = 0; i < (intMulticastAddresses.size()); i++)
            EV << intMulticastAddresses[i] << ", ";
        EV << endl;

        newentry.setIntMulticastAddresses(newentry.deleteLocalIPs(intMulticastAddresses));

        // add new entry to pim interfaces table
        pimIft->addInterface(newentry);

        // get next interface
        iface = xmlParser::GetInterface(iface, NULL);
    }
}
