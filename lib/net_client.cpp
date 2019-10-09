#include "net_client.hpp"

#include "app.hpp"
#include "message.hpp"
#include "net_handler.hpp"
#include "uv_socket.hpp"

#include <uv.h>

#include <cassert>
#include <iostream>
#include <sstream>

using namespace sample;
using namespace std;


NetClientBase::NetClientBase(BaseApp* app_in, string const & nodeAddr_in) :
myApp(app_in),
myNodeAddr(nodeAddr_in),
myState(State::NotConnected)
{
}

NetClientBase::~NetClientBase()
{
    //cout << "~NetClientBase " << myNodeAddr << endl;
}

void NetClientBase::setUvStream(uv_tcp_t* stream_in)
{
    myUvStream = stream_in;
    myUvStream->data = (void*)dynamic_cast<IUvSocket*>(this);
}

int NetClientBase::sendMessage(BaseMessage const & msg_in)
{
    //cout << "NetClientBase::sendMessage " << msg_in.toString() << endl;
    if (myState == State::Closing || myState == State::Closed)
    {
        return 0;
    }
    myState = State::Sending;
    SerializerMessageVisitor visitor;
    msg_in.visit(visitor);
    string msg = visitor.getMessage();
    //cout << "sendMessage " << msg.length() << " '" << msg << "'" << endl;
    msg += '\n'; // terminator
    // convert to byte array
    vector<uint8_t> binmsg(msg.begin(), msg.end());

    uv_write_t* req = new uv_write_t();
    // wrap buffers into a UvWriteRequest object
    UvWriteRequest* wrreq = new UvWriteRequest(dynamic_cast<IUvSocket*>(this), 1);
    wrreq->add(binmsg);
    req->data = (void*)wrreq;
    int res = ::uv_write(req, (uv_stream_t*)myUvStream, &(wrreq->bufs[0]), wrreq->nbuf, NetClientBase::on_write);
    if (res)
    {
        if (res == -EBADF)
        {
            // socket closed
        }
        else
        {
            cerr << "Error from uv_write " << res << " " << ::uv_err_name(res) << endl;
        }
        close();
    }
}

void NetClientBase::on_close(uv_handle_t* handle)
{
    //cout << "on_close" << endl;
    IUvSocket* uvSocket = (IUvSocket*)handle->data;
    if (uvSocket == nullptr)
    {
        cerr << "Fatal error: uvSocket is nullptr " << endl;
        return;
    }
    uvSocket->onClose(handle);
}

void NetClientBase::onClose(uv_handle_t* handle)
{
    //cout << "onClose" << endl;
    if (myApp != nullptr)
    {
        myApp->connectionClosed(this);
    }
    if (handle != NULL)
    {
        delete handle;
    }
    myState = State::Closed;
}

int NetClientBase::close()
{
    //cout << "NetClientBase::close " << getNodeAddr() << endl;
    myState = State::Closing;
    uv_handle_t* handle = (uv_handle_t*)myUvStream;
    if (handle == nullptr) return 0;
    myUvStream = nullptr; // prevent double close
    if (::uv_is_closing(handle))
    {
        // already closing
        cerr << "Warning: Socket is already closing " << getNodeAddr() << endl;
        onClose(handle);
        return 0;
    }
    handle->data = (void*)dynamic_cast<IUvSocket*>(this);
    ::uv_close(handle, NetClientBase::on_close);
    //cout << "NetClientBase::close closed" << endl;
}

void NetClientBase::on_write(uv_write_t* req, int status) 
{
    //cout << "on_write " << status << endl;
    UvWriteRequest* wrreq = (UvWriteRequest*)req->data;
    if (wrreq == nullptr)
    {
        cerr << "Fatal error: uv_write_t->data is nullptr " << endl;
        //uv_close((uv_handle_t*)req->handle, NULL);
        return;
    }
    IUvSocket* uvSocket = wrreq->uvSocket;
    if (uvSocket == nullptr)
    {
        cerr << "Fatal error: uvSocket is nullptr " << endl;
        //uv_close((uv_handle_t*)req->handle, NULL);
        return;
    }
    uvSocket->onWrite(req, status);
    delete wrreq;
    delete req;
}

