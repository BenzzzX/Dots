// 2018/10/04 - modified by Tsung-Wei Huang
// 
// Removed shutdown, spawn, and wait_for_all to simplify the design
// of the threadpool. The threadpool now can operates on fixed memory
// closure to improve the performance.
//
// 2018/09/12 - created by Tsung-Wei Huang and Chun-Xun Lin
//
// Speculative threadpool is similar to proactive threadpool except
// each thread will speculatively move a new task to its local worker
// data structure to reduce extract hit to the task queue.
// This can save time from locking the mutex during dynamic tasking.

#pragma once

#include <iostream>
#include <functional>
#include <vector>
#include <mutex>
#include <deque>
#include <thread>
#include <stdexcept>
#include <condition_variable>
#include <memory>
#include <future>
#include <unordered_set>
#include <unordered_map>
#include <optional>


namespace tf {

// Class: SpeculativeThreadpool
template <typename Closure>
class SpeculativeThreadpool {

  struct Worker {
    std::condition_variable cv;
    std::optional<Closure> task;
    bool ready {false};
  };

  public:

    SpeculativeThreadpool(unsigned);
    ~SpeculativeThreadpool();

    size_t num_tasks() const;
    size_t num_workers() const;

    bool is_owner() const;

    template <typename... ArgsT>
    void emplace(ArgsT&&...);

    void emplace(std::vector<Closure> &&);

  private:
    
    const std::thread::id _owner {std::this_thread::get_id()};

    mutable std::mutex _mutex;

    std::vector<Closure> _tasks;
    std::vector<std::thread> _threads;
    std::vector<Worker*> _idlers; 
    std::vector<Worker> _workers;
    std::unordered_map<std::thread::id, Worker*> _worker_maps;    

    bool _exiting {false};

    auto _this_worker() const;
    
    void _shutdown();
    void _spawn(unsigned);

};  // class BasicSpeculativeThreadpool. --------------------------------------

// Constructor
template <typename Closure>
SpeculativeThreadpool<Closure>::SpeculativeThreadpool(unsigned N) : 
  _workers {N} {
  _spawn(N);
}

// Destructor
template <typename Closure>
SpeculativeThreadpool<Closure>::~SpeculativeThreadpool(){
  _shutdown();
}

// Function: is_owner
template <typename Closure>
bool SpeculativeThreadpool<Closure>::is_owner() const {
  return std::this_thread::get_id() == _owner;
}

// Function: num_tasks
template <typename Closure>
size_t SpeculativeThreadpool<Closure>::num_tasks() const { 
  return _tasks.size(); 
}

// Function: num_workers
template <typename Closure>
size_t SpeculativeThreadpool<Closure>::num_workers() const { 
  return _threads.size();  
}
    
// Function: _this_worker
template <typename Closure>
auto SpeculativeThreadpool<Closure>::_this_worker() const {
  auto id = std::this_thread::get_id();
  return _worker_maps.find(id);
}

// Function: shutdown
template <typename Closure>
void SpeculativeThreadpool<Closure>::_shutdown(){

  assert(is_owner());

  { 
    std::unique_lock lock(_mutex);

    _exiting = true;
    
    for(auto w : _idlers){
      w->ready = true;
      w->task = std::nullopt;
      w->cv.notify_one();
    }
    _idlers.clear();
  }

  for(auto& t : _threads){
    t.join();
  } 
  _threads.clear();  

  _workers.clear();
  _worker_maps.clear();
  
  _exiting = false;
}

// Function: spawn 
template <typename Closure>
void SpeculativeThreadpool<Closure>::_spawn(unsigned N) {

  assert(is_owner() && _workers.size() == N);

  // Lock to synchronize all workers before creating _worker_mapss
  std::scoped_lock lock(_mutex);

  for(size_t i=0; i<N; ++i){

    _threads.emplace_back([this, &w=_workers[i]]() -> void {

       std::optional<Closure> t;

       std::unique_lock lock(_mutex);
       
       while(!_exiting){
         if(_tasks.empty()){
           w.ready = false;
           _idlers.push_back(&w);

           while(!w.ready) {
             w.cv.wait(lock);
           }

           t = std::move(w.task);
           w.task = std::nullopt;
         }
         else{
           t = std::move(_tasks.back());
           _tasks.pop_back();
         } 

         if(t) {
           lock.unlock();
           // speculation loop
           while(t) {
             (*t)();
             if(w.task) {
               t = std::move(w.task);
               w.task = std::nullopt;
             }
             else {
               t = std::nullopt;
             }
           }
           lock.lock();
         }
       }
    });     

    _worker_maps[_threads[i].get_id()] = &_workers[i];
  } // End of For ---------------------------------------------------------------------------------
}

template <typename Closure>
template <typename... ArgsT>
void SpeculativeThreadpool<Closure>::emplace(ArgsT&&... args) {

  //no worker thread available
  if(num_workers() == 0){
    Closure{std::forward<ArgsT>(args)...}();
    return;
  }

  // speculation
  auto tid = std::this_thread::get_id();

  if(tid != _owner){
    auto iter = _worker_maps.find(tid);
    if(iter != _worker_maps.end() && !(iter->second->task)){
      iter->second->task.emplace(std::forward<ArgsT>(args)...);
      return ;
    }
  }

  std::scoped_lock lock(_mutex);
  if(_idlers.empty()){
    _tasks.emplace_back(std::forward<ArgsT>(args)...);
  } 
  else{
    Worker* w = _idlers.back();
    _idlers.pop_back();
    w->ready = true;
    w->task.emplace(std::forward<ArgsT>(args)...);
    w->cv.notify_one();   
  }
}


template <typename Closure>
void SpeculativeThreadpool<Closure>::emplace(std::vector<Closure>&& tasks){
  size_t consumed {0};

  //no worker thread available
  if(num_workers() == 0){
    for(auto& c: tasks){
      c();
    }
    return;
  }

  // speculation
  if(std::this_thread::get_id() != _owner){
    auto iter = _this_worker();
    if(iter != _worker_maps.end() && !(iter->second->task)){
      iter->second->task.emplace(std::move(tasks[consumed++]));
      if(tasks.size() == consumed) {
        return ;
      }
    }
  }

  std::scoped_lock lock(_mutex);
  while(!_idlers.empty() && tasks.size() != consumed) {
    Worker* w = _idlers.back();
    _idlers.pop_back();
    w->ready = true;
    w->task.emplace(std::move(tasks[consumed ++]));
    w->cv.notify_one();   
  }

  if(tasks.size() == consumed) return ;
  _tasks.reserve(_tasks.size() + tasks.size() - consumed);
  std::move(tasks.begin()+consumed, tasks.end(), std::back_inserter(_tasks));
}


};  // end of namespace tf. ---------------------------------------------------





