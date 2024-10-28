#include <iostream>
#include <fstream>
#include "zlib.h"
#include <list>
#include <fcntl.h>
#include <sys/stat.h>
#include <cstring>
#include <csignal>

typedef struct {
    int height;        //Incremental ID of the block in the chain
    int timestamp;    //Time of the mine in seconds since epoch
    unsigned int hash;        //Current block hash value
    unsigned int prev_hash;    //Hash value of the previous block
    int difficulty;    //Amount of preceding zeros in the hash
    int nonce;        //Incremental integer to change the hash value
    int relayed_by;    //Miner ID
} BLOCK_T;

unsigned int calculateChecksum(const BLOCK_T& block);
bool validateDifficulty(unsigned int checksum);
BLOCK_T createGenesisBlock();
BLOCK_T createNewBlock(const BLOCK_T& minedBlock, const std::list<BLOCK_T>& blockchain);
void readDifficulty(std::ofstream& logFile);
void setDefaultDifficulty(std::ofstream& logFile);
void sendBlockToMiner(int minerPipeFD, const BLOCK_T& newBlock);
bool blockIsValid(unsigned int checksum, const std::list<BLOCK_T>& blockchain, const BLOCK_T& minedBlock, std::ofstream& logFile);
void handleSubscriptionRequest(const char* buffer, std::list<int>& minersJoined, std::ofstream& logFile);
void handleMinedBlockRequest(const char* buffer, std::list<BLOCK_T>& blockchain, std::ofstream& logFile);
void log_message(std::ofstream& logFile, const std::string& message);
void server_func();
void sigint_handler(int signum);

int difficulty = 0;
BLOCK_T blockToMine = {};
const std::string block_header = "BLK:";
const std::string subscription_header = "SUB:";
const char* server_pipe_name = "/mnt/mta/server_pipe";
const char* server_log_name = "/var/log/mtacoin.log";
const char* config_path = "/mnt/mta/mtacoin.conf";
const int header_length = 4;
//list of pairs (miner id, pipe fd)
std::list<std::pair<int,int>> miner_subscribed;

unsigned int calculateChecksum(const BLOCK_T &block) { //calculates checksum based on block's fields
    //initialize crc
    uLong crc = crc32(0L, Z_NULL, 0);

    crc = crc32(crc, reinterpret_cast<const Bytef *>(&block.height), sizeof(block.height));
    crc = crc32(crc, reinterpret_cast<const Bytef *>(&block.timestamp), sizeof(block.timestamp));
    crc = crc32(crc, reinterpret_cast<const Bytef *>(&block.prev_hash), sizeof(block.prev_hash));
    crc = crc32(crc, reinterpret_cast<const Bytef *>(&block.nonce), sizeof(block.nonce));
    crc = crc32(crc, reinterpret_cast<const Bytef *>(&block.relayed_by), sizeof(block.relayed_by));

    return crc;
}

bool validateDifficulty(unsigned int checksum) { //validates given checksum based on difficulty (number of leading zeroes)
    int numOfLeadingZeroes = 0;

    //count number of leading zeroes
    for (int i = 31; i >= 0; i--) {
        //shift right i bits AND 1
        if ((checksum >> i) & 1)
            break;
        numOfLeadingZeroes++;
    }
    //valid checksum
    if (numOfLeadingZeroes >= difficulty)
        return true;

    return false;
}

BLOCK_T createGenesisBlock() {
    //create genesis block
    BLOCK_T genesisBlock = {};

    genesisBlock.difficulty = difficulty;
    genesisBlock.prev_hash = 0;
    genesisBlock.height = 0;
    genesisBlock.hash = 0xAAAAAAAA;
    genesisBlock.timestamp = static_cast<int>(time(nullptr));
    genesisBlock.relayed_by = -1;
    genesisBlock.nonce = 0;

    return genesisBlock;
}

