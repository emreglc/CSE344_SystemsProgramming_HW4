#define _GNU_SOURCE // For getline
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h> // For write in signal handler

#include "buffer.h"

// Global variables
Buffer shared_buffer;
pthread_barrier_t barrier;
char *g_search_term;
int g_num_workers;
int *worker_match_counts; // Array to store matches per worker, indexed by worker_id
int g_total_matches_summary = 0; // For final summary report

volatile sig_atomic_t sigint_received_flag = 0;

typedef struct {
    int id; // Worker ID
} worker_args_t;

// Signal handler for SIGINT (Ctrl+C)
void sigint_handler(int signum) {
    (void)signum; // Unused parameter
    sigint_received_flag = 1;
    // Use async-signal-safe function to notify user
    char msg[] = "\nSIGINT received, initiating shutdown...\n";
    write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    // The actual shutdown logic (setting buffer flags, broadcasting)
    // will be handled by the main thread loop when it detects sigint_received_flag.
    // This is safer than calling non-async-signal-safe functions from handler.
}

void* worker_function(void* arg) {
    worker_args_t* args = (worker_args_t*)arg;
    int worker_id = args->id;
    int local_matches = 0;
    char* line_from_buffer;

    printf("Worker %d started.\n", worker_id);

    while (1) {
        line_from_buffer = buffer_pop(&shared_buffer);

        if (line_from_buffer == NULL) {
            // This means either EOF marker from manager OR buffer is shutting down
            // printf("Worker %d received NULL, exiting.\n", worker_id); // Debug
            break;
        }

        // Search for the keyword in the line
        if (strstr(line_from_buffer, g_search_term) != NULL) {
            local_matches++;
        }
        free(line_from_buffer); // Line was allocated by getline in manager, worker frees it
    }

    worker_match_counts[worker_id] = local_matches;
    printf("Worker %d found %d matches.\n", worker_id, local_matches);

    // Synchronize with other workers before printing summary
    int barrier_rc = pthread_barrier_wait(&barrier);
    if (barrier_rc == PTHREAD_BARRIER_SERIAL_THREAD) {
        // This thread is the designated one to calculate and print the summary
        for (int i = 0; i < g_num_workers; i++) {
            g_total_matches_summary += worker_match_counts[i];
        }
        printf("Total matches found: %d\n", g_total_matches_summary);
    } else if (barrier_rc != 0) {
        fprintf(stderr, "Worker %d: Error waiting on barrier: %d\n", worker_id, barrier_rc);
        // Potentially exit or handle error
    }
    // printf("Worker %d finished.\n", worker_id); // Debug
    return NULL;
}

void cleanup_resources(pthread_t *threads) {
    // Join all worker threads if they were created
    if (threads) {
        for (int i = 0; i < g_num_workers; i++) {
            // If a thread was not created due to an earlier error, threads[i] might be 0.
            // However, pthread_join on a 0 thread id is undefined behavior.
            // Assuming threads array is initialized and pthread_create was attempted for all.
            // A more robust check would involve tracking successful creations.
            // For simplicity, we join all, assuming they were created or join will handle errors.
            // If thread creation failed partially, the error handling there should manage joins.
            if (threads[i] != 0) { // A basic check
                 pthread_join(threads[i], NULL);
            }
        }
    }

    buffer_destroy(&shared_buffer);
    pthread_barrier_destroy(&barrier);
    if (worker_match_counts) {
        free(worker_match_counts);
        worker_match_counts = NULL;
    }
}


