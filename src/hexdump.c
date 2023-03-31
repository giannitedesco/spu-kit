#include <stdint.h>
#include <stdio.h>
#include <ctype.h>

#include <spu-kit/system.h>

void hex_dump_addr(const size_t len;
		const uint8_t tmp[static len],
		const size_t len,
		size_t llen,
		const size_t addr)
{
	size_t i, j;
	size_t line;

	if (!len)
		return;
	if (!llen)
		llen = 0x10;

	for (j = 0; j < len; j += line, tmp += line) {
		if (j + llen > len) {
			line = len - j;
		} else {
			line = llen;
		}

		printf( " | %04zx : ", j + addr);

		for (i = 0; i < line; i++) {
			if (isprint(tmp[i])) {
				printf( "%c", tmp[i]);
			} else {
				printf( ".");
			}
		}

		for (; i < llen; i++)
			printf( " ");

		for (i = 0; i < line; i++)
			printf( " %02x", tmp[i]);

		printf( "\n");
	}
	//printf( "\n");
}

void hex_dump(const size_t len;
		const uint8_t tmp[static len],
		const size_t len,
		size_t llen)
{
	hex_dump_addr(tmp, len, llen, 0);
}
