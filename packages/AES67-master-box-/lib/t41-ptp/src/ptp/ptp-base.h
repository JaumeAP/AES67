#pragma once

using NanoTime = int64_t;

constexpr NanoTime NS_PER_S = 1000*1000*1000;

// LONGITUDS MINIMES D'UN MISSATGE PTPv2, en bytes.
//
// Existeixen perque parsePTPMessage rebia la mida del paquet i NO la
// feia servir. Els analitzadors llegeixen fins a buf[51], i el buffer
// que se'ls passa te exactament la mida del datagrama rebut: un
// datagrama d'un sol byte al port PTP feia llegir 51 bytes de pila
// adjacent, i aquells bytes acabaven convertits en marques de temps.
//
// PTP es multicast obert i sense autenticacio per disseny, de manera
// que qualsevol equip de la xarxa hi arribava.
constexpr int PTP_HEADER_LEN = 34;          // capcalera comuna
constexpr int PTP_SYNC_LEN = 44;            // Sync, Delay_Req, Follow_Up
constexpr int PTP_DELAY_RESP_LEN = 54;      // Delay_Resp i Pdelay_Resp_Follow_Up

// Quant s'espera la marca de temps d'enviament abans de rendir-se.
//
// Els dos bucles que l'esperaven no tenien ni temps maxim ni topall
// d'iteracions: si el maquinari no arribava a donar-la mai -- enllac
// caigut a mig enviar, trama descartada, qualsevol ensopegada -- el
// firmware es quedava girant dins de loop() PER SEMPRE. S'aturava tot.
//
// Un mil.lisegon es molt mes del que triga: a 100 Mbit una trama de 64
// bytes son uns 5 us, i el MAC publica la marca tot seguit. Si no ha
// arribat en mil vegades aixo, no arribara.
constexpr unsigned long TX_TIMESTAMP_TIMEOUT_US = 1000;

// El maxim de segons que es pot multiplicar per mil milions sense
// desbordar un int64_t.
//
// El camp de segons son 48 bits del cable, o sigui fins a
// 281.474.976.710.655, i multiplicat per 1e9 fa 2,8e23 contra els 9,2e18
// que hi caben. Desbordar un enter amb signe es comportament indefinit,
// no un valor equivocat, i el valor el tria qui envia.
constexpr NanoTime MAX_SAFE_SECONDS = 9223372036;

class PTPBase
{
public:
    PTPBase(bool master_, bool slave_, bool p2p_);
    void begin();
    void update();
    void reset();
    void setKi(double val);
    void setKp(double val);
    NanoTime getOffset();
    NanoTime getDelay();
    void syncMessage();
    void announceMessage();
    void ppsInterruptTriggered(NanoTime pps_ts, NanoTime local_ts);
    int getLockCount();

    // Announce dataset and advertised message intervals.
    //
    // The defaults below are exactly the values this library hardcoded before
    // these became configurable, so leaving them alone changes nothing.
    //
    // A grandmaster disciplined by an external reference needs to say so:
    // with clockClass fixed at 248 and timeSource at INTERNAL_OSCILLATOR it
    // announces itself as free-running whatever its actual lock state, which
    // both misinforms the BMCA and makes it impossible for receivers to tell
    // a locked clock from one that has lost its reference.
    void setClockClass(uint8_t val);
    void setClockAccuracy(uint8_t val);
    void setOffsetScaledLogVariance(uint16_t val);
    void setPriority1(uint8_t val);
    void setPriority2(uint8_t val);
    void setTimeSource(uint8_t val);
    void setCurrentUtcOffset(int16_t val);

    // currentUtcOffsetValid flag. Announcing an offset while leaving this
    // false says "here is a number, do not rely on it", which is the right
    // thing for a master with no traceable source of absolute time. Only set
    // it when the offset really is known good.
    void setUtcOffsetValid(bool val);

    // logMessageInterval as advertised in the PTP header. These only set what
    // is announced -- the caller still has to send at the matching rate, or
    // the announcement lies.
    void setLogSyncInterval(int8_t val);
    void setLogAnnounceInterval(int8_t val);

protected:
    virtual void initSockets()=0;
    virtual void updateSockets()=0;
    virtual void sendPTPMessage(const uint8_t *buf, int size, bool generalMessage)=0;
    
    void parsePTPMessage(const uint8_t *buf, int size, const timespec &recv_ts);

public:
    // Quants enviaments han fallat des de l'arrencada. Un grandmaster
    // que no aconsegueix treure els seus Sync es un problema silencios
    // sense aixo: el registre nomes ajuda si algu mira el port serie.
    uint32_t getTxFailureCount() const { return txFailureCount; }

protected:
    uint32_t txFailureCount = 0;

    bool master;
    bool slave;
    bool p2p;
    
private:
	void setT1(NanoTime ts);
	void setT2(NanoTime ts);
	void setT3(NanoTime ts);
	void setT4(NanoTime ts);
    void parseSyncMessage(const uint8_t *buf, const timespec &recv_ts);
    void parseFollowUpMessage(const uint8_t *buf);
    void parseDelayResponseMessage(const uint8_t *buf, const timespec &recv_ts);
    void parseDelayResponseFollowUpMessage(const uint8_t *buf);
    void parseDelayRequestMessage(const uint8_t *buf, const timespec &recv_ts);
    
    void delayRequestMessage();
    void followUpMessage(const timespec &send_ts);
    void delayResponseMessage(const uint8_t *request_buf, uint16_t sequenceID, const timespec &request_recv_ts);
    void initPTPMessage(uint8_t *buf, const uint16_t messageLength, const uint8_t messageType, const uint16_t sequenceID, const uint8_t controlField);
    void updateController();
    void updateTimer();
    void updatePPS();

    uint8_t clockID[8];
    bool initialised=false;

    uint8_t clockClass = 248;          // default, not traceable
    uint8_t clockAccuracy = 0xfe;      // unknown
    uint16_t offsetScaledLogVariance = 0xffff;  // unknown
    uint8_t priority1 = 128;
    uint8_t priority2 = 128;
    uint8_t timeSource = 0xa0;         // INTERNAL_OSCILLATOR
    int16_t currentUtcOffset = 37;
    bool utcOffsetValid = false;
    int8_t logSyncInterval = 0;        // 1 s
    int8_t logAnnounceInterval = 0;    // 1 s
    uint16_t delayRequestSequenceID = 0;
	int lockcount=0;
    uint16_t syncSequenceID=0;
    uint16_t syncServerSequenceID = 0;
    uint16_t announceServerSequenceID = 0;
    uint16_t followUpSequenceID=0;
    NanoTime t1=-1;
    NanoTime t1last = -1;
    NanoTime t2 = -1;
    NanoTime t2last = -1;
    NanoTime t2new = -1;
    NanoTime t3 = -1;
    NanoTime t4 = -1;
    NanoTime t5 = -1;
    NanoTime t6 = -1;

    NanoTime t1s = -1;
    NanoTime t4s = -1;

    bool t1updated=false;
    bool t2updated=false;
    bool t3updated=false;
    bool t4updated=false;
    bool t5updated=false;
    bool t6updated=false;
    bool t1lastvalid=false;
    bool t2lastvalid=false;
    bool ppsupdated=false;

    NanoTime currentOffset=0;
    NanoTime currentDelay=0;
    int nspsAccu=0;
    double driftNSPS=0;
    double KI=0.5;
    double KP=1.0;
    int updateCounter=0;
    
};
