// 2018/10/04 - modified by Tsung-Wei Huang
// 
// Removed shutdown, spawn, and wait_for_all to simplify the design
// of the threadpool. The threadpool now can operates on fixed memory
// closure to improve the performance.
//
// 2018/09/11 - modified by Tsung-Wei Huang & Guannan
//   - bug fix: shutdown method might hang due to dynamic tasking;
//     it can be non-empty task queue while all threads are gone;
//     workers need to be cleared as well under lock, since *_async
//     will access the worker data structure;
//   - renamed _worker to _idler
//     
// 2018/09/03 - modified by Guannan Guo
// 
// BasicProactiveThreadpool schedules independent jobs in a greedy manner.
// Whenever a job is inserted into the threadpool, the threadpool will check if there
// are any spare threads available. The spare thread will be woken through its local 
// condition variable. The new job will be directly moved into
// this thread instead of pushed at the back of the pending queue.

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
#include <optional>

namespace tf {
  
// Class: ProactiveThreadpool
template <typename Closure>
class ProactiveThreadpool {

  // Struct: Worker
  struct Worker {
    std::condition_variable cv;
    std::optional<Closure> task;
    bool ready;
  };

  public:

    ProactiveThreadpool(unsigned);
    ~ProactiveThreadpool();

    size_t num_tasks() const;
    size_t num_workers() const;
    
    bool is_owner() const;

    template <typename... ArgsT>
    void emplace(ArgsT&&...);

    void emplace(std::vector<Closure> &&);

  private:

    std::thread::id _owner {std::this_thread::get_id()};

    mutable std::mutex _mutex;

    std::vector<Closure> _tasks;
    std::vector<std::thread> _threads;
    std::vector<Worker*> _idlers; 

    bool _exiting {false};
    
    void _shutdown();
    void _spawn(unsigned);
};
    
// Constructor
template <typename Closure>
ProactiveThreadpool<Closure>::ProactiveThreadpool(unsigned N){
  _spawn(N);
}

// Destructor
template <typename Closure>
ProactiveThreadpool<Closure>::~ProactiveThreadpool(){
  _shutdown();
}

// Ftion: is_owner
template <typename Closure>
bool ProactiveThreadpool<Closure>::is_owner() const {
  return std::this_thread::get_id() == _owner;
}

// Ftion: num_tasks    
template <typename Closure>
size_t ProactiveThreadpool<Closure>::num_tasks() const { 
  return _tasks.size(); 
}

// Ftion: num_workers
template <typename Closure>
size_t ProactiveThreadpool<Closure>::num_workers() const { 
  return _threads.size();  
}

// Procedure: shutdown
template <typename Closure>
void ProactiveThreadpool<Closure>::_shutdown() {
  
  assert(is_owner());

  { 
    std::unique_lock lock(_mutex);

    _exiting = true;
    
    // we need to clear the workers under lock
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

  _exiting = false;
}

// Procedure: spawn
template <typename Closure>
void ProactiveThreadpool<Closure>::_spawn(unsigned N) {

  assert(is_owner());

  for(size_t i=0; i<N; ++i){
  
    _threads.emplace_back([this] () -> void {
      
      Worker w;
      
      std::unique_lock lock(_mutex);

      while(!_exiting) {

        if(_tasks.empty()){

          w.ready = false;
          _idlers.push_back(&w);

          while(!w.ready) {
            w.cv.wait(lock);
          }
          
          // shutdown cannot have task
          if(w.task) {
            lock.unlock();
            (*w.task)();
            w.task = std::nullopt;
            lock.lock();
          }
        }
        else{
          Closure t{std::move(_tasks.back())};
          _tasks.pop_back();
          lock.unlock();
          t();
          lock.lock();
        } 
      }
    });     

  } 
}

// Procedure: silent_async
template <typename Closure>
template <typename... ArgsT>
void ProactiveThreadpool<Closure>::emplace(ArgsT&&... args) {

  //no worker thread available
  if(num_workers() == 0){
    Closure{std::forward<ArgsT>(args)...}();
  }
  // ask one worker to run the task
  else {
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
}


// Procedure: silent_async
template <typename Closure>
void ProactiveThreadpool<Closure>::emplace(std::vector<Closure> &&tasks) {

  //no worker thread available
  if(num_workers() == 0){
    for(auto& t: tasks){
      t();
    }
  }
  // ask one worker to run the task
  else {
    size_t consumed {0};
    std::scoped_lock lock(_mutex);
    if(_idlers.empty()){
      std::move(tasks.begin(), tasks.end(), std::back_inserter(_tasks));
      return ;
    } 
    else{
      while(consumed != tasks.size() && !_idlers.empty()) {
        Worker* w = _idlers.back();
        _idlers.pop_back();
        w->ready = true;
        w->task.emplace(std::move(tasks[consumed++]));
        w->cv.notify_one();   
      }
    }
    if(consumed == tasks.size()) return ;
    _tasks.reserve(_tasks.size() + tasks.size() - consumed);
    std::move(tasks.begin()+consumed, tasks.end(), std::back_inserter(_tasks));
  }
}

};  // namespace tf -----------------------------------------------------------