void NetClientBase::onWrite(uv_write_t* req, int status) 
{
    //cout << "NetClientBase::onWrite " << status << " "  << myState << endl;
    assert(myState == State::Sending || myState == State::Receiving || myState == State::Received);
    if (status != 0) 
    {
        cerr << "write error " << status << " " << ::uv_strerror(status) << endl;
        //uv_close((uv_handle_t*) req->handle, NULL);
        close();
        return;
    }
    process();
}

void NetClientBase::doProcessReceivedBuffer()
{
    if (myReceiveBuffer.empty())
    {
        return;
    }
    int terminatorIdx;
    while ((myReceiveBuffer.length() > 0) && ((terminatorIdx = myReceiveBuffer.find('\n')) >= 0))
    {
        string msg1 = myReceiveBuffer.substr(0, terminatorIdx); // without the terminator
        myReceiveBuffer = myReceiveBuffer.substr(terminatorIdx + 1);
        //cout << "Incoming message: from " << myNodeAddr << " '" << msg1 << "' " << myReceiveBuffer.length() << endl;
        // split into tokens
        std::vector<std::string> tokens; // Create vector to hold our words
        {
            std::string buf;                 // Have a buffer string
            std::stringstream ss(msg1);       // Insert the string into a stream
            while (ss >> buf) tokens.push_back(buf);
        }
        BaseMessage* msg = MessageDeserializer::parseMessage(tokens);
        if (msg == nullptr)
        {
            cerr << "Error: Unparseable message '" << msg1 << "' " << tokens.size() << endl;
            continue;
        }
        myState = State::Received;
        assert(myApp != nullptr);
        myApp->messageReceived(*this, *msg);
        delete msg;
    }
}

void NetClientBase::on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf)
{
    //cout << "on_read " << nread << endl; // << " " << (long)buf << " " << (long)buf->base << endl;
    assert(stream != NULL);
    IUvSocket* uvSocket = (IUvSocket*)(stream->data);
    if (uvSocket == nullptr)
    {
        cerr << "Fatal error: uvSocket is nullptr " << endl;
        //uv_close((uv_handle_t*)stream, NULL);
        //delete stream;
        return;
    }
    //cerr << (long)uvSocket << " " << buf->base[0] << endl;
    uvSocket->onRead(stream, nread, buf);
}

void NetClientBase::alloc_buffer(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf)
{
    //cerr << "alloc_buffer " << suggested_size << endl;
    size_t s = std::min(suggested_size, (size_t)16384);
    buf->base = new char[s];
    buf->len = s;
}

void NetClientBase::onRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf)
{
    //cout << "onRead " << nread << endl;
    if (nread < 0)
    {
        string errtxt = ::uv_strerror(nread);
        if (errtxt == "end of file")
        {
            // ...
        }
        else
        {
            cerr << "Read error " << errtxt << " " << nread << " pending " << myReceiveBuffer.length() << endl;
        }
        // close socket
        close();
        //delete stream;
        return;
    }
    if (nread == 0)
    {
        cerr << "Socket closed while reading " << ::uv_strerror(nread) << "  pending " << myReceiveBuffer.length() << endl;
        close();
        //delete stream;
        return;
    }
    if (buf != nullptr && buf->base != nullptr)
    {
        myReceiveBuffer.append(string(buf->base, nread));
        //cerr << "ReceiveBuffer increased to " << myReceiveBuffer.length() << endl;
        doProcessReceivedBuffer();
    }
    //delete stream;

    process();
}

int NetClientBase::doRead()
{
    // communicate with client, process one message
    //cout << "doRead " << myState << endl;
    assert(myState == State::Accepted || myState == State::Sent || myState == State::Sending || myState == State::Receiving);
    myState = State::Receiving;
    //cout << "doRead " << endl; //(long)((IUvSocket*)this) << " " << (long)((NetClientBase*)this) << " " << (long)((NetClientIn*)this) << endl;
    //myReceiveBuffer.clear();
    //static const int buflen = 256;
    //char buffer[buflen];
    ((uv_stream_t*)myUvStream)->data = (void*)dynamic_cast<IUvSocket*>(this);
    int res = ::uv_read_start((uv_stream_t*)myUvStream, NetClientBase::alloc_buffer, NetClientBase::on_read);
    if (res < 0)
    {
        cerr << "Error from uv_read_start() " << res << " "<< ::uv_err_name(res) << endl;
        close();
        return res;
    }
    return 0;
}

