#ifndef BUFFER_H
#define BUFFER_H

#include <pthread.h>
#include <stdbool.h>

typedef struct {
    char **lines;          // Array of strings (lines from the file)
    int capacity;          // Max number of items in buffer
    int count;             // Current number of items in buffer
    int head;              // Index to pop from
    int tail;              // Index to push to
    bool shutting_down;    // Flag to indicate if the system is shutting down (e.g., due to SIGINT)
    pthread_mutex_t mutex; // Mutex for buffer access
    pthread_cond_t cond_full; // Condition variable: buffer is full, producer waits
    pthread_cond_t cond_empty; // Condition variable: buffer is empty, consumer waits
} Buffer;

/**
 * @brief Initializes the buffer.
 * @param buffer Pointer to the Buffer struct.
 * @param capacity The maximum capacity of the buffer.
 */
void buffer_init(Buffer *buffer, int capacity);

/**
 * @brief Destroys the buffer, freeing allocated resources.
 *          IMPORTANT: This function assumes that any dynamically allocated strings
 *          (lines) remaining in the buffer will be freed. This is crucial if
 *          the buffer is destroyed while containing unprocessed lines, e.g. during shutdown.
 * @param buffer Pointer to the Buffer struct.
 */
void buffer_destroy(Buffer *buffer);

/**
 * @brief Pushes a line into the buffer. Blocks if the buffer is full, unless shutting down.
 * @param buffer Pointer to the Buffer struct.
 * @param line The line (string) to push. Ownership of the string is transferred to the buffer.
 *             NULL can be pushed as an EOF marker.
 * @return true if the line was pushed successfully, false if shutting down and line was not pushed.
 */
bool buffer_push(Buffer *buffer, char *line);

/**
 * @brief Pops a line from the buffer. Blocks if the buffer is empty, unless shutting down.
 * @param buffer Pointer to the Buffer struct.
 * @return The popped line (string). The caller is responsible for freeing this string if it's not NULL.
 *         Returns NULL if an EOF marker is popped or if the system is shutting down and the buffer is empty.
 */
char* buffer_pop(Buffer *buffer);

/**
 * @brief Signals the buffer (and waiting threads) that the system is shutting down.
 *        Sets the shutting_down flag and broadcasts to all condition variables.
 * @param buffer Pointer to the Buffer struct.
 */
void buffer_signal_shutdown(Buffer *buffer);

#endif // BUFFER_H