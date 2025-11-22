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

    numLoadVectors = params.numLoadVectors;

    for(ThreadID tid = 0; tid < numThreads; tid++) {
        threadEntries[tid] = 0;
        headptr[tid] = 0;
        tailptr[tid] = 0;

        for(int loadVector = 0; loadVector < numLoadVectors; loadVector++) {
            freeLoadVectors[tid].push_back(loadVector);
        }
    }

    for (ThreadID tid = numThreads; tid < MaxThreads; tid++) {
        maxEntries[tid] = 0;
    }

    even = 1;
    resetState();
}

void ROB::wibPush(ThreadID tid, DynInstPtr instr, std::vector<int> &loadPtrs){

    WIBEntry* wibEntry = new WIBEntry();

    wibEntry->loadPtrs = new int[numLoadVectors];
    wibEntry->instr = instr;

    for(int i=0; i<numLoadVectors; i++){
        wibEntry->loadPtrs[i] = 0;
    }

    while(!loadPtrs.empty()){
        int i = loadPtrs.back();
        loadPtrs.pop_back();

        if(i >= 0){
            if(wibEntry->loadPtrs[i] == 0){
                DPRINTF(ROB, "WIB: [tid:%d] loadPtr %d set for instruction. [sn:%llu]\n", tid, i, wibEntry->instr->seqNum);
                wibEntry->loadPtrs[i] = 1;
                assert(wibEntry->loadPtrs[i]);
            }
        }
    }

    WIB[tid].push_back(wibEntry);
    // DPRINTF(ROB, "[tid:%d] Instruction has been pushed to WIB. [sn:%llu]\n", tid, wibEntry->instr->seqNum);
}

bool 
ROB::instrWaiting(WIBEntry *wibEntry){
    for(int i=0; i<numLoadVectors; i++){
        if(wibEntry->loadPtrs[i] == 1){
            DPRINTF(ROB, "WIB: Instruction waiting on loadPtr %d. [sn:%llu]\n",i, wibEntry->instr->seqNum);
            return true;
        }
    }
    return false;
}

