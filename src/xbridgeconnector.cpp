//******************************************************************************
//******************************************************************************

#include "xbridgeconnector.h"
#include "base58.h"
#include "init.h"
#include "util.h"
#include "ui_interface.h"

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
    : m_ip("127.0.0.1")
    , m_port(SERVER_LISTEN_PORT)
    , m_socket(m_io)
    , m_timer(m_io, boost::posix_time::seconds(TIMER_INTERVAL))
{
    std::string addr = GetArg("-xbridge-address", "");
    if (addr.size())
    {
        int pos = addr.find(':');
        if (pos == -1)
        {
            m_ip = addr;
        }
        else
        {
            m_ip = addr.substr(0, pos);
            addr = addr.substr(pos+1, addr.length()-pos-1);
            int port = atoi(addr);
            if (port)
            {
                m_port = port;
            }
        }
    }

    m_processors[xbcInvalid]          .bind(this, &XBridgeConnector::processInvalid);
    m_processors[xbcXChatMessage]     .bind(this, &XBridgeConnector::processXChatMessage);

    m_processors[xbcExchangeWallets]  .bind(this, &XBridgeConnector::processExchangeWallets);

    // transactions
    m_processors[xbcTransactionHold]    .bind(this, &XBridgeConnector::processTransactionHold);
    m_processors[xbcTransactionPay]     .bind(this, &XBridgeConnector::processTransactionPay);
    m_processors[xbcTransactionCommit]  .bind(this, &XBridgeConnector::processTransactionCommit);
    m_processors[xbcTransactionFinished].bind(this, &XBridgeConnector::processTransactionFinished);
    m_processors[xbcTransactionDropped] .bind(this, &XBridgeConnector::processTransactionDropped);
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
        boost::asio::ip::tcp::endpoint ep(boost::asio::ip::address::from_string(m_ip), m_port);

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
    }

    if (m_socket.is_open())
    {
        if (m_txLocker.try_lock())
        {
            // send pending transactions
            for (std::map<uint256, XBridgeTransactionPtr>::iterator i = m_pendingTransactions.begin();
                 i != m_pendingTransactions.end(); ++i)
            {
                XBridgeTransactionPtr ptr = i->second;
                if (!ptr->packet)
                {
                    ptr->packet.reset(new XBridgePacket(xbcTransaction));

                    // field length must be 8 bytes
                    std::vector<unsigned char> fc(8, 0);
                    std::copy(ptr->fromCurrency.begin(), ptr->fromCurrency.end(), fc.begin());

                    // field length must be 8 bytes
                    std::vector<unsigned char> tc(8, 0);
                    std::copy(ptr->toCurrency.begin(), ptr->toCurrency.end(), tc.begin());

                    // 20 bytes - id of transaction
                    // 2x
                    // 20 bytes - address
                    //  8 bytes - currency
                    //  4 bytes - amount
                    ptr->packet->append(ptr->id.begin(), 32);
                    ptr->packet->append(ptr->from);
                    ptr->packet->append(fc);
                    ptr->packet->append(ptr->fromAmount);
                    ptr->packet->append(ptr->to);
                    ptr->packet->append(tc);
                    ptr->packet->append(ptr->toAmount);
                }

                if (!sendPacket(*(ptr->packet)))
                {
                    qDebug() << "send transaction error " << __FUNCTION__;
                }
            }

            m_txLocker.unlock();
        }
    }

    if (m_socket.is_open())
    {
        // send received transactions
        std::set<uint256> tmp = m_receivedTransactions;
        m_receivedTransactions.clear();

        for (std::set<uint256>::iterator i = tmp.begin(); i != tmp.end(); ++i)
        {
            transactionReceived(*i);
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
bool XBridgeConnector::sendPacket(XBridgePacket & packet)
{
    boost::system::error_code error;
    m_socket.send(boost::asio::buffer(packet.header(), packet.allSize()), 0, error);
    if (error)
    {
        qDebug() << "packet send error <"
                 << error.value() << "> " << error.message().c_str()
                 << " " << __FUNCTION__;
        return false;
    }
    return true;
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

        if (!sendPacket(p))
        {
            qDebug() << "send address error " << __FUNCTION__;
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

    if (!sendPacket(p))
    {
        qDebug() << "send xchat message error " << __FUNCTION__;
        return false;
    }

    return true;
}

//******************************************************************************
//******************************************************************************
uint256 XBridgeConnector::sendXBridgeTransaction(const std::vector<unsigned char> & from,
                                                 const std::string & fromCurrency,
                                                 const boost::uint64_t fromAmount,
                                                 const std::vector<unsigned char> & to,
                                                 const std::string & toCurrency,
                                                 const boost::uint64_t toAmount)
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

    XBridgeTransactionPtr ptr(new XBridgeTransaction);
    ptr->id           = id;
    ptr->from         = from;
    ptr->fromCurrency = fromCurrency;
    ptr->fromAmount   = fromAmount;
    ptr->to           = to;
    ptr->toCurrency   = toCurrency;
    ptr->toAmount     = toAmount;

    {
        boost::mutex::scoped_lock l(m_txLocker);
        m_pendingTransactions[id] = ptr;
    }

    return id;
}

//******************************************************************************
//******************************************************************************
bool XBridgeConnector::transactionReceived(const uint256 & hash)
{
    if (!m_socket.is_open())
    {
        m_receivedTransactions.insert(hash);
        return true;
    }

    // packet for this xbridge client, without adress
    XBridgePacket p(xbcReceivedTransaction);
    p.append(hash.begin(), 32);

    if (!sendPacket(p))
    {
        qDebug() << "send transaction hash error " << __FUNCTION__;
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

    std::vector<std::pair<std::string, std::string> > wallets;

    for (std::vector<std::string>::iterator i = strs.begin(); i != strs.end(); ++i)
    {
        std::string wallet = *i;
        std::string title = *(++i);
        qDebug() << "wallet " << wallet.c_str() << " " << title.c_str();

        wallets.push_back(std::make_pair(wallet, title));
    }

    if (wallets.size())
    {
        uiInterface.NotifyXBridgeExchangeWalletsReceived(wallets);
    }
    return true;
}

//******************************************************************************
//******************************************************************************
bool XBridgeConnector::processTransactionHold(XBridgePacketPtr packet)
{
    if (packet->size() != 104)
    {
        qDebug() << "incorrect packet size for xbcTransactionHold" << __FUNCTION__;
        return false;
    }

    // smart hub addr
    std::vector<unsigned char> hubAddress(packet->data()+20, packet->data()+40);

    // read packet data
    uint256 id   (packet->data()+40);
    uint256 newid(packet->data()+72);

    // remove transaction from pending
    // move to processing
    {
        boost::mutex::scoped_lock l(m_txLocker);
        m_transactions[newid] = m_pendingTransactions[id];
        m_pendingTransactions.erase(id);
    }

    // send hold apply
    XBridgePacket reply(xbcTransactionHoldApply);
    reply.append(hubAddress);
    reply.append(newid.begin(), 32);

    if (!sendPacket(reply))
    {
        qDebug() << "error sending transaction hold reply packet " << __FUNCTION__;
        return false;
    }
    return true;
}

//******************************************************************************
//******************************************************************************
bool XBridgeConnector::processTransactionPay(XBridgePacketPtr packet)
{
    if (packet->size() != 92)
    {
        qDebug() << "incorrect packet size for xbcTransactionPay" << __FUNCTION__;
        return false;
    }

    // smart hub addr
    std::vector<unsigned char> hubAddress(packet->data()+20, packet->data()+40);

    // transaction id
    uint256 id (packet->data()+40);
    {
        boost::mutex::scoped_lock l(m_txLocker);
        if (!m_transactions.count(id))
        {
            qDebug() << "unknown xbridge transaction id <"
                     << EncodeBase64(id.begin(), 32).c_str() << "> "
                     << __FUNCTION__;
            // TODO
            return false;
        }

        if (!m_transactions[id])
        {
            qDebug() << "empty xbridge transaction id <"
                     << EncodeBase64(id.begin(), 32).c_str() << "> "
                     << __FUNCTION__;
            // TODO
            return false;
        }
    }

    // wallet address
    // std::vector<unsigned char> walletAddress(packet->data()+72, packet->data()+92);
    CScript addr;
    {
        uint160 uikey(packet->data()+72);
        CKeyID key(uikey);
        CBitcoinAddress baddr;
        baddr.Set(key);
        addr.SetDestination(baddr.Get());
    }

    boost::uint64_t amount = ((double)(m_transactions[id]->fromAmount) / 1000000) * COIN;

    std::vector<std::pair<CScript, int64_t> > txpair;
    txpair.push_back(std::make_pair(addr, amount));

    // send money to specified wallet address for this transaction
    uint256 transactionId;
    if (!pwalletMain->createAndCommitTransaction(txpair, transactionId))
    {
        qDebug() << "transaction not created <"
                 << EncodeBase64(id.begin(), 32).c_str() << ">"
                 << __FUNCTION__;

        XBridgePacket reply(xbcTransactionCancel);
        reply.append(hubAddress);
        reply.append(id.begin(), 32);
        if (!sendPacket(reply))
        {
            qDebug() << "error sending transaction cancel packet "
                     << __FUNCTION__;
            return false;
        }

        // cancelled
        return true;
    }

    qDebug() << "xbridge transaction <" << EncodeBase64(id.begin(), 32).c_str()
             << "> <" << transactionId.ToString().c_str() << "> committed";

    // send pay apply
    XBridgePacket reply(xbcTransactionPayApply);
    reply.append(hubAddress);
    reply.append(id.begin(), 32);
    reply.append(transactionId.begin(), 32);

    if (!sendPacket(reply))
    {
        qDebug() << "error sending transaction pay reply packet " << __FUNCTION__;
        return false;
    }

    return true;
}

//******************************************************************************
//******************************************************************************
bool XBridgeConnector::processTransactionCommit(XBridgePacketPtr packet)
{
    if (packet->size() != 100)
    {
        qDebug() << "incorrect packet size for xbcTransactionCommit" << __FUNCTION__;
        return false;
    }

    // smart hub addr
    std::vector<unsigned char> hubAddress(packet->data()+20, packet->data()+40);

    // transaction id
    uint256 id (packet->data()+40);

    // destination wallet address
    CScript addr;
    {
        uint160 uikey(packet->data()+72);
        CKeyID key(uikey);
        CBitcoinAddress baddr;
        baddr.Set(key);
        addr.SetDestination(baddr.Get());
    }


    // amount
    boost::uint64_t amount = *static_cast<boost::uint64_t *>(static_cast<void *>(packet->data()+92));
    amount = ((double)amount / 1000000) * COIN;

    std::vector<std::pair<CScript, int64_t> > txpair;
    txpair.push_back(std::make_pair(addr, amount));

    // send money to specified wallet address for this transaction
    uint256 transactionId;
    if (!pwalletMain->createAndCommitTransaction(txpair, transactionId))
    {
        qDebug() << "transaction not commited <"
                 << EncodeBase64(id.begin(), 32).c_str() << ">"
                 << __FUNCTION__;

        // send cancel to hub
        XBridgePacket reply(xbcTransactionCancel);
        reply.append(hubAddress);
        reply.append(id.begin(), 32);
        if (!sendPacket(reply))
        {
            qDebug() << "error sending transaction cancel packet "
                     << __FUNCTION__;
            return false;
        }

        // cancelled
        return true;
    }

    // send commit apply to hub
    XBridgePacket reply(xbcTransactionCommitApply);
    reply.append(hubAddress);
    reply.append(id.begin(), 32);
    if (!sendPacket(reply))
    {
        qDebug() << "error sending transaction commit apply packet "
                 << __FUNCTION__;
        return false;
    }

    return true;
}

//******************************************************************************
//******************************************************************************
bool XBridgeConnector::processTransactionFinished(XBridgePacketPtr packet)
{
    if (packet->size() != 52)
    {
        qDebug() << "incorrect packet size for xbcTransactionFinished" << __FUNCTION__;
        return false;
    }

    // transaction id
    // uint256 id(packet->data()+20);

    // TODO update transaction state for gui
    return true;
}

//******************************************************************************
//******************************************************************************
bool XBridgeConnector::processTransactionDropped(XBridgePacketPtr packet)
{
    if (packet->size() != 52)
    {
        qDebug() << "incorrect packet size for xbcTransactionDropped" << __FUNCTION__;
        return false;
    }

    // transaction id
    // uint256 id(packet->data()+20);

    // TODO update transaction state for gui
    return true;
}
