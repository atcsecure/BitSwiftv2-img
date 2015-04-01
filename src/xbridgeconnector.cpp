//******************************************************************************
//******************************************************************************

#include "xbridgeconnector.h"
#include "base58.h"

#include <boost/foreach.hpp>
#include <boost/thread.hpp>
#include <boost/algorithm/string.hpp>

// TODO remove this
#include <QtDebug>

//******************************************************************************
//******************************************************************************
std::vector<std::string> getLocalBitcoinAddresses();

//******************************************************************************
//******************************************************************************
XBridgeConnector & xbridge()
{
    static XBridgeConnector connector;
    return connector;
}

//******************************************************************************
//******************************************************************************
XBridgeConnector::XBridgeConnector()
    : m_socket(m_io)
    , m_timer(m_io, boost::posix_time::seconds(TIMER_INTERVAL))
{
    m_processors[xbcInvalid]          .bind(this, &XBridgeConnector::processInvalid);
    m_processors[xbcXChatMessage]     .bind(this, &XBridgeConnector::processXChatMessage);

    m_processors[xbcExchangeWallets]  .bind(this, &XBridgeConnector::processExchangeWallets);
}

//******************************************************************************
//******************************************************************************
void XBridgeConnector::run()
{
    m_timer.async_wait(boost::bind(&XBridgeConnector::onTimer, this));

    m_io.run();
}

//*****************************************************************************
//*****************************************************************************
void XBridgeConnector::disconnect()
{
    m_socket.close();
}

//*****************************************************************************
//*****************************************************************************
void XBridgeConnector::onTimer()
{
    if (!m_socket.is_open())
    {
        // connect to localhost
        boost::asio::ip::tcp::endpoint ep(boost::asio::ip::address::from_string("127.0.0.1"), SERVER_LISTEN_PORT);

        boost::system::error_code error;
        m_socket.connect(ep, error);
        if (!error)
        {
            doReadHeader(XBridgePacketPtr(new XBridgePacket));
        }
        else
        {
            disconnect();
        }

    }

    if (m_socket.is_open())
    {
        // send local addresses
        announceLocalAddresses();

        // send pending transactions
        for (std::map<uint256, XBridgePacketPtr>::iterator i = m_pendingTransactions.begin();
             i != m_pendingTransactions.end(); ++i)
        {
            boost::system::error_code error;
            m_socket.send(boost::asio::buffer(i->second->header(), i->second->allSize()), 0, error);
            if (error)
            {
                qDebug() << "send transaction error <"
                         << error.value() << "> " << error.message().c_str()
                         << " " << __FUNCTION__;
            }
        }
    }


    // next
    m_timer.expires_at(m_timer.expires_at() + boost::posix_time::seconds(TIMER_INTERVAL));
    m_timer.async_wait(boost::bind(&XBridgeConnector::onTimer, this));
}

//*****************************************************************************
//*****************************************************************************
void XBridgeConnector::doReadHeader(XBridgePacketPtr packet,
                                  const std::size_t offset)
{
    m_socket.async_read_some(
                boost::asio::buffer(packet->header()+offset,
                                    packet->headerSize-offset),
                boost::bind(&XBridgeConnector::onReadHeader,
                            this,
                            packet, offset,
                            boost::asio::placeholders::error,
                            boost::asio::placeholders::bytes_transferred));
}

//*****************************************************************************
//*****************************************************************************
void XBridgeConnector::onReadHeader(XBridgePacketPtr packet,
                                  const std::size_t offset,
                                  const boost::system::error_code & error,
                                  std::size_t transferred)
{
    if (error)
    {
        qDebug() << "ERROR <" << error.value() << "> " << error.message().c_str();
        disconnect();
        return;
    }

    if (offset + transferred != packet->headerSize)
    {
        qDebug() << "partially read header, read " << transferred
                 << " of " << packet->headerSize << " bytes";

        doReadHeader(packet, offset + transferred);
        return;
    }

    packet->alloc();
    doReadBody(packet);
}

