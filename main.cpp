#define HAVE_CONFIG_H
#include <univalue.h>
#include <boost/asio.hpp>

#include <string>
#include <thread>
#include <fstream>
#include <streambuf>
#include <stdexcept>
#include <random>
#include "key.h"
#include "uint256.h"
#include "base58.h"
#include "script/script.h"
#include "streams.h"
#include "utilstrencodings.h"
#include "leakybucket.h"

using namespace std;

static const auto REGTEST_MSG_START = ParseHex("dab5bffa");
static const auto NOLNET_MSG_START = ParseHex("00000000"); // ParseHex("fbcec4e9");

class UTXO
{
public:
    COutPoint prevout;

    CScript constraintScript;
    uint64_t satoshi;
    CKey    privKey;
    CPubKey publicKey;

    CPubKey& pubKey()
    {
        if (publicKey.IsValid()) return publicKey;
        publicKey = privKey.GetPubKey();
        return publicKey;
    }

    CScript createP2PKH()
    {
        // TODO inefficient to GetPubKey more than once, should I cache it in the object?
        CKeyID dest = pubKey().GetID();
        constraintScript.clear();
        constraintScript << OP_DUP << OP_HASH160 << ToByteVector(dest) << OP_EQUALVERIFY << OP_CHECKSIG;
        return constraintScript;
    }
};

/** Generate transactions at the maximum rate possible to the specified host */
void MaxSpeed(const string& host, vector<UTXO>& utxo, vector<UTXO>& txo);

class ConfigException:public runtime_error
{
public:
    explicit ConfigException(const string& what):runtime_error(what) {}
};

class FeeProducer
{
    CAmount minFee = -1;
    CAmount maxFee = -1;
    CAmount constantFee = -1;

    std::default_random_engine rnd;
    // returns a uniformly distributed random number in the inclusive range: [0, UINT_MAX]
    std::uniform_int_distribution<long int> randRange;
public:
    FeeProducer(uint64_t fee=0): constantFee(fee) {}

    FeeProducer(const UniValue& v):constantFee(parseUnivalue(v)), rnd(std::random_device()()), randRange((int)minFee, (int)maxFee)
    {}

    void set(const UniValue& v)
    {
        parseUnivalue(v);
        randRange = std::uniform_int_distribution<long int>(minFee, maxFee);
    }

    CAmount parseUnivalue(const UniValue& v)
    {
        if (v.isNum())
        {
            constantFee = v.get_int64();
            return constantFee;
        }
        else constantFee = -1;

        if (v.isArray())
        {
            minFee = v[0].get_int64();
            maxFee = v[1].get_int64();
        }
        return constantFee;
    }


    CAmount operator() ()
    {
        if (constantFee >= 0) return constantFee;
        else return randRange(rnd);
        return 0;
    }
};

class GlobalConfig
{
public:
    FeeProducer fee;
    unsigned int splitPerTx = 23;
    unsigned int defaultPort = 18444;
    unsigned int minUtxos = 4*1000*1000;
    unsigned int maxThreads = 10;
    string bitcoind = "127.0.0.1:18444";  // The default host for non-rate-generation operations like coin splitting.
    std::vector<unsigned char> msgStart = REGTEST_MSG_START;
    std::string net = "regtest";

    void Load(UniValue settings)
    {
        if (settings.exists("fee")) fee.set(settings["fee"]);
        if (settings.exists("splitPerTx")) splitPerTx = settings["splitPerTx"].get_int64();
        if (settings.exists("defaultPort")) defaultPort  = settings["defaultPort"].get_int64();
        if (settings.exists("minUtxos"))  minUtxos = settings["minUtxos"].get_int64();
        if (settings.exists("maxThreads")) maxThreads = settings["maxThreads"].get_int64();
        if (settings.exists("bitcoind")) bitcoind = settings["bitcoind"].get_str();
        if (settings.exists("netMagic")) msgStart=ParseHex(settings["netMagic"].get_str());
        if (settings.exists("net"))
        {
            std::string n = settings["net"].get_str();
            if (n == "regtest") net = CBaseChainParams::REGTEST;
            else if (n == "testnet") net = CBaseChainParams::TESTNET;
            else if (n == "chain_nol") net = CBaseChainParams::UNL;
            else if (n == "mainnet") net = CBaseChainParams::MAIN;
            else throw ConfigException("Unknown value specified in 'net' field.");
        }
    }
};

GlobalConfig gc;



