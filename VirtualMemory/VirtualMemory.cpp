#include <cstdint>
#include "PhysicalMemory.h"

#define SUCCESS 1
#define FAILURE 0
#define ONE_BEFORE_OFFSET 1
#define EMPTY_PORT 0
#define OFFSET_INDEX 0
#define FRAME_NOT_FOUND -1

typedef struct EvictData{
    uint64_t maxCyclicValue = 0;
    word_t frameIndex = 0;
    uint64_t pageIndex = 0;
    word_t evictedParentFrameIndex = 0;
    uint64_t evictedParentOffset = 0;
}EvictData;

// ============================= FUNCTION DECLARATIONS ========================
uint64_t findPhysicalAddress(uint64_t virtualAddress);
void splitToSubAddresses(uint64_t virtualAddress, uint64_t subAddresses[]);
void initFrame(word_t frameToInit);
word_t findNextFrame(word_t currentFrame, word_t notToOverride,
                     word_t sourceLink, int depth, word_t& maxFrame);
word_t getNewFrame (word_t notToOverride, uint64_t targetPageIndex);
void findPageToEvict(EvictData& evictData,
                     uint64_t targetPageIndex,
                     int depth,
                     word_t curFrame,
                     uint64_t curPath,
                     word_t parentFrameIndex,
                     uint64_t parentPort);
// ============================= REQUIRED FUNCTIONS ===========================
void VMinitialize(){
    for(int i = 0; i < RAM_SIZE; ++i){
            PMwrite(i, 0);
    }
}

int VMread(uint64_t virtualAddress, word_t* value){
    if(virtualAddress >= VIRTUAL_MEMORY_SIZE){
        return FAILURE;
    }
    uint64_t physicalAddress = findPhysicalAddress(virtualAddress);
    PMread(physicalAddress, value);
    return SUCCESS;
}


int VMwrite(uint64_t virtualAddress, word_t value){
    if(virtualAddress >= VIRTUAL_MEMORY_SIZE){
        return FAILURE;
    }
    uint64_t physicalAddress = findPhysicalAddress(virtualAddress);
    PMwrite(physicalAddress, value);
    return SUCCESS;
}

// ============================= HELPER FUNCTIONS ===========================

void initFrame(word_t frameToInit)
{
    auto frameAddress = frameToInit * PAGE_SIZE;
    for(int i = 0; i < PAGE_SIZE; i++)
    {
        PMwrite(frameAddress + i, 0);
    }
}


uint64_t findPhysicalAddress(uint64_t virtualAddress)
{
    // debug function to check correctness - check the returned address (see if 9)
    uint64_t relativeAddresses[TABLES_DEPTH + 1];
    splitToSubAddresses(virtualAddress, relativeAddresses);
    word_t parentFrameIndex = 0;
    word_t nextFrameIndex= 0;
    for(int i = TABLES_DEPTH ; i > 0; --i)
    {
        PMread(parentFrameIndex * PAGE_SIZE + relativeAddresses[i], &nextFrameIndex);
        if (nextFrameIndex == EMPTY_PORT){
            word_t nextFrame = getNewFrame(parentFrameIndex, virtualAddress >> OFFSET_WIDTH); //notToOverride,
            if (i == ONE_BEFORE_OFFSET){
                PMrestore (nextFrame, virtualAddress >> OFFSET_WIDTH);
            }
            else{
                initFrame(nextFrame);
            }
            PMwrite(parentFrameIndex * PAGE_SIZE + relativeAddresses[i], nextFrame);
            parentFrameIndex = nextFrame;
        }
        else{
            parentFrameIndex = nextFrameIndex;
        }
    }
    return parentFrameIndex * PAGE_SIZE + relativeAddresses[OFFSET_INDEX];
}

word_t getNewFrame (word_t notToOverride, uint64_t targetPageIndex)
{
    int depth = 0;
    word_t maxFrame = 0;
    //stage 1 - when we find a frame with all values=0 and we can use it
    word_t frame = findNextFrame(0, notToOverride, 0, depth, maxFrame);
    if(frame == FRAME_NOT_FOUND){
        //stage 2 - when we dont find zeroed frame and the RAM is not full and we create new frame
        if(maxFrame + 1 < NUM_FRAMES){
            return maxFrame + 1;
        }else{
            //stage 3 - when we dont find zeroed frame, and the RAM is full and we need to evict some frame out
            EvictData evictData;
            findPageToEvict(evictData, targetPageIndex, 0, 0, 0, 0, 0);
            PMevict(evictData.frameIndex, evictData.pageIndex);
            PMwrite(evictData.evictedParentFrameIndex * PAGE_SIZE + evictData.evictedParentOffset, 0);
            frame = evictData.frameIndex;
        }
    }
    return frame;
}

void findPageToEvict(EvictData& evictData,
                     uint64_t targetPageIndex,
                     int depth,
                     word_t curFrame,
                     uint64_t curPath,
                     word_t parentFrameIndex,
                     uint64_t parentPort){
    //if we are in a leaf
    if(depth == TABLES_DEPTH){
        uint64_t absDistance = targetPageIndex - curPath <= 0 ?
            (curPath - targetPageIndex) :
            (targetPageIndex - curPath);

        uint64_t minVal = NUM_PAGES - absDistance < absDistance ?
            (NUM_PAGES - absDistance) : absDistance;

        if(minVal > evictData.maxCyclicValue){
            evictData.maxCyclicValue = minVal;
            evictData.frameIndex = curFrame;
            evictData.pageIndex = curPath;
            evictData.evictedParentFrameIndex = parentFrameIndex;
            evictData.evictedParentOffset = parentPort;
        }
        return;
    }

    //if we are not in a leaf
    word_t value;
    for(int i = 0; i < PAGE_SIZE; ++i){
        PMread(curFrame * PAGE_SIZE + i, &value);
        if(value != EMPTY_PORT){
            findPageToEvict(evictData, targetPageIndex, depth + 1, value, ((curPath << OFFSET_WIDTH) | i), curFrame, i);
        }
    }

}

word_t findNextFrame(word_t currentFrame, word_t notToOverride, word_t sourceLink, int depth, word_t& maxFrame){
        if(depth == TABLES_DEPTH){
            return FRAME_NOT_FOUND;
        }

        word_t value;
        bool allZero = true;
        for(int i = 0; i < PAGE_SIZE; ++i){
            PMread(currentFrame * PAGE_SIZE + i, &value);
            if(value != EMPTY_PORT){
                allZero = false;
                if(maxFrame < value){
                    maxFrame = value;
                }
                word_t newFrame = findNextFrame(value, notToOverride, (currentFrame * PAGE_SIZE + i), depth + 1, maxFrame);
                if(newFrame != FRAME_NOT_FOUND){
                    return newFrame;
                }
            }
        }
        if(allZero && currentFrame != notToOverride){
            PMwrite(sourceLink, 0);
            return currentFrame;
        }
        return FRAME_NOT_FOUND;
}

void splitToSubAddresses (uint64_t virtualAddress, uint64_t subAddresses[])
{
    //calculating only the offset
    subAddresses[OFFSET_INDEX] = virtualAddress & ((1ll << OFFSET_WIDTH) - 1);
    virtualAddress = virtualAddress >> OFFSET_WIDTH;

    //calculating all sub addresses
    for(int idx = 1; idx <= TABLES_DEPTH; ++idx){
        subAddresses[idx] = virtualAddress & ((PAGE_SIZE - 1));
        virtualAddress = virtualAddress >> OFFSET_WIDTH;
    }
}


