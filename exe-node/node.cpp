#include "node.hpp"
#include "peer_conn.hpp"
#include "../lib/net_handler.hpp"
#include "../lib/net_client.hpp"

#include <cassert>
#include <iostream>

using namespace sample;
using namespace std;


NodeApp::PeerInfo::PeerInfo(string endpoint_in) :
myEndpoint(endpoint_in),
myOutClient(nullptr),
myInClient(nullptr),
myOutHost(""),
myOutPort(0),
myOutFlag(false),
myStickyOutFlag(false),
myOutConnTryCount(0)
{
}

NodeApp::PeerInfo::PeerInfo(std::string outHost_in, int outPort_in, bool sticky_in) :
myEndpoint(outHost_in + ":" + to_string(outPort_in)),
myOutClient(nullptr),
myInClient(nullptr),
myOutHost(outHost_in),
myOutPort(outPort_in),
myOutFlag(true),
myStickyOutFlag(sticky_in),
myOutConnTryCount(0)
{
}

void NodeApp::PeerInfo::setOutClient(std::shared_ptr<PeerClientOut>& outClient_in)
{
    myOutClient = outClient_in;
}

void NodeApp::PeerInfo::setInClient(std::shared_ptr<NetClientIn>& inClient_in)
{
    myInClient = inClient_in;
}

void NodeApp::PeerInfo::resetOutClient()
{
    myOutClient = nullptr;
}

void NodeApp::PeerInfo::resetInClient()
{
    myOutClient = nullptr;
}


NodeApp::NodeApp() :
BaseApp()
{
    myNetHandler = new NetHandler(this);
}

void NodeApp::start()
{
    int actualPort = myNetHandler->startWithListen(5000, 10);
    if (actualPort <= 0) return;
    myName = ":" + to_string(actualPort);
}

void NodeApp::listenStarted(int port)
{
    cout << "App: Listening on port " << port << endl;

    // add sticky bootstrap peers
    int n = 2;
    // try to connect to clients
    for (int i = 0; i < n; ++i)
    {
        int port1 = 5000 + i;
        //clis[i] = nullptr;
        // skip connection to self
        if (port != port1)
        {
            addOutPeer("127.0.0.1", port1, true);
        }
    }
    tryOutConnections();
}


void NodeApp::addOutPeer(std::string host_in, int port_in, bool sticky_in)
{
    PeerInfo peer = PeerInfo(host_in, port_in, sticky_in);
    if (myPeers.find(peer.myEndpoint) != myPeers.end())
    {
        // already present
        return;
    }
    myPeers[peer.myEndpoint] = peer;
    cout << "App: Added sticky peer " << peer.myEndpoint << " " << myPeers.size() << endl;
}

void NodeApp::tryOutConnections()
{
    //cout << "NodeApp::tryOutConnections" << endl;
    // try to connect to clients
    for (auto i = myPeers.begin(); i != myPeers.end(); ++i)
    {
        //cout << i->second.myOutFlag << " " << i->second.myStickyOutFlag << " " << i->second.myOutConnTryCount << " " << (i->second.myOutClient == nullptr) << " " << i->second.myOutHost << ":" << i->second.myOutPort << endl;
        if (i->second.myOutFlag)
        {
            if (i->second.myOutConnTryCount == 0 || i->second.myStickyOutFlag)
            {
                // try outgoing connection
                //cout << "Trying out conn to " << i->second.myOutHost << ":" << i->second.myOutPort << endl;
                if (i->second.myOutClient == nullptr)
                {
                    auto peerout = make_shared<PeerClientOut>(this, i->second.myOutHost, i->second.myOutPort);
                    i->second.setOutClient(peerout);
                }
                //cout << "isConnected " << i->second.myOutClient->isConnected() << endl;
                if (!i->second.myOutClient->isConnected())
                {
                    ++i->second.myOutConnTryCount;
                    int res = i->second.myOutClient->connect();
                    if (res)
                    {
                        // error
                    }
                }
            }
        }
    }
}

void NodeApp::stop()
{
    myNetHandler->stop();
}

void NodeApp::inConnectionReceived(std::shared_ptr<NetClientBase>& client_in)
{
    assert(client_in != nullptr);
    string cliaddr = client_in->getNodeAddr();
    cout << "App: New incoming connection: " << cliaddr << endl;
    if (myPeers.find(cliaddr) == myPeers.end())
    {
        myPeers[cliaddr] = PeerInfo(cliaddr);
    }
    auto cliIn = dynamic_pointer_cast<NetClientIn>(client_in);
    myPeers[cliaddr].setInClient(cliIn);
}