BLOCK_T createNewBlock(const BLOCK_T& minedBlock, const std::list<BLOCK_T>& blockchain){
    BLOCK_T newBlock;
    newBlock = {};
    newBlock.prev_hash = minedBlock.hash;
    newBlock.height = (int) blockchain.size();
    newBlock.difficulty = difficulty;

    return newBlock;
}

void readDifficulty(std::ofstream& logFile) {
    //open config file
    int configFD = open(config_path, O_RDONLY);

    if (configFD == -1) {
        log_message(logFile, std::string("ERROR: No configuration file"));
        setDefaultDifficulty(logFile);
    }
    else {
        char buffer[256];
        size_t bytesRead;

        //log reading attempt and read from file
        log_message(logFile, "Reading " + std::string(config_path) + "..");
        bytesRead = read(configFD, buffer, 256);

        if (bytesRead == -1) {
            log_message(logFile, std::string("ERROR: No data in configuration file"));
            setDefaultDifficulty(logFile);
        }
        else {
            buffer[bytesRead] = '\0';
            //search for expected template "DIFFICULTY="
            //and get pointer to index of equal sign
            char *equalsSign = strstr(buffer, "DIFFICULTY") ? strchr(buffer, '=') : nullptr;

            //if found, parse number after equal sign
            if (equalsSign) {
                char* endPtr;
                long tempDiff = strtol(equalsSign + 1, &endPtr, 10);

                if (equalsSign + 1 == endPtr) {
                    log_message(logFile, std::string("ERROR: No value for difficulty in config file"));
                    setDefaultDifficulty(logFile);
                }
                else if (tempDiff >= 0 && tempDiff <= 31) {
                    log_message(logFile, std::string("Difficulty set to " + std::to_string(tempDiff)));
                    difficulty = static_cast<int>(tempDiff);
                }
                else{
                    log_message(logFile, std::string("ERROR: Difficulty must be between 0 and 31"));
                    setDefaultDifficulty(logFile);
                }
            }
            else { //invalid data in config file
                log_message(logFile, std::string("ERROR: Invalid format in config file"));
                setDefaultDifficulty(logFile);
            }
        }

        close(configFD);
    }
}

void sendBlockToMiner(int minerPipeFD, const BLOCK_T &newBlock) {
        write(minerPipeFD, &newBlock, sizeof(BLOCK_T));
}

void setDefaultDifficulty(std::ofstream& logFile) {
    log_message(logFile, std::string("Setting difficulty to 16 (default)"));
    difficulty = 16;
}

bool blockIsValid(unsigned int checksum, const std::list<BLOCK_T>& blockchain, const BLOCK_T& minedBlock, std::ofstream& logFile) { //validate mined block
    char buffer[512];

    //validate difficulty
    if (!validateDifficulty(checksum)) {
        std::sprintf(buffer, "Server: Miner #%d provided bad hash (0x%x) for block.", minedBlock.relayed_by, minedBlock.hash);
        log_message(logFile, std::string(buffer));
        return false;
    }

    //validate checksum
    if (checksum != minedBlock.hash) {
        std::sprintf(buffer, "Server: Miner #%d provided hash (0x%x) but server calculated (0x%x).", minedBlock.relayed_by, minedBlock.hash, checksum);
        log_message(logFile, std::string(buffer));
        return false;
    }

    //validate height and prev_hash
    if (minedBlock.prev_hash != blockchain.front().hash || minedBlock.height != blockToMine.height) {
        std::sprintf(buffer, "Server: Miner #%d provided incorrect prev_hash (0x%x), does not reference most recent block in blockchain (0x%x).", minedBlock.relayed_by, minedBlock.prev_hash, blockchain.front().hash);
        log_message(logFile, std::string(buffer));
        return false;
    }

    return true;
}

