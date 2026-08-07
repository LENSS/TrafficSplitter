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
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/file.h>

// Path to eBPF map
#define SAFLO_MAP_PATH "/sys/fs/bpf/saflo_map"
#define DETECTION_MAP_PATH "/sys/fs/bpf/detection_map"

typedef struct {
    uint32_t token;
    int16_t local_id;
    uint8_t remote_id;
} saflo_key_t;

typedef struct {
    uint32_t avg_pacing;
    uint32_t wmem;
    uint64_t linger_time;
    bool enabled;
    bool safe;
} saflo_data_t;

typedef struct {
	uint32_t token;
	uint64_t timestamp_ns; 
} detection_key_t;

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

// User Space Hash function
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

// Function to convert uint32_t to a hexadecimal string
char *uint32_to_hex_string(uint32_t value) {
    static char hex_string[9]; // Static buffer to hold the result (8 digits + null terminator)
    snprintf(hex_string, sizeof(hex_string), "%08x", value);
    return hex_string;
}

#define MAX_TOKENS 1000
// Function to extract tokens as uint32_t values from the command output
uint32_t *extract_tokens_as_uint32(int *token_count) {
    FILE *pipe;
    char buffer[4096];
    uint32_t *tokens = malloc(MAX_TOKENS * sizeof(uint32_t)); // Dynamically allocate space for tokens
    const char *command = "ss --mptcp -i | grep token";
    *token_count = 0;

    if (tokens == NULL) {
        perror("malloc failed");
        return NULL;
    }

    // Run the command and open a pipe
    pipe = popen(command, "r");
    if (pipe == NULL) {
        perror("popen failed");
        free(tokens);
        return NULL;
    }

    // Read the command output line by line
    while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
        const char *token_prefix = "token:";
        char *start = strstr(buffer, token_prefix); // Find "token:"
        if (start) {
            start += strlen(token_prefix); // Move pointer past "token:"
            const char *end = start;

            // Find the end of the token
            while (*end && *end != ' ') {
                end++;
            }

            // Extract the token string
            size_t token_length = end - start;
            if (token_length > 0 && *token_count < MAX_TOKENS) {
                char token_str[11]; // Enough to hold a uint32 in hex (8 digits + null)
                if (token_length >= sizeof(token_str)) {
                    fprintf(stderr, "Token too long, skipping.\n");
                    continue;
                }
                strncpy(token_str, start, token_length);
                token_str[token_length] = '\0'; // Null-terminate the token string

                // Convert the token string to uint32_t
                char *endptr;
                errno = 0;
                uint32_t token_value = strtoul(token_str, &endptr, 16);
                if (errno != 0 || *endptr != '\0') {
                    fprintf(stderr, "Failed to convert token '%s' to uint32_t\n", token_str);
                    continue;
                }

                // Store the converted value in the tokens array
                tokens[*token_count] = token_value;
                (*token_count)++;
            }
        }
    }

    // Close the pipe
    int ret = pclose(pipe);
    if (ret == -1) {
        perror("pclose failed");
        free(tokens);
        return NULL;
    }

    return tokens;
}

// Function to check if a string exists in an array of strings
bool string_in_array(const char *str, const char *array[], int array_size) {
    for (int i = 0; i < array_size; i++) {
        if (strcmp(str, array[i]) == 0) {
            return true; // Match found
        }
    }
    return false; // No match
}

// Function to check if an integer exists in an array of integers
bool int_in_array(int value, const int array[], int array_size) {
    for (int i = 0; i < array_size; i++) {
        if (value == array[i]) {
            return true; // Match found
        }
    }
    return false; // No match
}

void flush_iptables() {
    int status = system("sudo iptables -F");
    if (status == -1) {
        perror("Error executing system command");
    } else {
        printf("Command executed with status: %d\n", WEXITSTATUS(status));
    }
}