string EncodeHexTx(const CTransaction &tx)
{
    CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
    ssTx << tx;
    return HexStr(ssTx.begin(), ssTx.end());
}


std::string readFile(const std::string &filename)
{
    std::ifstream t(filename);
    std::string str;

    t.seekg(0, std::ios::end);
    str.reserve(t.tellg());
    t.seekg(0, std::ios::beg);

    str.assign((std::istreambuf_iterator<char>(t)),
               std::istreambuf_iterator<char>());

    return str;
}

UniValue readJson(const std::string &jsondata)
{
    UniValue v;

    if (!v.read(jsondata)) // || !v.isObject())
    {
        printf("problem reading json\n");
        abort();
    }
    return v;
    // return v.get_obj();
}


void ParseInputCoins(UniValue coins, std::vector<UTXO>& utxo)
{
    for (unsigned int idx = 0; idx < coins.size(); idx++)
    {
        const UniValue &coin = coins[idx];
        UTXO u;
        u.prevout.hash.SetHex(coin["txid"].get_str());
        u.prevout.n = coin["vout"].get_int64();
        u.satoshi = coin["satoshi"].get_int64();
        CBitcoinSecret secret;
        if (!secret.SetString(coin["privKey"].get_str()))
        {
            printf("priv key bad");
            exit(1);
        }
        u.privKey = secret.GetKey();

        vector<unsigned char> scriptData(ParseHex(coin["scriptPubKey"].get_str()));
        u.constraintScript = CScript(scriptData.begin(), scriptData.end());

        // Don't need the address, I have the constraint script and the secret
        utxo.push_back(u);
    }
}

//std::mutex cs;

bool createTx(CMutableTransaction& tx, const std::vector<UTXO>::iterator& inStart, const std::vector<UTXO>::iterator& inEnd,
              std::vector<UTXO>::iterator& outStart, const std::vector<UTXO>::iterator& outEnd, uint64_t fee)
{
    uint64_t inQty = 0;
    int numSplits = outEnd - outStart;
    int numInputs = inEnd - inStart;

    tx.nVersion = CTransaction::CURRENT_VERSION;
    tx.nLockTime = 0;
    
    tx.vin.resize(numInputs);    

    int count=0;
    for(auto in = inStart; in != inEnd; in++,count++)
    {
        inQty += in->satoshi;

        CTxIn& txi = tx.vin[count];
        txi.prevout = in->prevout;
        // txi.scriptSig = CScript(); Will be cleared when signature is created
        txi.nSequence = CTxIn::SEQUENCE_FINAL;
    }

    if (fee > inQty) return false;

    uint64_t outQty = (inQty-fee)/numSplits;

    if (outQty == 0) return false;

    tx.vout.resize(numSplits);
    count=0;
    for(auto out = outStart; out != outEnd; out++,count++)
    {
        assert(count<numSplits);
        CTxOut& txo = tx.vout[count];
        txo.nValue = outQty;
        txo.scriptPubKey = out->createP2PKH();
        out->satoshi = outQty;
        out->constraintScript = txo.scriptPubKey;
        out->prevout.n = count;
    }

    // Sign
    int inputIdx=0;
    int sighashtype = SIGHASH_FORKID | SIGHASH_ALL;

    CTransaction ctx = tx;
    for(auto in = inStart; in != inEnd; in++,inputIdx++)
    {
        // std::lock_guard<std::mutex> lock(cs);
        CTxIn& txi = tx.vin[inputIdx];

        size_t nHashedOut = 0;
        uint256 sighash = SignatureHash(in->constraintScript, ctx, inputIdx, sighashtype, in->satoshi, &nHashedOut);
        // printf("script: %s\nqty:%lld input:%d\nSigHash: %s\n", HexStr(in->constraintScript.begin(), in->constraintScript.end()).c_str(),  (long long int) in->satoshi, inputIdx, sighash.ToString().c_str());
        std::vector<unsigned char> sig;
        if (!in->privKey.SignECDSA(sighash, sig))
        {
            printf("signing error");
            abort();
        }

        sig.push_back((unsigned char)sighashtype);

        txi.scriptSig.clear();
        CPubKey pub = in->pubKey();
        //printf("pubkey: %s\n", HexStr(pub.begin(), pub.end()).c_str());
        if (in->constraintScript[0] == OP_DUP)  // P2PKH
        {
            txi.scriptSig << sig << ToByteVector(pub);
        }
        else  // assume P2PK
        {
            txi.scriptSig << sig;
        }
    }

    uint256 txHash = tx.GetHash();
    // printf("TX: %s\n", txHash.ToString().c_str());
    for(auto out = outStart; out != outEnd; out++,count++)
    {
        out->prevout.hash = txHash;
    }

    return true;
}


