#include <QNEthernet.h>

#include "l3ptp.h"

// El maxim que llegim d'un datagrama. El missatge PTP mes llarg que
// aquesta biblioteca analitza en son 54 bytes; la resta, si n'hi ha,
// son TLV que no mira ningu.
//
// Buffer de mida fixa i no variable. Abans era `uint8_t buf[esize]`,
// amb esize sortint de la xarxa: la mida del que anava a la pila la
// decidia qui enviava. Acotat pel marc en el cas normal, pero
// IP_REASSEMBLY esta actiu per defecte a lwIP i un datagrama fragmentat
// pot arribar mes gran.
static constexpr int PTP_RECV_BUF_LEN = 64;

const IPAddress adr{224, 0, 1, 129};
const IPAddress pAdr{224, 0, 0, 107};
const int eventPort = 319;
const int generalPort = 320;

l3PTP::l3PTP(bool master_, bool slave_, bool p2p_):
PTPBase(master_,slave_,p2p_)
{

}

void l3PTP::initSockets()
{
    eventSocket = new qindesign::network::EthernetUDP;
    generalSocket = new qindesign::network::EthernetUDP;

    eventSocket->beginMulticast(adr, eventPort, true);
    generalSocket->beginMulticast(adr, generalPort, true);

    if(p2p){
        pEventSocket = new qindesign::network::EthernetUDP;
        pGeneralSocket = new qindesign::network::EthernetUDP;
        pEventSocket->beginMulticast(pAdr, eventPort, true);
        pGeneralSocket->beginMulticast(pAdr, generalPort, true);
    }
}

void l3PTP::updateSockets()
{
    const int esize = eventSocket->parsePacket();
    if (esize > 0)
    {
        timespec erecv_ts;
        eventSocket->timestamp(erecv_ts);
        uint8_t ebuf[PTP_RECV_BUF_LEN];
        const int en = esize > PTP_RECV_BUF_LEN ? PTP_RECV_BUF_LEN : esize;
        eventSocket->read(ebuf, en);
        parsePTPMessage(ebuf,en,erecv_ts);
    }

    const int gsize = generalSocket->parsePacket();
    if (gsize > 0)
    {
        timespec grecv_ts;
        generalSocket->timestamp(grecv_ts);
        uint8_t gbuf[PTP_RECV_BUF_LEN];
        const int gn = gsize > PTP_RECV_BUF_LEN ? PTP_RECV_BUF_LEN : gsize;
        generalSocket->read(gbuf, gn);
        parsePTPMessage(gbuf,gn,grecv_ts);
    }
    if(p2p){
        const int esize = pEventSocket->parsePacket();
        if (esize > 0)
        {
            timespec erecv_ts;
            pEventSocket->timestamp(erecv_ts);
            uint8_t ebuf[PTP_RECV_BUF_LEN];
            const int en = esize > PTP_RECV_BUF_LEN ? PTP_RECV_BUF_LEN : esize;
            pEventSocket->read(ebuf, en);
            parsePTPMessage(ebuf,en,erecv_ts);
        }

        const int gsize = pGeneralSocket->parsePacket();
        if (gsize > 0)
        {
            timespec grecv_ts;
            pGeneralSocket->timestamp(grecv_ts);
            uint8_t gbuf[PTP_RECV_BUF_LEN];
            const int gn = gsize > PTP_RECV_BUF_LEN ? PTP_RECV_BUF_LEN : gsize;
            pGeneralSocket->read(gbuf, gn);
            parsePTPMessage(gbuf,gn,grecv_ts);
        }
    }
}

void l3PTP::sendPTPMessage(const uint8_t *buf, int size, bool generalMessage){
    // El resultat es mira: abans es descartava del tot, de manera que un
    // Sync o un Announce que no sortia deixava els esclaus sense
    // referencia sense que ho sabes ningu.
    const bool sent = generalMessage
        ? generalSocket->send(adr,generalPort,buf,size)
        : eventSocket->send(adr,eventPort,buf,size);

    if (!sent)
    {
        txFailureCount++;
    }
}
