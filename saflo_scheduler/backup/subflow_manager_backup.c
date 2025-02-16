#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <bpf/libbpf.h>
#include <unistd.h>
#include <stdbool.h>
#include <linux/bpf.h>
#include <bpf/bpf.h>
#include <time.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <limits.h>
#include <semaphore.h>
#include <fcntl.h> // For O_CREAT
#include <signal.h>

// Path to eBPF map
#define RANFLOW_MAP_PATH "/sys/fs/bpf/ranflow_map"

typedef struct {
    uint32_t token;
    int16_t local_id;
    uint8_t remote_id;
} ranflow_key_t;

typedef struct {
    uint32_t avg_pacing;
    uint32_t wmem;
    uint64_t linger_time;
    bool enabled;
} ranflow_data_t;

// Hash funtion for subflow management.

#define MAX_TUPLES 8  // Maximum tuples per key
#define HASH_TABLE_SIZE 1024

typedef struct KeyValuePair {
    uint32_t key;               // Hash key
    int local_ids[MAX_TUPLES];  // Array of local IDs
    int remote_ids[MAX_TUPLES]; // Array of remote IDs
    int size;                   // Number of valid tuples
    struct KeyValuePair *next;  // Pointer to the next node in the chain
} KeyValuePair;

typedef struct {
    KeyValuePair *buckets[HASH_TABLE_SIZE];  // Array of bucket pointers
} HashTable;

// Global array to store all keys
uint32_t *all_keys = NULL;
int key_count = 0;     // Current number of keys
int key_capacity = 0;  // Capacity of the key array

// Hash function
unsigned int hash_function(uint32_t key) {
    return key % HASH_TABLE_SIZE;
}

// Resize the key array if needed
void resize_key_array() {
    if (key_count == key_capacity) {
        key_capacity = (key_capacity == 0) ? 4 : key_capacity * 2;
        all_keys = realloc(all_keys, key_capacity * sizeof(uint32_t));
        if (!all_keys) {
            fprintf(stderr, "Memory allocation failed\n");
            exit(1);
        }
    }
}

// Add a key to the global key array
void add_key(uint32_t key) {
    // Check if the key already exists
    for (int i = 0; i < key_count; i++) {
        if (all_keys[i] == key) return;  // Key already exists
    }
    resize_key_array();
    all_keys[key_count++] = key;
}

// Remove a key from the global key array
void remove_key(uint32_t key) {
    for (int i = 0; i < key_count; i++) {
        if (all_keys[i] == key) {
            // Shift the remaining keys to fill the gap
            for (int j = i; j < key_count - 1; j++) {
                all_keys[j] = all_keys[j + 1];
            }
            key_count--;
            return;
        }
    }
}

// Initialize the hash table
void hash_table_init(HashTable *table) {
    memset(table, 0, sizeof(HashTable));
}

// Insert a tuple into the hash table
int hash_table_insert(HashTable *table, uint32_t key, int local_id, int remote_id) {
    unsigned int index = hash_function(key);
    KeyValuePair *node = table->buckets[index];

    // Search for the key
    while (node) {
        if (node->key == key) {
            // Key found, add the tuple if there's space
            if (node->size < MAX_TUPLES) {
                node->local_ids[node->size] = local_id;
                node->remote_ids[node->size] = remote_id;
                node->size++;
                return 1;  // Success
            } else {
                fprintf(stderr, "Error: Maximum tuples reached for key %u\n", key);
                return 0;  // Failure
            }
        }
        node = node->next;
    }

    // Key not found, create a new node
    KeyValuePair *new_node = (KeyValuePair *)malloc(sizeof(KeyValuePair));
    new_node->key = key;
    new_node->local_ids[0] = local_id;
    new_node->remote_ids[0] = remote_id;
    new_node->size = 1;
    new_node->next = table->buckets[index];
    table->buckets[index] = new_node;

    // Add key to the global key array
    add_key(key);

    return 1;  // Success
}

// Search for tuples by key
int hash_table_search(HashTable *table, uint32_t key, int **local_ids, int **remote_ids, int *size) {
    unsigned int index = hash_function(key);
    KeyValuePair *node = table->buckets[index];

    while (node) {
        if (node->key == key) {
            *local_ids = node->local_ids;
            *remote_ids = node->remote_ids;
            *size = node->size;
            return 1;  // Key found
        }
        node = node->next;
    }

    return 0;  // Key not found
}