/** Given a string of the form "1.2.3.4:5678" return the integer 5678 */
int portFromHostname(std::string name, int defaultPort)
{
    auto colonPos = name.find(':');
    if (colonPos == string::npos) return defaultPort;
    string port = name.substr(colonPos+1);
    int ret = stoi(port);
    return ret;
}

/** Given a string of the form "1.2.3.4:5678" return the string "1.2.3.4" */
std::string hostFromHostname(string name)
{
    auto colonPos = name.find(':');
    if (colonPos == string::npos) return name;
    string ret = name.substr(0,colonPos);
    return ret;
}


static const char TX_MSG[12] = {'t','x',0,0, 0,0,0,0, 0,0,0,0};
static const char VERACK_MSG[12] = {'v','e','r','a', 'c','k',0,0, 0,0,0,0};
static const char VER_MSG[12] = {'v','e','r','s', 'i','o','n',0, 0,0,0,0};



/** An extremely simple bitcoind P2P compatible client*/
class SimpleClient
{
    unsigned int readCtr=0;
public:
    boost::asio::io_service ios;
    boost::asio::ip::tcp::endpoint endpoint;
    boost::asio::ip::tcp::socket socket;
    std::array<char, 2*1024*1024> readbuf;

    SimpleClient(std::string ip):endpoint(boost::asio::ip::address::from_string(hostFromHostname(ip))
                                          ,portFromHostname(ip, gc.defaultPort)), socket(ios)
    {
        while(1)
        {
        try
        {
	    socket.connect(endpoint);
            break;
        }
        catch(boost::system::system_error &e)
        {
            printf("Cannot connect to %s error %s, retrying...\n)", ip.c_str(), e.what());
            sleep(1);
        }
        }
        //auto VERSION_MSG = ParseHex("dab5bffa76657273696f6e00000000005e0000002ca922277e1101000100000000000000d6d1675d00000000010000000000000000000000000000000000ffff7f0000013bed010000000000000000000000000000000000ffff000000000000ecaff3bf4f09fcf309747847656e3a302e31ffffffff");
        auto VER_CONTENTS = ParseHex("7e1101000100000000000000d6d1675d00000000010000000000000000000000000000000000ffff7f0000013bed010000000000000000000000000000000000ffff000000000000ecaff3bf4f09fcf309747847656e3a302e31ffffffff");
        SendMessage(VER_MSG, (char*) &VER_CONTENTS.at(0), VER_CONTENTS.size());
        // Ack his version message even though we don't look for it
        SendMessage(VERACK_MSG, nullptr, 0);
    }
    void SendMessage(const char* msgname, const char* data, uint32_t size)
    {
        unsigned char header[4+12+4+4];

        memcpy(&header[0], &gc.msgStart.at(0), 4);  // magic
        memcpy(&header[4], msgname, 12);  // message command
        memcpy(&header[4+12], &size, 4);  // message size

        uint32_t checksum = 0;  // checksum can be 0 in BU to mean no checksum
        memcpy(&header[4+12+4], &checksum, 4);

        std::array<boost::asio::const_buffer, 2> sendGroup = {
            boost::asio::const_buffer(header,sizeof(header)),
            boost::asio::const_buffer(data, size)
        };
        
        boost::system::error_code error;
        auto bytesWritten = socket.write_some(sendGroup, error);
        if (bytesWritten==0)
        {
            printf("write error: %d\n", error.value());
        }

        // Periodically read some data and dump it because we don't care what the server sends back to us
        // If we don't do this though, the buffer will eventually fill up and block sends
        readCtr+=1;
        if ((readCtr & 0xfff) == 0)
        {
            boost::system::error_code error;
            //size_t len =
            if (socket.available())
                socket.read_some(boost::asio::buffer(readbuf), error);
            // printf("dropped %lu received bytes\n", (long unsigned int) len);
        }
    }
};


/** Get private and public keys for an large array of UTXO objects */
void calcKeys(vector<UTXO>::iterator st, vector<UTXO>::iterator end)
{
    for (auto it=st; it != end; ++it)
    {
        it->privKey.MakeNewKey(true);
        // precalculate the public key because we don't care about optimizing this so do it before timing happens
        it->publicKey = it->privKey.GetPubKey();
    }

}

