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


	// Define the target color (e.g., red)
	u8 target_color[3] = {127, 188, 217}; // RGB format

	// Calculate the starting position of the pixel data
	u8 *pixel_data = (u8 *) file_content.data + header->data_offset;

	int is_target_color_found = 0;
	u32 found_x = 0;
	u32 found_y = 0;
	// Iterate through the pixel data
	for (u32 y = 0; y < header->height; ++y) {
		for (u32 x = 0; x < header->width; ++x) {
			u8 *pixel = pixel_data + (y * header->width + x) * (header->bit_per_pixel / 8);
			if (memcmp(pixel, target_color, 3) == 0) {
				found_x = x;
				found_y = y;
				is_target_color_found = 1;
				printf("Found target color at (%u, %u)\n", x, y);
				break;
			}
		}
		if (is_target_color_found) {
			break;
		}
	}

	if (is_target_color_found) {
		printf("Target color not found\n");
	}

	if (is_target_color_found) {
		// Ensure the pixel 7 positions to the right is within bounds
		if (found_x + 7 < header->width) {
			u8 *message_pixel = pixel_data + ((found_y+ 8) * header->width + (found_x )) * (header->bit_per_pixel / 8);
			u8 message_length[3];
			memcpy(message_length, message_pixel, 3);
			printf("Message length color: R=%u, G=%u, B=%u\n", message_length[0], message_length[1], message_length[2]);

			u32 message_length_sum = message_length[0] + message_length[1] + message_length[2];
			printf("Message length sum: %u\n", message_length_sum);
		} else {
			printf("Pixel 7 positions to the right is out of bounds\n");
		}
	}

	printf("Target color not found\n");
	return 0;
}