void handleSubscriptionRequest(const char* buffer, std::list<int>& minersJoined, std::ofstream& logFile) {
    std::string lineRead(buffer);
    std::string prefix = "#";
    std::size_t pos = lineRead.find(prefix);

    if (pos != std::string::npos) {
        pos += 1;
        int minerID = lineRead[pos] - '0';
        std::string pipeName = "/mnt/mta/miner_pipe_";
        std::string currentPipeName = pipeName + std::to_string(minerID);

        int minerPipeFD = open(currentPipeName.c_str(), O_WRONLY);

        if (minerPipeFD == -1) {
            log_message(logFile, "Error opening miner #" + std::to_string(minerID) + " pipe");
            return;
        }

        //add miner pipe fd to list of miners fd's
        //add miner id to miners joined
        miner_subscribed.emplace_back(minerID, minerPipeFD);
        minersJoined.push_back(minerID);
        log_message(logFile, "Received connection request from miner #" + std::to_string(minerID) + ", pipe name: " + "/mnt/mta/miner_pipe_" + std::to_string(minerID));
        sendBlockToMiner(minerPipeFD, blockToMine);
    }
}

void handleMinedBlockRequest(const char* buffer, std::list<BLOCK_T>& blockchain, std::ofstream& logFile) {
    BLOCK_T minedBlock;
    memcpy(&minedBlock, buffer + block_header.length(), sizeof(BLOCK_T));
    unsigned int checksum = calculateChecksum(minedBlock);

    //validate block
    if (blockIsValid(checksum, blockchain, minedBlock, logFile)) {
        char logBuffer[256];
        std::sprintf(logBuffer, "Server: New block added by %d, attributes: height(%d), timestamp (%d), hash(0x%x), prev_hash(0x%x), difficulty(%d), nonce(%d)",
                     minedBlock.relayed_by, minedBlock.height, minedBlock.timestamp, minedBlock.hash, minedBlock.prev_hash, difficulty, minedBlock.nonce);
        log_message(logFile, std::string(logBuffer));

        //add mined block to blockchain
        blockchain.push_front(minedBlock);
        //create new block and send to miners pipe
        blockToMine = createNewBlock(minedBlock, blockchain);

        //send new block to subscribed miners
        for (auto& minerData : miner_subscribed) {
            sendBlockToMiner(minerData.second, blockToMine);
        }
    }
}

void log_message(std::ofstream& logFile, const std::string& message) {
    logFile << message << std::endl;
}

void server_func() {
    std::ofstream logFile(server_log_name);

    if (!logFile.is_open()) {
        std::cerr << "Failed to open log file: " << server_log_name << std::endl;
        return;
    }

    readDifficulty(logFile);
    mkfifo(server_pipe_name, 0666);

    int serverPipeFD = open(server_pipe_name, O_RDONLY);

    if (serverPipeFD == -1) {
        log_message(logFile, std::string("ERROR: Failed to open server pipe"));
        logFile.close();
        return;
    }

    log_message(logFile, "Listening on " + std::string(server_pipe_name));

    char buffer[256];
    std::list<int> minersJoined;
    std::list<BLOCK_T> blockchain;
    BLOCK_T genesisBlock = createGenesisBlock();

    //add genesis block to blockchain
    blockchain.push_front(genesisBlock);

    //create new block to mine based on genesis' data
    blockToMine.prev_hash = blockchain.front().hash;
    blockToMine.height = (int) blockchain.size();
    blockToMine.difficulty = difficulty;

    while (true) {
        size_t bytesRead = read(serverPipeFD, buffer, 256);

        if (bytesRead > 0) {
            //check which type of data was read by checking the header
            if (strncmp(buffer, subscription_header.c_str(), header_length) == 0)
            {
                buffer[bytesRead] = '\0';
                handleSubscriptionRequest(buffer, minersJoined, logFile);
            }
            else if (strncmp(buffer, block_header.c_str(), header_length) == 0)
            {
                handleMinedBlockRequest(buffer, blockchain, logFile);
            }
        }
    }

    logFile.close();
    for(auto& minerData : miner_subscribed)
        close(minerData.second);
    close(serverPipeFD);
}

void sigint_handler(int signum) {
    exit(1);
}

int main() {
    signal(SIGINT, sigint_handler);
    server_func();

    return 0;
}