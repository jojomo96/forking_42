#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <immintrin.h>

typedef char i8;
typedef unsigned char u8;
typedef unsigned short u16;
typedef int i32;
typedef unsigned u32;
typedef unsigned long u64;

#define PRINT_ERROR(cstring) write(STDERR_FILENO, cstring, sizeof(cstring) - 1)

#pragma pack(1)
struct bmp_header {
    i8 signature[2];
    u32 file_size;
    u32 unused_0;
    u32 data_offset;
    u32 info_header_size;
    u32 width;
    u32 height;
    u16 number_of_planes;
    u16 bit_per_pixel;
    u32 compression_type;
    u32 compressed_image_size;
    // rest omitted
};

struct file_content {
    i8 *data;
    u32 size;
};

static struct file_content read_entire_file(const char *filename) {
    int input_file_fd = open(filename, O_RDONLY);
    char *file_data = NULL;
    unsigned long file_size = 0;
    if (input_file_fd >= 0) {
        struct stat input_file_stat;
        if (fstat(input_file_fd, &input_file_stat) == 0) {
            file_size = (unsigned long)input_file_stat.st_size;
            file_data = mmap(0, file_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, input_file_fd, 0);
        }
        close(input_file_fd);
    }
    return (struct file_content){file_data, (u32)file_size};
}

// Pattern offsets
static const int pattern_offsets[][2] = {
    {0, 1}, {0, 2}, {0, 3}, {0, 4}, {0, 5}, {0, 6}, {0, 7},
    {0, 7}, {1, 7}, {2, 7}, {3, 7}, {4, 7}, {5, 7}, {6, 7}
};
static const int pattern_size = (int)(sizeof(pattern_offsets) / sizeof(pattern_offsets[0]));

// Inline pixel pointer calculation
static inline u8* pixel_ptr(u8 *pixel_data, u32 bytes_per_pixel, u32 width, u32 x, u32 y) {
    return pixel_data + ((u64)y * (u64)width + (u64)x) * bytes_per_pixel;
}

// Use SSE/AVX for color matching. We only have 3 bytes to compare, but we'll use a 32-bit load.
// We'll load 4 bytes from p and from target_color (with padded zero) and compare.
// To avoid misalignment issues, we just load into a 32-bit integer directly and compare.
static inline int color_match(const u8 *p, const u8 target_color[3]) {
    // We can pack target_color into a 32-bit integer
    uint32_t p_val = *(const uint32_t *)p & 0x00FFFFFF;
    uint32_t t_val = (uint32_t)target_color[0]
                   | ((uint32_t)target_color[1] << 8)
                   | ((uint32_t)target_color[2] << 16);
    return __builtin_expect(p_val == t_val, 0);
}

// Precompute max pattern offsets to avoid recomputing them every time:
static inline void pattern_max_offsets(u32 *max_dx, u32 *max_dy) {
    u32 local_max_dx = 0;
    u32 local_max_dy = 0;
    for (int i = 0; i < pattern_size; i++) {
        u32 dx = (u32)pattern_offsets[i][0];
        u32 dy = (u32)pattern_offsets[i][1];
        if (dx > local_max_dx) local_max_dx = dx;
        if (dy > local_max_dy) local_max_dy = dy;
    }
    *max_dx = local_max_dx;
    *max_dy = local_max_dy;
}

// Check the pattern at a given (x, y)
static inline int check_pattern(u8 *pixel_data, const struct bmp_header *header, const u8 target_color[3],
                               const u32 x, const u32 y, u32 bytes_per_pixel,
                               u32 max_dx, u32 max_dy) {
    const u32 width = header->width;
    const u32 height = header->height;

    if (__builtin_expect(x + max_dx >= width || y + max_dy >= height, 0)) {
        return 0;
    }

    for (int i = 0; i < pattern_size; i++) {
        u32 px = x + (u32)pattern_offsets[i][0];
        u32 py = y + (u32)pattern_offsets[i][1];
        const u8 *p = pixel_data + ((u64)py * width + px) * bytes_per_pixel;
        if (!color_match(p, target_color)) {
            return 0; // Mismatch
        }
    }

    return 1;
}

// Shared data for threads
struct thread_data {
    const struct bmp_header *header;
    const u8 *target_color;
    u8 *pixel_data;
    u32 bytes_per_pixel;
    u32 start_line;
    u32 end_line;
    volatile int *found;
    u32 *found_x;
    u32 *found_y;
    pthread_mutex_t *found_lock;
    u32 max_dx;
    u32 max_dy;
};