/** Generate a bunch of P2PKH transactions -- 1 for every entry in the utxo iterator.  It is expected that the txo 
    iterator contains at least utxoEnd - utxoSt items.
*/
void sendP2PKH(SimpleClient& sc, vector<UTXO>::iterator utxoSt, vector<UTXO>::iterator utxoEnd, vector<UTXO>::iterator txo)
{
    CMutableTransaction tx;

    for (auto uit=utxoSt; uit != utxoEnd; ++uit,++txo)
    {
        bool worked = createTx(tx, uit, uit+1, txo, txo+1, gc.fee());
        if (worked)
        {
            CDataStream serializer(SER_NETWORK, PROTOCOL_VERSION);
            serializer << tx;
            sc.SendMessage(TX_MSG, serializer.data(), serializer.size());
        }
        else
        {
            printf("UTXO didn't have enough balance\n");
        }
    }

}


/** A tx generation operation, consisting of sending transactions at a particular rate to a particular node */
class ScheduleOp
{
public:
    string host;
    uint64_t rateBegin = 0;
    uint64_t rateEnd = std::numeric_limits<unsigned long long int>::max();
    FeeProducer fee;

    void Load(const UniValue& u)
    {
        if (u.exists("fee")) fee.set(u["fee"]);
        else fee = gc.fee;

        if (u.exists("host")) host = u["host"].get_str();
        else ConfigException("Mandatory field 'host' is missing");
        if (u.exists("rate")) rateBegin = u["rate"].get_int64();
        else ConfigException("Mandatory field 'rate' is missing");
        if (u.exists("rateEnd")) rateEnd = u["rateEnd"].get_int64();
        else rateEnd = rateBegin;
    }
};

/** A period in the generation schedule, in which we are sending transactions at specific rates to a specific set
    of nodes 
*/
class SchedulePhase
{
public:
    string      name = "unnamed";
    uint64_t    startTime = 0;
    uint64_t    endTime = 0;
    vector<ScheduleOp> targets;

    uint64_t LoadATime(const UniValue& u, const string& fieldName)
    {
        uint64_t ret;
        if (u.exists(fieldName))
        {
            ret = u[fieldName].get_int64();
            if (ret < 1567000000)
            {
                ret = ret + GetTime();
            }
            return ret;
        }

        throw ConfigException("Mandatory field '" + fieldName + "' does not exist in " + name);
    }

    void Load(const UniValue& u)
    {
        if (u.exists("name")) name = u["name"].get_str();
        startTime = LoadATime(u, "start");
        endTime = LoadATime(u, "end");

        if (!u.exists("targets")) throw ConfigException("No 'targets' of schedule " + name + "defined");

        UniValue utargets = u["targets"];
        targets.resize(utargets.size());
        for(unsigned int idx=0; idx < utargets.size(); idx++)
        {
            const UniValue& utgt = utargets[idx];
            targets[idx].Load(utgt);
        }
    }
};


