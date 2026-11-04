#include "budget.h"
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace thumbgen::detail;
using State=Admission::State;
static void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
template<class Exception,class Function> static void rejects(Function fn) {
    try {fn();}catch(const Exception&){return;}
    throw std::runtime_error("invalid input was accepted");
}
int main() {
    try {
        // A small crop inside a huge display window must reserve the display canvas.
        auto small=estimate_memory({8,8,8,8,3,3,8,8,4});
        auto padded=estimate_memory({8,8,8192,8192,3,3,1024,1024,4});
        auto sparse=estimate_memory({8,8,8,8,100,3,8,8,4});
        auto grey=estimate_memory({8,8,8,8,1,3,8,8,4});
        require(padded.total>2ULL*1024*1024*1024,"display canvas missing from reservation");
        require(sparse.source>small.source && grey.source<small.source,"channel span ignored");
        require(small.total>small.source+small.working+small.output+small.codec,"worker/margin omitted");
        require(small.total==small.incremental+small.worker && small.incremental==small.source+small.working+small.output+small.codec+small.margin,"incremental buffers or fixed worker allowance counted incorrectly");
        require(small.codec>0,"JPEG encoded destination omitted");
        auto doubles=ImageFootprint{8,8,8,8,3,4,8,8,4,8};
        auto floats=doubles;floats.source_sample_bytes=4;
        require(estimate_memory(doubles).source==2*estimate_memory(floats).source,"native DOUBLE source bytes missing");
        rejects<std::invalid_argument>([&]{auto invalid=doubles;invalid.source_sample_bytes=0;estimate_memory(invalid);});
        rejects<std::invalid_argument>([&]{auto invalid=doubles;invalid.source_sample_bytes=9;estimate_memory(invalid);});
        rejects<std::invalid_argument>([]{estimate_memory({8,8,8,8,3,3,16,16,4});});
        rejects<std::invalid_argument>([]{estimate_memory({0,8,8,8,3,3,8,8,4});});
        rejects<std::overflow_error>([]{estimate_memory({std::numeric_limits<std::uint64_t>::max(),2,8,8,3,3,8,8,4});});
        // Source arithmetic fits, but the simultaneous-buffer sum must also be checked.
        rejects<std::overflow_error>([]{estimate_memory({1ULL<<29,1ULL<<29,1ULL<<29,1ULL<<29,8,3,1,1,4});});
        rejects<std::invalid_argument>([]{BudgetQueue({0,4,100});});
        BudgetQueue q({4,8,1000});
        require(q.threads_for(8)==2 && q.threads_for(1)==1,"global thread budget ignored");
        q.push({"first",8,600});q.push({"large",8,700});q.push({"small",8,100});
        require(q.take().state==State::admitted,"first task not admitted");
        auto blocked=q.take();require(blocked.state==State::waiting && blocked.task.id=="large","FIFO bypassed large task");
        require(q.pending()==2 && q.active()==1 && q.memory_in_use()==600 && q.threads_in_use()==2,"blocked queue changed reservations");
        q.release("first");auto large=q.take();require(large.task.id=="large" && large.state==State::admitted,"large task starved");
        require(q.take().state==State::admitted,"small task not admitted concurrently");
        q.release("small");q.release("large");
        require(q.memory_in_use()==0 && q.threads_in_use()==0 && q.active()==0,"resources leaked");
        rejects<std::invalid_argument>([&]{q.release("large");});
        rejects<std::invalid_argument>([&]{q.push({"first",4,1});});
        // An impossible head must not indefinitely block valid later work.
        q.push({"impossible",8,1001});q.push({"after",8,100});
        require(q.take().state==State::rejected && q.pending()==1,"oversized task not rejected");
        require(q.take().state==State::admitted,"queue did not continue after rejection");
        q.release("after");require(q.take().state==State::empty,"empty queue mismatch");
        // More requested workers than threads cannot reserve zero threads.
        BudgetQueue threads({8,2,100});
        threads.push({"a",256,1});threads.push({"b",256,1});threads.push({"c",256,1});
        require(threads.take().state==State::admitted && threads.take().state==State::admitted,"thread-limited lanes not admitted");
        require(threads.take().state==State::waiting && threads.threads_in_use()==2,"threads oversubscribed");
        // Near-UINT64_MAX budgets cannot wrap when checking a second reservation.
        auto maximum=std::numeric_limits<std::uint64_t>::max();BudgetQueue huge({2,2,maximum});
        huge.push({"a",1,maximum-1});huge.push({"b",1,2});
        require(huge.take().state==State::admitted && huge.take().state==State::waiting,"memory sum overflowed");
        huge.release("a");require(huge.take().state==State::admitted,"large reservation release failed");
        BudgetQueue retained({2,4,1000,200});
        retained.push({"too-large",4,801});retained.push({"fits",4,800});
        require(retained.memory_in_use()==200 && retained.take().state==State::rejected,"idle reserve was not charged");
        require(retained.take().state==State::admitted && retained.memory_in_use()==1000,"fixed reserve admission mismatch");
        retained.release("fits");require(retained.memory_in_use()==200,"release discarded idle/parent reserve");
        retained.push({"lost-worker",1,1});retained.push({"survivor",1,1});
        require(retained.erase_pending("lost-worker") && !retained.erase_pending("lost-worker"),"pending fault removal mismatch");
        rejects<std::invalid_argument>([&]{retained.push({"lost-worker",1,1});});
        require(retained.take().task.id=="survivor" && !retained.erase_pending("survivor"),"removal changed active reservations");
        retained.release("survivor");
        rejects<std::invalid_argument>([]{BudgetQueue({1,1,100,101});});
        std::cout<<"budget: overflow, canvas/span, FIFO, rejection, thread limits and release passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