void NodeApp::connectionClosed(NetClientBase* client_in)
{
    assert(client_in != nullptr);
    string cliaddr = client_in->getNodeAddr();
    cout << "App: Connection done: " << cliaddr << endl;
    for(auto i = myPeers.begin(); i != myPeers.end(); ++i)
    {
        if (i->second.myInClient.get() == client_in)
        {
            i->second.resetInClient();
        }
        if (i->second.myOutClient.get() == client_in)
        {
            i->second.resetOutClient();
        }
    }
    // remove disconnected clients
    bool changed = true;
    while (changed)
    {
        changed = false;
        for(auto i = myPeers.begin(); i != myPeers.end(); ++i)
        {
            if (i->second.myOutClient == nullptr && i->second.myInClient == nullptr && !i->second.myStickyOutFlag)
            {
                cout << "Removing disconnected client " << myPeers.size() << " " << i->first << endl;
                myPeers.erase(i->first);
                //cout << "Removed disconnected client " << myPeers.size() << " " << i->first << endl;
                changed = true;
                break; // for
            }
        }
    } 
}

void NodeApp::messageReceived(NetClientBase & client_in, BaseMessage const & msg_in)
{
    if (msg_in.getType() != MessageType::OtherPeer)
    {
        cout << "App: Received: from " << client_in.getNodeAddr() << " '" << msg_in.toString() << "'" << endl;
    }
    switch (msg_in.getType())
    {
        case MessageType::Handshake:
            {
                HandshakeMessage const & hsMsg = dynamic_cast<HandshakeMessage const &>(msg_in);
                //cout << "Handshake message received, '" << hsMsg.getMyAddr() << "'" << endl;
                if (hsMsg.getMyVersion() != "V01")
                {
                    cerr << "Wrong version ''" << hsMsg.getMyVersion() << "'" << endl;
                    client_in.close();
                    return;
                }
                HandshakeResponseMessage resp("V01", myName, client_in.getNodeAddr());
                client_in.sendMessage(resp);
                // try to connect ougoing too, host taken from actual remote peer, port reported by peer
                string reportedPeerName = hsMsg.getMyAddr();
                if (reportedPeerName.substr(0, 1) == ":")
                {
                    string port = reportedPeerName.substr(1);
                    string host = client_in.getNodeAddr();
                    int idx = host.find(':');
                    if (idx >= 0) host = host.substr(0, idx);
                    addOutPeer(host, stoi(port), false);
                }
                tryOutConnections();
            }
            break;

        case MessageType::Ping:
            {
                PingMessage const & pingMsg = dynamic_cast<PingMessage const &>(msg_in);
                //cout << "Ping message received, '" << pingMsg.getText() << "'" << endl;
                PingResponseMessage resp("Resp_to_" + pingMsg.getText() + "_from_" + myName);
                client_in.sendMessage(resp);
            }
            break;

        case MessageType::OtherPeer:
            {
                OtherPeerMessage const & peerMsg = dynamic_cast<OtherPeerMessage const &>(msg_in);
                //cout << "OtherPeer message received, " << peerMsg.getHost() << ":" << peerMsg.getPort() << " " << peerMsg.toString() << endl;
                addOutPeer(peerMsg.getHost(), peerMsg.getPort(), false);
                tryOutConnections();
            }
            break;

        case MessageType::HandshakeResponse:
        case MessageType::PingResponse:
            // OK, noop
            break;

        default:
            assert(false);
    }
}

vector<NodeApp::EndPoint> NodeApp::getOutPeers() const
{
    vector<EndPoint> peers;
    for(auto i = myPeers.begin(); i != myPeers.end(); ++i)
    {
        if (i->second.myOutFlag && i->second.myOutClient != nullptr)
        {
            peers.push_back(EndPoint { i->second.myOutHost, i->second.myOutPort });
        }
    }
    return peers;
}


int main()
{
    cout << "TCP LibUV Node" << endl;

    NodeApp app;
    app.start();

    cout << "Press Enter to exit ...";
    cin.get();
    cout << endl;

    app.stop();
    cout << "Done." << endl;
}
