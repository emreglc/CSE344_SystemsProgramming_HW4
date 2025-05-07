#include "buffer.h"
#include <stdlib.h>
#include <stdio.h>

void buffer_init(Buffer *buffer, int capacity) {
    buffer->lines = malloc(sizeof(char*) * capacity);
    if (!buffer->lines) {
        perror("Failed to allocate buffer lines array");
        exit(EXIT_FAILURE);
    }
    buffer->capacity = capacity;
    buffer->count = 0;
    buffer->head = 0;
    buffer->tail = 0;
    buffer->shutting_down = false;
    pthread_mutex_init(&buffer->mutex, NULL);
    pthread_cond_init(&buffer->cond_full, NULL);
    pthread_cond_init(&buffer->cond_empty, NULL);
}

void buffer_destroy(Buffer *buffer) {
    if (buffer->lines) {
        // Free any remaining lines in the buffer if they were dynamically allocated
        // This handles cases where workers might not have consumed all lines during a shutdown.
        for (int i = 0; i < buffer->count; i++) {
            int current_idx = (buffer->head + i) % buffer->capacity;
            if (buffer->lines[current_idx] != NULL) {
                free(buffer->lines[current_idx]);
            }
        }
        free(buffer->lines);
        buffer->lines = NULL;
    }
    pthread_mutex_destroy(&buffer->mutex);
    pthread_cond_destroy(&buffer->cond_full);
    pthread_cond_destroy(&buffer->cond_empty);
}

void buffer_signal_shutdown(Buffer *buffer) {
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = true;
    // Wake up any threads waiting on condition variables
    pthread_cond_broadcast(&buffer->cond_empty); // Wake up workers
    pthread_cond_broadcast(&buffer->cond_full);  // Wake up manager (producer)
    pthread_mutex_unlock(&buffer->mutex);
}

bool buffer_push(Buffer *buffer, char *line) {
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == buffer->capacity) {
        if (buffer->shutting_down) {
            pthread_mutex_unlock(&buffer->mutex);
            return false; // Cannot push, system is shutting down
        }
        pthread_cond_wait(&buffer->cond_full, &buffer->mutex);
        // Re-check condition after waking up
        if (buffer->shutting_down) {
            pthread_mutex_unlock(&buffer->mutex);
            return false;
        }
    }

    buffer->lines[buffer->tail] = line;
    buffer->tail = (buffer->tail + 1) % buffer->capacity;
    buffer->count++;

    pthread_cond_signal(&buffer->cond_empty); // Signal one waiting worker
    pthread_mutex_unlock(&buffer->mutex);
    return true;
}

char* buffer_pop(Buffer *buffer) {
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == 0) {
        if (buffer->shutting_down) {
            // If shutting down and buffer is empty, worker should terminate
            pthread_mutex_unlock(&buffer->mutex);
            return NULL;
        }
        pthread_cond_wait(&buffer->cond_empty, &buffer->mutex);
        // Re-check condition after waking up
        if (buffer->shutting_down && buffer->count == 0) {
            pthread_mutex_unlock(&buffer->mutex);
            return NULL;
        }
    }

    char *line = buffer->lines[buffer->head];
    buffer->lines[buffer->head] = NULL; // Optional: Clear the slot after popping
    buffer->head = (buffer->head + 1) % buffer->capacity;
    buffer->count--;

    pthread_cond_signal(&buffer->cond_full); // Signal manager if it was waiting
    pthread_mutex_unlock(&buffer->mutex);
    return line;
}