#include <iostream>
#include "zlib.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <cstring>
#include <fstream>
#include <csignal>

typedef struct {
    int height;        // Incremental ID of the block in the chain
    int timestamp;    // Time of the mine in seconds since epoch
    unsigned int hash;        // Current block hash value
    unsigned int prev_hash;    // Hash value of the previous block
    int difficulty;    // Amount of preceding zeros in the hash
    int nonce;        // Incremental integer to change the hash value
    int relayed_by;    // Miner ID
} BLOCK_T;

bool validateDifficulty(unsigned int checksum);
unsigned int calculateChecksum(const BLOCK_T& block);
int findNextAvailableMinerID();
bool subscribeAndRequestBlock(int minerID, const std::string& minerPipeName, int serverPipeFD, std::ofstream& logFile);
BLOCK_T tryToGetNewBlock(int minerPipeFD);
void log_message(std::ofstream& logFile, const std::string& message);
void miner_func();
void sigint_handler(int signum);

BLOCK_T blockToMine;
int difficulty = 0;
const std::string minerPipePrefix = "/mnt/mta/miner_pipe_";
const std::string serverPipeName = "/mnt/mta/server_pipe";
const std::string miner_log_path = "/var/log/mtacoin.log";

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

int findNextAvailableMinerID() {

    for (int i = 1; i <= 4; i++) { //maximum of 4 miners
        std::string pipeNameToCheck = minerPipePrefix + std::to_string(i);

        //check if pipe doesn't exist
        if (access(pipeNameToCheck.c_str(), F_OK) == -1) {
            return i;
        }
    }

    return -1; //no available ID's
}

bool subscribeAndRequestBlock(int minerID, const std::string& minerPipeName, int serverPipeFD, std::ofstream& logFile) {
    std::string message = "Miner #" + std::to_string(minerID) + " sent connect request on " + minerPipeName;
    //add "SUB:" header to request and log message
    write(serverPipeFD, ("SUB:" + message).c_str(), message.length());
    log_message(logFile, message);

    //open pipe and wait for response from server
    int minerPipeFD = open(minerPipeName.c_str(), O_RDONLY);

    if (minerPipeFD == -1){
        log_message(logFile, std::string("ERROR: Failed to open miner #" + std::to_string(minerID) + " pipe"));
        return false;
    }

    read(minerPipeFD, &blockToMine, sizeof(BLOCK_T));
    //log the block received data
    char logBuffer[512];
    std::sprintf(logBuffer, "Miner #%d received first block: relayed_by(%d), height(%d), timestamp (%d), hash(0x%x), prev_hash(0x%x), difficulty(%d), nonce(%d)",
                 minerID, blockToMine.relayed_by, blockToMine.height, blockToMine.timestamp, blockToMine.hash, blockToMine.prev_hash, blockToMine.difficulty, blockToMine.nonce);
    log_message(logFile, std::string(logBuffer));

    close(minerPipeFD);

    return true;
}

BLOCK_T tryToGetNewBlock(int minerPipeFD) {
    BLOCK_T block = {};

    read(minerPipeFD, &block, sizeof(BLOCK_T));

    return block;
}

void log_message(std::ofstream& logFile, const std::string& message) {
    logFile << message << std::endl;
}

void miner_func() {
    int minerID = findNextAvailableMinerID();

    if (minerID == -1)
    {
        std::cerr << "ERROR: No available ID's" << std::endl;
        return;
    }

    std::ofstream logFile(miner_log_path);

    if (!logFile.is_open()) {
        std::cerr << "Failed to open log file: " << miner_log_path << std::endl;
        return;
    }

    //create miner pipe
    std::string minerPipeName = minerPipePrefix + std::to_string(minerID);
    mkfifo(minerPipeName.c_str(), 0666);

    //open server pipe
    int serverPipeFD = open(serverPipeName.c_str(), O_WRONLY);

    if (serverPipeFD == -1){
        log_message(logFile, std::string("ERROR: Failed to open server pipe"));
        logFile.close();
        return;
    }

    //send connect request and get block from server
    if (!subscribeAndRequestBlock(minerID, minerPipeName, serverPipeFD, logFile)) {
        logFile.close();
        close(serverPipeFD);
        return;
    }

    //open miner pipe with non-blocking calls
    int minerPipeFD = open(minerPipeName.c_str(), O_RDONLY | O_NONBLOCK);

    if (minerPipeFD == -1){
        log_message(logFile, std::string("ERROR: Failed to open miner #" + std::to_string(minerID) + " pipe"));
        logFile.close();
        close(serverPipeFD);
        return;
    }

    //create buffer with fixed header symbolizing block data being sent
    char buffer[sizeof(BLOCK_T) + 5] = {'B', 'L', 'K', ':'};
    char logBuffer[256];

    while (true)
    {
        blockToMine.relayed_by = minerID;
        blockToMine.nonce = 0;
        difficulty = blockToMine.difficulty;

        while (true) {
            blockToMine.nonce++;
            blockToMine.timestamp = static_cast<int>(time(nullptr));
            blockToMine.hash = calculateChecksum(blockToMine);

            //validate mined block difficulty
            if (validateDifficulty(blockToMine.hash)) {
                std::sprintf(logBuffer, "Miner #%d mined a new block #%d, with the hash 0x%x, difficulty %d", minerID, blockToMine.height, blockToMine.hash, blockToMine.difficulty);
                log_message(logFile, std::string(logBuffer));
                //add block data after header ("BLK:")
                memcpy(buffer + 4, &blockToMine, sizeof(BLOCK_T));
                //write to server's pipe
                write(serverPipeFD, buffer, sizeof(buffer));
            }
            //check if new block was sent
            //if so, go to beginning of outside loop and process again
            BLOCK_T block = tryToGetNewBlock(minerPipeFD);

            if (block.height != 0 && block.height != blockToMine.height) {
                //update to the newly received block and log accordingly
                blockToMine = block;
                std::sprintf(logBuffer, "Miner #%d received a new block: height(%d), timestamp (%d), hash(0x%x), prev_hash(0x%x), difficulty(%d), nonce(%d)",
                             minerID, blockToMine.height, blockToMine.timestamp, blockToMine.hash, blockToMine.prev_hash, blockToMine.difficulty, blockToMine.nonce);
                log_message(logFile, std::string(logBuffer));
                break;
            }
        }
    }

    logFile.close();
    close(serverPipeFD);
    close(minerPipeFD);
}

void sigint_handler(int signum) {
    exit(1);
}

int main()
{
    signal(SIGINT, sigint_handler);
    miner_func();

    return 0;
}