bool NetClientBase::isConnected() const
{
    if (myUvStream == nullptr) return false;
    if (myState == State::Undefined || myState == State::NotConnected || myState == State::Connecting || myState == State::Closing || myState == State::Closed) return false;
    uv_os_fd_t fd;
    if (::uv_fileno((uv_handle_t*)myUvStream, &fd)) return false;
    if (fd <= 0) return false;
    return true;
}

NetClientIn::NetClientIn(ServerApp* app_in, uv_tcp_t* socket_in, string const & nodeAddr_in) :
NetClientBase(app_in, nodeAddr_in)
{
    setUvStream(socket_in);
    myState = State::Accepted;
}


NetClientOut::NetClientOut(BaseApp* app_in, string const & host_in, int port_in, int pingToSend_in) :
NetClientBase(app_in, host_in + ":" + to_string(port_in)),
myHost(host_in),
myPort(port_in),
myPingToSend(pingToSend_in),
mySendCounter(0)
{
}

void NetClientOut::on_connect(uv_connect_t* req, int status)
{
    //cout << "on_connect " << status << " " << req->type << endl;
    IUvSocket* uvSocket = (IUvSocket*)req->data;
    if (uvSocket == nullptr)
    {
        cerr << "Fatal error: uvSocket is nullptr " << endl;
        //uv_close((uv_handle_t*)req->handle, NULL);
        return;
    }
    uvSocket->onConnect(req, status);
}

void NetClientOut::onConnect(uv_connect_t* req, int status)
{
    //cout << "onConnect " << status << " " << req->type << endl;
    if (status != 0) 
    {
        cerr << "connect error " << myHost << ":" << myPort << " " << status << " " << ::uv_strerror(status) << endl;
        //uv_close((uv_handle_t*) req->handle, NULL);
        return;
    }
    myState = State::Connected;
    cerr << "Connected to " << myHost << ":" << myPort << endl;
    process();
}

int NetClientOut::connect()
{
    //cout << "NetClientOut::connect " << myHost << ":" << myPort << endl;
    if (myState >= State::Connected && myState < Closed)
    {
        cerr << "Fatal error: Connect on connected connection " << myState << endl;
        return -1;
    }
    myState = State::Connecting;
    mySendCounter = 0;
    uv_tcp_t* socket = new uv_tcp_t();
    ::uv_tcp_init(NetHandler::getUvLoop(), socket);
    setUvStream(socket);

    struct sockaddr_in dest;
    ::uv_ip4_addr(myHost.c_str(), myPort, &dest);

    uv_connect_t* connreq = new uv_connect_t();
    connreq->data = (void*)dynamic_cast<IUvSocket*>(this);
    //cout << "connecting..." << endl;
    int res = ::uv_tcp_connect(connreq, socket, (const struct sockaddr*)&dest, NetClientOut::on_connect);
    if (res)
    {
        cerr << "Error from uv_tcp_connect() " << res << " " << ::uv_err_name(res) << endl;
        return res;
    }
    return 0;
}

void NetClientOut::process()
{
    //cout << "NetClientOut::process " << myState << endl;
    switch (myState)
    {
        case State::Connected:
            {
                mySendCounter = 0;
                HandshakeMessage msg("V01", getNodeAddr(), myApp->getName());
                sendMessage(msg);
            }
            break;

        case State::Sending:
            ++mySendCounter;
            doRead();
            break;

        case State::Sent:
            doRead();
            break;

        case State::Receiving:
            doRead();
            break;

        case State::Received:
            if (mySendCounter >= 1 + myPingToSend)
            {
                close();
            }
            else if (mySendCounter >= 1)
            {
                PingMessage msg("Ping_" + getNodeAddr() + "_" + to_string(mySendCounter));
                sendMessage(msg);
            }
            break;

        default:
            cerr << "Fatal error: unhandled state " << myState << endl;
            assert(false);
            break;
    }
    return;
}