/** Generate transactions at a certain rate, starting at a certain time, and using utxo and txos provided by iterators.
    If all the utxos and txos are consumed, the routine uses prior generated transactions new utxos, creating chains
    of unspent transactions.
*/
void GenerateTxs(string name, FeeProducer& fee, uint64_t start, uint64_t end, string host, uint64_t rateBegin, uint64_t rateEnd, std::vector<UTXO>::iterator utxoIt, std::vector<UTXO>::iterator txoIt, uint64_t utxoQty)
{
    // The leaky bucket is based on integers so makes rounding errors if the rate is near 1.  By multiplying by 1024,
    // we essentially use fixed point arithmetric, giving us 10 binary decimal points of precision.
    const uint64_t FIXED_PT_SHIFT = 1024;

    const uint64_t delay = (1000000ULL/rateBegin)/2;  // find microseconds to delay

    CMutableTransaction tx;
    uint64_t curTime = GetTime();
    // Wait for our start time
    if (start > curTime)
        sleep(start-curTime);

    SimpleClient sc(host);

    {
        auto now = DateTimeStrFormat("%Y-%m-%d %H:%M:%S", curTime / 1000000);
        printf("%s: Starting %s to %s rate %lu tps .. %lu tps\n", now.c_str(), name.c_str(), host.c_str(), rateBegin, rateEnd);
    }

    rateBegin *= FIXED_PT_SHIFT;
    rateEnd *= FIXED_PT_SHIFT;

    auto uit = utxoIt;
    auto oit = txoIt;
    uint64_t passCount = 0;
    uint64_t count;

    CLeakyBucket rateCtrl(rateBegin+10, rateBegin, rateBegin/2);
    uint64_t stopwatchStart = GetStopwatch();

    while (curTime < end)
    {
        if (rateCtrl.try_leak(FIXED_PT_SHIFT))
        {
            if (passCount == utxoQty)  // at end, outputs now become inputs and start over.
            {
                std::swap(utxoIt, txoIt);
                uit = utxoIt;
                oit = txoIt;
                passCount = 0;
            }

            bool worked = createTx(tx, uit, uit+1, oit, oit+1, fee());
            if (worked)
            {
                CDataStream serializer(SER_NETWORK, PROTOCOL_VERSION);
                serializer << tx;
                sc.SendMessage(TX_MSG, serializer.data(), serializer.size());
            }
            else
            {
                printf("UTXO didn't have enough balance\n");
            }

            count++;
            passCount++;
            uit++;
            oit++;
        }
        else
        {
            curTime = GetTime();
            usleep(delay);
        }
    }

    uint64_t stopwatchEnd = GetStopwatch();
    float elapsedTime = ((float)(stopwatchEnd-stopwatchStart))/1000000000.0;

    {
        auto now = DateTimeStrFormat("%Y-%m-%d %H:%M:%S", GetLogTimeMicros() / 1000000);
        printf("%s: Ending %s to %s. Sent %lu tx in %6.2f sec, rate %6.2f tps\n", now.c_str(), name.c_str(), host.c_str(),count, elapsedTime, ((float)count)/elapsedTime);
    }
}

class Schedule
{
public:
    vector<SchedulePhase> phases;

    void Load(const UniValue& u)
    {
        phases.resize(u.size());
        for(unsigned int idx=0; idx < u.size(); idx++)
        {
            const UniValue& uphase = u[idx];
            phases[idx].Load(uphase);
        }
    }

    /** execute this schedule of transaction generation with the inputs and outputs provided
     *  If the inputs are exhausted, the vectors will be swapped.
     */
    void Execute(std::vector<UTXO>& utxo, std::vector<UTXO>& txo)
    {
        // The simplest thing is just to create thread for every phase and target, and sleep the thread until it
        // should run.

        unsigned int numEntities = 0;  // Let's see how many we have
        for(auto& p: phases)
        {
            numEntities += p.targets.size();
        }

        // This is inefficient in coins, but its simple to just split my utxos evenly among entities
        unsigned int txoPerEntity = utxo.size()/numEntities;

        vector<thread> thrds;
        thrds.reserve(numEntities);

        auto utxoIt = utxo.begin();
        auto txoIt = txo.begin();
        for(auto& p: phases)
        {
            for (vector<ScheduleOp>::iterator t = p.targets.begin(); t != p.targets.end(); ++t)
            {
                thrds.push_back(thread( [=] {
                            GenerateTxs(p.name, t->fee, p.startTime, p.endTime, t->host, t->rateBegin, t->rateEnd, utxoIt, txoIt, txoPerEntity);
                        }));
                utxoIt += txoPerEntity;
                txoIt += txoPerEntity;
            }
        }

        for (auto &t : thrds)
        {
            t.join();
        }
    }
};