// Delete a key from the hash table
int hash_table_delete(HashTable *table, uint32_t key) {
    unsigned int index = hash_function(key);
    KeyValuePair *node = table->buckets[index];
    KeyValuePair *prev = NULL;

    while (node) {
        if (node->key == key) {
            if (prev) {
                prev->next = node->next;
            } else {
                table->buckets[index] = node->next;
            }
            free(node);

            // Remove key from the global key array
            remove_key(key);

            return 1;  // Key deleted
        }
        prev = node;
        node = node->next;
    }
    return 0;  // Key not found
}

void hash_table_clear(HashTable *table) {
    for (int i = 0; i < HASH_TABLE_SIZE; ++i) {
        KeyValuePair *node = table->buckets[i];
        while (node) {
            KeyValuePair *temp = node;
            node = node->next;
            free(temp);
        }
        table->buckets[i] = NULL;  // Reset the bucket
    }
    key_count = 0;  // Reset the global key count
}

// Free the hash table
void hash_table_free(HashTable *table) {
    for (int i = 0; i < HASH_TABLE_SIZE; ++i) {
        KeyValuePair *node = table->buckets[i];
        while (node) {
            KeyValuePair *temp = node;
            node = node->next;
            printf("here?\n");
            free(temp);
            printf("ah?\n");
        }
    }
    free(all_keys);  // Free the global key array
}


#define SHM_KEY 0x12345  // Shared memory key
#define QUEUE_SIZE 512    // Maximum queue size
#define MAX_STRING_LEN 50 // Maximum length of a string in the queue

// Define the shared queue structure
typedef struct {
    char items[QUEUE_SIZE][MAX_STRING_LEN];
    int front;
    int rear;
    int count;
} SharedQueue;

int shmid;
SharedQueue *queue;
sem_t *mutex;        // Semaphore for mutual exclusion
sem_t *empty_slots;  // Semaphore for empty slots
sem_t *filled_slots; // Semaphore for filled slots

void enqueue(SharedQueue *queue, const char *value) {
    sem_wait(empty_slots); // Wait for an empty slot
    sem_wait(mutex);       // Enter critical section

    // Perform enqueue operation
    queue->rear = (queue->rear + 1) % QUEUE_SIZE;
    strncpy(queue->items[queue->rear], value, MAX_STRING_LEN - 1);
    queue->items[queue->rear][MAX_STRING_LEN - 1] = '\0'; // Ensure null termination
    queue->count++;

    sem_post(mutex);       // Exit critical section
    sem_post(filled_slots); // Signal that a slot is filled
}

char *dequeue(SharedQueue *queue, char *buffer) {
    sem_wait(filled_slots); // Wait for a filled slot
    sem_wait(mutex);        // Enter critical section

    // Perform dequeue operation
    strncpy(buffer, queue->items[queue->front], MAX_STRING_LEN);
    queue->front = (queue->front + 1) % QUEUE_SIZE;
    queue->count--;

    sem_post(mutex);       // Exit critical section
    sem_post(empty_slots); // Signal that a slot is empty
    return buffer;
}


uint32_t convert_to_uint32(const char *str) {
    char *endptr;
    errno = 0; // Reset errno before the conversion

    // Convert string to unsigned long
    unsigned long value = strtoul(str, &endptr, 16);

    // Check for conversion errors
    if (errno == ERANGE || value > UINT32_MAX) {
        fprintf(stderr, "Value out of range for uint32_t.\n");
        exit(EXIT_FAILURE);
    }

    if (endptr == str || *endptr != '\0') {
        fprintf(stderr, "Invalid input: '%s'\n", str);
        exit(EXIT_FAILURE);
    }

    // Safely cast to uint32_t
    return (uint32_t)value;
}

// Cleanup function for shared resources
void cleanup() {
    printf("\nCleaning up resources...\n");

    // Detach shared memory
    if (queue != NULL) {
        shmdt(queue);
    }

    // Remove shared memory segment
    shmctl(shmid, IPC_RMID, 0);

    // Close and unlink semaphores
    sem_close(mutex);
    sem_close(empty_slots);
    sem_close(filled_slots);
    sem_unlink("queue_mutex");
    sem_unlink("empty_slots");
    sem_unlink("filled_slots");

    printf("Resources cleaned up. Exiting.\n");
}

// Signal handler
void signal_handler(int signum) {
    printf("Caught signal %d\n", signum);
    cleanup();
    exit(0);
}