//*****************************************************************************
//*****************************************************************************
void XBridgeConnector::doReadBody(XBridgePacketPtr packet,
                const std::size_t offset)
{
    m_socket.async_read_some(
                boost::asio::buffer(packet->data()+offset,
                                    packet->size()-offset),
                boost::bind(&XBridgeConnector::onReadBody,
                            this,
                            packet, offset,
                            boost::asio::placeholders::error,
                            boost::asio::placeholders::bytes_transferred));
}

//*****************************************************************************
//*****************************************************************************
void XBridgeConnector::onReadBody(XBridgePacketPtr packet,
                                const std::size_t offset,
                                const boost::system::error_code & error,
                                std::size_t transferred = 0)
{
    if (error)
    {
        qDebug() << "ERROR <" << error.value() << "> " << error.message().c_str();
        disconnect();
        return;
    }

    if (offset + transferred != packet->size())
    {
        qDebug() << "partially read packet, read " << transferred
                 << " of " << packet->size() << " bytes";

        doReadBody(packet, offset + transferred);
        return;
    }

    if (!decryptPacket(packet))
    {
        qDebug() << "packet decoding error " << __FUNCTION__;
        return;
    }

    if (!processPacket(packet))
    {
        qDebug() << "packet processing error " << __FUNCTION__;
    }

    doReadHeader(XBridgePacketPtr(new XBridgePacket));
}

//*****************************************************************************
//*****************************************************************************
bool XBridgeConnector::encryptPacket(XBridgePacketPtr /*packet*/)
{
    // TODO implement this
    return true;
}

//*****************************************************************************
//*****************************************************************************
bool XBridgeConnector::decryptPacket(XBridgePacketPtr /*packet*/)
{
    // TODO implement this
    return true;
}

//*****************************************************************************
//*****************************************************************************
bool XBridgeConnector::processPacket(XBridgePacketPtr packet)
{
    XBridgeCommand c = packet->command();

    if (m_processors.count(c) == 0)
    {
        m_processors[xbcInvalid](packet);
        qDebug() << "incorrect command code <" << c << "> ";
        return false;
    }

    if (!m_processors[c](packet))
    {
        qDebug() << "packet processing error <" << c << "> ";
        return false;
    }

    return true;
}

//******************************************************************************
//******************************************************************************
bool XBridgeConnector::connect()
{
    // start internal handlers
    m_thread = boost::thread(&XBridgeConnector::run, this);

    return true;
}

//******************************************************************************
//******************************************************************************
bool XBridgeConnector::isConnected() const
{
    return m_socket.is_open();
}

//*****************************************************************************
//*****************************************************************************
bool XBridgeConnector::processInvalid(XBridgePacketPtr /*packet*/)
{
    qDebug() << "xbcInvalid command processed";
    return true;
}

//*****************************************************************************
//*****************************************************************************
bool XBridgeConnector::processXChatMessage(XBridgePacketPtr packet)
{
    // size must be > 20 bytes (160bit)
    if (packet->size() <= 20)
    {
        qDebug() << "invalid packet size for xbcXChatMessage " << __FUNCTION__;
        return false;
    }

    // skip 20 bytes dest address
    CDataStream stream((const char *)(packet->data()+20),
                       (const char *)(packet->data()+packet->size()));

    Message m;
    stream >> m;

    bool isForMe = false;
    if (!m.process(isForMe))
    {
        // TODO need relay?
        // relay, if message not for me
        // m.broadcast();
    }

    return true;
}

//******************************************************************************
//******************************************************************************
bool XBridgeConnector::announceLocalAddresses()
{
    if (!isConnected())
    {
        return false;
    }

    std::vector<std::string> addresses = getLocalBitcoinAddresses();

    BOOST_FOREACH(const std::string & addr, addresses)
    {
        std::vector<unsigned char> tmp;
        DecodeBase58Check(addr, tmp);
        if (tmp.empty())
        {
            continue;
        }

        // size of tmp must be 21 byte
        XBridgePacket p(xbcAnnounceAddresses);
        p.setData(&tmp[1], tmp.size()-1);

        // TODO encryption
//        if (!encryptPacket(p))
//        {
//            // TODO logs or signal to gui
//            return false;
//        }

        boost::system::error_code error;
        m_socket.send(boost::asio::buffer(p.header(), p.allSize()), 0, error);
        if (error)
        {
            qDebug() << "send address error <"
                     << error.value() << "> " << error.message().c_str()
                     << " " << __FUNCTION__;
        }
    }

    return true;
}

