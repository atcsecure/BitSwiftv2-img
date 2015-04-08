//******************************************************************************
//******************************************************************************

#ifndef XBRIDGECONNECTOR_H
#define XBRIDGECONNECTOR_H

#include "xbridgepacket.h"
#include "message.h"
#include "FastDelegate.h"

#include <boost/asio.hpp>
#include <boost/thread.hpp>

//******************************************************************************
//******************************************************************************
struct XBridgeTransaction
{
    uint256 id;
    std::vector<unsigned char> from;
    std::string fromCurrency;
    boost::uint64_t fromAmount;
    std::vector<unsigned char> to;
    std::string toCurrency;
    boost::uint64_t toAmount;

    // TODO add transaction state for gui

    XBridgePacketPtr packet;
};

typedef boost::shared_ptr<XBridgeTransaction> XBridgeTransactionPtr;

//******************************************************************************
//******************************************************************************
class XBridgeConnector
{
    friend XBridgeConnector & xbridge();

private:
    XBridgeConnector();

    enum
    {
        SERVER_LISTEN_PORT = 30330,
        TIMER_INTERVAL = 60
    };

public:
    bool connect();
    bool isConnected() const;

    bool announceLocalAddresses();
    bool sendXChatMessage(const Message & m);

    uint256 sendXBridgeTransaction(const std::vector<unsigned char> & from,
                                   const std::string & fromCurrency,
                                   const boost::uint64_t fromAmount,
                                   const std::vector<unsigned char> & to,
                                   const std::string & toCurrency,
                                   const boost::uint64_t toAmount);

    bool transactionReceived(const uint256 & hash);

private:
    void run();

    void disconnect();

    void onTimer();

    void doReadHeader(XBridgePacketPtr packet,
                      const std::size_t offset = 0);
    void onReadHeader(XBridgePacketPtr packet,
                      const std::size_t offset,
                      const boost::system::error_code & error,
                      std::size_t transferred);

    void doReadBody(XBridgePacketPtr packet,
                    const std::size_t offset = 0);
    void onReadBody(XBridgePacketPtr packet,
                    const std::size_t offset,
                    const boost::system::error_code & error,
                    std::size_t transferred);

    bool sendPacket(XBridgePacket & packet);

    bool encryptPacket(XBridgePacketPtr packet);
    bool decryptPacket(XBridgePacketPtr packet);
    bool processPacket(XBridgePacketPtr packet);

    bool processInvalid(XBridgePacketPtr packet);
    bool processXChatMessage(XBridgePacketPtr packet);

    bool processExchangeWallets(XBridgePacketPtr packet);

    bool processTransactionHold(XBridgePacketPtr packet);
    bool processTransactionPay(XBridgePacketPtr packet);
    bool processTransactionFinished(XBridgePacketPtr packet);
    bool processTransactionDropped(XBridgePacketPtr packet);

private:
    boost::asio::io_service      m_io;
    boost::asio::ip::tcp::socket m_socket;
    boost::thread                m_thread;
    boost::asio::deadline_timer  m_timer;


    typedef std::map<const int, fastdelegate::FastDelegate1<XBridgePacketPtr, bool> > PacketProcessorsMap;
    PacketProcessorsMap m_processors;

    std::map<uint256, XBridgeTransactionPtr> m_pendingTransactions;
    std::map<uint256, XBridgeTransactionPtr> m_transactions;

    std::set<uint256> m_receivedTransactions;
};

//******************************************************************************
//******************************************************************************
XBridgeConnector & xbridge();

#endif // XBRIDGECONNECTOR_H