// Function to terminate the socket with token
bool process_token_and_block(const char *token) {
    char buffer[4096];
    char prev_line[4096] = "";
    char found_line[4096];
    FILE *pipe;

    // Execute the "ss --mptcp -i" command
    pipe = popen("ss --mptcp -i", "r");
    if (pipe == NULL) {
        perror("popen failed");
        return false;
    }

    // Search for the line containing the token
    bool token_found = false;
    while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
        if (strstr(buffer, token) != NULL) {
            strncpy(found_line, prev_line, sizeof(found_line) - 1);
            found_line[sizeof(found_line) - 1] = '\0'; // Ensure null termination
            token_found = true;
            break;
        }
        strncpy(prev_line, buffer, sizeof(prev_line) - 1);
        prev_line[sizeof(prev_line) - 1] = '\0'; // Save the current line as the previous line
    }

    pclose(pipe);

    if (!token_found) {
        fprintf(stderr, "Token '%s' not found in ss output.\n", token);
        return false;
    }

    // Extract the dport from the found line (second IP port)
    char *start = strrchr(found_line, ':'); // Find the last colon in the line
    if (!start) {
        fprintf(stderr, "Failed to find the second IP port in the line: %s\n", found_line);
        return false;
    }

    start++; // Move past the colon
    int dport = atoi(start); // Convert the port to an integer
    if (dport <= 0) {
        fprintf(stderr, "Invalid dport extracted: %d\n", dport);
        return false;
    }

    printf("Found dport: %d\n", dport);

    // Check if the rule already exists
    char check_command[128];
    snprintf(check_command, sizeof(check_command), "sudo iptables -C OUTPUT -p tcp --dport %d -j DROP", dport);
    int ret = system(check_command);
    if (ret == 0) {
        printf("Rule already exists for dport %d. Skipping addition.\n", dport);
        return true;
    } else if (WEXITSTATUS(ret) == 1) {
        // Rule doesn't exist, add it
        char add_command[128];
        snprintf(add_command, sizeof(add_command), "sudo iptables -I OUTPUT -p tcp --dport %d -j DROP", dport);
        ret = system(add_command);
        if (ret == -1) {
            perror("system command failed");
            return false;
        }

        if (WEXITSTATUS(ret) != 0) {
            fprintf(stderr, "iptables command failed with exit code %d\n", WEXITSTATUS(ret));
            return false;
        }

        printf("Iptables rule added successfully for dport %d\n", dport);
        return true;
    } else {
        // Unexpected error during the check
        fprintf(stderr, "Error checking iptables rule for dport %d\n", dport);
        return false;
    }
}

int delete_file_with_lock(const char *file_path) {
    int fd = open(file_path, O_RDWR);
    if (fd == -1) {
        perror("Failed to open file");
        return -1;
    }

    // Acquire an exclusive lock
    if (flock(fd, LOCK_EX | LOCK_NB) == -1) {
        perror("Failed to acquire lock");
        close(fd);
        return -1;
    }

    // Delete the file
    if (remove(file_path) == 0) {
        printf("File '%s' deleted successfully.\n", file_path);
    } else {
        perror("Failed to delete the file");
        // Release the lock
        flock(fd, LOCK_UN);
        close(fd);
        return -1;
    }

    // Release the lock
    flock(fd, LOCK_UN);
    close(fd);
    return 0;
}