int main(int argc, char *argv[]) {
    // Get user inputs
    int opt;
    int time_interval = 0;
    float max_p = 0.0, min_p = 0.0;

    // Parse command-line arguments
    while ((opt = getopt(argc, argv, "i:x:n:")) != -1) {
        switch (opt) {
        case 'i': // Time Interval
            time_interval = atoi(optarg);
            break;
        case 'x': // Max Probability
            max_p = atof(optarg);
            break;
        case 'n': // Min Probability
            min_p = atof(optarg);
            break;
	default: // Invalid option
            fprintf(stderr, "Usage: %s -i <time_interval> -x <max_p> -n <min_p>\n", argv[0]);
            fprintf(stderr, "Example: sudo ./subflow_manager -i 500000 -x 0.8 -n 0.2\n");
	    exit(EXIT_FAILURE);
        }
    }

    // Check if all required arguments are provided
    if (time_interval == 0 || max_p == 0.0 || min_p == 0.0) {
        fprintf(stderr, "All options (-i, -x, -n) must be provided.\n");
        fprintf(stderr, "Usage: %s -i <time_interval(μs)> -x <max_p> -n <min_p>\n", argv[0]);
        fprintf(stderr, "Example: sudo ./subflow_manager -i 500000 -x 0.8 -n 0.2\n");
	exit(EXIT_FAILURE);
    }

    // Display the parsed values
    printf("You provided:\n");
    printf("Time Interval: %d microseconds\n", time_interval);
    printf("Max Probability: %.2f\n", max_p);
    printf("Min Probability: %.2f\n", min_p);

    // Additional validation
    if (max_p <= min_p) {
        fprintf(stderr, "Error: Max Probability must be greater than Min Probability.\n");
        exit(EXIT_FAILURE);
    }    

    // Create a shared memory segment
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    shmid = shmget(SHM_KEY, sizeof(SharedQueue), IPC_CREAT | 0666);
    if (shmid < 0) {
        perror("shmget");
        return 1;
    }
    // Attach the shared memory segment
    queue = (SharedQueue *)shmat(shmid, NULL, 0);
    if (queue == (void *)-1) {
        perror("shmat");
        return 1;
    }
    // Initialize the queue
    queue->front = 0;
    queue->rear = -1;
    queue->count = 0;
    // Create semaphores
    mutex = sem_open("queue_mutex", O_CREAT, 0666, 1); // Binary semaphore
    empty_slots = sem_open("empty_slots", O_CREAT, 0666, QUEUE_SIZE);
    filled_slots = sem_open("filled_slots", O_CREAT, 0666, 0);

    if (fork() == 0) {
        // Child process
        // + It enqueue the token of closed subflow to let the parent process know what mptcp subflow is closed.
        FILE *fp;
        char buffer[1024];
        const char substring[] = "CLOSED";

        // Open the process to execute the command
        fp = popen("ip mptcp monitor", "r");
        if (fp == NULL) {
            perror("popen");
            return 1;
        }

        // Read each line from the command output
        while (fgets(buffer, sizeof(buffer), fp) != NULL) {
            // Check if the line contains the substring
            if (strstr(buffer, substring)) {
                // Extract the part after "=" if it exists
                char *token = strchr(buffer, '=');
                if (token) {
                    // Move past the "=" and print the result
                    token[strcspn(token, "\n")] = '\0';
                    enqueue(queue, token + 1);
                    printf("[MPTCP Connection Closed]\n    token: %u\n", convert_to_uint32(token + 1));
                }
            }
        }
        // Close the process
        if (pclose(fp) == -1) {
            perror("pclose");
            return 1;
        }
    }
    else{
        // Parent process: Consumer
        //  + This is the main process that actually manages MPTCP subflows.
        sleep(1); // Ensure child starts first
        int map_fd;
        // "round_idx" is a temporal value. It seems the child process somehow misses the close of MPTCP connection rarely.
        // To maintain the bpf map properly, I decided to clean up bpf map every 60 intervals (if interval is 2s, it cleans up the map every 2 min).
        int round_idx = 0; 
        ranflow_key_t map_key = {0}, map_next_key = {0};
        ranflow_data_t map_value;
        HashTable sf_table;
        hash_table_init(&sf_table);
        char buffer[MAX_STRING_LEN];

        // Open the BPF map
        map_fd = bpf_obj_get(RANFLOW_MAP_PATH);
        if (map_fd < 0) {
            perror("Failed to open BPF map");
            return 1;
        }

        // Iterate through all keys in the map
        // if (bpf_map_get_next_key(map_fd, NULL, &map_key) < 0) {
        //     printf("There is no MPTCP connection. Wait....\n");
        //     close(map_fd);
        // }

        printf("Initialize and clean up the BPF map.\n");
        do {
            // Lookup the value for the current key
            if (bpf_map_lookup_elem(map_fd, &map_key, &map_value) == 0) {
                // Update the value in the map
                if (bpf_map_delete_elem(map_fd, &map_key) < 0) {
                    perror("Failed to update map element.");
                }
            }else {
                perror("  Failed to lookup map element. This is normal and means that BPF map is empty before cleaning up");
            }
        } while (bpf_map_get_next_key(map_fd, &map_key, &map_next_key) == 0 && (map_key = map_next_key, 1));
        close(map_fd);

        //Start of while
        while(true)
        {
            // Open the BPF map
            map_fd = bpf_obj_get(RANFLOW_MAP_PATH);
            if (map_fd < 0) {
                perror("Failed to open BPF map");
                return 1;
            }
        
            // Iterate through all keys in the map
            if (bpf_map_get_next_key(map_fd, NULL, &map_key) < 0) {
                printf("There is no MPTCP connection. Wait....\n");
                close(map_fd);
                sleep(2);
                continue;
            }
            do {
                if (bpf_map_lookup_elem(map_fd, &map_key, &map_value) == 0) {
                    hash_table_insert(&sf_table, map_key.token, map_key.local_id, map_key.remote_id);
                } else {
                    perror("Failed to list up map element");
                }
            } while (bpf_map_get_next_key(map_fd, &map_key, &map_next_key) == 0 && (map_key = map_next_key, 1));

            // Control sublfows
            for (int i = 0; i < key_count; i++) {
                int *local_ids, *remote_ids, size;
                if (hash_table_search(&sf_table, all_keys[i], &local_ids, &remote_ids, &size)) {
                    // Found token. In this process, one token represents one mptcp connection. One mptcp connection can have multiple subflows.
                    printf("[MPTCP Connection Found] token: %u, number of subflows: %d\n", all_keys[i], size);
                    uint64_t lt_sum = 0; // sum of linger time
                    int lt_min = -1;
                    int lt_min_local_id = -1;
                    int lt_min_remote_id = -1;
                    bool at_least_one_enabled = false;

                    // Calculate sum of linger time.
                    for (int j = 0; j < size; j++) { // Iterate subflows
                        map_key.token = all_keys[i];
                        map_key.local_id = local_ids[j];
                        map_key.remote_id = remote_ids[j];
                        // Look up subflow info in the bpf map
                        if (bpf_map_lookup_elem(map_fd, &map_key, &map_value) == 0) NULL; 
                        else perror("Failed to look up map element");
                        lt_sum = lt_sum + map_value.linger_time;
                    }
                    // Decide if each subflow will be enabled or disabled (weighted random decision).
                    for (int j = 0; j < size; j++){
                        map_key.token = all_keys[i];
                        map_key.local_id = local_ids[j];
                        map_key.remote_id = remote_ids[j];
                        
                        if (bpf_map_lookup_elem(map_fd, &map_key, &map_value) != 0) perror("Failed to look up map element");

                        if (map_value.linger_time < lt_min){
                            lt_min = map_value.linger_time;
                            lt_min_local_id = local_ids[j];
                            lt_min_remote_id = remote_ids[j];
                        }
			double pb;	
			if (lt_sum<1){
			    pb = 0.5;
			}else{
			    pb = 1.0 - ((double)map_value.linger_time / (double)lt_sum);
                            if (pb < min_p) pb = min_p;
                            else if (pb > max_p) pb = max_p;
			}

                        srand((unsigned int)time(NULL) ^ (unsigned int)clock());
                        float exc_thresh = (double)rand() / (double)(RAND_MAX);

                        if (pb>exc_thresh){
                            map_value.enabled = true; // Subflow enabled
                            if (bpf_map_update_elem(map_fd, &map_key, &map_value, BPF_ANY) < 0) perror("Failed to update map element");
                            printf("    Subflow %d ENABLED!!: local_id=%d, remote_id=%d, linger_time=%lu, wmem=%u enable_probability=%f, excute_thresh=%f\n", j + 1, local_ids[j], remote_ids[j], map_value.linger_time, map_value.wmem, pb, exc_thresh);
                            at_least_one_enabled = true;
                        }else{
                            map_value.enabled = false; // Subflow disabled
                            if (bpf_map_update_elem(map_fd, &map_key, &map_value, BPF_ANY) < 0) perror("Failed to update map element");
                            printf("    Subflow %d DISABLED!!: local_id=%d, remote_id=%d, linger_time=%lu, wmem=%u enable_probability=%f, excute_thresh=%f\n", j + 1, local_ids[j], remote_ids[j], map_value.linger_time, map_value.wmem, pb, exc_thresh);
                        }
                    }
                    if (at_least_one_enabled == false){
                        // If none of subflows enabled, enable the subflow with the shortest linger time.
                        map_key.token = all_keys[i];
                        map_key.local_id = lt_min_local_id;
                        map_key.remote_id = lt_min_remote_id;

                        if (bpf_map_lookup_elem(map_fd, &map_key, &map_value) != 0) perror("Failed to look up map element");

                        map_value.enabled = true; // enable
                        if (bpf_map_update_elem(map_fd, &map_key, &map_value, BPF_ANY) < 0) perror("Failed to update map element");
                        printf("    *Enable subflow with the shorest linger time: local_id=%d, remote_id=%d, linger_time=%lu\n", map_key.local_id, map_key.remote_id, map_value.linger_time);
                    }
                } else {
                    printf("Key for token (%u) not found\n", all_keys[i]);
                }
            }



            // ******************* Remove the entity for closed subflow in the bpfmap
            for (int queue_index = 0; queue_index < queue->count; queue_index++) {
                // Attempt to dequeue an item from the queue
                if (!dequeue(queue, buffer)) {
                    printf("Error in dequeue.\n");
                    break; // Exit loop if dequeue fails
                }

                // Convert the dequeued string to uint32_t
                uint32_t tmp_token = convert_to_uint32(buffer);

                // Iterate through all keys
                for (int key_index = 0; key_index < key_count; key_index++) {
                    if (all_keys[key_index] == tmp_token) {
                        int *local_ids, *remote_ids, size;
                        // Search for the key in the hash table
                        if (hash_table_search(&sf_table, all_keys[key_index], &local_ids, &remote_ids, &size)) {
                            // Iterate through ID combinations
                            for (int id_index = 0; id_index < size; id_index++) {
                                map_key.token = tmp_token;
                                map_key.local_id = local_ids[id_index];
                                map_key.remote_id = remote_ids[id_index];
                                // Delete the element from the BPF map
                                bpf_map_delete_elem(map_fd, &map_key);
                                printf("[Remove eBPF Map Entity]\n    token=%u, local_id=%d, remote_id=%d\n", map_key.token, map_key.local_id, map_key.remote_id);
                            }
                        }
                    }
                }
            }
            // Blow handles the specical case. It seems the kernel calls the scheduler to test, and that causes unregular bpf map entity.
            map_key.token = 0;
            map_key.local_id = -1;
            map_key.remote_id = 0;
            bpf_map_delete_elem(map_fd, &map_key);

            printf("--------------------------------------------\n[Interval %d/60 Ends]\n--------------------------------------------\n",round_idx+1);

            // ******************* Clean up the bpf map regularly
            round_idx += 1;
            if (round_idx >= 59) {
                printf("[Clean up the BPF map] \n");
                do {
                    // Lookup the value for the current key
                    if (bpf_map_lookup_elem(map_fd, &map_key, &map_value) == 0) {
                        // Update the value in the map
                        if (bpf_map_delete_elem(map_fd, &map_key) < 0) {
                            perror("Failed to update map element");
                        }
                    }else {
                        perror("    Failed to lookup map element (It can happen, not an error)");
                    }
                } while (bpf_map_get_next_key(map_fd, &map_key, &map_next_key) == 0 && (map_key = map_next_key, 1));
                round_idx = 0;
            }

            // Delete all entry in hash.
            hash_table_clear(&sf_table);
            close(map_fd);

            // Wait predfined time interval 
            usleep(time_interval);
        }
        // End of while.
        // Queue Cleanup
        shmdt(queue);              // Detach shared memory
        shmctl(shmid, IPC_RMID, 0); // Remove shared memory
        hash_table_free(&sf_table); // Free the hash table
    }
    return 0;
}
