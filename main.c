#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

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
	i8 signature[2]; // should equal "BM"
	u32 file_size;
	u32 unused_0;
	u32 data_offset;

	// Note: info header
	u32 info_header_size;
	u32 width; // in px
	u32 height; // in px
	u16 number_of_planes; // should be 1
	u16 bit_per_pixel; // 1,4,8,16,24,32
	u32 compression_type; // 0 if uncompressed
	u32 compressed_image_size; // usually 0 if not compressed
	// rest omitted
};

struct file_content {
	i8 *data;
	u32 size;
};

static struct file_content read_entire_file(const char *filename) {
	char *file_data = NULL;
	unsigned long file_size = 0;
	int input_file_fd = open(filename, O_RDONLY);
	if (input_file_fd >= 0) {
		struct stat input_file_stat;
		if (fstat(input_file_fd, &input_file_stat) == 0) {
			file_size = input_file_stat.st_size;
			file_data = mmap(0, file_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, input_file_fd, 0);
		}
		close(input_file_fd);
	}
	return (struct file_content){file_data, (u32)file_size};
}

static inline u8* pixel_ptr(u8 *pixel_data, u32 bytes_per_pixel, u32 width, u32 x, u32 y) {
	return pixel_data + ( (u64)y * (u64)width + (u64)x ) * bytes_per_pixel;
}

static inline int color_match(const u8 *p, const u8 *target_color) {
	// Compare 3 bytes
	return (p[0] == target_color[0] && p[1] == target_color[1] && p[2] == target_color[2]);
}

static int check_pattern(u8 *pixel_data, const struct bmp_header *header, const u8 target_color[3],
                         const u32 x, const u32 y, u32 bytes_per_pixel) {
	// Pattern offsets
	// It's best to keep these small and fixed in an array, but we can inline them if needed.
	const int pattern_offsets[][2] = {
		{0, 1}, {0, 2}, {0, 3}, {0, 4}, {0, 5}, {0, 6}, {0, 7},
		{0, 7}, {1, 7}, {2, 7}, {3, 7}, {4, 7}, {5, 7}, {6, 7}
	};
	const int pattern_size = (int)(sizeof(pattern_offsets) / sizeof(pattern_offsets[0]));

	const u32 width = header->width;
	const u32 height = header->height;

	for (int i = 0; i < pattern_size; i++) {
		u32 px = x + (u32)pattern_offsets[i][0];
		u32 py = y + (u32)pattern_offsets[i][1];

		if (px >= width || py >= height) {
			return 0; // Out of bounds
		}
		u8 *p = pixel_ptr(pixel_data, bytes_per_pixel, width, px, py);
		if (!color_match(p, target_color)) {
			return 0; // Mismatch
		}
	}

	return 1;
}

static void find_header(const struct bmp_header *header, const u8 target_color[3], u8 *pixel_data,
                        u32 *found_width, u32 *found_height) {
	const u32 width = header->width;
	const u32 height = header->height;
	const u32 bytes_per_pixel = header->bit_per_pixel / 8;

	// We will search in parallel. Once a match is found, we set a shared flag.
	int found = 0;
	u32 found_x = 0, found_y = 0;

	#pragma omp parallel for collapse(2) schedule(dynamic) shared(found, found_x, found_y)
	for (u32 y = 0; y < height; ++y) {
		for (u32 x = 0; x < width; ++x) {
			if (__atomic_load_n(&found, __ATOMIC_ACQUIRE)) {
				// If another thread found it, stop.
				break;
			}

			u8 *pixel = pixel_ptr(pixel_data, bytes_per_pixel, width, x, y);
			if (color_match(pixel, target_color)) {
				if (check_pattern(pixel_data, header, target_color, x, y, bytes_per_pixel)) {
					// Mark as found, store coordinates
					__atomic_store_n(&found, 1, __ATOMIC_RELEASE);
					found_x = x;
					found_y = y;
					break;
				}
			}
		}
		// Another possible check after inner loop
		if (__atomic_load_n(&found, __ATOMIC_ACQUIRE)) {
			// If found, break outer loop as well
			break;
		}
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

	// After finding the header pattern, decode the message
	if (found_width + 7 < width) {
		u8 *message_pixel = pixel_ptr(pixel_data, bytes_per_pixel, width, found_width + 7, found_height + 7);
		if (!message_pixel) {
			PRINT_ERROR("Message pixel is out of bounds\n");
			return 1;
		}

		u32 message_length = (u32)(message_pixel[0] + message_pixel[2]); // as per original logic

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

			// Extract up to 3 bytes from pixel
			for (int j = 0; j < 3; ++j) {
				message[message_index++] = pixel[j];
				message_length -= 1;
				if (message_length == 0) {
					write(1, message, message_index);
					write(1, "\n", 1);
					return 0;
				}
			}
			current_width += 1;
			// every 6 pixels, move up one row (as per original logic)
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
