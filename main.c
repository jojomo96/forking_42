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

struct file_content read_entire_file(const char *filename) {
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

static u8 *get_pixel(u8 *pixel_data, const struct bmp_header *header, const u32 x, const u32 y) {
	if (x >= header->width || y >= header->height) {
		return NULL;
	}
	return pixel_data + (y * header->width + x) * (header->bit_per_pixel / 8);
}

static int check_pattern(u8 *pixel_data, const struct bmp_header *header, u8 target_color[3], const u32 x,
                         const u32 y) {
	const int pattern_offsets[][2] = {
		{0, 1}, {0, 2}, {0, 3}, {0, 4}, {0, 5}, {0, 6}, {0, 7},
		{0, 7}, {1, 7}, {2, 7}, {3, 7}, {4, 7}, {5, 7}, {6, 7}
	};
	const int pattern_size = sizeof(pattern_offsets) / sizeof(pattern_offsets[0]);

	for (int i = 0; i < pattern_size; i++) {
		const u32 px = x + pattern_offsets[i][0];
		const u32 py = y + pattern_offsets[i][1];

		u8 *p = get_pixel(pixel_data, header, px, py);
		if (!p) {
			return 0; // Out of bounds
		}
		if (memcmp(p, target_color, 3) != 0) {
			return 0; // Mismatch
		}
	}

	return 1;
}

void find_header(const struct bmp_header *header, u8 target_color[3], u8 *pixel_data, u32 *found_width,
                 u32 *found_height) {
	for (u32 y = 0; y < header->height; ++y) {
		for (u32 x = 0; x < header->width; ++x) {
			u8 *pixel = get_pixel(pixel_data, header, x, y);
			if (pixel && memcmp(pixel, target_color, 3) == 0) {
				if (check_pattern(pixel_data, header, target_color, x, y)) {
					*found_width = x;
					*found_height = y;
					return;
				}
			}
		}
	}


	PRINT_ERROR("Target color not found\n");
	exit(1);
}

int main(const int argc, char **argv) {
	if (argc != 2) {
		PRINT_ERROR("Usage: decode <input_filename>\n");
		return 1;
	}
	const struct file_content file_content = read_entire_file(argv[1]);
	if (file_content.data == NULL) {
		PRINT_ERROR("Failed to read file\n");
		return 1;
	}
	const struct bmp_header *header = (struct bmp_header *) file_content.data;

	u8 target_color[3] = {127, 188, 217};

	u8 *pixel_data = (u8 *) file_content.data + header->data_offset;

	u32 found_width = 0;
	u32 found_height = 0;

	find_header(header, target_color, pixel_data, &found_width, &found_height);

	if (found_width + 7 < header->width) {
		u8 *message_pixel = get_pixel(pixel_data, header, found_width + 7, found_height + 7);
		if (!message_pixel) {
			PRINT_ERROR("Message pixel is out of bounds\n");
			return 1;
		}

		u32 message_length = message_pixel[0] + message_pixel[2];

		const u32 message_starting_pixel_width = found_width + 2;
		const u32 message_starting_pixel_height = found_height + 5;

		u32 current_width = message_starting_pixel_width;
		u32 current_height = message_starting_pixel_height;

		const u32 total_pixels = (message_length + 2) / 3;
		for (u32 i = 0; i < total_pixels; ++i) {
			const u8 *pixel = get_pixel(pixel_data, header, current_width, current_height);
			if (!pixel) {
				PRINT_ERROR("Pixel is out of bounds\n");
				return 1;
			}

			for (int j = 0; j < 3; ++j) {
				write(1, &pixel[j], 1);
				message_length -= 1;
				if (message_length == 0) {
					write(1, "\n", 1);
					return 0;
				}
			}
			current_width += 1;
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