static void* find_pattern_thread(void *arg) {
    struct thread_data *td = (struct thread_data *)arg;
    const struct bmp_header *header = td->header;
    const u32 width = header->width;
    const u32 bytes_per_pixel = td->bytes_per_pixel;
    u8 *pixel_data = td->pixel_data;
    const u8 *target_color = td->target_color;

    int local_found = 0;
    u32 local_x = 0, local_y = 0;

    for (u32 y = td->start_line; y < td->end_line && !local_found; ++y) {
        // Check global found flag occasionally
        if (__atomic_load_n(td->found, __ATOMIC_ACQUIRE))
            break;
        u8 *row_ptr = pixel_data + (u64)y * width * bytes_per_pixel;
        for (u32 x = 0; x < width && !local_found; ++x) {
            const u8 *pixel = row_ptr + x * bytes_per_pixel;
            if (color_match(pixel, target_color)) {
                // Check the pattern
                if (check_pattern(pixel_data, header, target_color, x, y, bytes_per_pixel, td->max_dx, td->max_dy)) {
                    local_found = 1;
                    local_x = x;
                    local_y = y;
                    break;
                }
            }
        }
    }

    if (local_found) {
        pthread_mutex_lock(td->found_lock);
        if (!(*td->found)) {
            *td->found = 1;
            *td->found_x = local_x;
            *td->found_y = local_y;
        }
        pthread_mutex_unlock(td->found_lock);
    }

    return NULL;
}

static void find_header(const struct bmp_header *header, const u8 target_color[3], u8 *pixel_data,
                        u32 *found_width, u32 *found_height) {
    const u32 height = header->height;
    const u32 bytes_per_pixel = header->bit_per_pixel / 8;

    int num_threads = 4; // Adjust according to your system
    pthread_t threads[num_threads];
    struct thread_data tdata[num_threads];

    volatile int found = 0;
    u32 found_x = 0, found_y = 0;

    pthread_mutex_t found_lock = PTHREAD_MUTEX_INITIALIZER;

    u32 max_dx, max_dy;
    pattern_max_offsets(&max_dx, &max_dy);

    u32 chunk_size = height / (u32)num_threads;
    for (int i = 0; i < num_threads; i++) {
        u32 start_line = i * chunk_size;
        u32 end_line = (i == num_threads - 1) ? height : start_line + chunk_size;

        tdata[i].header = header;
        tdata[i].target_color = target_color;
        tdata[i].pixel_data = pixel_data;
        tdata[i].bytes_per_pixel = bytes_per_pixel;
        tdata[i].start_line = start_line;
        tdata[i].end_line = end_line;
        tdata[i].found = &found;
        tdata[i].found_x = &found_x;
        tdata[i].found_y = &found_y;
        tdata[i].found_lock = &found_lock;
        tdata[i].max_dx = max_dx;
        tdata[i].max_dy = max_dy;

        pthread_create(&threads[i], NULL, find_pattern_thread, &tdata[i]);
    }

    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    if (!found) {
        PRINT_ERROR("Target color not found\n");
        exit(1);
    } else {
        *found_width = found_x;
        *found_height = found_y;
    }
}

int main(const int argc, char **argv) {
    if (argc != 2) {
        PRINT_ERROR("Usage: decode <input_filename>\n");
        return 1;
    }

    struct file_content file_content = read_entire_file(argv[1]);
    if (file_content.data == NULL) {
        PRINT_ERROR("Failed to read file\n");
        return 1;
    }
    const struct bmp_header *header = (struct bmp_header *) file_content.data;

    if (header->signature[0] != 'B' || header->signature[1] != 'M') {
        PRINT_ERROR("Not a valid BMP file.\n");
        return 1;
    }

    const u32 width = header->width;
    const u32 bytes_per_pixel = header->bit_per_pixel / 8;
    if (bytes_per_pixel < 3) {
        PRINT_ERROR("Unsupported BMP bit depth.\n");
        return 1;
    }

    u8 target_color[3] = {127, 188, 217};
    u8 *pixel_data = (u8 *) file_content.data + header->data_offset;

    u32 found_width = 0;
    u32 found_height = 0;
    find_header(header, target_color, pixel_data, &found_width, &found_height);

    // Decode message
    if (found_width + 7 < width) {
        u8 *message_pixel = pixel_ptr(pixel_data, bytes_per_pixel, width, found_width + 7, found_height + 7);
        if (!message_pixel) {
            PRINT_ERROR("Message pixel is out of bounds\n");
            return 1;
        }

        u32 message_length = (u32)(message_pixel[0] + message_pixel[2]);

        const u32 message_starting_pixel_width = found_width + 2;
        const u32 message_starting_pixel_height = found_height + 5;

        u32 current_width = message_starting_pixel_width;
        u32 current_height = message_starting_pixel_height;

        char message[511];
        int message_index = 0;
        const u32 total_pixels = (message_length + 2) / 3;

        for (u32 i = 0; i < total_pixels; ++i) {
            const u8 *pixel = pixel_ptr(pixel_data, bytes_per_pixel, width, current_width, current_height);
            if (!pixel) {
                PRINT_ERROR("Pixel is out of bounds\n");
                return 1;
            }

            for (int j = 0; j < 3; ++j) {
                message[message_index++] = pixel[j];
                message_length -= 1;
                if (message_length == 0) {
                    write(STDOUT_FILENO, message, (size_t)message_index);
                    write(STDOUT_FILENO, "\n", 1);
                    return 0;
                }
            }
            current_width += 1;
            // every 6 pixels, move up one row
            if ((i + 1) % 6 == 0) {
                current_width = message_starting_pixel_width;
                current_height -= 1;
            }
        }
    } else {
        PRINT_ERROR("Pixel 7 positions to the right is out of bounds\n");
        return 0;
    }

    return 0;
}