int main(int argc, char** argv)
{
    UniValue config;
    try
    {
        config = readJson(readFile("txunami.json"));
    }
    catch(std::length_error &e)
    {
        printf("Error: Missing txunami.json configuration file!\n");
        return -1;
    }

    gc.Load(config["config"]);

    SelectParams(gc.net);
    SHA256AutoDetect();
    ECC_Start();

    std::vector<UTXO> utxo;
    ParseInputCoins(config["coins"], utxo);

    printf("preparation: split coins\n");

    vector<UTXO> txo;
    unsigned int stepSize = 0;
    unsigned int step = 1;
    SimpleClient sc(gc.bitcoind);

    uint64_t start = GetStopwatch();
    uint64_t createTxLoopStart = 0;
    while (stepSize < gc.minUtxos)
    {
        // Split by the maximum allowed per tx, or by the minimum needed to get beyond our gc.minUtxos choice
        unsigned int curSplit = 1;
        if (gc.minUtxos/utxo.size() < gc.splitPerTx)
        {
            curSplit = (gc.minUtxos/utxo.size())+1;
        }
        else
        {
            curSplit = gc.splitPerTx;
        }

        stepSize = utxo.size()*curSplit;
        printf("Step %d: split %lu utxo into %lu, factor %u\n", step, (long unsigned int) utxo.size(), (long unsigned int) stepSize, curSplit);

        txo.resize(stepSize);
        if ((gc.maxThreads > 1)&&(stepSize > gc.maxThreads*100))
        {
            int threadedStep = stepSize/gc.maxThreads;
            vector<thread> thrds;
            thrds.reserve(gc.maxThreads);
            auto st = txo.begin();
            for (unsigned int t = 0; t<gc.maxThreads; t++)
            {
                auto end = st + threadedStep;
                thrds.push_back(thread([st, end] { calcKeys(st, end); }));
                st = end;
            }
            // Do whatever was missed in this thread
            calcKeys(st, txo.end());
            for (auto &t : thrds)
            {
                t.join();
            }
        }
        else
        {
            calcKeys(txo.begin(), txo.end());
        }

        auto txoIdx = txo.begin();

        createTxLoopStart = GetStopwatch();
        CMutableTransaction tx;
        for(vector<UTXO>::iterator u = utxo.begin(); u != utxo.end(); ++u)
        {
            auto txoStart = txoIdx;
            txoIdx += curSplit;
            bool worked = createTx(tx, u, u+1, txoStart, txoIdx, gc.fee());
            if (worked)
            {
                // printf("%s\n", EncodeHexTx(tx).c_str());
                //printf("%s\n", tx.GetHash().ToString().c_str());
                CDataStream serializer(SER_NETWORK, PROTOCOL_VERSION);
                serializer << tx;
                sc.SendMessage(TX_MSG, serializer.data(), serializer.size());
            }
            else
            {
                printf("UTXO didn't have enough balance\n");
            }
        }

        std::swap(txo,utxo);  // get outputs I just created into utxo for the next loop
        step += 1;
    }
    uint64_t end = GetStopwatch();
    
    printf("done in %f4.2 sec\n", ((float)(end-start))/1000000000.0);
    printf("create tx loop in %6.2f sec\n", ((float)(end-createTxLoopStart))/1000000000.0);

    txo = utxo;  // Copy the vector to send to myself

    // If the "schedule" object exists, load a defined generation sequence.
    // Otherwise we'll run as fast as possible later.
    // I load the schedule now so that any "offset" times are relative to the end beginning of the testing
    // phase -- that is, the coin split step is kept separate.
    Schedule sched;
    bool runAschedule = config.exists("schedule");
    if (runAschedule) sched.Load(config["schedule"]);

    // utxo is what's unspent, txo is where the coins are going
    if (runAschedule)
        sched.Execute(utxo, txo);
    else
    {
        printf("Generate a block <enter>\n");
        string input;
        cin >> input;
        MaxSpeed(gc.bitcoind, utxo, txo);
    }

}

void MaxSpeed(const string& host, vector<UTXO>& utxo, vector<UTXO>& txo)
{
    unsigned int step = 0;

    while(step < 20)
    {
        printf("Iteration %d: spam P2PKH\n", step);
        assert(txo.size() == utxo.size());
        int stepSize = utxo.size();
        int threadedStep = stepSize/gc.maxThreads;
        auto st = txo.begin();
        auto utxoSt = utxo.begin();
        uint64_t start = GetStopwatch();
        vector<thread> thrds;
        if (gc.maxThreads > 1)
        {
        thrds.reserve(gc.maxThreads);
        for (unsigned int t = 0; t<gc.maxThreads; t++)
        {
            auto end = st + threadedStep;
            auto utxoEnd = utxoSt + threadedStep;
            thrds.push_back(thread([host, utxoSt, utxoEnd, st] { SimpleClient sct(host); sendP2PKH(sct, utxoSt, utxoEnd, st); }));
            st = end;
            utxoSt = utxoEnd;
        }
        }
        // Do whatever was missed in this thread
        SimpleClient sc(host);
        sendP2PKH(sc, utxoSt, utxo.end(), st);
        if (gc.maxThreads > 1) for (auto &t : thrds)
        {
            t.join();
        }
        uint64_t end = GetStopwatch();
        float elapsedTime = ((float)(end-start))/1000000000.0;
        printf("Done in %6.2f sec. Rate %8.2f \n", elapsedTime, ((float) stepSize)/elapsedTime );

        std::swap(txo,utxo);  // get outputs I just created into utxo for the next loop
        step += 1;
    }
}

