#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>

typedef char i8;
typedef unsigned char u8;
typedef unsigned short u16;
typedef int i32;
typedef unsigned u32;
typedef unsigned long u64;

#define PRINT_ERROR(cstring) write(STDERR_FILENO, cstring, sizeof(cstring) - 1)

#pragma pack(1)
struct bmp_header {
    // Note: header
    i8 signature[2]; // should equal to "BM"
    u32 file_size;
    u32 unused_0;
    u32 data_offset;

    // Note: info header
    u32 info_header_size;
    u32 width; // in px
    u32 height; // in px
    u16 number_of_planes; // should be 1
    u16 bit_per_pixel; // 1, 4, 8, 16, 24 or 32
    u32 compression_type; // should be 0
    u32 compressed_image_size; // should be 0
    // Note: there are more stuff there but it is not important here
};

struct file_content {
    i8 *data;
    u32 size;
};

struct file_content read_entire_file(char *filename) {
    char *file_data = 0;
    unsigned long file_size = 0;
    int input_file_fd = open(filename, O_RDONLY);
    if (input_file_fd >= 0) {
        struct stat input_file_stat = {0};
        stat(filename, &input_file_stat);
        file_size = input_file_stat.st_size;
        file_data = mmap(0, file_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, input_file_fd, 0);
        close(input_file_fd);
    }
    return (struct file_content){file_data, file_size};
}

int main(int argc, char **argv) {
    if (argc != 2) {
        PRINT_ERROR("Usage: decode <input_filename>\n");
        return 1;
    }
    struct file_content file_content = read_entire_file(argv[1]);
    if (file_content.data == NULL) {
        PRINT_ERROR("Failed to read file\n");
        return 1;
    }
    struct bmp_header *header = (struct bmp_header *) file_content.data;
    printf(
        "signature: %.2s\nfile_size: %u\ndata_offset: %u\ninfo_header_size: %u\nwidth: %u\nheight: %u\nplanes: %i\nbit_per_px: %i\ncompression_type: %u\ncompression_size: %u\n",
        header->signature, header->file_size, header->data_offset, header->info_header_size, header->width,
        header->height, header->number_of_planes, header->bit_per_pixel, header->compression_type,
        header->compressed_image_size);

    // Create a copy of the BMP file data
    i8 *file_copy = malloc(file_content.size);
    memcpy(file_copy, file_content.data, file_content.size);

    // Define the target color (e.g., red)
    u8 target_color[3] = {127, 188, 217}; // RGB format

    // Calculate the starting position of the pixel data
    u8 *pixel_data = (u8 *) file_content.data + header->data_offset;
    u8 *pixel_data_copy = (u8 *) file_copy + header->data_offset;

    int is_target_color_found = 0;
    u32 found_width = 0;
    u32 found_height = 0;
    // Iterate through the pixel data
    for (u32 y = 0; y < header->height; ++y) {
        for (u32 x = 0; x < header->width; ++x) {
            u8 *pixel = pixel_data + (y * header->width + x) * (header->bit_per_pixel / 8);
            if (memcmp(pixel, target_color, 3) == 0) {
                found_width = x;
                found_height = y;
                is_target_color_found = 1;
                printf("Found target color at (%u, %u)\n", x, y);
                break;
            }
        }
        if (is_target_color_found) {
            break;
        }
    }

    if (!is_target_color_found) {
        printf("Target color not found\n");
        free(file_copy);
        return 0;
    }
    u32 message_length;
    // Ensure the pixel 7 positions to the right is within bounds
    if (found_width + 7 < header->width) {
        u8 *message_pixel = pixel_data + ((found_height + 7) * header->width + (found_width + 7)) * (header->bit_per_pixel / 8);
        u8 message_length_color[3];
        memcpy(message_length_color, message_pixel, 3);
        printf("Message length color: R=%u, G=%u, B=%u\n", message_length_color[0], message_length_color[1], message_length_color[2]);

        message_length = message_length_color[0] + message_length_color[2];
        printf("Message length sum: %u\n", message_length);

        // Color the message length pixel in the copy
        u8 *message_pixel_copy = pixel_data_copy + ((found_height + 7) * header->width + (found_width + 7)) * (header->bit_per_pixel / 8);
        message_pixel_copy[0] = 255; // Blue
        message_pixel_copy[1] = 0;   // Green
        message_pixel_copy[2] = 0;   // Red

        message_pixel_copy = pixel_data_copy + ((found_height + 7) * header->width + (found_width)) * (header->bit_per_pixel / 8);
        message_pixel_copy[0] = 0; // Blue
        message_pixel_copy[1] = 255;  // Green
        message_pixel_copy[2] = 0;   //Red


        u32 message_starting_pixel_width = found_width + 2;
        u32 message_starting_pixel_height = found_height + 5;

        u32 current_width = message_starting_pixel_width;
        u32 current_height = message_starting_pixel_height;

        // Assuming message_length is the total number of pixels (not triplets).
        // If message_length refers to triplets of pixels, adjust accordingly.
        u32 total_pixels = (message_length + 2) / 3;

        for (u32 i = 0; i < total_pixels; ++i) {
            // Compute the pixel location in memory:
            u8 *message_pixel = pixel_data_copy + (current_height * header->width + current_width) * (header->bit_per_pixel / 8);

            // Set pixel color (BGR format assumed):
            message_pixel[0] = 0;    // Blue
            message_pixel[1] = 0;    // Green
            message_pixel[2] = 255;  // Red

            // Move one pixel to the right
            current_width += 1;

            // Every 8 pixels, reset width and move up one row
            if ((i + 1) % 6 == 0) {
                current_width = message_starting_pixel_width; // Reset to start width
                current_height -= 1;                          // Move up one row
            }
        }
    } else {
        printf("Pixel 7 positions to the right is out of bounds\n");
        free(file_copy);
        return 0;
    }



    // Write the modified data to a new file
    int output_file_fd = open("test.bmp", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (output_file_fd >= 0) {
        write(output_file_fd, file_copy, file_content.size);
        close(output_file_fd);
    } else {
        PRINT_ERROR("Failed to write output file\n");
    }

    free(file_copy);
    return 0;
}