bool
ROB::wibPop(ThreadID tid, int loadPtr, DynInstPtr instr){
    for(auto it = WIB[tid].begin(); it != WIB[tid].end(); it++){
        WIBEntry* wibEntry = *it;
        if(wibEntry->instr == instr){
            WIB[tid].erase(it);
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
    // unsigned banksChecked[2*MaxWidth];


    // for(int i=0; i<2*MaxWidth; i++){
    //     banksChecked[i] = 0;
    // }
    using wibIT = std::list<WIBEntry*>::iterator;
    std::vector<wibIT> banksChecked(2 * MaxWidth, WIB[tid].end());

    findOldestReadyInstrs(tid, banksChecked);
    DPRINTF(ROB, "[tid:%d] WIB Read Cycle - Is even: %d \n", tid, even);
    // Currently doing oldest-first checking within even/odd banks
    auto it = WIB[tid].begin();

    // To do in case just turning off assert doesnt work.
    // Sorting across banks from oldest to youngest. (Not necessary in hardware since its parallel issue but for simulator, it needs dispatch order insertions for squashes)

    // Oldest Priority across half of banks (even or odd)
    for(int bank=!even; bank<2*MaxWidth; bank+=2){
        if((banksChecked[bank] != WIB[tid].end())){
            WIBEntry* wibEntry = *(banksChecked[bank]);
            // DPRINTF(ROB, "[tid:%d] Instruction is ready to re-issue from
            // WIBEntry* wibEntry = *it;
            // int bank = wibEntry->instr->bankNum;

            // DPRINTF(ROB, "[tid:%d] WIB Reading instruction in bank %d, Read: %d. Waiting Status:%d [sn:%llu]\n", tid, bank, banksChecked[bank], instrWaiting(wibEntry),wibEntry->instr->seqNum);
            
            DPRINTF(ROB, "[tid:%d] Instruction is ready to re-issue from WIB. [sn:%llu]\n", tid, wibEntry->instr->seqNum);
            
            readyInstrs.push_back(wibEntry->instr);
            
            //Remove from WIB
            it = WIB[tid].erase(banksChecked[bank]);
            delete[] wibEntry->loadPtrs;
            delete wibEntry;

            // //Break to avoid iterator invalidation
            // break;
        }

        ++it;
    }

    // Alternate between checking even or odd banks each cycle
    even = !even;
}

void
ROB::findOldestReadyInstrs(ThreadID tid, 
        std::vector<std::list<WIBEntry*>::iterator> &banksChecked){
    auto it = WIB[tid].begin();
    DPRINTF(ROB, "[tid:%d] Finding oldest ready instructions in WIB.\n", tid);
    assert(banksChecked[0] == WIB[tid].end());
    // Find Oldest Instruction per bank
    while(it != WIB[tid].end()){
        WIBEntry* wibEntry = *it;
        DPRINTF(ROB, "[tid:%d] Checking instruction in WIB in bank %d. [sn:%llu]\n", tid, wibEntry->instr->bankNum, wibEntry->instr->seqNum);
        int bank = wibEntry->instr->bankNum;
        assert(bank >= 0 && bank < (int)banksChecked.size());

        // DPRINTF(ROB, "[tid:%d] Checking instruction in bank %d. Waiting Status:%d [sn:%llu]\n", tid, bank, instrWaiting(wibEntry),wibEntry->instr->seqNum);
        if(!instrWaiting(wibEntry)){
            if(!(banksChecked[bank] == WIB[tid].end())){
                DPRINTF(ROB, "[tid:%d] Checking instruction for youngest in bank %d. [sn:%llu]\n", tid, bank, wibEntry->instr->seqNum);
                if(wibEntry->instr->seqNum < (*banksChecked[bank])->instr->seqNum){
                    DPRINTF(ROB, "[tid:%d] Setting instruction as youngest in bank %d. [sn:%llu]\n", tid, bank, wibEntry->instr->seqNum);
                    banksChecked[bank] = it;
                    // DPRINTF(ROB, "[tid:%d] Found oldest ready instruction in bank %d. [sn:%llu]\n", tid, bank, wibEntry->instr->seqNum);
                }
            }
            else{
                DPRINTF(ROB, "[tid:%d] Setting instruction as youngest in bank %d. [sn:%llu]\n", tid, bank, wibEntry->instr->seqNum);
                banksChecked[bank] = it;
            }
        }

        ++it;
    }
}

void 
ROB::clearLoadWaiting(ThreadID tid, int loadPtr){
    freeLoadVectors[tid].push_front(loadPtr);
    DPRINTF(ROB, "[tid:%d] Freed WIB Load Vector Pointer: %d\n", tid, loadPtr);
    for(auto it = WIB[tid].begin(); it != WIB[tid].end(); it++){
        // WIBEntry* wibEntry = *it;
        (*it)->loadPtrs[loadPtr] = 0;
        assert(!((*it)->loadPtrs[loadPtr]));
        DPRINTF(ROB, "[tid:%d] Cleared load vector pointer %d for instruction in WIB. [sn:%llu]\n", tid, loadPtr, (*it)->instr->seqNum);
    }
    
}

bool 
ROB::getLoadVectorPtr(ThreadID tid, int &loadPtr){
    if(!freeLoadVectors[tid].empty()){
        loadPtr = freeLoadVectors[tid].front();
        freeLoadVectors[tid].pop_front();
        DPRINTF(ROB, "[tid:%d] Assigned WIB Load Vector Pointer: %d\n", tid, loadPtr);
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

        squashIt[tid] = instList[tid].end();

        squashedSeqNum[tid] = 0;
        doneSquashing[tid] = true;
    }
    numInstsInROB = 0;

    // Initialize the "universal" ROB head & tail point to invalid
    // pointers
    head = instList[0].end();
    tail = instList[0].end();
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
        assert(instList[tid].empty());
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
    return instList[tid].size();
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

    //Set Up headptr if this is the 1st instruction for this thread in ROB
    if (threadEntries[tid] == 0) {
        headptr[tid] = 0;
    }

    tailptr[tid] = (tailptr[tid] + 1) % maxEntries[tid];

    inst->bankNum = tailptr[tid] % (2*issueWidth);

    instList[tid].push_back(inst); // Insert into next bank to model contigous bank access

    //Set Up head iterator if this is the 1st instruction in the ROB
    if (numInstsInROB == 0) {
        head = instList[tid].begin();
        assert((*head) == inst);
    }

    //Must Decrement for iterator to actually be valid  since __.end()
    //actually points to 1 after the last inst
    tail = instList[tid].end();
    tail--;

    inst->setInROB();

    ++numInstsInROB;
    ++threadEntries[tid];
    // tailptr[tid] = (tailptr[tid] + 1) % maxEntries[tid]; //rollover tail ptr at max ROB per thread size

    assert((*tail) == inst);

    DPRINTF(ROB, "[tid:%i] Now has %d instructions.\n", tid,
            threadEntries[tid]);
}

void
ROB::retireHead(ThreadID tid)
{
    stats.writes++;

    assert(numInstsInROB > 0);

    // unsigned int bank_num;
    // get_head_bank(tid, bank_num);

    // Get the head ROB instruction by copying it and remove it from the list
    InstIt head_it = instList[tid].begin();

    DynInstPtr head_inst = std::move(*head_it);
    instList[tid].erase(head_it);

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
        // unsigned int bank_num;
        // get_head_bank(tid, bank_num);
        // InstIt head_it = instList[tid].begin();
        // DynInstPtr head_inst = std::move(*head_it);

        DPRINTF(ROB, "[tid:%u] Checking if head is ready. ROB size=%d. [sn:%llu]\n",
        tid, instList[tid].size(), instList[tid].front()->seqNum);

        assert(!instList[tid].empty());
        // if(instList[tid][bank_num].empty()) {
        //     return false;
        // }
        return instList[tid].front()->readyToCommit();
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
    
    // unsigned int head_bank_num, tail_bank_num;
    // get_tail_bank(tid, tail_bank_num);
    assert(squashIt[tid] != instList[tid].end());

    if ((*squashIt[tid])->seqNum < squashedSeqNum[tid]) {
        DPRINTF(ROB, "[tid:%i] Done squashing instructions.\n",
                tid);

        squashIt[tid] = instList[tid].end();

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

    // get_head_bank(tid, head_bank_num);
    // get_tail_bank(tid, tail_bank_num);

    for (int numSquashed = 0;
         numSquashed < numInstsToSquash &&
         squashIt[tid] != instList[tid].end() &&
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
        wibPop(tid, 0, (*squashIt[tid]));
        
        // This is used only for full flush mode (squash at 0 events 
        // for all threads). Normal squashes wont trigger this condition 
        // due to the > in the loop.
        if (squashIt[tid] == instList[tid].begin()) {
            DPRINTF(ROB, "Reached head of instruction list while "
                    "squashing.\n");

            squashIt[tid] = instList[tid].end();

            doneSquashing[tid] = true;

            return;
        }

        if(countInsts(tid) > 0) {
            InstIt tail_thread = instList[tid].end();
            tail_thread--;

            if ((*squashIt[tid]) == (*tail_thread))
            robTailUpdate = true;
        }

        squashIt[tid]--;
    }


    // Check if ROB is done squashing.
    if( squashIt[tid] != instList[tid].end() )
    {if ((*squashIt[tid])->seqNum <= squashedSeqNum[tid]) {
        DPRINTF(ROB, "[tid:%i] Done squashing instructions.\n",
                tid);

        squashIt[tid] = instList[tid].end();

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
        // unsigned int bank_num, tail_bank_num;
        // get_head_bank(tid, bank_num);
        // get_tail_bank(tid, tail_bank_num);

        if (instList[tid].empty())
            continue;

        if (first_valid) {
            head = instList[tid].begin();
            lowest_num = (*head)->seqNum;
            first_valid = false;
            continue;
        }

        InstIt head_thread = instList[tid].begin();

        DynInstPtr head_inst = (*head_thread);

        assert(head_inst != 0);

        if (head_inst->seqNum < lowest_num) {
            head = head_thread;
            lowest_num = head_inst->seqNum;
        }
    }

    if (first_valid) {
        head = instList[0].end();
    }

}

void
ROB::updateTail()
{   
    DPRINTF(ROB, "Updating global tail ptr.\n");

    // unsigned int bank_num, tail_bank_num;
    // get_head_bank(0, bank_num);
    // get_tail_bank(0, tail_bank_num);

    tail = instList[0].end();
    bool first_valid = true;

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        // get_head_bank(tid, bank_num);
        // get_tail_bank(tid, tail_bank_num);

        if (instList[tid].empty()) {
            continue;
        }

        // If this is the first valid then assign w/out
        // comparison
        if (first_valid) {
            tail = instList[tid].end();
            tail--;
            first_valid = false;
            continue;
        }

        // Assign new tail if this thread's tail is younger
        // than our current "tail high"
        InstIt tail_thread = instList[tid].end();
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
        // unsigned int bank_num, tail_bank_num;
        // get_head_bank(tid, bank_num);
        // get_tail_bank(tid, tail_bank_num);

        // // Setting the squash cursors to the tails of the ROB banks
        // set_squashCursors(tid);

        // // Setting the squash iterator to the tail entry pointer
        // get_squashCursor(tid, tail_bank_num, squashIt[tid]);

        // Setting the bank iterator to the tail bank number
        // set_squashBankIt(tid, tail_bank_num);

        InstIt tail_thread = instList[tid].end();
        tail_thread--;
        
        squashIt[tid] = tail_thread;
        
        doSquash(tid);
    }
}

const DynInstPtr&
ROB::readHeadInst(ThreadID tid)
{
    DPRINTF(ROB, "[tid:%i] Reading Head Instr.\n",
                tid);
    
    if (threadEntries[tid] != 0) {
        // unsigned int bank_num, tail_bank_num;
        // get_head_bank(tid, bank_num);

        InstIt head_thread = instList[tid].begin();
        assert(!(instList[tid].empty()));

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

    // unsigned int bank_num, tail_bank_num;;
    // get_tail_bank(tid, tail_bank_num);
    assert(!(instList[tid].empty()));

    InstIt tail_thread = instList[tid].end();
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

    for (InstIt it = instList[tid].begin(); it != instList[tid].end(); it++) {
        if ((*it)->seqNum == squash_inst) {
            return *it;
        }
    }

    return NULL;
}

} // namespace o3
} // namespace gem5