//******************************************************************************
//******************************************************************************
bool XBridgeConnector::sendXChatMessage(const Message & m)
{
    CDataStream stream;
    stream << m;

    std::vector<unsigned char> addr;
    DecodeBase58Check(m.to, addr);
    if (addr.empty())
    {
        // incorrect address
        return false;
    }

    XBridgePacket p(xbcXChatMessage);

    // copy dest address
    assert(addr.size() ==  21 || "address length must be 20 bytes + 1");
    p.setData(&addr[1], addr.size()-1);

    // copy message
    std::vector<unsigned char> message;
    std::copy(stream.begin(), stream.end(), std::back_inserter(message));
    p.setData(message, 20);

    // TODO encryption
//        if (!encryptPacket(p))
//        {
//            // TODO logs or signal to gui
//            return false;
//        }

    boost::system::error_code error;
    m_socket.send(boost::asio::buffer(p.header(), p.allSize()), 0, error);
    if (error)
    {
        qDebug() << "send address error <"
                 << error.value() << "> " << error.message().c_str()
                 << " " << __FUNCTION__;
        return false;
    }

    return true;
}

//******************************************************************************
//******************************************************************************
uint256 XBridgeConnector::sendXBridgeTransaction(const std::vector<unsigned char> & from,
                                                 const std::string & fromCurrency,
                                                 const boost::uint32_t fromAmount,
                                                 const std::vector<unsigned char> & to,
                                                 const std::string & toCurrency,
                                                 const boost::uint32_t toAmount)
{
    if (fromCurrency.size() > 8 || toCurrency.size() > 8)
    {
        assert(false || "invalid currency");
        return uint256();
    }

    uint256 id = Hash(from.begin(), from.end(),
                      fromCurrency.begin(), fromCurrency.end(),
                      BEGIN(fromAmount), END(fromAmount),
                      to.begin(), to.end(),
                      toCurrency.begin(), toCurrency.end(),
                      BEGIN(toAmount), END(toAmount));

    std::vector<unsigned char> fc(8, 0);
    std::copy(fromCurrency.begin(), fromCurrency.end(), fc.begin());

    std::vector<unsigned char> tc(8, 0);
    std::copy(toCurrency.begin(), toCurrency.end(), tc.begin());

    XBridgePacketPtr packet(new XBridgePacket(xbcTransaction));
    // 20 bytes - id of transaction
    // 2x
    // 20 bytes - address
    //  8 bytes - currency
    //  4 bytes - amount
    packet->append(id.begin(), 20);
    packet->append(from);
    packet->append(fc);
    packet->append(fromAmount);
    packet->append(to);
    packet->append(tc);
    packet->append(toAmount);

    m_pendingTransactions[id] = packet;

    return id;
}

//******************************************************************************
//******************************************************************************
bool XBridgeConnector::transactionReceived(const uint256 & hash)
{
    XBridgePacket p(xbcReceivedTransaction);
    p.setData(hash.GetHex());

    boost::system::error_code error;
    m_socket.send(boost::asio::buffer(p.header(), p.allSize()), 0, error);
    if (error)
    {
        qDebug() << "send transaction hash error <"
                 << error.value() << "> " << error.message().c_str()
                 << " " << __FUNCTION__;
        return false;
    }

    return true;
}

//******************************************************************************
//******************************************************************************
bool XBridgeConnector::processExchangeWallets(XBridgePacketPtr packet)
{
    std::string packetData((char *)packet->data(), packet->size());
    std::vector<std::string> strs;
    boost::split(strs, packetData, boost::is_any_of("|"));

    if (strs.size() % 2)
    {
        qDebug() << "incorrect count of strings for xbcExchangeWallets" << __FUNCTION__;
        return false;
    }

    for (std::vector<std::string>::iterator i = strs.begin(); i != strs.end(); ++i)
    {
        std::string wallet = *i;
        std::string title = *(++i);
        qDebug() << "wallet " << wallet.c_str() << " " << title.c_str();
    }

    return true;
}