int main(int argc, char *argv[]) {
    if (argc != 5) {
        fprintf(stderr, "Usage: ./LogAnalyzer <buffer_size> <num_workers> <log_file> <search_term>\n");
        return EXIT_FAILURE;
    }

    int buffer_capacity = atoi(argv[1]);
    g_num_workers = atoi(argv[2]);
    char *log_file_path = argv[3];
    g_search_term = argv[4];

    if (buffer_capacity <= 0 || g_num_workers <= 0) {
        fprintf(stderr, "Error: Buffer size and number of workers must be positive integers.\n");
        fprintf(stderr, "Usage: ./LogAnalyzer <buffer_size> <num_workers> <log_file> <search_term>\n");
        return EXIT_FAILURE;
    }

    // Setup SIGINT handler
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask); // Do not block other signals during handler execution
    sa.sa_flags = 0; // No SA_RESTART, so syscalls like getline are interrupted
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction failed");
        return EXIT_FAILURE;
    }

    buffer_init(&shared_buffer, buffer_capacity);
    if (pthread_barrier_init(&barrier, NULL, g_num_workers) != 0) {
        perror("pthread_barrier_init failed");
        buffer_destroy(&shared_buffer);
        return EXIT_FAILURE;
    }

    worker_match_counts = calloc(g_num_workers, sizeof(int));
    if (!worker_match_counts) {
        perror("calloc for worker_match_counts failed");
        pthread_barrier_destroy(&barrier);
        buffer_destroy(&shared_buffer);
        return EXIT_FAILURE;
    }

    pthread_t *worker_threads = malloc(g_num_workers * sizeof(pthread_t));
    if (!worker_threads) {
        perror("malloc for worker_threads failed");
        free(worker_match_counts);
        pthread_barrier_destroy(&barrier);
        buffer_destroy(&shared_buffer);
        return EXIT_FAILURE;
    }
    memset(worker_threads, 0, g_num_workers * sizeof(pthread_t)); // Initialize for safer cleanup

    worker_args_t args[g_num_workers];
    for (int i = 0; i < g_num_workers; i++) {
        args[i].id = i;
        if (pthread_create(&worker_threads[i], NULL, worker_function, &args[i]) != 0) {
            perror("pthread_create failed");
            sigint_received_flag = 1; // Signal a general shutdown
            buffer_signal_shutdown(&shared_buffer); // Tell buffer system is shutting down
            // Join already created threads
            for (int k = 0; k < i; k++) {
                pthread_join(worker_threads[k], NULL);
            }
            free(worker_threads);
            cleanup_resources(NULL); // Call with NULL as threads array is locally managed here
            return EXIT_FAILURE;
        }
    }

    // Manager (main thread) logic: read file and push lines to buffer
    FILE *file = fopen(log_file_path, "r");
    if (!file) {
        perror("fopen failed");
        sigint_received_flag = 1;
        buffer_signal_shutdown(&shared_buffer);
        // Fall through to push EOFs and join/cleanup
    }

    char *current_line_ptr = NULL; // Buffer for getline
    size_t line_buffer_size = 0;   // Size of buffer for getline
    ssize_t read_len;

    if (file) { // Proceed only if file was opened successfully
        while ((read_len = getline(&current_line_ptr, &line_buffer_size, file)) != -1) {
            if (sigint_received_flag) {
                // printf("Manager: SIGINT detected, stopping file reading.\n"); // Debug
                buffer_signal_shutdown(&shared_buffer);
                break; // Exit file reading loop
            }

            // Remove newline character if present, as strstr might be affected
            if (read_len > 0 && current_line_ptr[read_len - 1] == '\n') {
                current_line_ptr[read_len - 1] = '\0';
            }
            
            char *line_to_push = current_line_ptr;
            current_line_ptr = NULL; // getline will allocate new buffer next time
            line_buffer_size = 0;    // Reset size too

            if (!buffer_push(&shared_buffer, line_to_push)) {
                // Push failed, likely because buffer is shutting down
                // printf("Manager: buffer_push failed (shutting down?), stopping.\n"); // Debug
                free(line_to_push); // Manager must free this line
                // buffer_signal_shutdown(&shared_buffer); // Ensure buffer knows (likely already does)
                break; // Exit file reading loop
            }

            usleep(50000); // Simulate some delay for processing

        }
        if (current_line_ptr != NULL) { // Free last buffer allocated by getline if loop exited
            free(current_line_ptr);
        }
        fclose(file);
    } else { // If file could not be opened
        // Ensure buffer is signaled for shutdown if not already by sigint_received_flag
        if (!sigint_received_flag) { // If fopen was the primary error, not SIGINT
            buffer_signal_shutdown(&shared_buffer);
        }
    }
    
    // If SIGINT occurred, ensure buffer is fully in shutdown mode
    if (sigint_received_flag) {
        buffer_signal_shutdown(&shared_buffer);
    }

    // Push EOF markers (NULL pointers) for each worker thread
    // This signals normal completion. If shutting_down, workers will get NULL from pop anyway.
    // printf("Manager: Pushing EOF markers to workers.\n"); // Debug
    for (int i = 0; i < g_num_workers; i++) {
        if (!buffer_push(&shared_buffer, NULL)) {
            // If push fails here, it's likely due to shutdown; workers will still terminate correctly.
            // printf("Manager: Failed to push EOF marker for worker %d (buffer likely shutting down).\n", i); // Debug
            break;
        }
    }

    // Wait for all worker threads to complete
    for (int i = 0; i < g_num_workers; i++) {
        pthread_join(worker_threads[i], NULL);
    }
    free(worker_threads);
    worker_threads = NULL;

    // printf("Manager thread finished processing and joining workers.\n"); // Debug

    // Cleanup all global resources
    // buffer_destroy, barrier_destroy, free worker_match_counts
    // Note: cleanup_resources expects threads array to be passed, but we free it above.
    // For this structure, it's better to call components of cleanup directly.
    buffer_destroy(&shared_buffer);
    pthread_barrier_destroy(&barrier);
    if (worker_match_counts) {
        free(worker_match_counts);
        worker_match_counts = NULL;
    }
    
    // printf("All resources cleaned up.\n"); // Debug
    return EXIT_SUCCESS;
}