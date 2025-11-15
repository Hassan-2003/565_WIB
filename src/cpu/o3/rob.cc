/*
 * Copyright (c) 2012 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2004-2006 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "cpu/o3/rob.hh"

#include <list>

#include "base/logging.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/limits.hh"
#include "debug/Fetch.hh"
#include "debug/ROB.hh"
#include "params/BaseO3CPU.hh"
// #include "base/types.hh"

namespace gem5
{

namespace o3
{

ROB::ROB(CPU *_cpu, const BaseO3CPUParams &params)
    : robPolicy(params.smtROBPolicy),
      cpu(_cpu),
      numEntries(params.numROBEntries),
      squashWidth(params.squashWidth),
      numInstsInROB(0),
      numThreads(params.numThreads),
      stats(_cpu)
{
    DPRINTF(ROB, "Created ROB\n");
    issueWidth = params.issueWidth;
    //Figure out rob policy
    if (robPolicy == SMTQueuePolicy::Dynamic) {
        //Set Max Entries to Total ROB Capacity
        for (ThreadID tid = 0; tid < numThreads; tid++) {
            maxEntries[tid] = numEntries;
        }

    } else if (robPolicy == SMTQueuePolicy::Partitioned) {
        DPRINTF(Fetch, "ROB sharing policy set to Partitioned\n");

        //@todo:make work if part_amt doesnt divide evenly.
        int part_amt = numEntries / numThreads;

        //Divide ROB up evenly
        for (ThreadID tid = 0; tid < numThreads; tid++) {
            maxEntries[tid] = part_amt;
        }

    } else if (robPolicy == SMTQueuePolicy::Threshold) {
        DPRINTF(Fetch, "ROB sharing policy set to Threshold\n");

        int threshold =  params.smtROBThreshold;;

        //Divide up by threshold amount
        for (ThreadID tid = 0; tid < numThreads; tid++) {
            maxEntries[tid] = threshold;
        }
    }

    for(ThreadID tid = 0; tid < numThreads; tid++) {
        threadEntries[tid] = 0;
        headptr[tid] = 0;
        tailptr[tid] = 0;
        for( unsigned bank_num = 0; bank_num < 2*issueWidth; bank_num++) {
            squashCursorIt[tid][bank_num] = instList[tid][bank_num].end();
            searchCursorIt[tid][bank_num] = instList[tid][bank_num].end();
        }
    }

    numLoadVectors = params.numLoadVectors;

    for (ThreadID tid = numThreads; tid < MaxThreads; tid++) {
        maxEntries[tid] = 0;
        for( unsigned loadVector = 0; loadVector < numLoadVectors; loadVector++) {
            freeLoadVectors[tid].push_back(loadVector);
        }
    }

    even = 1;
    resetState();
}

void ROB::get_tail_bank(ThreadID tid, unsigned &bank_num)
{   
    unsigned ptr = tailptr[tid];
    if( ptr == (unsigned)-1 ) {
        bank_num = 0;
        DPRINTF(ROB, "[tid:%d] Getting Tail Bank: %d\n",tid, bank_num);
        return;
    }
    bank_num = ptr%(2*issueWidth);
    DPRINTF(ROB, "[tid:%d] Getting Tail Bank: %d\n",tid, bank_num);
}

void ROB::get_head_bank(ThreadID tid, unsigned &bank_num)
{   
    unsigned ptr = headptr[tid];
    if( ptr == (unsigned)-1 ) {
        bank_num = 0;
        DPRINTF(ROB, "[tid:%d] Getting Head Bank: %d\n",tid, bank_num);
        return;
    }
    bank_num = ptr%(2*issueWidth);
    DPRINTF(ROB, "[tid:%d] Getting Head Bank: %d\n",tid, bank_num);
    return;
}

void 
ROB::set_squashCursors(ThreadID tid)
{
    DPRINTF(ROB, "[tid:%d] Setting cursors to tails\n",tid);
    for (unsigned bank_num = 0; bank_num < 2*issueWidth; bank_num++) {
        squashCursorIt[tid][bank_num] = instList[tid][bank_num].end();
        if(!instList[tid][bank_num].empty())
            {squashCursorIt[tid][bank_num]--;}
    }
}

void 
ROB::set_searchBankIt(ThreadID tid, unsigned bank_num)
{
    searchBankIt[tid] = bank_num;
    DPRINTF(ROB, "[tid:%d] Setting search bank iterator with bank: %d\n",tid, bank_num);
}

void 
ROB::set_squashBankIt(ThreadID tid, unsigned bank_num)
{
    squashBankIt[tid] = bank_num;
    DPRINTF(ROB, "[tid:%d] Setting squash bank iterator with bank: %d\n",tid, bank_num);
}

void 
ROB::increment_searchBankIt(ThreadID tid)
{
    DPRINTF(ROB, "[tid:%d] Incrementing search bank iterator\n",tid);
    searchBankIt[tid] = (searchBankIt[tid]+1) % (2*issueWidth);
}

void 
ROB::decrement_squashBankIt(ThreadID tid)
{
    DPRINTF(ROB, "[tid:%d] Decrementing squash bank iterator\n",tid);
    if(squashBankIt[tid] == 0){
        squashBankIt[tid] = 2*issueWidth - 1;
    } else {
        squashBankIt[tid] = (squashBankIt[tid]-1) % (2*issueWidth);
    }
}

void 
ROB::get_squashCursor(ThreadID tid, unsigned bank_num, InstIt &it)
{   
    DPRINTF(ROB, "[tid:%d] squash cursor now pointing to bank: %d\n",tid, bank_num);
    it = squashCursorIt[tid][bank_num];
}

void 
ROB::get_searchCursor(ThreadID tid, unsigned bank_num, InstIt &it)
{   
    DPRINTF(ROB, "[tid:%d] search cursor now pointing to bank: %d\n",tid, bank_num);
    it = searchCursorIt[tid][bank_num];
}

void 
ROB::set_searchCursors(ThreadID tid)
{
    DPRINTF(ROB, "[tid:%i] Setting cursors to heads.\n",
                tid);
    for (unsigned bank_num = 0; bank_num < 2*issueWidth; bank_num++) {
        searchCursorIt[tid][bank_num] = instList[tid][bank_num].begin();
    }
}

void 
ROB::decrement_cursor(ThreadID tid, unsigned bank_num){
    DPRINTF(ROB, "[tid:%d] Decrementing squash cursor\n",tid);
    // cursorIt[tid][bank_num] = instList[tid][bank_num].end();
    unsigned tail_bank_num;
    if (squashCursorIt[tid][bank_num] != instList[tid][bank_num].begin()) {
        --squashCursorIt[tid][bank_num];
    }else {
        DPRINTF(ROB, "[tid:%d] Finished squash scanning bank %d.\n",tid,bank_num);
        // get_tail_bank(tid, tail_bank_num);
        // squashCursorIt[tid][bank_num] = instList[tid][tail_bank_num].end();
        squashCursorIt[tid][bank_num] = instList[tid][bank_num].end();
    }
}

void 
ROB::increment_cursor(ThreadID tid, unsigned bank_num){
    DPRINTF(ROB, "[tid:%d] Incrementing search cursor\n",tid);
    unsigned tail_bank_num;
    if (searchCursorIt[tid][bank_num] != instList[tid][bank_num].end()) {
        ++searchCursorIt[tid][bank_num];
    }
    else{
        DPRINTF(ROB, "[tid:%d] Finished search scanning bank %d.\n",tid,bank_num);
        // get_tail_bank(tid, tail_bank_num);
        // searchCursorIt[tid][bank_num] = instList[tid][tail_bank_num].end();
        searchCursorIt[tid][bank_num] = instList[tid][bank_num].end();
    }
}

void ROB::get_bank(ThreadID tid, DynInstPtr instr, unsigned &bank_num){
    for(bank_num=0; bank_num<2*MaxWidth; bank_num++){
        for(InstIt it = instList[tid][bank_num].begin(); it != instList[tid][bank_num].end(); it++){
            if(*it = instr){
                return;
            }
        }
    }
}

void ROB::wibPush(ThreadID tid, DynInstPtr instr, std::vector<int> loadPtrs){

    WIBEntry* wibEntry = new WIBEntry;

    wibEntry->loadPtrs = new int[numLoadVectors];
    wibEntry->instr = instr;

    for(int i=0; i<numLoadVectors; i++){
        wibEntry->loadPtrs[i] = 0;
    }

    while(!loadPtrs.empty()){
        int i = loadPtrs.back();
        loadPtrs.pop_back();

        if(wibEntry->loadPtrs[i] == 0){
            wibEntry->loadPtrs[i] = 1;
        }
    }

    unsigned bank_num;
    get_bank(tid, instr, bank_num);

    WIB[tid][bank_num].push_back(wibEntry);
    DPRINTF(ROB, "[tid:%d] Instruction has been pushed to WIB. [sn:%llu]\n", tid, wibEntry->instr->seqNum);
}

bool 
ROB::instrWaiting(int *loadPtrs){
    for(int i=0; i<numLoadVectors; i++){
        if(loadPtrs[i] == 1){
            return true;
        }
    }
    return false;
}

bool
ROB::wibPop(ThreadID tid, int loadPtr, DynInstPtr instr,
                    unsigned bank_num){
    for(auto it = WIB[tid][bank_num].begin(); it != WIB[tid][bank_num].end(); it++){
        WIBEntry* wibEntry = *it;
        if(wibEntry->instr == instr){
            WIB[tid][bank_num].erase(it);
            delete[] wibEntry->loadPtrs;
            delete wibEntry;

            // DPRINTF(ROB, "[tid:%d] Cleared load vector pointer %d for instruction in WIB. [sn:%llu]\n", tid, loadPtr, wibEntry->instr->seqNum);
            return true;
        }
    }
    return false;
}

void 
ROB::readCycle(ThreadID tid, std::list<DynInstPtr> &readyInstrs){
    unsigned initial;
    
    // Alternate between checking even or odd banks each cycle
    if(even){
        initial = 0;
    } else {
        initial = 1;
    }
    even = !even;

    // Currently doing oldest-first checking within even/odd banks
    for(unsigned bank_num=initial; bank_num < 2*issueWidth; bank_num+=2){
        for(auto it = WIB[tid][bank_num].begin(); it != WIB[tid][bank_num].end(); it++){
            WIBEntry* wibEntry = *it;
            if(!instrWaiting(wibEntry->loadPtrs)){
                DPRINTF(ROB, "[tid:%d] Instruction is ready to re-issue from WIB. [sn:%llu]\n", tid, wibEntry->instr->seqNum);
                
                readyInstrs.push_back(wibEntry->instr);

                //Remove from WIB
                WIB[tid][bank_num].erase(it);
                delete[] wibEntry->loadPtrs;
                delete wibEntry;

                //Break to avoid iterator invalidation
                break;
            }
        }
    }
}

void 
ROB::clearLoadWaiting(ThreadID tid, int loadPtr){
    for(unsigned bank_num=0; bank_num < 2*issueWidth; bank_num++){
        for(auto it = WIB[tid][bank_num].begin(); it != WIB[tid][bank_num].end(); it++){
            WIBEntry* wibEntry = *it;
            wibEntry->loadPtrs[loadPtr] = 0;
            freeLoadVectors[tid].push_back(loadPtr);
            DPRINTF(ROB, "[tid:%d] Cleared load vector pointer %d for instruction in WIB. [sn:%llu]\n", tid, loadPtr, wibEntry->instr->seqNum);
        }
    }
}

bool 
ROB::getLoadVectorPtr(ThreadID tid, int &loadPtr){
    if(!freeLoadVectors[tid].empty()){
        loadPtr = freeLoadVectors[tid].back();
        freeLoadVectors[tid].pop_back();
        DPRINTF(ROB, "[tid:%d] Assigned Load Vector Pointer: %d\n", tid, loadPtr);
        return true;
    }

    return false;
}

void
ROB::resetState()
{
    DPRINTF(ROB, "Reseting state.\n");
    for (ThreadID tid = 0; tid  < MaxThreads; tid++) {
        threadEntries[tid] = 0;
        headptr[tid] = -1;
        tailptr[tid] = -1;

        unsigned int tail_bank_num;
        get_tail_bank(tid, tail_bank_num);

        squashIt[tid] = instList[tid][tail_bank_num].end();
        for( unsigned bank_num = 0; bank_num < 2*issueWidth; bank_num++) {
            squashCursorIt[tid][bank_num] = instList[tid][bank_num].end();
            searchCursorIt[tid][bank_num] = instList[tid][bank_num].end();
        }
        squashedSeqNum[tid] = 0;
        doneSquashing[tid] = true;
    }
    numInstsInROB = 0;

    // Initialize the "universal" ROB head & tail point to invalid
    // pointers
    head = instList[0][0].end();
    tail = instList[0][0].end();
}

std::string
ROB::name() const
{
    return cpu->name() + ".rob";
}

void
ROB::setActiveThreads(std::list<ThreadID> *at_ptr)
{
    DPRINTF(ROB, "Setting active threads list pointer.\n");
    activeThreads = at_ptr;
}

void
ROB::drainSanityCheck() const
{
    DPRINTF(ROB, "Performing drain sanity check.\n");
    for (ThreadID tid = 0; tid  < numThreads; tid++)
        for( auto &bank : instList[tid])
            assert(bank.empty());
    assert(isEmpty());
}

void
ROB::takeOverFrom()
{
    resetState();
}

void
ROB::resetEntries()
{
    if (robPolicy != SMTQueuePolicy::Dynamic || numThreads > 1) {
        auto active_threads = activeThreads->size();

        std::list<ThreadID>::iterator threads = activeThreads->begin();
        std::list<ThreadID>::iterator end = activeThreads->end();

        while (threads != end) {
            ThreadID tid = *threads++;

            if (robPolicy == SMTQueuePolicy::Partitioned) {
                maxEntries[tid] = numEntries / active_threads;
            } else if (robPolicy == SMTQueuePolicy::Threshold &&
                       active_threads == 1) {
                maxEntries[tid] = numEntries;
            }
        }
    }
}

int
ROB::entryAmount(ThreadID num_threads)
{
    if (robPolicy == SMTQueuePolicy::Partitioned) {
        return numEntries / num_threads;
    } else {
        return 0;
    }
}

int
ROB::countInsts()
{
    DPRINTF(ROB, "Counting Instructions.\n");
    int total = 0;

    for (ThreadID tid = 0; tid < numThreads; tid++)
        total += countInsts(tid);

    return total;
}

size_t
ROB::countInsts(ThreadID tid)
{
    unsigned sum = 0;
    for(auto &bank : instList[tid]) {
        sum += bank.size();
    }
    return sum;
}

void
ROB::insertInst(const DynInstPtr &inst)
{
    assert(inst);

    stats.writes++;

    DPRINTF(ROB, "Adding inst PC %s to the ROB.\n", inst->pcState());

    assert(numInstsInROB != numEntries);

    ThreadID tid = inst->threadNumber;
    assert(threadEntries[tid] < maxEntries[tid]);

    unsigned int bank_num;

    if(threadEntries[tid] == 0) {
        bank_num = -1;
    } else {
        get_tail_bank(tid, bank_num);
        if(bank_num == (2*issueWidth -1)) {
            bank_num = -1;
        }
    }

    instList[tid][bank_num+1].push_back(inst); // Insert into next bank to model contigous bank access

    //Set Up head iterator if this is the 1st instruction in the ROB
    if (numInstsInROB == 0) {
        head = instList[tid][0].begin();
        assert((*head) == inst);
    }

    //Set Up headptr if this is the 1st instruction for this thread in ROB
    if (threadEntries[tid] == 0) {
        headptr[tid] = 0;
    }

    //Must Decrement for iterator to actually be valid  since __.end()
    //actually points to 1 after the last inst
    tail = instList[tid][bank_num+1].end();
    tail--;

    inst->setInROB();

    ++numInstsInROB;
    ++threadEntries[tid];
    tailptr[tid] = (tailptr[tid] + 1) % maxEntries[tid]; //rollover tail ptr at max ROB per thread size

    assert((*tail) == inst);

    DPRINTF(ROB, "[tid:%i] Now has %d instructions.\n", tid,
            threadEntries[tid]);
}

void
ROB::retireHead(ThreadID tid)
{
    stats.writes++;

    assert(numInstsInROB > 0);

    unsigned int bank_num;
    get_head_bank(tid, bank_num);

    // Get the head ROB instruction by copying it and remove it from the list
    InstIt head_it = instList[tid][bank_num].begin();

    DynInstPtr head_inst = std::move(*head_it);
    instList[tid][bank_num].erase(head_it);

    // if(!instList[tid][bank_num].empty()) {
    //     assert(!(head_inst == instList[tid][bank_num].front()));
    // }

    assert(head_inst->readyToCommit());

    DPRINTF(ROB, "[tid:%i] Retiring head instruction, "
            "instruction PC %s, [sn:%llu]\n", tid, head_inst->pcState(),
            head_inst->seqNum);

    --numInstsInROB;
    --threadEntries[tid];

    if(threadEntries[tid]==0) {
        //ROB is now empty for this thread
        headptr[tid] = -1;
        tailptr[tid] = -1;
    } else
    {   headptr[tid] = (headptr[tid] + 1) % maxEntries[tid]; //rollover head ptr at max ROB per thread size
    }

    head_inst->clearInROB();
    head_inst->setCommitted();

    //Update "Global" Head of ROB
    updateHead();

    // @todo: A special case is needed if the instruction being
    // retired is the only instruction in the ROB; otherwise the tail
    // iterator will become invalidated.
    cpu->removeFrontInst(head_inst);
}

bool
ROB::isHeadReady(ThreadID tid)
{
    stats.reads++;
    if (threadEntries[tid] != 0) {
        unsigned int bank_num;
        get_head_bank(tid, bank_num);
        DPRINTF(ROB, "[tid:%u] Checking if head is ready. Head bank: %u size=%d\n",
        tid, bank_num, instList[tid][bank_num].size());

        assert(!instList[tid][bank_num].empty());
        // if(instList[tid][bank_num].empty()) {
        //     return false;
        // }
        return instList[tid][bank_num].front()->readyToCommit();
    }

    return false;
}

bool
ROB::canCommit()
{
    //@todo: set ActiveThreads through ROB or CPU
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();
    DPRINTF(ROB, "Checking if ROB can commit from any thread.\n");
    while (threads != end) {
        ThreadID tid = *threads++;

        if (isHeadReady(tid)) {
            return true;
        }
    }

    return false;
}

unsigned
ROB::numFreeEntries()
{
    return numEntries - numInstsInROB;
}

unsigned
ROB::numFreeEntries(ThreadID tid)
{
    return maxEntries[tid] - threadEntries[tid];
}

void
ROB::doSquash(ThreadID tid)
{
    stats.writes++;
    DPRINTF(ROB, "[tid:%i] Squashing instructions until [sn:%llu].\n",
            tid, squashedSeqNum[tid]);
    
    unsigned int head_bank_num, tail_bank_num;
    get_tail_bank(tid, tail_bank_num);
    assert(squashIt[tid] != instList[tid][tail_bank_num].end());

    if ((*squashIt[tid])->seqNum < squashedSeqNum[tid]) {
        DPRINTF(ROB, "[tid:%i] Done squashing instructions.\n",
                tid);

        squashIt[tid] = instList[tid][tail_bank_num].end();

        doneSquashing[tid] = true;
        return;
    }

    bool robTailUpdate = false;

    // unsigned int numInstsToSquash = std::min(squashWidth,2*issueWidth);
    unsigned int numInstsToSquash = squashWidth;

    // If the CPU is exiting, squash all of the instructions
    // it is told to, even if that exceeds the squashWidth.
    // Set the number to the number of entries (the max).
    if (cpu->isThreadExiting(tid))
    {
        numInstsToSquash = numEntries;
    }

    get_head_bank(tid, head_bank_num);
    get_tail_bank(tid, tail_bank_num);

    for (int numSquashed = 0;
         numSquashed < numInstsToSquash &&
         squashIt[tid] != instList[tid][tail_bank_num].end() &&
         (*squashIt[tid])->seqNum > squashedSeqNum[tid];
         ++numSquashed)
    {
        DPRINTF(ROB, "[tid:%i] Squashing instruction PC %s, seq num %i.\n",
                (*squashIt[tid])->threadNumber,
                (*squashIt[tid])->pcState(),
                (*squashIt[tid])->seqNum);

        // Mark the instruction as squashed, and ready to commit so that
        // it can drain out of the pipeline.
        (*squashIt[tid])->setSquashed();

        (*squashIt[tid])->setCanCommit();

        // Clear any WIB entries for this instruction
        wibPop(tid, 0, (*squashIt[tid]), squashBankIt[tid]);
        
        // This is used only for full flush mode (squash at 0 events 
        // for all threads). Normal squashes wont trigger this condition 
        // due to the > in the loop.
        if (squashIt[tid] == instList[tid][head_bank_num].begin()) {
            DPRINTF(ROB, "Reached head of instruction list while "
                    "squashing.\n");

            squashIt[tid] = instList[tid][tail_bank_num].end();

            doneSquashing[tid] = true;

            return;
        }

        if(countInsts(tid) > 0) {
            InstIt tail_thread = instList[tid][tail_bank_num].end();
            tail_thread--;

            if ((*squashIt[tid]) == (*tail_thread))
            robTailUpdate = true;
        }
        // InstIt tail_thread = instList[tid][tail_bank_num].end();
        // tail_thread--;

        // if ((*squashIt[tid]) == (*tail_thread))
        //     robTailUpdate = true;

        
        // squashIt[tid]--;

        // Get current bank number
        unsigned bank_num = squashBankIt[tid];
        // Update cursor for the bank we just squashed from
        decrement_cursor(tid, bank_num);

        // Get the next bank and bank instr pointer to squash
        decrement_squashBankIt(tid);
        bank_num = squashBankIt[tid];
        get_squashCursor(tid, bank_num, squashIt[tid]);
    }


    // Check if ROB is done squashing.
    if( squashIt[tid] != instList[tid][tail_bank_num].end() )
    {if ((*squashIt[tid])->seqNum <= squashedSeqNum[tid]) {
        DPRINTF(ROB, "[tid:%i] Done squashing instructions.\n",
                tid);

        squashIt[tid] = instList[tid][tail_bank_num].end();

        doneSquashing[tid] = true;
    }
    }

    if (robTailUpdate) {
        updateTail();
    }
}


void
ROB::updateHead()
{

    DPRINTF(ROB, "Updating global head ptr.\n");
    InstSeqNum lowest_num = 0;
    bool first_valid = true;

    // @todo: set ActiveThreads through ROB or CPU
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;
        unsigned int bank_num, tail_bank_num;
        get_head_bank(tid, bank_num);
        get_tail_bank(tid, tail_bank_num);

        if (instList[tid][bank_num].empty())
            continue;

        if (first_valid) {
            head = instList[tid][bank_num].begin();
            lowest_num = (*head)->seqNum;
            first_valid = false;
            continue;
        }

        InstIt head_thread = instList[tid][bank_num].begin();

        DynInstPtr head_inst = (*head_thread);

        assert(head_inst != 0);

        if (head_inst->seqNum < lowest_num) {
            head = head_thread;
            lowest_num = head_inst->seqNum;
        }
    }

    if (first_valid) {
        head = instList[0][0].end();
    }

}

void
ROB::updateTail()
{   
    DPRINTF(ROB, "Updating global tail ptr.\n");

    unsigned int bank_num, tail_bank_num;
    get_head_bank(0, bank_num);
    get_tail_bank(0, tail_bank_num);

    tail = instList[0][tail_bank_num].end();
    bool first_valid = true;

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        get_head_bank(tid, bank_num);
        get_tail_bank(tid, tail_bank_num);

        if (instList[tid][tail_bank_num].empty()) {
            continue;
        }

        // If this is the first valid then assign w/out
        // comparison
        if (first_valid) {
            tail = instList[tid][tail_bank_num].end();
            tail--;
            first_valid = false;
            continue;
        }

        // Assign new tail if this thread's tail is younger
        // than our current "tail high"
        InstIt tail_thread = instList[tid][tail_bank_num].end();
        tail_thread--;

        if ((*tail_thread)->seqNum > (*tail)->seqNum) {
            tail = tail_thread;
        }
    }
}


void
ROB::squash(InstSeqNum squash_num, ThreadID tid)
{
    if (isEmpty(tid)) {
        DPRINTF(ROB, "Does not need to squash due to being empty "
                "[sn:%llu]\n",
                squash_num);

        return;
    }

    DPRINTF(ROB, "Starting to squash within the ROB.\n");

    robStatus[tid] = ROBSquashing;

    doneSquashing[tid] = false;

    squashedSeqNum[tid] = squash_num;

    if (countInsts(tid) > 0) {
        unsigned int bank_num, tail_bank_num;
        get_head_bank(tid, bank_num);
        get_tail_bank(tid, tail_bank_num);

        // Setting the squash cursors to the tails of the ROB banks
        set_squashCursors(tid);

        // Setting the squash iterator to the tail entry pointer
        get_squashCursor(tid, tail_bank_num, squashIt[tid]);

        // Setting the bank iterator to the tail bank number
        set_squashBankIt(tid, tail_bank_num);
        
        doSquash(tid);
    }
}

const DynInstPtr&
ROB::readHeadInst(ThreadID tid)
{
    DPRINTF(ROB, "[tid:%i] Reading Head Instr.\n",
                tid);
    
    if (threadEntries[tid] != 0) {
        unsigned int bank_num, tail_bank_num;
        get_head_bank(tid, bank_num);

        InstIt head_thread = instList[tid][bank_num].begin();
        assert(!(instList[tid][bank_num].empty()));

        assert((*head_thread)->isInROB());

        return *head_thread;
    } else {
        return dummyInst;
    }
}

DynInstPtr
ROB::readTailInst(ThreadID tid)
{   
    DPRINTF(ROB, "[tid:%i] Reading Tail Instr.\n",
                tid);

    unsigned int bank_num, tail_bank_num;;
    get_tail_bank(tid, tail_bank_num);
    assert(!(instList[tid][tail_bank_num].empty()));

    InstIt tail_thread = instList[tid][tail_bank_num].end();
    tail_thread--;

    return *tail_thread;
}

ROB::ROBStats::ROBStats(statistics::Group *parent)
  : statistics::Group(parent, "rob"),
    ADD_STAT(reads, statistics::units::Count::get(),
        "The number of ROB reads"),
    ADD_STAT(writes, statistics::units::Count::get(),
        "The number of ROB writes")
{
}

DynInstPtr
ROB::findInst(ThreadID tid, InstSeqNum squash_inst)
{
    DPRINTF(ROB, "[tid:%i] Checking for squash instruction with seq. number: %d\n",
                tid, squash_inst);

    unsigned int head_bank_num, tail_bank_num;
    get_head_bank(tid, head_bank_num);
    get_tail_bank(tid, tail_bank_num);

    set_searchBankIt(tid, head_bank_num);
    unsigned cur_bank_num = searchBankIt[tid];

    InstIt it = instList[tid][head_bank_num].begin(); 
    set_searchCursors(tid);

    while(it != instList[tid][tail_bank_num].end()){// && it != instList[tid][cur_bank_num].end()) {
        DPRINTF(ROB, "[tid:%i] Bank %d has %d instructions.\n",
                tid, cur_bank_num, instList[tid][cur_bank_num].size());
        if ((*it)->seqNum == squash_inst) {
            return *it;
        }

        increment_cursor(tid, cur_bank_num);
        increment_searchBankIt(tid);
        cur_bank_num = searchBankIt[tid];
        get_searchCursor(tid, cur_bank_num, it);
    }


    return NULL;
}

} // namespace o3
} // namespace gem5