int main(int argc, char *argv[]) {
    // Get user inputs
    int opt;
    int time_interval = 0;
    float max_p = 0.0, min_p = 0.0;
    int enable_detection = 0; // Default: disabled

    // Parse command-line arguments
    while ((opt = getopt(argc, argv, "i:x:n:d:")) != -1) {
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
        case 'd': // Enable/Disable Detection
            enable_detection = atoi(optarg);
            if (enable_detection != 0 && enable_detection != 1) {
                fprintf(stderr, "Error: Detection flag (-d) must be 0 (disable) or 1 (enable).\n");
                exit(EXIT_FAILURE);
            }
            break;
        default: // Invalid option
            fprintf(stderr, "Usage: %s -i <time_interval> -x <max_p> -n <min_p> -d <enable_detection>\n", argv[0]);
            fprintf(stderr, "Example: sudo ./subflow_manager -i 500000 -x 0.8 -n 0.2 -d 1\n");
            exit(EXIT_FAILURE);
        }
    }

    // Check if all required arguments are provided
    if (time_interval == 0 || max_p == 0.0 || min_p == 0.0) {
        fprintf(stderr, "All options (-i, -x, -n) must be provided.\n");
        fprintf(stderr, "Usage: %s -i <time_interval(μs)> -x <max_p> -n <min_p> -d <enable_detection>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Display the parsed values
    printf("You provided:\n");
    printf("Time Interval: %d microseconds\n", time_interval);
    printf("Max Probability: %.2f\n", max_p);
    printf("Min Probability: %.2f\n", min_p);
    printf("Attack Detection: %s\n", enable_detection ? "Enabled" : "Disabled");    

    // Parent process: Consumer
    //  + This is the main process that actually manages MPTCP subflows.
    sleep(1); // Ensure child starts first
    int map_fd, detection_map_fd;
    // "round_idx" is a temporal value. It seems the child process somehow misses the close of MPTCP connection rarely.
    // To maintain the bpf map properly, I decided to clean up bpf map every 60 intervals (if interval is 2s, it cleans up the map every 2 min).
    int round_idx = 0; 
    saflo_key_t map_key = {0}, map_next_key = {0};
    saflo_data_t map_value;
    detection_key_t detection_key= {0}, detection_next_key = {0};
    HashTable sf_table;
    hash_table_init(&sf_table);

    // Open the BPF map
    map_fd = bpf_obj_get(SAFLO_MAP_PATH);
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
        }
        // else {
        //     perror("  Failed to lookup map element. This is normal and means that BPF map is empty before cleaning up");
        // }
    } while (bpf_map_get_next_key(map_fd, &map_key, &map_next_key) == 0 && (map_key = map_next_key, 1));
    close(map_fd);

    const char *log_file_path = "./detection.log";
    //Start of while
    while(true)
    {
        if(enable_detection>0){
            /////////////////////////////////////////////////////////////////////////////////////////////////
            // Start of attack detection ////////////////////////////////////////////////////////////////////
            /////////////////////////////////////////////////////////////////////////////////////////////////
            // Open the BPF map for detection
            detection_map_fd = bpf_obj_get(DETECTION_MAP_PATH);
            if (detection_map_fd < 0) {
                perror("Failed to open Detection BPF map");
                return 1;
            }
            // Write the burst (traffic) data in to log file
            uint32_t burst;
            FILE *log_file = fopen(log_file_path, "a");
            if (!log_file) {
                perror("Failed to open detection.log");
                return 1; // Exit the loop if the file cannot be opened
            }
            do {
                // Lookup the value for the current key
                if (bpf_map_lookup_elem(detection_map_fd, &detection_key, &burst) == 0) {
                    // Open the log file in append mode


                    // Write the data to the file
                    fprintf(log_file, "token:%u time:%lu burst:%u\n", detection_key.token, detection_key.timestamp_ns, burst);

                    // Delete the processed key from the map
                    if (bpf_map_delete_elem(detection_map_fd, &detection_key) < 0) {
                        perror("Failed to delete map element.");
                    }
                } 
                // else {
                //     perror("Failed to lookup map element from detection map. This is fine if there is no active subflow.");
                // }
            } while (bpf_map_get_next_key(detection_map_fd, &detection_key, &detection_next_key) == 0 && (detection_key = detection_next_key, 1));
            close(detection_map_fd);
            // Close the log file
            fclose(log_file);

            // Read the list of malicious tokens
            const char *at_shm_name = "shm_detect";  // Shared memory for attck detection
            size_t size = 4000;  // Size in bytes (must match Python)
            // Open the shared memory block
            int at_shm_fd = shm_open(at_shm_name, O_RDONLY, 0666);
            if (at_shm_fd == -1) {
                perror("Failed to open shared memory for detection: the attack detector may be terminated.");
                return 1;
            }
            // Map the shared memory block
            void *at_shm_ptr = mmap(NULL, size, PROT_READ, MAP_SHARED, at_shm_fd, 0);
            if (at_shm_ptr == MAP_FAILED) {
                perror("Failed to map shared memory for detection");
                close(at_shm_fd);
                return 1;
            }
            // Access the data
            int *at_data = (int *)at_shm_ptr;
            size_t num_integers = size / sizeof(uint32_t);  // Calculate the number of integers
            uint32_t *malicious_tokens = malloc(size);  // Allocate memory for the local copy
            if (malicious_tokens == NULL) {
                perror("Failed to allocate memory for local copy");
                munmap(at_shm_ptr, size);
                close(at_shm_fd);
                return 1;
            }
            else{
                memcpy(malicious_tokens, at_data, size);  // Copy shared memory data to the local array
            }
            close(at_shm_fd);

            // Open the BPF map
            map_fd = bpf_obj_get(SAFLO_MAP_PATH);
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
                    ///////////////////////////////////////////////////
                    // Mark if token is unsafe ///////////////////////
                    //////////////////////////////////////////////////
                    for (int z = 0; z < num_integers; z ++){
                        if (malicious_tokens[z] == 0) break;
                        else if (malicious_tokens[z] == map_key.token){
                            // If the subflow is a secured endpoint, we just leave it operate.
                            if (map_key.local_id == 0 && map_key.remote_id == 0) continue;
                            map_value.safe = 0;
                            bpf_map_update_elem(map_fd, &map_key, &map_value, BPF_ANY);
                            break;
                        }
                    }
                    hash_table_insert(&sf_table, map_key.token, map_key.local_id, map_key.remote_id);
                } else {
                    perror("Failed to list up map element");
                }
            } while (bpf_map_get_next_key(map_fd, &map_key, &map_next_key) == 0 && (map_key = map_next_key, 1));
            /////////////////////////////////////////////////////////////////////////////////////////////////
            // End of attack detection //////////////////////////////////////////////////////////////////////
            /////////////////////////////////////////////////////////////////////////////////////////////////
        }
        else{ //Detection is not enabled
            // Open the BPF map for detection
            detection_map_fd = bpf_obj_get(DETECTION_MAP_PATH);
            if (detection_map_fd < 0) {
                perror("Failed to open Detection BPF map");
                return 1;
            }
            uint32_t burst;
            do {
                // Lookup the value for the current key
                if (bpf_map_lookup_elem(detection_map_fd, &detection_key, &burst) == 0) {
                    // Delete the processed key from the map
                    if (bpf_map_delete_elem(detection_map_fd, &detection_key) < 0) {
                        perror("Failed to delete map element.");
                    }
                } 

            } while (bpf_map_get_next_key(detection_map_fd, &detection_key, &detection_next_key) == 0 && (detection_key = detection_next_key, 1));
            close(detection_map_fd);
            // Close the log file

            // Open the BPF map
            map_fd = bpf_obj_get(SAFLO_MAP_PATH);
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
        }

        /////////////////////////////////////////////////////////////////////////////////////////////////
        // Start of random decision for subflows ////////////////////////////////////////////////////////
        /////////////////////////////////////////////////////////////////////////////////////////////////
        for (int i = 0; i < key_count; i++) {
            int *local_ids, *remote_ids, size;
            if (hash_table_search(&sf_table, all_keys[i], &local_ids, &remote_ids, &size)) {
                // Found token. In this process, one token represents one mptcp connection. One mptcp connection can have multiple subflows.
                printf("[MPTCP Connection Found] token: %u, number of subflows: %d\n", all_keys[i], size);
                uint64_t lt_sum = 0; // sum of linger time
                int lt_min = -1;
                int lt_min_local_id = -1;
                int lt_min_remote_id = -1;
                int unsafe_sf = 0;
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

                    if (map_value.safe == 0){
                        map_value.enabled = false; // Subflow is unsafe
                        if (bpf_map_update_elem(map_fd, &map_key, &map_value, BPF_ANY) < 0) perror("Failed to update map element");
                        printf("    Subflow %d UNSAFE!!: local_id=%d, remote_id=%d, linger_time=%lu, wmem=%u enable_probability=%f, excute_thresh=%f\n", j + 1, local_ids[j], remote_ids[j], map_value.linger_time, map_value.wmem, pb, exc_thresh);
                        unsafe_sf++;
                    }
                    else if (pb>exc_thresh){
                        map_value.enabled = true; // Subflow enabled
                        if (bpf_map_update_elem(map_fd, &map_key, &map_value, BPF_ANY) < 0) perror("Failed to update map element");
                        printf("    Subflow %d ENABLED!!: local_id=%d, remote_id=%d, linger_time=%lu, wmem=%u enable_probability=%f, excute_thresh=%f\n", j + 1, local_ids[j], remote_ids[j], map_value.linger_time, map_value.wmem, pb, exc_thresh);
                        at_least_one_enabled = true;
                    }else{
                        map_value.enabled = false; // Subflow disabled
                        if (bpf_map_update_elem(map_fd, &map_key, &map_value, BPF_ANY) < 0) perror("Failed to update map element");
                        printf("    Subflow %d DISABLED!!: local_id=%d, remote_id=%d, linger_time=%lu, wmem=%u enable_probability=%f, excute_thresh=%f\n", j + 1, local_ids[j], remote_ids[j], map_value.linger_time, map_value.wmem, pb, exc_thresh);
                        // We need to know the subflow with the shortest linger time ONLY when all subflows are disabled
                        if (map_value.linger_time < lt_min){
                            lt_min = map_value.linger_time;
                            lt_min_local_id = local_ids[j];
                            lt_min_remote_id = remote_ids[j];
                        }
                    }
                }
                if (unsafe_sf == size){
                    // Here we should handle the case where all subflows are not safe.
                    // Simply, we just terminate the socket.
                    // Somehow the below codes do not block the traffic. Hmm It does not work for now.
                    if (process_token_and_block(uint32_to_hex_string(all_keys[i]))){
                        printf("    *BLOCK THIS MPTCP SOCKET\n");
                    }
                }
                if (at_least_one_enabled == false && lt_min_local_id > -1){
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
        /////////////////////////////////////////////////////////////////////////////////////////////////
        // End of random decision for subflows //////////////////////////////////////////////////////////
        /////////////////////////////////////////////////////////////////////////////////////////////////

        // ******************* Remove the entity for closed subflow in the bpfmap
        int token_count = 0;
        uint32_t *tokens = extract_tokens_as_uint32(&token_count);

        if (tokens != NULL) {
            // Iterate through all keys
            for (int key_index = 0; key_index < key_count; key_index++) {
                if (!int_in_array(all_keys[key_index], tokens, token_count)) {
                    int *local_ids, *remote_ids, size;
                    // Search for the key in the hash table
                    if (hash_table_search(&sf_table, all_keys[key_index], &local_ids, &remote_ids, &size)) {
                        // Iterate through ID combinations
                        for (int id_index = 0; id_index < size; id_index++) {
                            map_key.token = all_keys[key_index];
                            map_key.local_id = local_ids[id_index];
                            map_key.remote_id = remote_ids[id_index];
                            // Delete the element from the BPF map
                            bpf_map_delete_elem(map_fd, &map_key);
                            printf("[Remove eBPF Map Entity]\n    token=%u, local_id=%d, remote_id=%d\n", map_key.token, map_key.local_id, map_key.remote_id);
                        }
                    }
                }
            }
            free(tokens); // Free the array of tokens
        } else {
            printf("Failed to extract tokens.\n");
        }

        printf("--------------------------------------------\n[Interval %d/60 Ends]\n--------------------------------------------\n",round_idx+1);

        // ******************* Clean up the bpf map regularly
        round_idx += 1;
        if (round_idx > 59){
            round_idx=0;
            if(enable_detection>0){
                if (delete_file_with_lock(log_file_path) == 0) {
                    printf("Detection log deleted with lock successfully.\n");
                } else {
                    printf("Failed to delete Detection log with lock.\n");
                }
            }
        }
        // Delete all entry in hash.
        hash_table_clear(&sf_table);
        close(map_fd);

        // Wait predfined time interval 
        usleep(time_interval);
    }
    // End of while.
    // Queue Cleanup
    hash_table_free(&sf_table); // Free the hash table
    return 0;
}