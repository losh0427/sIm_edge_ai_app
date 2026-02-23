#pragma once
#include <semaphore.h>
#include <pthread.h>
#include <cstdio>
#include <cstring>

// Single-producer single-consumer ring buffer using POSIX unnamed semaphores.
// T = slot type (must be trivially copyable or have no dynamic alloc in hot path)
// N = number of slots (power of 2 recommended but not required)
template <typename T, int N>
class RingBuffer {
public:
    int init() {
        if (sem_init(&empty_count_, 0, N) != 0) {
            fprintf(stderr, "RingBuffer: sem_init(empty) failed\n");
            return -1;
        }
        if (sem_init(&fill_count_, 0, 0) != 0) {
            fprintf(stderr, "RingBuffer: sem_init(fill) failed\n");
            sem_destroy(&empty_count_);
            return -1;
        }
        if (pthread_mutex_init(&write_mtx_, nullptr) != 0) {
            fprintf(stderr, "RingBuffer: mutex_init(write) failed\n");
            sem_destroy(&empty_count_);
            sem_destroy(&fill_count_);
            return -1;
        }
        if (pthread_mutex_init(&read_mtx_, nullptr) != 0) {
            fprintf(stderr, "RingBuffer: mutex_init(read) failed\n");
            sem_destroy(&empty_count_);
            sem_destroy(&fill_count_);
            pthread_mutex_destroy(&write_mtx_);
            return -1;
        }
        write_idx_ = 0;
        read_idx_ = 0;
        shutdown_ = false;
        return 0;
    }

    void destroy() {
        sem_destroy(&empty_count_);
        sem_destroy(&fill_count_);
        pthread_mutex_destroy(&write_mtx_);
        pthread_mutex_destroy(&read_mtx_);
    }

    // Producer: get a slot to write into. Returns pointer to slot, or nullptr on shutdown.
    T* acquire_write_slot() {
        while (sem_wait(&empty_count_) != 0) {
            if (shutdown_) return nullptr;
            // EINTR: retry
        }
        if (shutdown_) return nullptr;

        pthread_mutex_lock(&write_mtx_);
        int idx = write_idx_;
        write_idx_ = (write_idx_ + 1) % N;
        pthread_mutex_unlock(&write_mtx_);

        return &slots_[idx];
    }

    // Producer: mark slot as ready for consumer.
    void commit_write_slot() {
        sem_post(&fill_count_);
    }

    // Consumer: get next filled slot. Returns pointer to slot, or nullptr on shutdown.
    T* acquire_read_slot() {
        while (sem_wait(&fill_count_) != 0) {
            if (shutdown_) return nullptr;
            // EINTR: retry
        }
        if (shutdown_) return nullptr;

        pthread_mutex_lock(&read_mtx_);
        int idx = read_idx_;
        read_idx_ = (read_idx_ + 1) % N;
        pthread_mutex_unlock(&read_mtx_);

        return &slots_[idx];
    }

    // Consumer: release slot back to producer.
    void commit_read_slot() {
        sem_post(&empty_count_);
    }

    // Signal shutdown: unblock any waiting threads.
    void shutdown() {
        shutdown_ = true;
        sem_post(&empty_count_);
        sem_post(&fill_count_);
    }

    bool is_shutdown() const { return shutdown_; }

private:
    T slots_[N];
    sem_t empty_count_;
    sem_t fill_count_;
    pthread_mutex_t write_mtx_;
    pthread_mutex_t read_mtx_;
    int write_idx_ = 0;
    int read_idx_ = 0;
    volatile bool shutdown_ = false;
};
